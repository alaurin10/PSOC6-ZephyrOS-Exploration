/*
 * Audio analysis: sound level, spectrum and clap detection.
 *
 * Levels are in dB relative to digital full scale: a full-scale sine has a
 * peak of 0 dBFS and an RMS of -3 dBFS. Spectrum bands are scaled so that a
 * full-scale sine reads 0 dB in its band.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_dsp.h"

#define FULL_SCALE 32768.0f

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static inline long clampl(long v, long lo, long hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* ---- Level -------------------------------------------------------------- */

float dsp_db(float ratio)
{
	if (ratio <= 1e-6f) {
		return DSP_DB_MIN;
	}

	float db = 20.0f * log10f(ratio);

	return db < DSP_DB_MIN ? DSP_DB_MIN : db;
}

void dsp_level_measure(const int16_t *x, size_t n, struct dsp_level *out)
{
	int64_t sum_sq = 0;
	int32_t peak = 0;
	bool clipped = false;

	for (size_t i = 0; i < n; i++) {
		int32_t v = x[i];

		sum_sq += v * v;
		if (abs(v) > peak) {
			peak = abs(v);
		}
		if (v == INT16_MAX || v == INT16_MIN) {
			clipped = true;
		}
	}

	float rms = n ? sqrtf((float)sum_sq / (float)n) : 0.0f;

	out->rms_dbfs = dsp_db(rms / FULL_SCALE);
	out->peak_dbfs = dsp_db((float)peak / FULL_SCALE);
	out->clipped = clipped;
}

/* ---- Spectrum ------------------------------------------------------------ */

int dsp_spectrum_init(struct dsp_spectrum *s, uint32_t sample_rate)
{
	memset(s, 0, sizeof(*s));
	s->sample_rate = sample_rate;

	if (arm_rfft_fast_init_1024_f32(&s->fft) != ARM_MATH_SUCCESS) {
		return -1;
	}

	/* Periodic Hann window: low leakage, so a loud band doesn't smear far. */
	for (int i = 0; i < DSP_FFT_SIZE; i++) {
		float w = 0.5f - 0.5f * cosf(2.0f * PI * (float)i / DSP_FFT_SIZE);

		s->window[i] = w;
		s->window_power += w * w;
	}

	/* Log-spaced band edges from DSP_BAND_F_MIN to Nyquist, mapped to FFT
	 * bins (skipping DC). At the low end a band is narrower than a bin, so
	 * neighbouring bands share one.
	 */
	const float nyquist = sample_rate / 2.0f;
	const float bin_hz = (float)sample_rate / DSP_FFT_SIZE;
	const float ratio = nyquist / DSP_BAND_F_MIN;

	for (int b = 0; b <= DSP_BANDS; b++) {
		s->band_edge_hz[b] = DSP_BAND_F_MIN * powf(ratio, (float)b / DSP_BANDS);
	}
	for (int b = 0; b < DSP_BANDS; b++) {
		int lo = (int)lroundf(s->band_edge_hz[b] / bin_hz);
		int hi = (int)lroundf(s->band_edge_hz[b + 1] / bin_hz);

		lo = (int)clampl(lo, 1, DSP_FFT_SIZE / 2 - 1);
		hi = (int)clampl(hi, lo + 1, DSP_FFT_SIZE / 2);
		s->band_lo[b] = (uint16_t)lo;
		s->band_hi[b] = (uint16_t)hi;
	}
	return 0;
}

void dsp_spectrum_push(struct dsp_spectrum *s, const int16_t *x, size_t n)
{
	if (n > DSP_FFT_SIZE) {
		x += n - DSP_FFT_SIZE;
		n = DSP_FFT_SIZE;
	}

	/* Slide the analysis window along by n samples. */
	memmove(s->history, s->history + n, (DSP_FFT_SIZE - n) * sizeof(int16_t));
	memcpy(s->history + DSP_FFT_SIZE - n, x, n * sizeof(int16_t));

	/* Samples as fractions of full scale, windowed (CMSIS vector ops). */
	arm_q15_to_float(s->history, s->frame, DSP_FFT_SIZE);
	arm_mult_f32(s->frame, s->window, s->frame, DSP_FFT_SIZE);
	arm_rfft_fast_f32(&s->fft, s->frame, s->bins, 0);

	/* Output: bins[0] = DC, bins[1] = Nyquist, then re/im pairs for bins
	 * 1..N/2-1. Their squared magnitudes go into frame[] (free now), so
	 * frame[k - 1] = |X[k]|^2.
	 */
	float *power = s->frame;

	arm_cmplx_mag_squared_f32(&s->bins[2], power, DSP_FFT_SIZE / 2 - 1);

	/* By Parseval, the positive-frequency bins of a windowed sine of
	 * amplitude A hold N * A^2 / 4 * sum(w^2): scale that to A^2.
	 */
	const float scale = 4.0f / ((float)DSP_FFT_SIZE * s->window_power);

	for (int b = 0; b < DSP_BANDS; b++) {
		float p = 0.0f;

		for (int k = s->band_lo[b]; k < s->band_hi[b] && k < DSP_FFT_SIZE / 2; k++) {
			p += power[k - 1];
		}
		s->acc[b] += p * scale;
	}
	s->acc_count++;
}

void dsp_spectrum_take(struct dsp_spectrum *s, float smoothing, float gain_db, float floor_db,
		       float range_db, uint8_t out[DSP_BANDS])
{
	smoothing = clampf(smoothing, 0.0f, 1.0f);

	for (int b = 0; b < DSP_BANDS; b++) {
		if (s->acc_count > 0) {
			float avg = s->acc[b] / (float)s->acc_count;

			s->smooth[b] = smoothing * s->smooth[b] + (1.0f - smoothing) * avg;
		}
		s->acc[b] = 0.0f;

		float db = dsp_spectrum_band_db(s, b) + gain_db;

		out[b] = (uint8_t)clampl(lroundf(db - floor_db), 0, (long)range_db);
	}
	s->acc_count = 0;
}

float dsp_spectrum_band_db(const struct dsp_spectrum *s, int band)
{
	float p = s->smooth[band];

	if (p <= 1e-12f) {
		return DSP_DB_MIN;
	}

	float db = 10.0f * log10f(p);

	return db < DSP_DB_MIN ? DSP_DB_MIN : db;
}

float dsp_band_center_hz(const struct dsp_spectrum *s, int band)
{
	return sqrtf(s->band_edge_hz[band] * s->band_edge_hz[band + 1]);
}

/* ---- Clap detector -------------------------------------------------------- */

/*
 * A clap is a short, broadband burst: its energy jumps up within a few
 * milliseconds and dies away within a few tens. Speech and music build up
 * and sustain; room rumble is low-frequency. So the detector:
 *
 *  1. works on the first difference of the signal (a gentle high-pass that
 *     weights the clap's high frequencies and ignores rumble), in ~8 ms
 *     sub-frames;
 *  2. tracks the background level (quick to follow it down, slow to follow
 *     it up, and frozen while a burst is being judged);
 *  3. starts a candidate on a sharp rise well above the background, and
 *     confirms it only if the energy falls back by DECAY_DB within
 *     ONSET_MAX_MS. Longer bursts are treated as sustained sound.
 *
 * A second clap 100..700 ms after the first is reported as a double clap.
 */
#define SUBFRAMES_PER_SECOND 125     /* 8 ms sub-frames */
#define RISE_DB              9.0f    /* jump from the previous sub-frame */
#define ABS_MIN_DB           (-70.0f) /* ignore anything quieter than this */
#define THRESH_DB_MAX        30.0f   /* above background, at sensitivity 0 */
#define THRESH_DB_MIN        10.0f   /* above background, at sensitivity 1 */
#define DECAY_DB             12.0f
#define ONSET_MAX_MS         56
#define REFRACTORY_MS        100
#define SUSTAIN_MAX_MS       2000
#define DOUBLE_MIN_MS        100
#define DOUBLE_MAX_MS        700
#define FLOOR_DOWN           0.25f   /* background follows quieter sound quickly */
#define FLOOR_UP             0.01f   /* ... and louder sound slowly (~0.8 s) */

static uint32_t ms_to_subs(const struct dsp_clap *c, uint32_t ms)
{
	return (ms * c->subs_per_second + 999U) / 1000U;
}

void dsp_clap_init(struct dsp_clap *c, uint32_t sample_rate)
{
	memset(c, 0, sizeof(*c));
	c->sub_len = sample_rate / SUBFRAMES_PER_SECOND;
	if (c->sub_len < 16) {
		c->sub_len = 16;
	}
	c->subs_per_second = sample_rate / c->sub_len;
	c->floor_db = -60.0f;
	c->prev_db = DSP_DB_MIN;
	c->state = CLAP_IDLE;
	c->subs_since_clap = UINT32_MAX / 2;
	c->last_peak_dbfs = DSP_DB_MIN;
}

static void set_state(struct dsp_clap *c, enum dsp_clap_state state)
{
	c->state = state;
	c->state_subs = 0;
}

static enum dsp_clap_event clap_subframe(struct dsp_clap *c, float sensitivity)
{
	float energy = (float)c->sub_energy / ((float)c->sub_len * FULL_SCALE * FULL_SCALE);
	float db = energy > 1e-12f ? 10.0f * log10f(energy) : DSP_DB_MIN;
	float thresh = THRESH_DB_MAX - (THRESH_DB_MAX - THRESH_DB_MIN) * clampf(sensitivity, 0, 1);
	enum dsp_clap_event ev = DSP_CLAP_NONE;

	if (c->subs_since_clap < UINT32_MAX / 2) {
		c->subs_since_clap++;
	}
	c->state_subs++;

	switch (c->state) {
	case CLAP_IDLE:
		if (db > c->floor_db + thresh && db - c->prev_db > RISE_DB && db > ABS_MIN_DB) {
			set_state(c, CLAP_ONSET);
			c->onset_peak_db = db;
			c->onset_peak_sample = c->sub_peak;
		} else {
			c->floor_db += (db - c->floor_db) * (db < c->floor_db ? FLOOR_DOWN : FLOOR_UP);
		}
		break;

	case CLAP_ONSET:
		if (db > c->onset_peak_db) {
			c->onset_peak_db = db;
		}
		if (c->sub_peak > c->onset_peak_sample) {
			c->onset_peak_sample = c->sub_peak;
		}
		if (db < c->onset_peak_db - DECAY_DB) {
			/* A short burst that died away: a clap. */
			bool pair = c->subs_since_clap >= ms_to_subs(c, DOUBLE_MIN_MS) &&
				    c->subs_since_clap <= ms_to_subs(c, DOUBLE_MAX_MS);

			ev = pair ? DSP_CLAP_DOUBLE : DSP_CLAP_SINGLE;
			/* After a pair, a third clap starts a new one. */
			c->subs_since_clap = pair ? UINT32_MAX / 2 : 0;
			c->last_peak_dbfs = dsp_db((float)c->onset_peak_sample / FULL_SCALE);
			set_state(c, CLAP_REFRACTORY);
		} else if (c->state_subs > ms_to_subs(c, ONSET_MAX_MS)) {
			set_state(c, CLAP_SUSTAINED);
		}
		break;

	case CLAP_SUSTAINED:
		/* Let a steady sound slowly become the new background. */
		c->floor_db += (db - c->floor_db) * FLOOR_UP;
		if (db < c->floor_db + thresh / 2 ||
		    c->state_subs > ms_to_subs(c, SUSTAIN_MAX_MS)) {
			set_state(c, CLAP_IDLE);
		}
		break;

	case CLAP_REFRACTORY:
		if (c->state_subs >= ms_to_subs(c, REFRACTORY_MS)) {
			set_state(c, CLAP_IDLE);
		}
		break;
	}

	c->prev_db = db;
	return ev;
}

enum dsp_clap_event dsp_clap_process(struct dsp_clap *c, const int16_t *x, size_t n,
				     float sensitivity)
{
	enum dsp_clap_event result = DSP_CLAP_NONE;

	for (size_t i = 0; i < n; i++) {
		/* Integer maths per sample: no float on the hot path. */
		int32_t d = (int32_t)x[i] - c->prev_sample;
		int32_t a = abs(x[i]);

		c->prev_sample = x[i];
		/* |d| <= 65535, so d^2 always fits unsigned 32 bits. */
		c->sub_energy += (uint32_t)d * (uint32_t)d;
		if (a > c->sub_peak) {
			c->sub_peak = a;
		}

		if (++c->sub_fill == c->sub_len) {
			enum dsp_clap_event ev = clap_subframe(c, sensitivity);

			if (ev != DSP_CLAP_NONE) {
				result = ev;
			}
			c->sub_fill = 0;
			c->sub_energy = 0;
			c->sub_peak = 0;
		}
	}
	return result;
}

/*
 * Unit tests for the audio analysis (src/audio_dsp.c), on synthetic signals.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/ztest.h>

#include "audio_dsp.h"

#define RATE  16000
#define BLOCK 512

static int16_t sig[RATE * 2]; /* up to 2 s of audio */
static struct dsp_spectrum spec;
static struct dsp_clap clap;
static uint32_t rng = 1;

/* Uniform noise in -1..1 (deterministic). */
static float noise(void)
{
	rng = rng * 1664525u + 1013904223u;
	return (float)(int32_t)rng / 2147483648.0f;
}

static int16_t to_pcm(float v)
{
	v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
	return (int16_t)lroundf(v * 32767.0f);
}

static void fill_sine(int16_t *x, size_t n, float hz, float amp)
{
	for (size_t i = 0; i < n; i++) {
		x[i] = to_pcm(amp * sinf(2.0f * PI * hz * (float)i / RATE));
	}
}

static void fill_noise(int16_t *x, size_t n, float amp)
{
	for (size_t i = 0; i < n; i++) {
		x[i] = to_pcm(amp * noise());
	}
}

/* Add a clap: a white-noise burst that decays with a 5 ms time constant. */
static void add_clap(int16_t *x, size_t at, float amp)
{
	for (size_t i = 0; i < RATE / 25 && at + i < ARRAY_SIZE(sig); i++) {
		float env = amp * expf(-(float)i / (0.005f * RATE));

		x[at + i] = to_pcm((float)x[at + i] / 32767.0f + env * noise());
	}
}

/* Run the detector over n samples in capture-sized blocks; count events. */
static void run_clap(const int16_t *x, size_t n, float sens, int *singles, int *doubles)
{
	*singles = *doubles = 0;
	for (size_t i = 0; i + BLOCK <= n; i += BLOCK) {
		enum dsp_clap_event ev = dsp_clap_process(&clap, x + i, BLOCK, sens);

		*singles += ev == DSP_CLAP_SINGLE;
		*doubles += ev == DSP_CLAP_DOUBLE;
	}
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	rng = 1;
	memset(sig, 0, sizeof(sig));
	dsp_clap_init(&clap, RATE);
}

ZTEST_SUITE(dsp, NULL, NULL, before, NULL, NULL);

/* ---- Level ---------------------------------------------------------------- */

ZTEST(dsp, test_level_of_half_scale_sine)
{
	struct dsp_level l;

	fill_sine(sig, RATE / 10, 1000.0f, 0.5f);
	dsp_level_measure(sig, RATE / 10, &l);

	/* RMS of a sine is amplitude / sqrt(2): 0.5 -> -9.03 dB; peak 0.5 -> -6.02 dB. */
	zassert_within(l.rms_dbfs, -9.03f, 0.05f, "rms %f", (double)l.rms_dbfs);
	zassert_within(l.peak_dbfs, -6.02f, 0.05f, "peak %f", (double)l.peak_dbfs);
	zassert_false(l.clipped);
}

ZTEST(dsp, test_level_silence_and_clipping)
{
	struct dsp_level l;

	dsp_level_measure(sig, 1000, &l);
	zassert_equal(l.rms_dbfs, DSP_DB_MIN);
	zassert_equal(l.peak_dbfs, DSP_DB_MIN);

	sig[500] = INT16_MIN;
	dsp_level_measure(sig, 1000, &l);
	zassert_true(l.clipped);
	zassert_within(l.peak_dbfs, 0.0f, 0.01f);
}

/* ---- Spectrum -------------------------------------------------------------- */

ZTEST(dsp, test_bands_cover_40hz_to_nyquist)
{
	zassert_ok(dsp_spectrum_init(&spec, RATE));
	zassert_within(spec.band_edge_hz[0], DSP_BAND_F_MIN, 0.01f);
	zassert_within(spec.band_edge_hz[DSP_BANDS], RATE / 2.0f, 0.5f);

	for (int b = 0; b < DSP_BANDS; b++) {
		zassert_true(spec.band_lo[b] >= 1, "band %d uses DC", b);
		zassert_true(spec.band_hi[b] > spec.band_lo[b], "band %d is empty", b);
		zassert_true(spec.band_hi[b] <= DSP_FFT_SIZE / 2, "band %d past Nyquist", b);
		if (b > 0) {
			zassert_true(spec.band_lo[b] >= spec.band_lo[b - 1], "band %d out of order", b);
			zassert_true(dsp_band_center_hz(&spec, b) > dsp_band_center_hz(&spec, b - 1));
		}
	}
}

/* A tone shows up in the band that contains it, at its level, and nowhere else. */
static void check_tone(float hz)
{
	uint8_t out[DSP_BANDS];
	int loudest = 0;

	zassert_ok(dsp_spectrum_init(&spec, RATE));
	fill_sine(sig, 6 * BLOCK, hz, 0.5f);
	/* Fill the window first: a tone switching on mid-window splatters. */
	for (int i = 0; i < 2; i++) {
		dsp_spectrum_push(&spec, sig + i * BLOCK, BLOCK);
	}
	dsp_spectrum_take(&spec, 0.0f, 0.0f, -100.0f, 100.0f, out);
	for (int i = 2; i < 6; i++) {
		dsp_spectrum_push(&spec, sig + i * BLOCK, BLOCK);
	}
	dsp_spectrum_take(&spec, 0.0f, 0.0f, -100.0f, 100.0f, out);

	for (int b = 1; b < DSP_BANDS; b++) {
		if (dsp_spectrum_band_db(&spec, b) > dsp_spectrum_band_db(&spec, loudest)) {
			loudest = b;
		}
	}
	zassert_true(hz >= spec.band_edge_hz[loudest] * 0.9f &&
		     hz <= spec.band_edge_hz[loudest + 1] * 1.1f,
		     "%.0f Hz tone peaked in band %d (%.0f..%.0f Hz)", (double)hz, loudest,
		     (double)spec.band_edge_hz[loudest], (double)spec.band_edge_hz[loudest + 1]);

	/* The window spreads the tone over a few bins (maybe two bands): their
	 * sum is the tone's power, -6.02 dB for amplitude 0.5.
	 */
	float p = 0.0f;

	for (int b = 0; b < DSP_BANDS; b++) {
		if (abs(b - loudest) <= 2 && spec.band_lo[b] != spec.band_lo[loudest]) {
			p += powf(10.0f, dsp_spectrum_band_db(&spec, b) / 10.0f);
		}
	}
	p += powf(10.0f, dsp_spectrum_band_db(&spec, loudest) / 10.0f);
	zassert_within(10.0f * log10f(p), -6.02f, 1.0f, "tone power %f dB",
		       (double)(10.0f * log10f(p)));
	zassert_within(out[loudest], 94, 3, "byte value %u", out[loudest]);

	/* Far from the tone, only leakage remains. */
	for (int b = 0; b < DSP_BANDS; b++) {
		float f = dsp_band_center_hz(&spec, b);

		if (f < hz / 2.0f || f > hz * 2.0f) {
			zassert_true(dsp_spectrum_band_db(&spec, b) < -6.0f - 40.0f,
				     "band %d (%.0f Hz) at %.1f dB", b, (double)f,
				     (double)dsp_spectrum_band_db(&spec, b));
		}
	}
}

ZTEST(dsp, test_tone_250hz)
{
	check_tone(250.0f);
}

ZTEST(dsp, test_tone_1khz)
{
	check_tone(1000.0f);
}

ZTEST(dsp, test_tone_5khz)
{
	check_tone(5000.0f);
}

ZTEST(dsp, test_smoothing_and_gain)
{
	uint8_t out[DSP_BANDS];
	int b;

	zassert_ok(dsp_spectrum_init(&spec, RATE));
	fill_sine(sig, 4 * BLOCK, 1000.0f, 0.5f);
	for (int i = 0; i < 4; i++) {
		dsp_spectrum_push(&spec, sig + i * BLOCK, BLOCK);
	}
	dsp_spectrum_take(&spec, 0.0f, 0.0f, -100.0f, 100.0f, out);
	for (b = 0; spec.band_edge_hz[b + 1] < 1000.0f; b++) {
	}
	float before_db = dsp_spectrum_band_db(&spec, b);

	/* Flush the tone out of the 1024-sample window; smoothing 1 holds the bands. */
	memset(sig, 0, sizeof(sig));
	for (int i = 0; i < 2; i++) {
		dsp_spectrum_push(&spec, sig, BLOCK);
	}
	dsp_spectrum_take(&spec, 1.0f, 0.0f, -100.0f, 100.0f, out);

	/* Silence with smoothing 0.5: the band keeps half its power (-3 dB). */
	for (int i = 0; i < 2; i++) {
		dsp_spectrum_push(&spec, sig, BLOCK);
	}
	uint8_t plain[DSP_BANDS];

	dsp_spectrum_take(&spec, 0.5f, 0.0f, -100.0f, 100.0f, plain);
	zassert_within(dsp_spectrum_band_db(&spec, b), before_db - 3.01f, 0.1f);

	/* Gain only shifts the output: -10 dB = -10 counts (smoothing 1 holds). */
	dsp_spectrum_take(&spec, 1.0f, -10.0f, -100.0f, 100.0f, out);
	zassert_equal(out[b], plain[b] - 10);
}

/* ---- Clap detector ------------------------------------------------------------ */

ZTEST(dsp, test_quiet_room_has_no_claps)
{
	int singles, doubles;

	/* Background noise around -60 dBFS plus a loud 30 Hz rumble. */
	fill_noise(sig, RATE * 2, 0.002f);
	for (size_t i = 0; i < RATE * 2; i++) {
		sig[i] += to_pcm(0.05f * sinf(2.0f * PI * 30.0f * (float)i / RATE));
	}
	run_clap(sig, RATE * 2, 1.0f, &singles, &doubles);
	zassert_equal(singles + doubles, 0);
}

ZTEST(dsp, test_single_clap)
{
	int singles, doubles;

	fill_noise(sig, RATE * 2, 0.002f);
	add_clap(sig, RATE, 0.5f);
	run_clap(sig, RATE * 2, 0.5f, &singles, &doubles);
	zassert_equal(singles, 1);
	zassert_equal(doubles, 0);
	zassert_within(clap.last_peak_dbfs, -6.0f, 6.0f, "peak %f", (double)clap.last_peak_dbfs);
}

ZTEST(dsp, test_double_clap)
{
	int singles, doubles;

	fill_noise(sig, RATE * 2, 0.002f);
	add_clap(sig, RATE / 2, 0.5f);
	add_clap(sig, RATE / 2 + RATE * 3 / 10, 0.5f); /* 300 ms later */
	run_clap(sig, RATE * 2, 0.5f, &singles, &doubles);
	zassert_equal(singles, 1);
	zassert_equal(doubles, 1);
}

ZTEST(dsp, test_claps_far_apart_are_two_singles)
{
	int singles, doubles;

	fill_noise(sig, RATE * 2, 0.002f);
	add_clap(sig, RATE / 4, 0.5f);
	add_clap(sig, RATE / 4 + RATE, 0.5f); /* 1 s later */
	run_clap(sig, RATE * 2, 0.5f, &singles, &doubles);
	zassert_equal(singles, 2);
	zassert_equal(doubles, 0);
}

ZTEST(dsp, test_sustained_sound_is_not_a_clap)
{
	int singles, doubles;

	/* A loud 400 ms burst that starts sharply (like a shout or a note). */
	fill_noise(sig, RATE * 2, 0.002f);
	for (size_t i = 0; i < RATE * 4 / 10; i++) {
		float env = 0.3f * MIN(1.0f, (float)i / (0.005f * RATE));

		sig[RATE / 2 + i] += to_pcm(env * noise());
	}
	run_clap(sig, RATE * 2, 1.0f, &singles, &doubles);
	zassert_equal(singles + doubles, 0);
}

ZTEST(dsp, test_sensitivity)
{
	int singles, doubles;

	/* A quiet clap, ~20 dB over the background. */
	fill_noise(sig, RATE * 2, 0.002f);
	add_clap(sig, RATE, 0.03f);
	run_clap(sig, RATE * 2, 0.0f, &singles, &doubles);
	zassert_equal(singles, 0, "insensitive detector heard a quiet clap");

	dsp_clap_init(&clap, RATE);
	run_clap(sig, RATE * 2, 1.0f, &singles, &doubles);
	zassert_equal(singles, 1, "sensitive detector missed a quiet clap");
}

ZTEST(dsp, test_knock_is_not_a_clap)
{
	int singles, doubles;

	/* A knock on the desk or the board: a loud 300 Hz thump that dies in ~10 ms. */
	fill_noise(sig, RATE * 2, 0.002f);
	for (size_t i = 0; i < RATE / 25; i++) {
		float env = 0.5f * expf(-(float)i / (0.004f * RATE));

		sig[RATE + i] += to_pcm(env * sinf(2.0f * PI * 300.0f * (float)i / RATE));
	}
	run_clap(sig, RATE * 2, 1.0f, &singles, &doubles);
	zassert_equal(singles + doubles, 0);
	zassert_equal(clap.rejected_thump, 1, "the knock was not judged as a burst");
	zassert_within(clap.last_burst_hz, 300.0f, 100.0f, "%f Hz", (double)clap.last_burst_hz);
}

ZTEST(dsp, test_quiet_click_needs_high_sensitivity)
{
	int singles, doubles;

	/* A keyboard click in a very quiet room: peak about -40 dBFS, 30 dB over the
	 * background, so only its level tells it from a clap.
	 */
	fill_noise(sig, RATE * 2, 0.0002f);
	add_clap(sig, RATE, 0.01f);
	run_clap(sig, RATE * 2, 0.5f, &singles, &doubles);
	zassert_equal(singles, 0, "a -40 dBFS click counted at sensitivity 0.5");
	zassert_equal(clap.rejected_quiet, 1);

	dsp_clap_init(&clap, RATE);
	run_clap(sig, RATE * 2, 1.0f, &singles, &doubles);
	zassert_equal(singles, 1, "sensitivity 1 should accept a -40 dBFS clap");
}

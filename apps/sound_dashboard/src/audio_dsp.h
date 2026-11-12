/*
 * Audio analysis: sound level, spectrum and clap detection.
 *
 * Pure signal processing with no Zephyr or hardware dependencies, so the
 * same code runs in the unit tests (tests/dsp) and can move to another core.
 * Uses CMSIS-DSP for the FFT.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AUDIO_DSP_H_
#define AUDIO_DSP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <arm_math.h>

#define DSP_FFT_SIZE    1024
#define DSP_BANDS       64
#define DSP_BAND_F_MIN  40.0f    /* Hz, lower edge of the first band */
#define DSP_DB_MIN      (-120.0f) /* reported for silence */

/* ---- Level -------------------------------------------------------------- */

struct dsp_level {
	float rms_dbfs;   /* relative to a full-scale square wave (0 dBFS) */
	float peak_dbfs;  /* largest sample magnitude */
	bool clipped;     /* a sample reached full scale */
};

/** 20 log10(ratio), floored at DSP_DB_MIN. */
float dsp_db(float ratio);

/** Level of n samples. */
void dsp_level_measure(const int16_t *x, size_t n, struct dsp_level *out);

/* ---- Spectrum ------------------------------------------------------------ */

struct dsp_spectrum {
	arm_rfft_fast_instance_f32 fft;
	uint32_t sample_rate;
	float window[DSP_FFT_SIZE];
	float window_power;              /* sum of window^2, for scaling */
	int16_t history[DSP_FFT_SIZE];   /* the latest DSP_FFT_SIZE samples */
	float frame[DSP_FFT_SIZE];       /* FFT input (destroyed by the FFT) */
	float bins[DSP_FFT_SIZE];        /* FFT output, packed complex */
	uint16_t band_lo[DSP_BANDS];     /* first FFT bin of each band */
	uint16_t band_hi[DSP_BANDS];     /* one past its last bin */
	float band_edge_hz[DSP_BANDS + 1];
	float acc[DSP_BANDS];            /* summed band power since the last take */
	uint32_t acc_count;
	float smooth[DSP_BANDS];         /* smoothed band power */
};

/**
 * Set up the window, the FFT and DSP_BANDS log-spaced bands from
 * DSP_BAND_F_MIN to the Nyquist frequency.
 */
int dsp_spectrum_init(struct dsp_spectrum *s, uint32_t sample_rate);

/**
 * Append n samples (n <= DSP_FFT_SIZE) to the analysis window and run one
 * FFT over the latest DSP_FFT_SIZE samples, adding its band powers to the
 * accumulator.
 */
void dsp_spectrum_push(struct dsp_spectrum *s, const int16_t *x, size_t n);

/**
 * Average the spectra pushed since the last call, apply exponential
 * smoothing (0 = none .. 1 = frozen), add gain_db, and write each band as
 * dB above floor_db, clamped to 0..range_db.
 */
void dsp_spectrum_take(struct dsp_spectrum *s, float smoothing, float gain_db, float floor_db,
		       float range_db, uint8_t out[DSP_BANDS]);

/** Band power in dBFS (full-scale sine = 0 dB) after the last take, for tests. */
float dsp_spectrum_band_db(const struct dsp_spectrum *s, int band);

/** Geometric centre of a band in Hz. */
float dsp_band_center_hz(const struct dsp_spectrum *s, int band);

/* ---- Clap detector -------------------------------------------------------- */

enum dsp_clap_event {
	DSP_CLAP_NONE,
	DSP_CLAP_SINGLE,
	DSP_CLAP_DOUBLE, /* the second clap of a pair */
};

enum dsp_clap_state {
	CLAP_IDLE,
	CLAP_ONSET,      /* sharp rise seen, waiting for it to decay */
	CLAP_SUSTAINED,  /* too long to be a clap: wait for it to end */
	CLAP_REFRACTORY, /* just detected one: ignore its tail */
};

struct dsp_clap {
	uint32_t sub_len;        /* samples per analysis sub-frame (~8 ms) */
	uint32_t sub_fill;
	uint64_t sub_energy;     /* summed squared first differences (raw sample units) */
	int32_t sub_peak;
	int16_t prev_sample;
	float prev_db;
	float floor_db;          /* adaptive background level */
	enum dsp_clap_state state;
	uint32_t state_subs;     /* sub-frames spent in the current state */
	float onset_peak_db;
	int32_t onset_peak_sample;
	uint32_t subs_since_clap;
	uint32_t subs_per_second;
	float last_peak_dbfs;    /* peak sample level of the last detected clap */
};

void dsp_clap_init(struct dsp_clap *c, uint32_t sample_rate);

/**
 * Feed n samples. sensitivity: 0 (only loud, sharp claps) .. 1 (quiet ones).
 * Returns the event detected in these samples, if any.
 */
enum dsp_clap_event dsp_clap_process(struct dsp_clap *c, const int16_t *x, size_t n,
				     float sensitivity);

#endif /* AUDIO_DSP_H_ */

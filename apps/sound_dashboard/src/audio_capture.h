/*
 * Audio capture: the PDM microphone stream and its analysis thread.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AUDIO_CAPTURE_H_
#define AUDIO_CAPTURE_H_

#include <stddef.h>
#include <stdint.h>

/** Start the capture thread; it configures the microphone and starts streaming. */
void audio_capture_start(void);

/** Centre frequency (Hz) of a spectrum band on audio_spectrum_chan. */
float audio_capture_band_center_hz(int band);

/**
 * Record the next CONFIG_APP_AUDIO_REC_SECONDS of raw samples into RAM, with
 * the user LED on meanwhile. -ENOTSUP without a recorder, -EBUSY while one runs.
 */
int audio_rec_start(void);

/** The finished recording (mono, 16 bit), or NULL while recording or before the first. */
const int16_t *audio_rec_get(size_t *n_samples);

#endif /* AUDIO_CAPTURE_H_ */

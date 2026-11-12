/*
 * Audio capture: the PDM microphone stream and its analysis thread.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AUDIO_CAPTURE_H_
#define AUDIO_CAPTURE_H_

/** Start the capture thread; it configures the microphone and starts streaming. */
void audio_capture_start(void);

#endif /* AUDIO_CAPTURE_H_ */

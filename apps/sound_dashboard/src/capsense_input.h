/*
 * CapSense buttons and slider, published on input_chan and ctrl_chan.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CAPSENSE_INPUT_H_
#define CAPSENSE_INPUT_H_

/** Start the scanning thread (it initializes and calibrates the sensors first). */
void capsense_input_start(void);

#endif /* CAPSENSE_INPUT_H_ */

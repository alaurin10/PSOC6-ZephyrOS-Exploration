/*
 * Onboard NTC thermistor (RT1) on the CY8CPROTO-062-4343W.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef THERMISTOR_H_
#define THERMISTOR_H_

/** Configure the ADC/GPIOs and start periodic sampling on the system work queue. */
int thermistor_init(void);

#endif /* THERMISTOR_H_ */

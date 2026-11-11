/*
 * LED controller: a hierarchical state machine (Zephyr SMF) that decides how
 * LED0 behaves from the temperature, the user's blink period and the mode.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LED_CTRL_H_
#define LED_CTRL_H_

#include <stdint.h>

enum led_mode {
	LED_MODE_TEMP,   /* LED follows the temperature (COLD / NORMAL / HOT) */
	LED_MODE_MANUAL, /* LED always blinks at the user period */
};

/** Configure the LED pin and start the controller thread. */
int led_ctrl_start(void);

/* Requests are queued to the controller thread; safe to call from any thread
 * (not from ISRs that can't tolerate a failed put). Return -ENOMSG if full.
 */
int led_ctrl_request_period(uint32_t period_ms);
int led_ctrl_request_mode(enum led_mode mode);

#endif /* LED_CTRL_H_ */

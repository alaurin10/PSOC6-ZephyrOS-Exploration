/*
 * zbus channels shared between modules.
 *
 * A channel is a typed, statically allocated message slot plus a list of
 * observers. Publishers don't know who observes; observers don't know who
 * published. The channel also always holds the latest message, so it can be
 * read at any time.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CHANNELS_H_
#define CHANNELS_H_

#include <stdint.h>
#include <stdlib.h>
#include <zephyr/zbus/zbus.h>

/* Published once per thermistor sample. */
struct temp_msg {
	uint32_t seq;         /* sample number, 0 = no reading yet */
	int32_t centi_c;      /* temperature in hundredths of a degree C */
	uint32_t ntc_ohm;
	uint16_t raw_a;
	uint16_t raw_b;
	int32_t latency_us;   /* counter IRQ -> sample start, -1 if not timer-triggered */
	int64_t timestamp_ms; /* k_uptime_get() when sampled */
};

ZBUS_CHAN_DECLARE(temp_chan);

/* Published by the LED controller whenever its mode, state or periods change. */
struct led_msg {
	uint8_t manual;            /* 1 = manual mode, 0 = temperature mode */
	char state[8];             /* "COLD", "NORMAL", "HOT", "MANUAL" */
	uint32_t blink_ms;         /* current toggle interval, 0 = solid on */
	uint32_t manual_period_ms; /* period used in manual mode */
};

ZBUS_CHAN_DECLARE(led_chan);

/* printf helpers for centi-degree values: printk(CENTI_FMT " C", CENTI_ARGS(v)) */
#define CENTI_FMT       "%s%d.%02d"
#define CENTI_ARGS(v)   ((v) < 0 ? "-" : ""), abs(v) / 100, abs(v) % 100

#endif /* CHANNELS_H_ */

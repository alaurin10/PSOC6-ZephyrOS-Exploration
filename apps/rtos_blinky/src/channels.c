/*
 * zbus channel definitions. Observers attach themselves from their own
 * modules with ZBUS_CHAN_ADD_OBS(), so this file never needs to change when
 * a new consumer is added.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "channels.h"

#define TEMP_MIN_PLAUSIBLE_CENTI (-4000)
#define TEMP_MAX_PLAUSIBLE_CENTI 12500

/*
 * Runs inside zbus_chan_pub(): returning false rejects the message (-ENOMSG)
 * before any observer sees it. Here: drop physically impossible readings.
 */
static bool temp_msg_validator(const void *msg, size_t msg_size)
{
	const struct temp_msg *m = msg;

	return msg_size == sizeof(*m) && m->centi_c >= TEMP_MIN_PLAUSIBLE_CENTI &&
	       m->centi_c <= TEMP_MAX_PLAUSIBLE_CENTI;
}

ZBUS_CHAN_DEFINE(temp_chan,               /* name */
		 struct temp_msg,         /* message type */
		 temp_msg_validator,      /* validator */
		 NULL,                    /* user data */
		 ZBUS_OBSERVERS_EMPTY,    /* observers are added elsewhere */
		 ZBUS_MSG_INIT(0)         /* initial value: seq 0 = "no reading yet" */
);

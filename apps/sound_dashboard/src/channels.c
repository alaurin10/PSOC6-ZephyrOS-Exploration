/*
 * zbus channel definitions. Observers attach themselves from their own
 * modules with ZBUS_CHAN_ADD_OBS(), so this file never needs to change when
 * a new consumer is added.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "channels.h"

const char *const audio_event_names[AUDIO_EVT_COUNT] = {"", "clap", "double", "clip"};
const char *const ctrl_source_names[CTRL_SRC_COUNT] = {"boot", "capsense", "web", "shell"};
const char *const net_state_names[NET_STATE_COUNT] = {"idle", "connecting", "connected",
						      "online"};

#define TEMP_MIN_PLAUSIBLE_CENTI (-4000)
#define TEMP_MAX_PLAUSIBLE_CENTI 12500

/*
 * Validators run inside zbus_chan_pub(): returning false rejects the message
 * (-ENOMSG) before any observer sees it.
 */
static bool temp_msg_validator(const void *msg, size_t msg_size)
{
	const struct temp_msg *m = msg;

	return msg_size == sizeof(*m) && m->centi_c >= TEMP_MIN_PLAUSIBLE_CENTI &&
	       m->centi_c <= TEMP_MAX_PLAUSIBLE_CENTI;
}

/* Commands can come from the network, so reject unknown ones at the door.
 * Value ranges are checked by ui_ctrl, which knows what each command means.
 */
static bool ctrl_msg_validator(const void *msg, size_t msg_size)
{
	const struct ctrl_msg *m = msg;

	return msg_size == sizeof(*m) && m->cmd < CTRL_CMD_COUNT;
}

ZBUS_CHAN_DEFINE(temp_chan,               /* name */
		 struct temp_msg,         /* message type */
		 temp_msg_validator,      /* validator */
		 NULL,                    /* user data */
		 ZBUS_OBSERVERS_EMPTY,    /* observers are added elsewhere */
		 ZBUS_MSG_INIT(0)         /* initial value: seq 0 = "no reading yet" */
);

ZBUS_CHAN_DEFINE(audio_level_chan, struct audio_level_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(audio_spectrum_chan, struct audio_spectrum_msg, NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(audio_event_chan, struct audio_event_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(input_chan, struct input_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.slider = -1));

ZBUS_CHAN_DEFINE(ctrl_chan, struct ctrl_msg, ctrl_msg_validator, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/* Overwritten with the saved state as soon as ui_ctrl starts. */
ZBUS_CHAN_DEFINE(ui_chan, struct ui_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(net_chan, struct net_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

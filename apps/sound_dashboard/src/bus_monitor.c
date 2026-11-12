/*
 * zbus diagnostics:
 *   bus                 list channels (message size, publish count) and observers
 *   bus watch on|off    log every message on the low-rate channels, decoded
 *
 * The watcher is a zbus listener: it runs synchronously inside
 * zbus_chan_pub(), in the publisher's thread, so it only formats a log line.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include "channels.h"

LOG_MODULE_REGISTER(bus, LOG_LEVEL_INF);

static atomic_t watching;

static void watch_cb(const struct zbus_channel *chan)
{
	if (!atomic_get(&watching)) {
		return;
	}

	/* Listeners may peek at the message in place: the channel is locked
	 * for the duration of the notification, so no copy is needed.
	 */
	const void *msg = zbus_chan_const_msg(chan);

	if (chan == &ctrl_chan) {
		const struct ctrl_msg *m = msg;

		LOG_INF("ctrl_chan: src %u cmd %u param %u value %d", m->source, m->cmd,
			m->param, m->value);
	} else if (chan == &input_chan) {
		const struct input_msg *m = msg;

		LOG_INF("input_chan: #%u btn0 %u btn1 %u slider %d", m->seq, m->btn0, m->btn1,
			m->slider);
	} else if (chan == &ui_chan) {
		const struct ui_msg *m = msg;

		LOG_INF("ui_chan: #%u view %u colour %u freeze %u params %u/%u/%u src %u", m->seq,
			m->view, m->colour, m->freeze, m->param[0], m->param[1], m->param[2],
			m->source);
	} else if (chan == &audio_event_chan) {
		const struct audio_event_msg *m = msg;

		LOG_INF("audio_event_chan: #%u type %u peak %d cdB", m->seq, m->type, m->peak_cdb);
	} else if (chan == &net_chan) {
		const struct net_msg *m = msg;

		LOG_INF("net_chan: state %u ssid '%s' ip %s rssi %d", m->state, m->ssid, m->ipv4,
			m->rssi);
	} else {
		LOG_INF("%s published", zbus_chan_name(chan));
	}
}

ZBUS_LISTENER_DEFINE(bus_watch_lis, watch_cb);
/* Priority 0: see the message before the other observers react to it. */
ZBUS_CHAN_ADD_OBS(ctrl_chan, bus_watch_lis, 0);
ZBUS_CHAN_ADD_OBS(input_chan, bus_watch_lis, 0);
ZBUS_CHAN_ADD_OBS(ui_chan, bus_watch_lis, 0);
ZBUS_CHAN_ADD_OBS(audio_event_chan, bus_watch_lis, 0);
ZBUS_CHAN_ADD_OBS(net_chan, bus_watch_lis, 0);

static bool print_observer(const struct zbus_observer *obs, void *user_data)
{
	static const char *const type_names[] = {
		[ZBUS_OBSERVER_LISTENER_TYPE] = "listener",
		[ZBUS_OBSERVER_SUBSCRIBER_TYPE] = "subscriber",
		[ZBUS_OBSERVER_MSG_SUBSCRIBER_TYPE] = "msg subscriber",
	};
	const struct shell *sh = user_data;

	shell_print(sh, "  observer %-20s %s", zbus_obs_name(obs),
		    obs->type < ARRAY_SIZE(type_names) ? type_names[obs->type] : "?");
	return true;
}

static bool print_channel(const struct zbus_channel *chan, void *user_data)
{
	const struct shell *sh = user_data;

	shell_print(sh, "  channel  %-20s %3u-byte msg, published %u times",
		    zbus_chan_name(chan), zbus_chan_msg_size(chan),
		    zbus_chan_pub_stats_count(chan));
	return true;
}

static int cmd_bus(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "zbus:");
	zbus_iterate_over_channels_with_user_data(print_channel, (void *)sh);
	zbus_iterate_over_observers_with_user_data(print_observer, (void *)sh);
	return 0;
}

static int cmd_bus_watch(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "on") == 0) {
		atomic_set(&watching, 1);
	} else if (strcmp(argv[1], "off") == 0) {
		atomic_set(&watching, 0);
	} else {
		shell_error(sh, "Usage: bus watch on|off");
		return -EINVAL;
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(bus_cmds,
	SHELL_CMD_ARG(watch, NULL, "Log low-rate channel messages: bus watch on|off",
		      cmd_bus_watch, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(bus, &bus_cmds, "List zbus channels and observers", cmd_bus);

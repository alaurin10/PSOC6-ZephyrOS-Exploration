/*
 * Consumers of temp_chan that demonstrate two kinds of zbus observer:
 *
 * - Listener  (temp_log_lis):   callback run synchronously inside
 *   zbus_chan_pub(), in the publisher's thread. Must be quick.
 * - Subscriber (temp_stats_sub): only gets a notification ("temp_chan
 *   changed") in a queue; its own thread wakes up and reads the channel.
 *
 * Also provides "temp stats" and "bus" shell commands.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include "channels.h"

LOG_MODULE_REGISTER(temp_monitor, LOG_LEVEL_INF);

#define LOG_DELTA_CENTI 50 /* log when the temperature moves by 0.5 C or more */
#define STATS_PRIORITY  10
#define STATS_STACK     1024

/* ---- Listener: log significant changes ---------------------------------- */

static void temp_log_cb(const struct zbus_channel *chan)
{
	static int32_t last_logged = INT32_MIN;
	/* Listeners may peek at the message in place: the channel is locked
	 * for the duration of the notification, so no copy is needed.
	 */
	const struct temp_msg *m = zbus_chan_const_msg(chan);

	if (last_logged == INT32_MIN || abs(m->centi_c - last_logged) >= LOG_DELTA_CENTI) {
		last_logged = m->centi_c;
		LOG_INF("Temperature " CENTI_FMT " C", CENTI_ARGS(m->centi_c));
	}
}

ZBUS_LISTENER_DEFINE(temp_log_lis, temp_log_cb);
/* Attach to the channel; the last argument orders observers (lower = notified first). */
ZBUS_CHAN_ADD_OBS(temp_chan, temp_log_lis, 1);

/* ---- Subscriber: running min / max / average ----------------------------- */

struct temp_stats {
	uint32_t count;
	int32_t min_centi;
	int32_t max_centi;
	int64_t sum_centi;
};

static K_MUTEX_DEFINE(stats_lock);
static struct temp_stats stats = {.min_centi = INT32_MAX, .max_centi = INT32_MIN};

/* Queue of 4 pending notifications; if the thread falls behind, publish blocks/fails. */
ZBUS_SUBSCRIBER_DEFINE(temp_stats_sub, 4);
ZBUS_CHAN_ADD_OBS(temp_chan, temp_stats_sub, 2);

static void stats_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const struct zbus_channel *chan;
	struct temp_msg m;

	/* zbus_sub_wait() blocks until some observed channel is published. */
	while (zbus_sub_wait(&temp_stats_sub, &chan, K_FOREVER) == 0) {
		if (chan != &temp_chan || zbus_chan_read(chan, &m, K_MSEC(100)) != 0) {
			continue;
		}

		k_mutex_lock(&stats_lock, K_FOREVER);
		stats.count++;
		stats.sum_centi += m.centi_c;
		stats.min_centi = MIN(stats.min_centi, m.centi_c);
		stats.max_centi = MAX(stats.max_centi, m.centi_c);
		k_mutex_unlock(&stats_lock);
	}
}

K_THREAD_DEFINE(stats_tid, STATS_STACK, stats_thread, NULL, NULL, NULL, STATS_PRIORITY, 0, 0);

/* ---- Shell commands ------------------------------------------------------ */

static int cmd_temp_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct temp_stats s;

	k_mutex_lock(&stats_lock, K_FOREVER);
	if (argc == 2 && strcmp(argv[1], "reset") == 0) {
		stats = (struct temp_stats){.min_centi = INT32_MAX, .max_centi = INT32_MIN};
	}
	s = stats;
	k_mutex_unlock(&stats_lock);

	if (s.count == 0) {
		shell_print(sh, "No samples yet");
		return 0;
	}

	int32_t avg = (int32_t)(s.sum_centi / s.count);

	shell_print(sh, "%u samples: min " CENTI_FMT " C, max " CENTI_FMT " C, avg " CENTI_FMT
		    " C", s.count, CENTI_ARGS(s.min_centi), CENTI_ARGS(s.max_centi),
		    CENTI_ARGS(avg));
	return 0;
}

SHELL_SUBCMD_ADD((temp), stats, NULL, "Min/max/average since boot: temp stats [reset]",
		 cmd_temp_stats, 1, 1);

static bool print_observer(const struct zbus_observer *obs, void *user_data)
{
	static const char *const type_names[] = {
		[ZBUS_OBSERVER_LISTENER_TYPE] = "listener",
		[ZBUS_OBSERVER_SUBSCRIBER_TYPE] = "subscriber",
		[ZBUS_OBSERVER_MSG_SUBSCRIBER_TYPE] = "msg subscriber",
	};
	const struct shell *sh = user_data;

	shell_print(sh, "  observer %-16s %s", zbus_obs_name(obs),
		    obs->type < ARRAY_SIZE(type_names) ? type_names[obs->type] : "?");
	return true;
}

static bool print_channel(const struct zbus_channel *chan, void *user_data)
{
	const struct shell *sh = user_data;

	shell_print(sh, "  channel  %-16s %u-byte msg, published %u times", zbus_chan_name(chan),
		    zbus_chan_msg_size(chan), zbus_chan_pub_stats_count(chan));
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

SHELL_CMD_REGISTER(bus, NULL, "List zbus channels and observers", cmd_bus);

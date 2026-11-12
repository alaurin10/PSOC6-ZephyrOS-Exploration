/*
 * Persistent application settings.
 *
 * Keys live under the "dash/" subtree (dash/view, dash/gain, ...), so they don't
 * mix with keys other apps (rtos_blinky uses "app/") left in the same partition.
 * Each value is an int32 with a default and an allowed range; values that are
 * missing or out of range in flash fall back to the default.
 * Backend: ZMS on the storage_partition (see boards/cy8cproto_062_4343w.overlay).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include "app_settings.h"
#include "channels.h"

LOG_MODULE_REGISTER(app_settings, LOG_LEVEL_INF);

#define SAVE_DELAY K_SECONDS(2)

/*
 * Saves run on their own low-priority work queue, not the (cooperative) system
 * work queue: a flash write can take a while, and ZMS keeps 512-byte row
 * buffers on the caller's stack. When a save triggers ZMS garbage collection
 * (moving entries out of a full sector) those buffers nest, which needs
 * more than 2 KB.
 */
#define SAVE_WQ_PRIORITY   13
#define SAVE_WQ_STACK_SIZE 4096

static K_THREAD_STACK_DEFINE(save_wq_stack, SAVE_WQ_STACK_SIZE);
static struct k_work_q save_wq;

struct setting_def {
	const char *name; /* key below "dash/" */
	int32_t def;
	int32_t min;
	int32_t max;
};

static const struct setting_def defs[APP_SET_COUNT] = {
	[APP_SET_VIEW] = {"view", UI_VIEW_BARS, 0, UI_VIEW_COUNT - 1},
	[APP_SET_COLOUR] = {"colour", 0, 0, UI_COLOUR_COUNT - 1},
	[APP_SET_LED_ON_CLAP] = {"ledclap", 1, 0, 1},
	[APP_SET_SLIDER_PARAM] = {"slider", UI_PARAM_GAIN, 0, UI_PARAM_COUNT - 1},
	[APP_SET_GAIN] = {"gain", 50, 0, UI_PARAM_MAX},
	[APP_SET_SMOOTHING] = {"smooth", 40, 0, UI_PARAM_MAX},
	[APP_SET_CLAP_SENS] = {"clapsens", 50, 0, UI_PARAM_MAX},
};

BUILD_ASSERT(APP_SET_GAIN + UI_PARAM_COUNT == APP_SET_COUNT,
	     "the UI params must be the last settings, in enum ui_param order");
BUILD_ASSERT(APP_SET_COUNT <= 32, "dirty mask is one atomic_t");

/* Read by several threads, written by setters and the loader: atomics, no lock needed. */
static atomic_t values[APP_SET_COUNT];
static atomic_t dirty;

static bool in_range(enum app_setting id, int32_t v)
{
	return v >= defs[id].min && v <= defs[id].max;
}

/*
 * Called by settings_load() once per stored key in the "dash" subtree.
 * `name` is the key with the "dash/" prefix already removed.
 */
static int app_settings_set_cb(const char *name, size_t len, settings_read_cb read_cb,
			       void *cb_arg)
{
	const char *next;
	int32_t value;

	for (size_t i = 0; i < APP_SET_COUNT; i++) {
		if (!settings_name_steq(name, defs[i].name, &next) || next) {
			continue;
		}
		if (len != sizeof(value) || read_cb(cb_arg, &value, sizeof(value)) < 0) {
			return -EINVAL;
		}
		if (in_range(i, value)) {
			atomic_set(&values[i], value);
		} else {
			LOG_WRN("Stored dash/%s=%d out of range, using default", defs[i].name, value);
		}
		return 0;
	}
	return -ENOENT;
}

/* Registers the "dash" subtree handler at build time (no runtime call needed). */
SETTINGS_STATIC_HANDLER_DEFINE(dash, "dash", NULL, app_settings_set_cb, NULL, NULL);

/* Runs on save_wq: the only place that writes these keys to flash. */
static void save_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	char key[24];
	int saved = 0;

	for (size_t i = 0; i < APP_SET_COUNT; i++) {
		if (!atomic_test_and_clear_bit(&dirty, i)) {
			continue;
		}

		int32_t v = atomic_get(&values[i]);
		int rc;

		snprintk(key, sizeof(key), "dash/%s", defs[i].name);
		rc = settings_save_one(key, &v, sizeof(v));
		if (rc != 0) {
			LOG_ERR("Saving %s failed (%d)", key, rc);
		} else {
			saved++;
		}
	}
	LOG_DBG("%d setting(s) saved", saved);
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_handler);

int app_settings_init(void)
{
	static const struct k_work_queue_config wq_cfg = {.name = "settings_wq"};

	k_work_queue_start(&save_wq, save_wq_stack, K_THREAD_STACK_SIZEOF(save_wq_stack),
			   SAVE_WQ_PRIORITY, &wq_cfg);

	for (size_t i = 0; i < APP_SET_COUNT; i++) {
		atomic_set(&values[i], defs[i].def);
	}

	int rc = settings_subsys_init();

	if (rc != 0) {
		LOG_ERR("Settings init failed (%d)", rc);
		return rc;
	}

	rc = settings_load_subtree("dash");
	if (rc != 0) {
		LOG_ERR("Settings load failed (%d)", rc);
	}
	return rc;
}

int32_t app_settings_get(enum app_setting id)
{
	return id < APP_SET_COUNT ? (int32_t)atomic_get(&values[id]) : 0;
}

int app_settings_set(enum app_setting id, int32_t value)
{
	if (id >= APP_SET_COUNT || !in_range(id, value)) {
		return -EINVAL;
	}
	if ((int32_t)atomic_set(&values[id], value) != value) {
		atomic_set_bit(&dirty, id);
		/* Reschedule (not schedule): every new change pushes the save out again. */
		k_work_reschedule_for_queue(&save_wq, &save_work, SAVE_DELAY);
	}
	return 0;
}

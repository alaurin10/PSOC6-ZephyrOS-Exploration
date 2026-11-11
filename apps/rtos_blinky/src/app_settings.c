/*
 * Persistent application settings.
 *
 * Keys live under the "app/" subtree: app/period, app/cold, app/hot.
 * Backend: ZMS on the storage_partition (see boards/cy8cproto_062_4343w.overlay).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include "app_settings.h"

LOG_MODULE_REGISTER(app_settings, LOG_LEVEL_INF);

#define DEFAULT_PERIOD_MS   500
#define DEFAULT_COLD_CENTI  1800
#define DEFAULT_HOT_CENTI   2800
#define TEMP_MIN_CENTI      (-2000)
#define TEMP_MAX_CENTI      8000
#define MIN_GAP_CENTI       200 /* keeps cold/hot apart by more than the hysteresis */
#define SAVE_DELAY          K_SECONDS(2)

/* Read by several threads, written by setters and the loader: atomics, no lock needed. */
static atomic_t period_ms = ATOMIC_INIT(DEFAULT_PERIOD_MS);
static atomic_t cold_centi = ATOMIC_INIT(DEFAULT_COLD_CENTI);
static atomic_t hot_centi = ATOMIC_INIT(DEFAULT_HOT_CENTI);

enum { DIRTY_PERIOD, DIRTY_THRESHOLDS };
static atomic_t dirty;

static bool period_valid(uint32_t v)
{
	return v >= APP_PERIOD_MIN_MS && v <= APP_PERIOD_MAX_MS;
}

static bool thresholds_valid(int32_t cold, int32_t hot)
{
	return cold >= TEMP_MIN_CENTI && hot <= TEMP_MAX_CENTI && hot - cold >= MIN_GAP_CENTI;
}

/*
 * Called by settings_load() once per stored key in the "app" subtree.
 * `name` is the key with the "app/" prefix already removed.
 */
static int app_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	int32_t value;
	int rc;

	if (len != sizeof(value)) {
		return -EINVAL;
	}
	rc = read_cb(cb_arg, &value, sizeof(value));
	if (rc < 0) {
		return rc;
	}

	if (settings_name_steq(name, "period", &next) && !next) {
		if (period_valid((uint32_t)value)) {
			atomic_set(&period_ms, value);
		}
	} else if (settings_name_steq(name, "cold", &next) && !next) {
		atomic_set(&cold_centi, value);
	} else if (settings_name_steq(name, "hot", &next) && !next) {
		atomic_set(&hot_centi, value);
	} else {
		return -ENOENT;
	}
	return 0;
}

/* Registers the "app" subtree handler at build time (no runtime call needed). */
SETTINGS_STATIC_HANDLER_DEFINE(app, "app", NULL, app_settings_set, NULL, NULL);

/* Runs on the system work queue: the only place that writes flash. */
static void save_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int rc = 0;

	if (atomic_test_and_clear_bit(&dirty, DIRTY_PERIOD)) {
		int32_t v = atomic_get(&period_ms);

		rc = settings_save_one("app/period", &v, sizeof(v));
	}
	if (rc == 0 && atomic_test_and_clear_bit(&dirty, DIRTY_THRESHOLDS)) {
		int32_t cold = atomic_get(&cold_centi);
		int32_t hot = atomic_get(&hot_centi);

		rc = settings_save_one("app/cold", &cold, sizeof(cold));
		if (rc == 0) {
			rc = settings_save_one("app/hot", &hot, sizeof(hot));
		}
	}

	if (rc != 0) {
		LOG_ERR("Saving settings failed (%d)", rc);
	} else {
		LOG_INF("Settings saved");
	}
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_handler);

static void schedule_save(int dirty_bit)
{
	atomic_set_bit(&dirty, dirty_bit);
	/* Reschedule (not schedule): every new change pushes the save out again. */
	k_work_reschedule(&save_work, SAVE_DELAY);
}

int app_settings_init(void)
{
	int rc = settings_subsys_init();

	if (rc != 0) {
		LOG_ERR("Settings init failed (%d)", rc);
		return rc;
	}

	rc = settings_load_subtree("app");
	if (rc != 0) {
		LOG_ERR("Settings load failed (%d)", rc);
		return rc;
	}

	if (!thresholds_valid(atomic_get(&cold_centi), atomic_get(&hot_centi))) {
		LOG_WRN("Stored thresholds invalid, using defaults");
		atomic_set(&cold_centi, DEFAULT_COLD_CENTI);
		atomic_set(&hot_centi, DEFAULT_HOT_CENTI);
	}

	LOG_INF("Loaded: period %u ms, cold %d.%02d C, hot %d.%02d C",
		(uint32_t)atomic_get(&period_ms), (int)atomic_get(&cold_centi) / 100,
		abs((int)atomic_get(&cold_centi)) % 100, (int)atomic_get(&hot_centi) / 100,
		abs((int)atomic_get(&hot_centi)) % 100);
	return 0;
}

uint32_t app_settings_period_ms(void)
{
	return (uint32_t)atomic_get(&period_ms);
}

int32_t app_settings_cold_centi(void)
{
	return (int32_t)atomic_get(&cold_centi);
}

int32_t app_settings_hot_centi(void)
{
	return (int32_t)atomic_get(&hot_centi);
}

int app_settings_set_period_ms(uint32_t v)
{
	if (!period_valid(v)) {
		return -EINVAL;
	}
	if ((uint32_t)atomic_set(&period_ms, v) != v) {
		schedule_save(DIRTY_PERIOD);
	}
	return 0;
}

int app_settings_set_thresholds_centi(int32_t cold, int32_t hot)
{
	if (!thresholds_valid(cold, hot)) {
		return -EINVAL;
	}
	atomic_set(&cold_centi, cold);
	atomic_set(&hot_centi, hot);
	schedule_save(DIRTY_THRESHOLDS);
	return 0;
}

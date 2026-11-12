/*
 * Wall-clock time: an offset from uptime, set once SNTP has answered.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <time.h>
#include <zephyr/kernel.h>

#include "app_time.h"

/* Unix ms at uptime 0; 0 = not set. 64-bit, so guard it with a spinlock. */
static int64_t offset_ms;
static struct k_spinlock lock;

void app_time_set_unix_ms(int64_t unix_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	offset_ms = unix_ms - k_uptime_get();
	k_spin_unlock(&lock, key);
}

bool app_time_valid(void)
{
	return app_time_unix_ms_at(0) >= 0;
}

int64_t app_time_unix_ms_at(int64_t uptime_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int64_t off = offset_ms;

	k_spin_unlock(&lock, key);
	return off == 0 ? -1 : off + uptime_ms;
}

int64_t app_time_unix_ms(void)
{
	return app_time_unix_ms_at(k_uptime_get());
}

int app_time_format(int64_t uptime_ms, char *buf, size_t len)
{
	int64_t unix_ms = app_time_unix_ms_at(uptime_ms);

	if (unix_ms < 0) {
		return snprintk(buf, len, "+%lld.%03llds", uptime_ms / 1000, uptime_ms % 1000);
	}

	time_t secs = (time_t)(unix_ms / 1000);
	struct tm tm;

	gmtime_r(&secs, &tm);
	return snprintk(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
			tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
			(int)(unix_ms % 1000));
}

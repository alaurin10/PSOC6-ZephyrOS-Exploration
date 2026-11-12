/*
 * Wall-clock time: an offset from uptime, set once SNTP has answered.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_TIME_H_
#define APP_TIME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Record the current Unix time (ms since 1970, UTC). */
void app_time_set_unix_ms(int64_t unix_ms);

/** True once the time has been set. */
bool app_time_valid(void);

/** Unix time (ms) of an uptime instant, or -1 if the time isn't known yet. */
int64_t app_time_unix_ms_at(int64_t uptime_ms);

/** Unix time (ms) now, or -1 if the time isn't known yet. */
int64_t app_time_unix_ms(void);

/**
 * Format an uptime instant as "2026-11-12T19:08:34.178Z", or as
 * "+1234.567s" (uptime) if the wall-clock time isn't known yet.
 */
int app_time_format(int64_t uptime_ms, char *buf, size_t len);

#endif /* APP_TIME_H_ */

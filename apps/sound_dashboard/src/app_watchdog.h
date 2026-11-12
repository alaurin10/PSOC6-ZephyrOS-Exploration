/*
 * Hardware watchdog with a liveness supervisor, plus reset-cause reporting.
 *
 * The key threads each check in with app_watchdog_alive(). A supervisor
 * thread feeds the hardware watchdog only when every enabled client has
 * checked in since the last feed, so one stuck thread resets the chip even
 * though the others are fine.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_WATCHDOG_H_
#define APP_WATCHDOG_H_

#define APP_WDT_TIMEOUT_MS 4000
/* How often the supervisor checks. Clients must check in at least this often. */
#define APP_WDT_CHECK_MS   1000

enum app_wdt_client {
	WDT_CLIENT_UI,
	WDT_CLIENT_CAPSENSE,
	WDT_CLIENT_AUDIO,
	WDT_CLIENT_WEB,
	WDT_CLIENT_LOG,
	WDT_CLIENT_COUNT,
};

/** Log why the chip last reset (and which client stalled, if any), then clear it. */
void app_watchdog_report_reset_cause(void);

/**
 * Start the supervisor, and the hardware watchdog unless a debugger has
 * enabled debug mode.
 */
void app_watchdog_start(void);

/** Include a client in the liveness check from now on (also counts as a check-in). */
void app_watchdog_enable(enum app_wdt_client client);

/** Stop checking a client, e.g. when its hardware failed to initialize. */
void app_watchdog_disable(enum app_wdt_client client);

/** Proof of life from a client thread. */
void app_watchdog_alive(enum app_wdt_client client);

#endif /* APP_WATCHDOG_H_ */

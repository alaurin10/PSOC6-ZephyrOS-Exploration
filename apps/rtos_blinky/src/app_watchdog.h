/*
 * Hardware watchdog and reset-cause reporting.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_WATCHDOG_H_
#define APP_WATCHDOG_H_

#define APP_WDT_TIMEOUT_MS 4000

/** Log why the chip last reset (e.g. watchdog), then clear the cause. */
void app_watchdog_report_reset_cause(void);

/** Start the watchdog unless a debugger has enabled debug mode. */
void app_watchdog_start(void);

/** Feed the watchdog. A no-op if it isn't running. */
void app_watchdog_feed(void);

#endif /* APP_WATCHDOG_H_ */

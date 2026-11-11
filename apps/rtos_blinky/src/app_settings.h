/*
 * Persistent application settings (blink period, temperature thresholds),
 * stored in flash through the Zephyr Settings subsystem.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SETTINGS_H_
#define APP_SETTINGS_H_

#include <stdint.h>

#define APP_PERIOD_MIN_MS 10
/* The blink thread feeds the watchdog once per loop, so its longest wait
 * (one blink period) must stay well below the watchdog timeout.
 */
#define APP_PERIOD_MAX_MS 2000

/** Initialize the Settings subsystem and load saved values (or defaults). */
int app_settings_init(void);

uint32_t app_settings_period_ms(void);
/** Thresholds in hundredths of a degree C (1800 = 18.00 C). */
int32_t app_settings_cold_centi(void);
int32_t app_settings_hot_centi(void);

/*
 * Setters update the value immediately and schedule a save a couple of
 * seconds later, so a burst of changes costs a single flash write.
 */
int app_settings_set_period_ms(uint32_t period_ms);
int app_settings_set_thresholds_centi(int32_t cold, int32_t hot);

#endif /* APP_SETTINGS_H_ */

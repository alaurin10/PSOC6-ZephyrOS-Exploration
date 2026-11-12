/*
 * Persistent application settings: a small table of integer values stored in
 * flash through the Zephyr Settings subsystem (ZMS backend, storage_partition).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SETTINGS_H_
#define APP_SETTINGS_H_

#include <stdint.h>

enum app_setting {
	APP_SET_VIEW,          /* enum ui_view */
	APP_SET_COLOUR,
	APP_SET_LED_ON_CLAP,
	APP_SET_SLIDER_PARAM,  /* enum ui_param */
	APP_SET_GAIN,          /* the UI_PARAM_* values, in enum ui_param order */
	APP_SET_SMOOTHING,
	APP_SET_CLAP_SENS,
	APP_SET_COUNT,
};

/** Initialize the Settings subsystem and load saved values (or defaults). */
int app_settings_init(void);

int32_t app_settings_get(enum app_setting id);

/*
 * Update a value immediately and save it a couple of seconds later, so a burst
 * of changes (e.g. dragging the slider) costs a single flash write.
 * Returns -EINVAL if the value is out of range for that setting.
 */
int app_settings_set(enum app_setting id, int32_t value);

#endif /* APP_SETTINGS_H_ */

/*
 * Dashboard UI state machine: the single owner of the state on ui_chan.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UI_CTRL_H_
#define UI_CTRL_H_

#include <stddef.h>

#include "channels.h"

/* Names used by the shell and the web API ("bars", "gain", ...). */
extern const char *const ui_view_names[UI_VIEW_COUNT];
extern const char *const ui_param_names[UI_PARAM_COUNT];

/** Index of `name` in `names`, or -1. */
int ui_name_index(const char *const *names, size_t count, const char *name);

/** Start the UI thread. Call after app_settings_init(), it restores the saved state. */
int ui_ctrl_start(void);

#endif /* UI_CTRL_H_ */

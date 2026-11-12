/*
 * Dashboard UI state machine (Zephyr State Machine Framework).
 *
 *   ROOT                          colour, parameters, slider target, LED-on-clap;
 *   |                             claps flash the user LED
 *   +-- LIVE                      view commands switch leaf; freeze -> FROZEN
 *   |   +-- BARS, LINE,           one leaf per view; entering one selects it
 *   |       WATERFALL, LEVEL
 *   +-- FROZEN                    display frozen; unfreeze -> back to the view,
 *                                 a view command unfreezes and switches
 *
 * Commands arrive on ctrl_chan from CapSense, web clients and the shell, all
 * through one zbus message subscriber, so they're applied one at a time in
 * order. Every change is validated here, saved (view, colour and parameters
 * persist across reboots) and published on ui_chan, which all renderers follow.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include "app_settings.h"
#include "app_watchdog.h"
#include "channels.h"
#include "ui_ctrl.h"

LOG_MODULE_REGISTER(ui_ctrl, LOG_LEVEL_INF);

#define UI_PRIORITY    7
#define UI_STACK_SIZE  1536
#define LED_FLASH_MS   120

const char *const ui_view_names[UI_VIEW_COUNT] = {"bars", "line", "waterfall", "level"};
const char *const ui_param_names[UI_PARAM_COUNT] = {"gain", "smooth", "clap"};

static const char *const source_names[] = {"boot", "capsense", "web", "shell"};

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Every message published on these channels is queued for the UI thread. */
ZBUS_MSG_SUBSCRIBER_DEFINE(ui_sub);
ZBUS_CHAN_ADD_OBS(ctrl_chan, ui_sub, 1);
ZBUS_CHAN_ADD_OBS(audio_event_chan, ui_sub, 1);

int ui_name_index(const char *const *names, size_t count, const char *name)
{
	for (size_t i = 0; i < count; i++) {
		if (strcmp(names[i], name) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* ---- LED flash on claps ------------------------------------------------------ */

static void led_off_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	gpio_pin_set_dt(&led, 0);
}

static K_WORK_DELAYABLE_DEFINE(led_off_work, led_off_handler);

static void led_flash(void)
{
	gpio_pin_set_dt(&led, 1);
	k_work_reschedule(&led_off_work, K_MSEC(LED_FLASH_MS));
}

/* ---- State machine object ---------------------------------------------------- */

enum ui_state { ROOT, LIVE, BARS, LINE, WATERFALL, LEVEL, FROZEN, NUM_STATES };

/* View leaves, in enum ui_view order. */
BUILD_ASSERT(LINE - BARS == UI_VIEW_LINE && LEVEL - BARS == UI_VIEW_LEVEL &&
	     LEVEL - BARS + 1 == UI_VIEW_COUNT);
#define VIEW_STATE(v) ((enum ui_state)(BARS + (v)))

struct ui_obj {
	struct smf_ctx ctx; /* must be first: SMF casts the object to smf_ctx */
	const struct zbus_channel *chan; /* channel of the message being processed */
	union {
		struct ctrl_msg cmd;
		struct audio_event_msg audio;
	} msg;
	struct ui_msg ui;   /* current state, published on ui_chan */
	bool dirty;         /* ui changed while handling this message */
};

static struct ui_obj obj;
static const struct smf_state states[NUM_STATES];

static void changed(struct ui_obj *o)
{
	o->dirty = true;
}

static void publish(struct ui_obj *o, uint8_t source)
{
	o->ui.seq++;
	o->ui.source = source;
	/* Short timeout: never hold up command handling for a slow observer. */
	if (zbus_chan_pub(&ui_chan, &o->ui, K_MSEC(50)) != 0) {
		LOG_WRN("ui_chan publish failed");
	}
}

static bool is_cmd(const struct ui_obj *o, enum ctrl_cmd cmd)
{
	return o->chan == &ctrl_chan && o->msg.cmd.cmd == cmd;
}

/* Value of a 0/1 command; anything else is rejected. */
static int bool_value(const struct ui_obj *o)
{
	int32_t v = o->msg.cmd.value;

	return v == 0 || v == 1 ? (int)v : -EINVAL;
}

/* Target view of a view command, or -1 if it isn't one (or is invalid). */
static int view_target(const struct ui_obj *o)
{
	if (is_cmd(o, CTRL_VIEW_NEXT)) {
		return (o->ui.view + 1) % UI_VIEW_COUNT;
	}
	if (is_cmd(o, CTRL_VIEW_SET)) {
		int32_t v = o->msg.cmd.value;

		return v >= 0 && v < UI_VIEW_COUNT ? (int)v : -1;
	}
	return -1;
}

static void reject(const struct ui_obj *o)
{
	LOG_WRN("Rejected command %u (param %u, value %d) from %s", o->msg.cmd.cmd,
		o->msg.cmd.param, o->msg.cmd.value, source_names[o->msg.cmd.source]);
}

/* ---- ROOT: the parameters that apply in every state --------------------------- */

static enum smf_state_result root_run(void *o_)
{
	struct ui_obj *o = o_;

	if (o->chan == &audio_event_chan) {
		if (o->msg.audio.type != AUDIO_EVT_CLIP && o->ui.led_on_clap) {
			led_flash();
		}
		return SMF_EVENT_HANDLED;
	}

	const struct ctrl_msg *c = &o->msg.cmd;
	int32_t v = c->value;

	switch (c->cmd) {
	case CTRL_COLOUR_NEXT:
		v = (o->ui.colour + 1) % UI_COLOUR_COUNT;
		__fallthrough;
	case CTRL_COLOUR_SET:
		if (app_settings_set(APP_SET_COLOUR, v) != 0) {
			reject(o);
		} else if (o->ui.colour != v) {
			o->ui.colour = v;
			changed(o);
		}
		break;
	case CTRL_PARAM_SET:
		if (c->param >= UI_PARAM_COUNT ||
		    app_settings_set(APP_SET_GAIN + c->param, v) != 0) {
			reject(o);
		} else if (o->ui.param[c->param] != v) {
			o->ui.param[c->param] = v;
			changed(o);
		}
		break;
	case CTRL_SLIDER_PARAM_SET:
		if (app_settings_set(APP_SET_SLIDER_PARAM, v) != 0) {
			reject(o);
		} else if (o->ui.slider_param != v) {
			o->ui.slider_param = v;
			changed(o);
		}
		break;
	case CTRL_LED_CLAP_SET:
		if (bool_value(o) < 0 || app_settings_set(APP_SET_LED_ON_CLAP, v) != 0) {
			reject(o);
		} else if (o->ui.led_on_clap != v) {
			o->ui.led_on_clap = v;
			changed(o);
		}
		break;
	case CTRL_VIEW_SET:
		/* Only reaches ROOT if the child states found it invalid. */
		reject(o);
		break;
	case CTRL_FREEZE_SET:
		/* Reaching ROOT means we're already in the requested state. */
		if (bool_value(o) < 0) {
			reject(o);
		}
		break;
	default:
		/* CTRL_VIEW_NEXT and CTRL_FREEZE_TOGGLE are always handled by a child. */
		break;
	}
	return SMF_EVENT_HANDLED;
}

/* ---- LIVE and its view leaves --------------------------------------------------- */

static enum smf_state_result live_run(void *o_)
{
	struct ui_obj *o = o_;
	int target = view_target(o);

	if (target >= 0) {
		if (target != o->ui.view) {
			smf_set_state(SMF_CTX(o), &states[VIEW_STATE(target)]);
		}
		return SMF_EVENT_HANDLED;
	}
	if (is_cmd(o, CTRL_FREEZE_TOGGLE) || (is_cmd(o, CTRL_FREEZE_SET) && bool_value(o) == 1)) {
		smf_set_state(SMF_CTX(o), &states[FROZEN]);
		return SMF_EVENT_HANDLED;
	}
	return SMF_EVENT_PROPAGATE;
}

static void view_entry(void *o_)
{
	struct ui_obj *o = o_;
	/* SMF passes the object, not the state: find which leaf we're entering. */
	const struct smf_state *s = SMF_CTX(o)->current;
	uint8_t view = (uint8_t)(s - &states[BARS]);

	if (o->ui.view != view) {
		o->ui.view = view;
		(void)app_settings_set(APP_SET_VIEW, view);
		changed(o);
	}
	LOG_INF("View: %s", ui_view_names[view]);
}

/* ---- FROZEN ------------------------------------------------------------------- */

static void frozen_entry(void *o_)
{
	struct ui_obj *o = o_;

	o->ui.freeze = 1;
	changed(o);
	LOG_INF("Display frozen");
}

static void frozen_exit(void *o_)
{
	struct ui_obj *o = o_;

	o->ui.freeze = 0;
	changed(o);
	LOG_INF("Display live");
}

static enum smf_state_result frozen_run(void *o_)
{
	struct ui_obj *o = o_;
	int target = view_target(o);

	if (target >= 0) {
		/* Choosing a view means "show me that": unfreeze into it. */
		smf_set_state(SMF_CTX(o), &states[VIEW_STATE(target)]);
		return SMF_EVENT_HANDLED;
	}
	if (is_cmd(o, CTRL_FREEZE_TOGGLE) || (is_cmd(o, CTRL_FREEZE_SET) && bool_value(o) == 0)) {
		smf_set_state(SMF_CTX(o), &states[VIEW_STATE(o->ui.view)]);
		return SMF_EVENT_HANDLED;
	}
	return SMF_EVENT_PROPAGATE;
}

/* ---- State table ------------------------------------------------------------- */

/* SMF_CREATE_STATE(entry, run, exit, parent, initial child) */
static const struct smf_state states[NUM_STATES] = {
	[ROOT] = SMF_CREATE_STATE(NULL, root_run, NULL, NULL, NULL),
	[LIVE] = SMF_CREATE_STATE(NULL, live_run, NULL, &states[ROOT], &states[BARS]),
	[BARS] = SMF_CREATE_STATE(view_entry, NULL, NULL, &states[LIVE], NULL),
	[LINE] = SMF_CREATE_STATE(view_entry, NULL, NULL, &states[LIVE], NULL),
	[WATERFALL] = SMF_CREATE_STATE(view_entry, NULL, NULL, &states[LIVE], NULL),
	[LEVEL] = SMF_CREATE_STATE(view_entry, NULL, NULL, &states[LIVE], NULL),
	[FROZEN] = SMF_CREATE_STATE(frozen_entry, frozen_run, frozen_exit, &states[ROOT], NULL),
};

/* ---- Thread ------------------------------------------------------------------- */

static void ui_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct ui_obj *o = &obj;

	/* Restore the saved state, then enter its view (runs ROOT, LIVE, leaf entries). */
	o->ui.view = (uint8_t)app_settings_get(APP_SET_VIEW);
	o->ui.colour = (uint8_t)app_settings_get(APP_SET_COLOUR);
	o->ui.led_on_clap = (uint8_t)app_settings_get(APP_SET_LED_ON_CLAP);
	o->ui.slider_param = (uint8_t)app_settings_get(APP_SET_SLIDER_PARAM);
	for (int p = 0; p < UI_PARAM_COUNT; p++) {
		o->ui.param[p] = (uint8_t)app_settings_get(APP_SET_GAIN + p);
	}
	smf_set_initial(SMF_CTX(o), &states[VIEW_STATE(o->ui.view)]);
	publish(o, CTRL_SRC_BOOT);

	app_watchdog_enable(WDT_CLIENT_UI);

	while (true) {
		app_watchdog_alive(WDT_CLIENT_UI);

		/* Wake up at least twice per supervisor period to check in. */
		if (zbus_sub_wait_msg(&ui_sub, &o->chan, &o->msg,
				      K_MSEC(APP_WDT_CHECK_MS / 2)) != 0) {
			continue;
		}

		o->dirty = false;
		smf_run_state(SMF_CTX(o));
		if (o->dirty) {
			publish(o, o->chan == &ctrl_chan ? o->msg.cmd.source : CTRL_SRC_BOOT);
		}
	}
}

K_THREAD_DEFINE(ui_tid, UI_STACK_SIZE, ui_thread, NULL, NULL, NULL, UI_PRIORITY, 0,
		SYS_FOREVER_MS);

int ui_ctrl_start(void)
{
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	if (ret == 0) {
		k_thread_start(ui_tid);
	}
	return ret;
}

/* ---- Shell: "ui" ------------------------------------------------------------------ */

static int send_shell_cmd(const struct shell *sh, uint8_t cmd, uint8_t param, int32_t value)
{
	struct ctrl_msg m = {.source = CTRL_SRC_SHELL, .cmd = cmd, .param = param, .value = value};
	int err = zbus_chan_pub(&ctrl_chan, &m, K_MSEC(100));

	if (err != 0) {
		shell_error(sh, "Command not accepted (%d)", err);
	}
	return err;
}

static int cmd_ui(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct ui_msg ui;

	if (zbus_chan_read(&ui_chan, &ui, K_MSEC(100)) != 0) {
		return -EBUSY;
	}

	shell_print(sh, "View %s%s, colour %u, LED on clap %s (state #%u, last change by %s)",
		    ui_view_names[ui.view], ui.freeze ? " (frozen)" : "", ui.colour,
		    ui.led_on_clap ? "on" : "off", ui.seq,
		    ui.source < ARRAY_SIZE(source_names) ? source_names[ui.source] : "?");
	for (int p = 0; p < UI_PARAM_COUNT; p++) {
		shell_print(sh, "  %-7s %3u%s", ui_param_names[p], ui.param[p],
			    p == ui.slider_param ? "  <- slider" : "");
	}
	return 0;
}

static int cmd_ui_view(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "next") == 0) {
		return send_shell_cmd(sh, CTRL_VIEW_NEXT, 0, 0);
	}

	int v = ui_name_index(ui_view_names, UI_VIEW_COUNT, argv[1]);

	if (v < 0) {
		shell_error(sh, "Views: next bars line waterfall level");
		return -EINVAL;
	}
	return send_shell_cmd(sh, CTRL_VIEW_SET, 0, v);
}

static int cmd_ui_colour(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "next") == 0) {
		return send_shell_cmd(sh, CTRL_COLOUR_NEXT, 0, 0);
	}

	int err = 0;
	long v = shell_strtol(argv[1], 10, &err);

	if (err != 0) {
		shell_error(sh, "Usage: ui colour next|<0..%d>", UI_COLOUR_COUNT - 1);
		return -EINVAL;
	}
	return send_shell_cmd(sh, CTRL_COLOUR_SET, 0, v);
}

static int cmd_ui_param(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	int p = ui_name_index(ui_param_names, UI_PARAM_COUNT, argv[1]);
	int err = 0;
	long v = shell_strtol(argv[2], 10, &err);

	if (p < 0 || err != 0) {
		shell_error(sh, "Usage: ui param gain|smooth|clap <0..100>");
		return -EINVAL;
	}
	return send_shell_cmd(sh, CTRL_PARAM_SET, p, v);
}

static int cmd_ui_slider(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	int p = ui_name_index(ui_param_names, UI_PARAM_COUNT, argv[1]);

	if (p < 0) {
		shell_error(sh, "Usage: ui slider gain|smooth|clap");
		return -EINVAL;
	}
	return send_shell_cmd(sh, CTRL_SLIDER_PARAM_SET, 0, p);
}

static int on_off(const struct shell *sh, const char *arg)
{
	if (strcmp(arg, "on") == 0) {
		return 1;
	}
	if (strcmp(arg, "off") == 0) {
		return 0;
	}
	shell_error(sh, "Expected on|off");
	return -EINVAL;
}

static int cmd_ui_freeze(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		return send_shell_cmd(sh, CTRL_FREEZE_TOGGLE, 0, 0);
	}

	int v = on_off(sh, argv[1]);

	return v < 0 ? v : send_shell_cmd(sh, CTRL_FREEZE_SET, 0, v);
}

static int cmd_ui_ledclap(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	int v = on_off(sh, argv[1]);

	return v < 0 ? v : send_shell_cmd(sh, CTRL_LED_CLAP_SET, 0, v);
}

SHELL_STATIC_SUBCMD_SET_CREATE(ui_cmds,
	SHELL_CMD_ARG(view, NULL, "ui view next|bars|line|waterfall|level", cmd_ui_view, 2, 0),
	SHELL_CMD_ARG(colour, NULL, "ui colour next|<n>", cmd_ui_colour, 2, 0),
	SHELL_CMD_ARG(param, NULL, "ui param gain|smooth|clap <0..100>", cmd_ui_param, 3, 0),
	SHELL_CMD_ARG(slider, NULL, "ui slider gain|smooth|clap (what the slider sets)",
		      cmd_ui_slider, 2, 0),
	SHELL_CMD_ARG(freeze, NULL, "ui freeze [on|off] (no argument toggles)", cmd_ui_freeze,
		      1, 1),
	SHELL_CMD_ARG(ledclap, NULL, "ui ledclap on|off (flash the LED on claps)",
		      cmd_ui_ledclap, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ui, &ui_cmds, "Show/change the dashboard state", cmd_ui);

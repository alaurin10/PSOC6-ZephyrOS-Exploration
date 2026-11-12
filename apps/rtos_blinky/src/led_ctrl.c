/*
 * LED controller built on the Zephyr State Machine Framework (SMF).
 *
 *   ROOT                      new user period -> save, apply if in MANUAL
 *   +-- TEMP                  temperature -> pick child; mode -> MANUAL
 *   |   +-- COLD              LED solid on
 *   |   +-- NORMAL (initial)  slow 1 s blink
 *   |   +-- HOT               fast 100 ms blink
 *   +-- MANUAL                blink at the user period; mode -> TEMP
 *
 * Events arrive on one message queue (button, shell, zbus listener). Each
 * event runs the state machine once: the current state's run action gets it
 * first and returns SMF_EVENT_PROPAGATE to pass it up to its parent.
 *
 * Blinking itself is mechanism, not behavior: the thread toggles the LED
 * when its deadline passes while waiting for events.
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
#include "led_ctrl.h"

LOG_MODULE_REGISTER(led_ctrl, LOG_LEVEL_INF);

#define LED_PRIORITY      7
#define LED_STACK_SIZE    1024
#define NORMAL_BLINK_MS   1000
#define HOT_BLINK_MS      100
#define HYSTERESIS_CENTI  20   /* 0.2 C */
#define IDLE_WAKE_MS      1000 /* max wait when not blinking, keeps the watchdog fed */

BUILD_ASSERT(APP_PERIOD_MAX_MS * 2 <= APP_WDT_TIMEOUT_MS,
	     "blink period too close to watchdog timeout");

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* ---- Events -------------------------------------------------------------- */

enum led_evt_type { EVT_PERIOD, EVT_MODE, EVT_TEMP };

struct led_evt {
	uint8_t type;
	int32_t value; /* ms, enum led_mode, or centi-degrees */
};

K_MSGQ_DEFINE(led_evq, sizeof(struct led_evt), 8, 4);

static int post(uint8_t type, int32_t value)
{
	struct led_evt e = {.type = type, .value = value};

	return k_msgq_put(&led_evq, &e, K_NO_WAIT);
}

int led_ctrl_request_period(uint32_t period_ms)
{
	return post(EVT_PERIOD, (int32_t)period_ms);
}

int led_ctrl_request_mode(enum led_mode mode)
{
	return post(EVT_MODE, mode);
}

/* zbus listener: runs in the publisher's thread, so just forward the value. */
static void temp_to_led_cb(const struct zbus_channel *chan)
{
	const struct temp_msg *m = zbus_chan_const_msg(chan);

	(void)post(EVT_TEMP, m->centi_c); /* if the queue is full, the next sample will do */
}

ZBUS_LISTENER_DEFINE(temp_led_lis, temp_to_led_cb);
ZBUS_CHAN_ADD_OBS(temp_chan, temp_led_lis, 3);

/* ---- State machine object ------------------------------------------------ */

enum led_state { ROOT, TEMP, COLD, NORMAL, HOT, MANUAL, NUM_STATES };

static const char *const state_names[NUM_STATES] = {
	"ROOT", "TEMP", "COLD", "NORMAL", "HOT", "MANUAL",
};

struct led_obj {
	struct smf_ctx ctx; /* must be first: SMF casts the object to smf_ctx */
	struct led_evt evt; /* event currently being processed */
	uint32_t user_period_ms;
	uint32_t blink_ms;  /* 0 = not blinking (solid) */
	bool follows_user;  /* current state blinks at the user period */
	int64_t next_toggle_ms;
	int32_t temp_centi;
	bool have_temp;
};

static struct led_obj obj;
static const struct smf_state states[NUM_STATES];

/* Read by the shell thread. */
static atomic_t cur_state = ATOMIC_INIT(NORMAL);
static atomic_t cur_blink_ms;

static void enter_leaf(struct led_obj *o, enum led_state s, uint32_t blink_ms, bool follows)
{
	atomic_set(&cur_state, s);
	o->follows_user = follows;
	o->blink_ms = blink_ms;
	atomic_set(&cur_blink_ms, blink_ms);
	if (blink_ms == 0) {
		gpio_pin_set_dt(&led, 1);
	} else {
		o->next_toggle_ms = k_uptime_get() + blink_ms;
	}
	LOG_INF("LED state -> %s (%s)", state_names[s], blink_ms ? "blinking" : "solid on");
}

/* Pick COLD/NORMAL/HOT. Leaving COLD or HOT requires moving 0.5 C back past
 * the threshold, so the LED doesn't flap when the temperature sits on it.
 */
static enum led_state classify(enum led_state current, int32_t t)
{
	int32_t cold = app_settings_cold_centi();
	int32_t hot = app_settings_hot_centi();

	if (current == COLD && t < cold + HYSTERESIS_CENTI) {
		return COLD;
	}
	if (current == HOT && t >= hot - HYSTERESIS_CENTI) {
		return HOT;
	}
	if (t < cold) {
		return COLD;
	}
	if (t >= hot) {
		return HOT;
	}
	return NORMAL;
}

/* ---- ROOT ---------------------------------------------------------------- */

static enum smf_state_result root_run(void *o_)
{
	struct led_obj *o = o_;

	if (o->evt.type == EVT_PERIOD) {
		o->user_period_ms = o->evt.value;
		app_settings_set_period_ms(o->user_period_ms); /* saved to flash later */
		LOG_INF("User period now %u ms", o->user_period_ms);
		if (o->follows_user) {
			o->blink_ms = o->user_period_ms;
			atomic_set(&cur_blink_ms, o->blink_ms);
		}
	}
	/* Anything else reaching ROOT (e.g. "mode TEMP" while in TEMP) is ignored. */
	return SMF_EVENT_HANDLED;
}

/* ---- TEMP (parent) and its children -------------------------------------- */

static void temp_entry(void *o_)
{
	ARG_UNUSED(o_);
	LOG_INF("Mode: temperature");
}

static enum smf_state_result temp_run(void *o_)
{
	struct led_obj *o = o_;

	switch (o->evt.type) {
	case EVT_TEMP: {
		enum led_state now = (enum led_state)atomic_get(&cur_state);
		enum led_state next = classify(now, o->temp_centi);

		if (next != now) {
			smf_set_state(SMF_CTX(o), &states[next]);
		}
		return SMF_EVENT_HANDLED;
	}
	case EVT_MODE:
		if (o->evt.value == LED_MODE_MANUAL) {
			smf_set_state(SMF_CTX(o), &states[MANUAL]);
			return SMF_EVENT_HANDLED;
		}
		break;
	default:
		break;
	}
	return SMF_EVENT_PROPAGATE; /* let ROOT handle it */
}

static void cold_entry(void *o_)
{
	enter_leaf(o_, COLD, 0, false);
}

static void normal_entry(void *o_)
{
	/* Temperature mode has its own rates; the user period is for MANUAL only. */
	enter_leaf(o_, NORMAL, NORMAL_BLINK_MS, false);
}

static void hot_entry(void *o_)
{
	enter_leaf(o_, HOT, HOT_BLINK_MS, false);
}

/* ---- MANUAL -------------------------------------------------------------- */

static void manual_entry(void *o_)
{
	struct led_obj *o = o_;

	LOG_INF("Mode: manual");
	enter_leaf(o, MANUAL, o->user_period_ms, true);
}

static enum smf_state_result manual_run(void *o_)
{
	struct led_obj *o = o_;

	if (o->evt.type == EVT_MODE && o->evt.value == LED_MODE_TEMP) {
		/* Jump straight to the right TEMP child for the current temperature. */
		enum led_state next = o->have_temp ? classify(NORMAL, o->temp_centi) : NORMAL;

		smf_set_state(SMF_CTX(o), &states[next]);
		return SMF_EVENT_HANDLED;
	}
	return SMF_EVENT_PROPAGATE; /* temperature events end up ignored in ROOT */
}

/* ---- State table ---------------------------------------------------------- */

/* SMF_CREATE_STATE(entry, run, exit, parent, initial child) */
static const struct smf_state states[NUM_STATES] = {
	[ROOT] = SMF_CREATE_STATE(NULL, root_run, NULL, NULL, NULL),
	[TEMP] = SMF_CREATE_STATE(temp_entry, temp_run, NULL, &states[ROOT], &states[NORMAL]),
	[COLD] = SMF_CREATE_STATE(cold_entry, NULL, NULL, &states[TEMP], NULL),
	[NORMAL] = SMF_CREATE_STATE(normal_entry, NULL, NULL, &states[TEMP], NULL),
	[HOT] = SMF_CREATE_STATE(hot_entry, NULL, NULL, &states[TEMP], NULL),
	[MANUAL] = SMF_CREATE_STATE(manual_entry, manual_run, NULL, &states[ROOT], NULL),
};

/* ---- Thread --------------------------------------------------------------- */

static void led_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct led_obj *o = &obj;

	o->user_period_ms = app_settings_period_ms();
	/* Runs the entry actions of ROOT, TEMP and NORMAL. */
	smf_set_initial(SMF_CTX(o), &states[NORMAL]);

	while (true) {
		/* Proof of life: if this loop stops running, the chip resets. */
		app_watchdog_feed();

		/* Wait for an event, but no later than the next toggle (an absolute
		 * deadline, so frequent events don't keep pushing the toggle back).
		 */
		k_timeout_t timeout = o->blink_ms ? K_TIMEOUT_ABS_MS(o->next_toggle_ms)
						  : K_MSEC(IDLE_WAKE_MS);

		if (k_msgq_get(&led_evq, &o->evt, timeout) == 0) {
			if (o->evt.type == EVT_TEMP) {
				o->temp_centi = o->evt.value;
				o->have_temp = true;
			}
			smf_run_state(SMF_CTX(o));
		} else if (o->blink_ms) {
			gpio_pin_toggle_dt(&led);
			o->next_toggle_ms += o->blink_ms;
			if (o->next_toggle_ms < k_uptime_get()) {
				o->next_toggle_ms = k_uptime_get() + o->blink_ms;
			}
		}
	}
}

K_THREAD_DEFINE(led_tid, LED_STACK_SIZE, led_thread, NULL, NULL, NULL, LED_PRIORITY, 0,
		SYS_FOREVER_MS);

int led_ctrl_start(void)
{
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	if (ret == 0) {
		k_thread_start(led_tid);
	}
	return ret;
}

/* ---- Shell: "led", "led mode temp|manual" -------------------------------- */

static int cmd_led(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	enum led_state s = (enum led_state)atomic_get(&cur_state);
	uint32_t blink = (uint32_t)atomic_get(&cur_blink_ms);

	shell_print(sh, "Mode %s, state %s", s == MANUAL ? "manual" : "temperature",
		    state_names[s]);
	if (blink) {
		shell_print(sh, "Blinking every %u ms (manual-mode period %u ms)", blink,
			    app_settings_period_ms());
	} else {
		shell_print(sh, "Solid on (manual-mode period %u ms)", app_settings_period_ms());
	}
	return 0;
}

static int cmd_led_mode(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	enum led_mode mode;

	if (strcmp(argv[1], "temp") == 0) {
		mode = LED_MODE_TEMP;
	} else if (strcmp(argv[1], "manual") == 0) {
		mode = LED_MODE_MANUAL;
	} else {
		shell_error(sh, "Usage: led mode temp|manual");
		return -EINVAL;
	}

	if (led_ctrl_request_mode(mode) != 0) {
		shell_error(sh, "LED event queue full, try again");
		return -EBUSY;
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(led_cmds,
	SHELL_CMD_ARG(mode, NULL, "Select mode: led mode temp|manual", cmd_led_mode, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(led, &led_cmds, "Show LED mode/state", cmd_led);

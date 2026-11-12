/*
 * RTOS blinky for the CY8CPROTO-062-4343W.
 *
 * Modules:
 * - led_ctrl.c:     LED state machine (SMF): temperature modes / manual mode.
 * - thermistor.c:   samples RT1 once per second (hardware counter) and
 *                   publishes on the zbus temp_chan.
 * - temp_monitor.c: zbus observers for logging and min/max/avg statistics.
 * - app_settings.c: blink period and thresholds persisted in flash.
 * - app_watchdog.c: hardware watchdog, fed by the LED thread.
 * - main.c (here):  startup, SW0 button thread and the "blink" shell command.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "app_settings.h"
#include "app_watchdog.h"
#include "led_ctrl.h"
#include "thermistor.h"

LOG_MODULE_REGISTER(rtos_blinky, LOG_LEVEL_INF);

/* Pins come from the board devicetree aliases, not hard-coded port/pin numbers. */
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

#define BUTTON_PRIORITY 5 /* lower number = higher priority */
#define STACK_SIZE      1024
#define DEBOUNCE_MS     50

static const uint32_t preset_periods_ms[] = {500, 250, 100, 1000};

/* Binary semaphore (max count 1) used to signal "button pressed" from ISR to thread. */
static K_SEM_DEFINE(button_sem, 0, 1);
static struct gpio_callback button_cb_data;

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Keep ISRs short: just wake the thread that does the real work. */
	k_sem_give(&button_sem);
}

static void button_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	size_t idx = 0;

	while (true) {
		k_sem_take(&button_sem, K_FOREVER);

		/* Mechanical buttons bounce: wait, then confirm it is still pressed. */
		k_msleep(DEBOUNCE_MS);
		if (gpio_pin_get_dt(&button) == 1) {
			idx = (idx + 1) % ARRAY_SIZE(preset_periods_ms);
			LOG_INF("Button pressed -> requesting %u ms", preset_periods_ms[idx]);
			if (led_ctrl_request_period(preset_periods_ms[idx]) != 0) {
				LOG_WRN("LED event queue full, press ignored");
			}
		}

		/* Discard extra gives caused by bounce edges during the debounce window. */
		k_sem_reset(&button_sem);
	}
}

/* Created but not started (SYS_FOREVER_MS); main() starts it once the GPIO is set up. */
K_THREAD_DEFINE(button_tid, STACK_SIZE, button_thread, NULL, NULL, NULL,
		BUTTON_PRIORITY, 0, SYS_FOREVER_MS);

/* Shell handler: runs in the shell's own thread when "blink set <ms>" is entered. */
static int cmd_blink_set(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); /* the shell already enforced exactly 1 argument */

	int err = 0;
	unsigned long period_ms = shell_strtoul(argv[1], 10, &err);

	if (err != 0 || period_ms < APP_PERIOD_MIN_MS || period_ms > APP_PERIOD_MAX_MS) {
		shell_error(sh, "Period must be a number from %d to %d (ms)",
			    APP_PERIOD_MIN_MS, APP_PERIOD_MAX_MS);
		return -EINVAL;
	}

	if (led_ctrl_request_period((uint32_t)period_ms) != 0) {
		shell_error(sh, "LED event queue full, try again");
		return -EBUSY;
	}

	return 0;
}

/*
 * Register "blink" as a root command with one subcommand, "set".
 * SHELL_CMD_ARG(..., 2, 0): 2 mandatory args (the "set" token + <ms>), 0 optional.
 */
SHELL_STATIC_SUBCMD_SET_CREATE(blink_cmds,
	SHELL_CMD_ARG(set, NULL, "Set the manual-mode blink period: blink set <ms>", cmd_blink_set, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(blink, &blink_cmds, "Control the LED blink rate", NULL);

static int button_init(void)
{
	if (!gpio_is_ready_dt(&button)) {
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&button, GPIO_INPUT);

	if (ret == 0) {
		ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	}
	if (ret == 0) {
		gpio_init_callback(&button_cb_data, button_isr, BIT(button.pin));
		ret = gpio_add_callback(button.port, &button_cb_data);
	}
	if (ret == 0) {
		k_thread_start(button_tid);
	}
	return ret;
}

int main(void)
{
	int ret;

	LOG_INF("RTOS blinky on %s", CONFIG_BOARD);
	app_watchdog_report_reset_cause();

	/* Load saved settings before starting the threads that use them. */
	ret = app_settings_init();
	if (ret < 0) {
		LOG_ERR("Settings unavailable (%d), using defaults", ret);
	}

	/* Start the watchdog just before the thread that feeds it. */
	app_watchdog_start();
	ret = led_ctrl_start();
	if (ret < 0) {
		LOG_ERR("LED controller failed (%d)", ret);
	}

	ret = button_init();
	if (ret < 0) {
		LOG_ERR("Button setup failed (%d)", ret);
	}

	ret = thermistor_init();
	if (ret < 0) {
		LOG_ERR("Thermistor init failed (%d)", ret);
	}

	LOG_INF("Started. Try 'led', 'led mode manual', 'blink set <ms>', 'temp'.");

	/* Returning from main() just ends the main thread; the others keep running. */
	return 0;
}

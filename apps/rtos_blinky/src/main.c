/*
 * RTOS blinky for the CY8CPROTO-062-4343W.
 *
 * - blink thread:  owns LED0 and the blink period. Waits on a message queue
 *                  with the period as timeout: timeout -> toggle the LED,
 *                  message -> adopt the new period.
 * - button thread: woken by the SW0 interrupt via a semaphore; sends the
 *                  next preset period to the message queue.
 * - shell:         "blink set <ms>" sends any period to the same queue.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(rtos_blinky, LOG_LEVEL_INF);

/* Pins come from the board devicetree aliases, not hard-coded port/pin numbers. */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

#define BLINK_PRIORITY  7
#define BUTTON_PRIORITY 5 /* lower number = higher priority */
#define STACK_SIZE      1024
#define DEBOUNCE_MS     50
#define PERIOD_MIN_MS   10
#define PERIOD_MAX_MS   10000

static const uint32_t preset_periods_ms[] = {500, 250, 100, 1000};

/*
 * Queue of requested blink periods: each message is one uint32_t (ms),
 * up to 4 can be pending, buffer aligned to 4 bytes. The queue copies the
 * data in and out, so producers and the consumer never share memory.
 */
K_MSGQ_DEFINE(period_msgq, sizeof(uint32_t), 4, 4);

/* Binary semaphore (max count 1) used to signal "button pressed" from ISR to thread. */
static K_SEM_DEFINE(button_sem, 0, 1);
static struct gpio_callback button_cb_data;

/* Producer side, used by both the button thread and the shell thread. */
static int request_period(uint32_t period_ms)
{
	/* K_NO_WAIT: never block the caller; returns -ENOMSG if the queue is full. */
	return k_msgq_put(&period_msgq, &period_ms, K_NO_WAIT);
}

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Keep ISRs short: just wake the thread that does the real work. */
	k_sem_give(&button_sem);
}

static void blink_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t period_ms = preset_periods_ms[0];
	uint32_t new_period_ms;

	while (true) {
		/*
		 * Block until a message arrives OR one blink period passes. This
		 * replaces k_msleep(): the thread still sleeps, but a new period
		 * takes effect immediately instead of after the current wait.
		 */
		if (k_msgq_get(&period_msgq, &new_period_ms, K_MSEC(period_ms)) == 0) {
			period_ms = new_period_ms;
			LOG_INF("Blink period now %u ms", period_ms);
		} else {
			/* -EAGAIN: timed out with no message, time to toggle. */
			gpio_pin_toggle_dt(&led);
		}
	}
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
			if (request_period(preset_periods_ms[idx]) != 0) {
				LOG_WRN("Period queue full, press ignored");
			}
		}

		/* Discard extra gives caused by bounce edges during the debounce window. */
		k_sem_reset(&button_sem);
	}
}

/* Shell handler: runs in the shell's own thread when "blink set <ms>" is entered. */
static int cmd_blink_set(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); /* the shell already enforced exactly 1 argument */

	int err = 0;
	unsigned long period_ms = shell_strtoul(argv[1], 10, &err);

	if (err != 0 || period_ms < PERIOD_MIN_MS || period_ms > PERIOD_MAX_MS) {
		shell_error(sh, "Period must be a number from %d to %d (ms)",
			    PERIOD_MIN_MS, PERIOD_MAX_MS);
		return -EINVAL;
	}

	if (request_period((uint32_t)period_ms) != 0) {
		shell_error(sh, "Period queue full, try again");
		return -EBUSY;
	}

	return 0;
}

/*
 * Register "blink" as a root command with one subcommand, "set".
 * SHELL_CMD_ARG(..., 2, 0): 2 mandatory args (the "set" token + <ms>), 0 optional.
 */
SHELL_STATIC_SUBCMD_SET_CREATE(blink_cmds,
	SHELL_CMD_ARG(set, NULL, "Set the blink period: blink set <ms>", cmd_blink_set, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(blink, &blink_cmds, "Control the LED blink rate", NULL);

/*
 * Statically defined threads. The delay of SYS_FOREVER_MS means "created but
 * not started"; main() starts them once the GPIOs are configured.
 */
K_THREAD_DEFINE(blink_tid, STACK_SIZE, blink_thread, NULL, NULL, NULL,
		BLINK_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(button_tid, STACK_SIZE, button_thread, NULL, NULL, NULL,
		BUTTON_PRIORITY, 0, SYS_FOREVER_MS);

int main(void)
{
	int ret;

	LOG_INF("RTOS blinky on %s", CONFIG_BOARD);

	if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&button)) {
		LOG_ERR("GPIO device not ready");
		return 0;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure LED (%d)", ret);
		return 0;
	}

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure button (%d)", ret);
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure button interrupt (%d)", ret);
		return 0;
	}

	gpio_init_callback(&button_cb_data, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	k_thread_start(blink_tid);
	k_thread_start(button_tid);

	LOG_INF("Threads started. Press SW0 or type 'blink set <ms>'.");

	/* Returning from main() just ends the main thread; the others keep running. */
	return 0;
}

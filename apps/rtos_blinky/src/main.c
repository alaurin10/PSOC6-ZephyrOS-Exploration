/*
 * RTOS blinky for the CY8CPROTO-062-4343W.
 *
 * - blink thread:  toggles LED0 at the current blink period.
 * - button thread: sleeps on a semaphore; the SW0 interrupt gives the
 *                  semaphore and the thread cycles the blink period.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(rtos_blinky, LOG_LEVEL_INF);

/* Pins come from the board devicetree aliases, not hard-coded port/pin numbers. */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static const uint32_t blink_periods_ms[] = {500, 250, 100, 1000};
static atomic_t period_idx = ATOMIC_INIT(0);

/* Binary semaphore (max count 1) used to signal "button pressed" from ISR to thread. */
static K_SEM_DEFINE(button_sem, 0, 1);
static struct gpio_callback button_cb_data;

#define BLINK_PRIORITY  7
#define BUTTON_PRIORITY 5 /* lower number = higher priority */
#define STACK_SIZE      1024
#define DEBOUNCE_MS     50

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

	while (true) {
		gpio_pin_toggle_dt(&led);
		/* k_msleep() blocks this thread, letting the scheduler run others. */
		k_msleep(blink_periods_ms[atomic_get(&period_idx)]);
	}
}

static void button_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&button_sem, K_FOREVER);

		/* Mechanical buttons bounce: wait, then confirm it is still pressed. */
		k_msleep(DEBOUNCE_MS);
		if (gpio_pin_get_dt(&button) == 1) {
			atomic_val_t next = (atomic_get(&period_idx) + 1) % ARRAY_SIZE(blink_periods_ms);

			atomic_set(&period_idx, next);
			LOG_INF("Button pressed -> blink period %u ms", blink_periods_ms[next]);
		}

		/* Discard extra gives caused by bounce edges during the debounce window. */
		k_sem_reset(&button_sem);
	}
}

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

	LOG_INF("Threads started. Press SW0 to change the blink rate.");

	/* Returning from main() just ends the main thread; the others keep running. */
	return 0;
}

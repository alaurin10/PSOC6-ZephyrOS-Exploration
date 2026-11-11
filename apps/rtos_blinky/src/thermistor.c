/*
 * Onboard NTC thermistor (RT1) on the CY8CPROTO-062-4343W.
 *
 * Divider: [res end] -- R39 10k -- THERM_OUT -- RT1 NTC -- [ntc end]
 *
 * Ratiometric measurement: sample THERM_OUT twice with opposite polarity.
 *   A: res end high, ntc end low  ->  raw_a ~ V * Rntc / (R + Rntc)
 *   B: res end low,  ntc end high ->  raw_b ~ V * R    / (R + Rntc)
 *   => Rntc = R * raw_a / raw_b
 * The supply voltage and the ADC reference/full-scale cancel out, so neither
 * needs to be known exactly.
 *
 * Timing: a hardware TCPWM counter wraps once per second; its interrupt
 * submits the sample work item. The ADC read itself can't run in the ISR
 * because adc_read() blocks.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "app_settings.h"
#include "channels.h"
#include "thermistor.h"

LOG_MODULE_REGISTER(thermistor, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

static const struct adc_dt_spec therm_adc = ADC_DT_SPEC_GET(USER_NODE);
static const struct gpio_dt_spec res_end = GPIO_DT_SPEC_GET(USER_NODE, therm_res_gpios);
static const struct gpio_dt_spec ntc_end = GPIO_DT_SPEC_GET(USER_NODE, therm_ntc_gpios);
static const struct device *const sample_counter =
	DEVICE_DT_GET(DT_PHANDLE(USER_NODE, sample_counter));

/* R39 and RT1 (Murata NCP18XH103F03RB: 10k at 25 C, B25/50 = 3380 K) */
#define R_FIXED_OHM      10000.0f
#define NTC_R25_OHM      10000.0f
#define NTC_BETA_K       3380.0f
#define KELVIN_25C       298.15f
#define KELVIN_0C        273.15f

#define SAMPLE_PERIOD_MS 1000
#define SETTLE_US        50

static void sample_work_handler(struct k_work *work);
static K_WORK_DEFINE(sample_work, sample_work_handler);

/* Set by the ISR so the work handler knows the counter was just reset to 0. */
static atomic_t timer_fired;
static atomic_t timer_ticks;

/*
 * Counter "top value" callback: runs in interrupt context every time the
 * counter wraps (once per SAMPLE_PERIOD_MS). Keep it tiny: hand off to a thread.
 */
static void sample_tick_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	atomic_inc(&timer_ticks);
	atomic_set(&timer_fired, 1);
	k_work_submit(&sample_work);
}

static int read_raw(uint16_t *raw)
{
	struct adc_sequence seq = {
		.buffer = raw,
		.buffer_size = sizeof(*raw),
	};
	int err = adc_sequence_init_dt(&therm_adc, &seq);

	if (err == 0) {
		err = adc_read_dt(&therm_adc, &seq);
	}
	return err;
}

static int sample_with_polarity(bool res_high, uint16_t *raw)
{
	gpio_pin_set_dt(&res_end, res_high);
	gpio_pin_set_dt(&ntc_end, !res_high);
	k_busy_wait(SETTLE_US);
	return read_raw(raw);
}

static int measure(struct temp_msg *r)
{
	const uint16_t raw_max = BIT(therm_adc.resolution) - 1;
	int err = sample_with_polarity(true, &r->raw_a);

	if (err == 0) {
		err = sample_with_polarity(false, &r->raw_b);
	}

	/* Both ends low: no current flows through the divider between samples. */
	gpio_pin_set_dt(&res_end, 0);
	gpio_pin_set_dt(&ntc_end, 0);

	if (err != 0) {
		return err;
	}

	/* 0 or full scale means the input clipped, so the ratio would be meaningless. */
	if (r->raw_a == 0 || r->raw_b == 0 || r->raw_a >= raw_max || r->raw_b >= raw_max) {
		return -ERANGE;
	}

	float ntc_ohm = R_FIXED_OHM * r->raw_a / r->raw_b;
	/* Beta equation: 1/T = 1/T25 + ln(R/R25) / B */
	float kelvin = 1.0f / (1.0f / KELVIN_25C + logf(ntc_ohm / NTC_R25_OHM) / NTC_BETA_K);

	r->ntc_ohm = (uint32_t)lroundf(ntc_ohm);
	r->centi_c = (int32_t)lroundf((kelvin - KELVIN_0C) * 100.0f);
	r->timestamp_ms = k_uptime_get();
	return 0;
}

/* Runs on the system work queue thread, not in its own thread. */
static void sample_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	static uint32_t seq;
	int32_t latency_us = -1;
	struct temp_msg r;

	/* The counter restarted from 0 at the interrupt, so its current value is
	 * exactly how long the work item waited to run.
	 */
	if (atomic_cas(&timer_fired, 1, 0)) {
		uint32_t ticks;

		if (counter_get_value(sample_counter, &ticks) == 0) {
			latency_us = (int32_t)counter_ticks_to_us(sample_counter, ticks);
		}
	}

	int err = measure(&r);

	if (err != 0) {
		LOG_WRN("Measurement failed (%d)", err);
		return;
	}

	r.seq = ++seq;
	r.latency_us = latency_us;

	/*
	 * Copy the message into the channel and notify every observer. Listeners
	 * run right here, in this thread; subscribers are woken up to run later.
	 * The timeout bounds how long we wait if another thread holds the channel.
	 */
	err = zbus_chan_pub(&temp_chan, &r, K_MSEC(100));
	if (err != 0) {
		LOG_WRN("Publish failed (%d)", err); /* -ENOMSG: rejected by the validator */
	}
}

static int sample_timer_start(void)
{
	if (!device_is_ready(sample_counter)) {
		LOG_ERR("Sample counter not ready");
		return -ENODEV;
	}

	/* The counter counts 0..top inclusive, so one period is top + 1 ticks. */
	const struct counter_top_cfg top_cfg = {
		.ticks = counter_us_to_ticks(sample_counter, SAMPLE_PERIOD_MS * 1000U) - 1U,
		.callback = sample_tick_isr,
		.user_data = NULL,
		.flags = 0,
	};
	int err = counter_set_top_value(sample_counter, &top_cfg);

	if (err != 0) {
		LOG_ERR("Counter top value failed (%d)", err);
		return err;
	}

	err = counter_start(sample_counter);
	if (err != 0) {
		LOG_ERR("Counter start failed (%d)", err);
		return err;
	}

	LOG_INF("Sample timer: %u Hz counter, top %u", counter_get_frequency(sample_counter),
		top_cfg.ticks);
	return 0;
}

int thermistor_init(void)
{
	int err;

	if (!adc_is_ready_dt(&therm_adc) || !gpio_is_ready_dt(&res_end) ||
	    !gpio_is_ready_dt(&ntc_end)) {
		LOG_ERR("Thermistor ADC/GPIO not ready");
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&therm_adc);
	if (err != 0) {
		LOG_ERR("ADC channel setup failed (%d)", err);
		return err;
	}

	err = gpio_pin_configure_dt(&res_end, GPIO_OUTPUT_INACTIVE);
	if (err == 0) {
		err = gpio_pin_configure_dt(&ntc_end, GPIO_OUTPUT_INACTIVE);
	}
	if (err != 0) {
		LOG_ERR("Thermistor GPIO setup failed (%d)", err);
		return err;
	}

	/* First reading right away, then one per counter wrap. */
	k_work_submit(&sample_work);
	return sample_timer_start();
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct temp_msg r;

	/* The channel always holds the latest message: read a consistent copy of it. */
	int err = zbus_chan_read(&temp_chan, &r, K_MSEC(100));

	if (err != 0 || r.seq == 0) {
		shell_error(sh, "No reading yet");
		return -EAGAIN;
	}

	shell_print(sh, "Temperature: " CENTI_FMT " C (sample #%u)", CENTI_ARGS(r.centi_c), r.seq);
	shell_print(sh, "NTC: %u ohm (raw A=%u, B=%u), %lld ms ago", r.ntc_ohm, r.raw_a,
		    r.raw_b, k_uptime_get() - r.timestamp_ms);
	if (r.latency_us >= 0) {
		shell_print(sh, "Timer IRQ -> sample latency: %d us (timer ticks: %d)",
			    r.latency_us, (int)atomic_get(&timer_ticks));
	}
	return 0;
}

/* "temp limits" shows, "temp limits <cold> <hot>" sets (whole degrees C, saved to flash). */
static int cmd_temp_limits(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 3) {
		int err = 0;
		long cold = shell_strtol(argv[1], 10, &err);
		long hot = err == 0 ? shell_strtol(argv[2], 10, &err) : 0;

		if (err != 0 || app_settings_set_thresholds_centi(cold * 100, hot * 100) != 0) {
			shell_error(sh, "Need -20 <= cold, hot <= 80 and hot >= cold + 2");
			return -EINVAL;
		}
	} else if (argc != 1) {
		shell_error(sh, "Usage: temp limits [<cold> <hot>]");
		return -EINVAL;
	}

	int32_t cold = app_settings_cold_centi();
	int32_t hot = app_settings_hot_centi();

	shell_print(sh, "Cold below " CENTI_FMT " C, hot at or above " CENTI_FMT " C",
		    CENTI_ARGS(cold), CENTI_ARGS(hot));
	return 0;
}

/*
 * Extensible subcommand set: any file can add "temp <something>" with
 * SHELL_SUBCMD_ADD((temp), ...), e.g. "temp stats" lives in temp_monitor.c.
 */
SHELL_SUBCMD_SET_CREATE(temp_cmds, (temp));
SHELL_SUBCMD_ADD((temp), limits, NULL,
		 "Show/set LED-mode thresholds: temp limits [<cold> <hot>]",
		 cmd_temp_limits, 1, 2);

/* "temp" on its own runs cmd_temp; "temp limits ..." runs the subcommand. */
SHELL_CMD_REGISTER(temp, &temp_cmds, "Show the latest thermistor reading", cmd_temp);

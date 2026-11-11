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
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "app_settings.h"
#include "thermistor.h"

LOG_MODULE_REGISTER(thermistor, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

static const struct adc_dt_spec therm_adc = ADC_DT_SPEC_GET(USER_NODE);
static const struct gpio_dt_spec res_end = GPIO_DT_SPEC_GET(USER_NODE, therm_res_gpios);
static const struct gpio_dt_spec ntc_end = GPIO_DT_SPEC_GET(USER_NODE, therm_ntc_gpios);

/* R39 and RT1 (Murata NCP18XH103F03RB: 10k at 25 C, B25/50 = 3380 K) */
#define R_FIXED_OHM      10000.0f
#define NTC_R25_OHM      10000.0f
#define NTC_BETA_K       3380.0f
#define KELVIN_25C       298.15f
#define KELVIN_0C        273.15f

#define SAMPLE_PERIOD_MS 1000
#define SETTLE_US        50
#define LOG_DELTA_CENTI  50 /* log when the temperature moves by 0.5 C or more */

struct reading {
	int32_t centi_c;
	uint32_t ntc_ohm;
	uint16_t raw_a;
	uint16_t raw_b;
	int64_t timestamp_ms;
};

/* The work item writes `latest`, the shell thread reads it: protect with a mutex. */
static K_MUTEX_DEFINE(latest_lock);
static struct reading latest;
static bool have_reading;

static void sample_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sample_work, sample_work_handler);

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

static int measure(struct reading *r)
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
	static int32_t last_logged_centi_c = INT32_MIN;
	struct reading r;
	int err = measure(&r);

	if (err != 0) {
		LOG_WRN("Measurement failed (%d)", err);
	} else {
		k_mutex_lock(&latest_lock, K_FOREVER);
		latest = r;
		have_reading = true;
		k_mutex_unlock(&latest_lock);

		if (last_logged_centi_c == INT32_MIN ||
		    abs(r.centi_c - last_logged_centi_c) >= LOG_DELTA_CENTI) {
			last_logged_centi_c = r.centi_c;
			LOG_INF("Temperature %s%d.%02d C", r.centi_c < 0 ? "-" : "",
				abs(r.centi_c) / 100, abs(r.centi_c) % 100);
		}
	}

	/* Re-arm: this is what makes a one-shot work item periodic. */
	k_work_schedule(k_work_delayable_from_work(work), K_MSEC(SAMPLE_PERIOD_MS));
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

	k_work_schedule(&sample_work, K_NO_WAIT);
	return 0;
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct reading r;
	bool valid;

	k_mutex_lock(&latest_lock, K_FOREVER);
	r = latest;
	valid = have_reading;
	k_mutex_unlock(&latest_lock);

	if (!valid) {
		shell_error(sh, "No reading yet");
		return -EAGAIN;
	}

	shell_print(sh, "Temperature: %s%d.%02d C", r.centi_c < 0 ? "-" : "",
		    abs(r.centi_c) / 100, abs(r.centi_c) % 100);
	shell_print(sh, "NTC: %u ohm (raw A=%u, B=%u), %lld ms ago", r.ntc_ohm, r.raw_a,
		    r.raw_b, k_uptime_get() - r.timestamp_ms);
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

	shell_print(sh, "Cold below %d.%02d C, hot at or above %d.%02d C", cold / 100,
		    abs(cold) % 100, hot / 100, abs(hot) % 100);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(temp_cmds,
	SHELL_CMD_ARG(limits, NULL, "Show/set LED-mode thresholds: temp limits [<cold> <hot>]",
		      cmd_temp_limits, 1, 2),
	SHELL_SUBCMD_SET_END);

/* "temp" on its own runs cmd_temp; "temp limits ..." runs the subcommand. */
SHELL_CMD_REGISTER(temp, &temp_cmds, "Show the latest thermistor reading", cmd_temp);

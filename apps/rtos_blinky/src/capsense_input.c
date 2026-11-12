/*
 * CapSense buttons and slider -> LED controller.
 *
 *   BTN0 (CSX)  : LED follows the temperature   (led mode temp)
 *   BTN1 (CSX)  : LED blinks at the user period (led mode manual)
 *   Slider (CSD): sets the manual-mode blink rate, 0.5 Hz (left) .. 10 Hz
 *                 (right); visible in manual mode, saved in either mode
 *
 * Infineon's CapSense middleware does the sensing; this file replaces the
 * ModusToolbox BSP init (clock divider, pins, interrupt) with Zephyr code:
 *
 *   capsense thread: start scan -> wait for end-of-scan semaphore (given from
 *   the CSD interrupt) -> process -> act on changes -> sleep -> repeat.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <cy_gpio.h>
#include <cyhal_hwmgr.h>

#include "cycfg_capsense.h"
#include "app_settings.h"
#include "led_ctrl.h"

LOG_MODULE_REGISTER(capsense, LOG_LEVEL_INF);

/* Below the control threads (LED 7, BLE 9, stats 10) but above shell/logging:
 * the middleware's calibration busy-waits, which must never starve the LED
 * thread (it feeds the watchdog). Scanning itself sleeps on a semaphore.
 */
#define CS_PRIORITY       12
#define CS_STACK_SIZE     2048
#define CS_SCAN_PERIOD_MS 20  /* 50 scans per second */
#define CS_IRQ_PRIORITY   3

#define SLIDER_MAX        300 /* xResolution from the generated configuration */
#define FREQ_MIN_MHZ      500 /* 0.5 Hz toggle rate at the left end */
#define FREQ_MAX_MHZ      10000
#define PERIOD_CHANGE_PCT 3   /* only send a new period if it moved by >= 3 % */

/* CSD driver context (the BSP's cycfg_peripherals.c used to provide it). */
cy_stc_csd_context_t cy_csd_0_context = {
	.lockKey = CY_CSD_NONE_KEY,
};

/* ---- Clock divider reservation ------------------------------------------- */

static cy_rslt_t div_reserve_rslt;

/*
 * Runs before any driver initializes (PRE_KERNEL_1, priority 0), so the HAL's
 * resource manager never hands our divider to the UART, timer or ADC.
 */
static int capsense_reserve_divider(void)
{
	static const cyhal_resource_inst_t div = {
		.type = CYHAL_RSC_CLOCK,
		.block_num = CYHAL_CLOCK_BLOCK_PERIPHERAL_8BIT,
		.channel_num = CY_CAPSENSE_PERI_DIV_INDEX,
	};

	div_reserve_rslt = cyhal_hwmgr_reserve(&div);
	return 0;
}

SYS_INIT(capsense_reserve_divider, PRE_KERNEL_1, 0);

/* ---- Interrupt and end-of-scan signalling ---------------------------------- */

static K_SEM_DEFINE(scan_done, 0, 1);

/* CSD interrupt: the middleware advances to the next sensor, or finishes. */
static void csd_isr(const void *arg)
{
	ARG_UNUSED(arg);
	Cy_CapSense_InterruptHandler(CYBSP_CSD_HW, &cy_capsense_context);
}

/* Called by the middleware from the ISR when every widget has been scanned. */
static void end_of_scan_cb(cy_stc_active_scan_sns_t *active)
{
	ARG_UNUSED(active);
	k_sem_give(&scan_done);
}

/* ---- Hardware setup ---------------------------------------------------------- */

struct cs_pin {
	GPIO_PRT_Type *port;
	uint8_t pin;
};

static const struct cs_pin cs_pins[] = {
	{Cmod_PORT, Cmod_PIN},
	{CintA_PORT, CintA_PIN},
	{CintB_PORT, CintB_PIN},
	{Button0_Tx_PORT, Button0_Tx_PIN},
	{Button0_Rx0_PORT, Button0_Rx0_PIN},
	{Button1_Rx0_PORT, Button1_Rx0_PIN},
	{LinearSlider0_Sns0_PORT, LinearSlider0_Sns0_PIN},
	{LinearSlider0_Sns1_PORT, LinearSlider0_Sns1_PIN},
	{LinearSlider0_Sns2_PORT, LinearSlider0_Sns2_PIN},
	{LinearSlider0_Sns3_PORT, LinearSlider0_Sns3_PIN},
	{LinearSlider0_Sns4_PORT, LinearSlider0_Sns4_PIN},
};

static int capsense_hw_init(void)
{
	cy_capsense_status_t st;

	if (div_reserve_rslt != CY_RSLT_SUCCESS) {
		LOG_ERR("Clock divider 8-bit #%u already in use (0x%08x)",
			CY_CAPSENSE_PERI_DIV_INDEX, (unsigned int)div_reserve_rslt);
		return -EBUSY;
	}

	/* Same as the BSP's init_cycfg_pins(): sensor and capacitor pins analog. */
	for (size_t i = 0; i < ARRAY_SIZE(cs_pins); i++) {
		Cy_GPIO_Pin_FastInit(cs_pins[i].port, cs_pins[i].pin, CY_GPIO_DM_ANALOG, 0,
				     HSIOM_SEL_GPIO);
	}

	/*
	 * Same as the BSP's init_cycfg_routing(): the analog mux buses (AMUXBUS A
	 * and B) are split into segments between port groups. Close splitters 2
	 * and 4 so the CSD block, Cmod/CintA/CintB (port 7) and the sensors
	 * (ports 1 and 8) are on one bus. Without this, Cmod never charges and
	 * Cy_CapSense_Enable() spins forever in its calibration.
	 */
	static const uint32_t split_closed = HSIOM_V2_AMUX_SPLIT_CTL_SWITCH_AA_SL_Msk |
					     HSIOM_V2_AMUX_SPLIT_CTL_SWITCH_AA_SR_Msk |
					     HSIOM_V2_AMUX_SPLIT_CTL_SWITCH_BB_SL_Msk |
					     HSIOM_V2_AMUX_SPLIT_CTL_SWITCH_BB_SR_Msk;

	HSIOM->AMUX_SPLIT_CTL[2] = split_closed;
	HSIOM->AMUX_SPLIT_CTL[4] = split_closed;

	/* Route the divider to the CSD block; the middleware sets its value. */
	Cy_SysClk_PeriphAssignDivider(PCLK_CSD_CLOCK, CY_CAPSENSE_PERI_DIV_TYPE,
				      CY_CAPSENSE_PERI_DIV_INDEX);

	st = Cy_CapSense_Init(&cy_capsense_context);
	if (st != CY_CAPSENSE_STATUS_SUCCESS) {
		LOG_ERR("Cy_CapSense_Init failed (0x%08x)", (unsigned int)st);
		return -EIO;
	}

	/* Zephyr's equivalent of Cy_SysInt_Init(): register and enable the ISR. */
	IRQ_CONNECT(csd_interrupt_IRQn, CS_IRQ_PRIORITY, csd_isr, NULL, 0);
	irq_enable(csd_interrupt_IRQn);

	/* Calibrates and, with SmartSense, auto-tunes every sensor (scans use the IRQ). */
	st = Cy_CapSense_Enable(&cy_capsense_context);
	if (st != CY_CAPSENSE_STATUS_SUCCESS) {
		LOG_ERR("Cy_CapSense_Enable failed (0x%08x)", (unsigned int)st);
		return -EIO;
	}

	Cy_CapSense_RegisterCallback(CY_CAPSENSE_END_OF_SCAN_E, end_of_scan_cb,
				     &cy_capsense_context);
	return 0;
}

/* ---- Input handling --------------------------------------------------------- */

static atomic_t slider_pos = ATOMIC_INIT(-1); /* -1 = not touched */
static atomic_t scans;

static uint32_t slider_to_period_ms(uint32_t pos)
{
	uint32_t f_mhz = FREQ_MIN_MHZ + (FREQ_MAX_MHZ - FREQ_MIN_MHZ) * pos / SLIDER_MAX;

	return CLAMP(1000000U / f_mhz, APP_PERIOD_MIN_MS, APP_PERIOD_MAX_MS);
}

static void handle_inputs(void)
{
	static bool btn0_was, btn1_was;
	static uint32_t last_sent_ms;

	bool btn0 = Cy_CapSense_IsWidgetActive(CY_CAPSENSE_BUTTON0_WDGT_ID, &cy_capsense_context);
	bool btn1 = Cy_CapSense_IsWidgetActive(CY_CAPSENSE_BUTTON1_WDGT_ID, &cy_capsense_context);

	/* Act on the touch, not while it's held. */
	if (btn0 && !btn0_was) {
		LOG_INF("BTN0 touched -> temperature mode");
		(void)led_ctrl_request_mode(LED_MODE_TEMP);
	}
	if (btn1 && !btn1_was) {
		LOG_INF("BTN1 touched -> manual mode");
		(void)led_ctrl_request_mode(LED_MODE_MANUAL);
	}
	btn0_was = btn0;
	btn1_was = btn1;

	const cy_stc_capsense_touch_t *touch =
		Cy_CapSense_GetTouchInfo(CY_CAPSENSE_LINEARSLIDER0_WDGT_ID, &cy_capsense_context);

	if (touch->numPosition == 0U) {
		if (atomic_set(&slider_pos, -1) != -1) {
			LOG_INF("Slider released at %u ms", last_sent_ms);
		}
		return;
	}

	uint32_t pos = touch->ptrPosition[0].x;
	uint32_t period = slider_to_period_ms(pos);

	atomic_set(&slider_pos, pos);

	/* Sliding produces a new position every scan; skip tiny changes so the
	 * LED queue isn't flooded. Settings save once, 2 s after the last change.
	 */
	if (last_sent_ms == 0U ||
	    (uint32_t)abs((int)period - (int)last_sent_ms) * 100U >= last_sent_ms * PERIOD_CHANGE_PCT) {
		if (led_ctrl_request_period(period) == 0) {
			last_sent_ms = period;
		}
	}
}

static void capsense_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (capsense_hw_init() != 0) {
		return;
	}
	LOG_INF("CapSense ready: BTN0 = temp mode, BTN1 = manual mode, slider = blink rate");

	while (true) {
		k_sem_reset(&scan_done);
		if (Cy_CapSense_ScanAllWidgets(&cy_capsense_context) != CY_CAPSENSE_STATUS_SUCCESS) {
			k_msleep(CS_SCAN_PERIOD_MS);
			continue;
		}

		/* The CPU is free while the CSD hardware scans; the ISR wakes us. */
		if (k_sem_take(&scan_done, K_MSEC(100)) != 0) {
			LOG_WRN("CapSense scan timed out");
			continue;
		}

		/* Filters, baselines, thresholds, debounce, slider centroid. */
		Cy_CapSense_ProcessAllWidgets(&cy_capsense_context);
		atomic_inc(&scans);
		handle_inputs();

		k_msleep(CS_SCAN_PERIOD_MS);
	}
}

K_THREAD_DEFINE(capsense_tid, CS_STACK_SIZE, capsense_thread, NULL, NULL, NULL, CS_PRIORITY,
		0, 0);

/* ---- Shell: "capsense" ------------------------------------------------------- */

static const char *const wd_names[CY_CAPSENSE_WIDGET_COUNT] = {"BTN0", "BTN1", "SLIDER"};

static int cmd_capsense(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int32_t pos = (int32_t)atomic_get(&slider_pos);

	shell_print(sh, "%u scans, slider %s%d", (uint32_t)atomic_get(&scans),
		    pos < 0 ? "not touched " : "at ", pos < 0 ? 0 : pos);
	if (pos >= 0) {
		shell_print(sh, "  -> period %u ms", slider_to_period_ms(pos));
	}

	/* Per-sensor signal: raw count, baseline (slow average), diff = raw - baseline. */
	for (uint32_t w = 0; w < CY_CAPSENSE_WIDGET_COUNT; w++) {
		const cy_stc_capsense_widget_config_t *wd = &cy_capsense_context.ptrWdConfig[w];

		shell_print(sh, "%-6s %s", wd_names[w],
			    Cy_CapSense_IsWidgetActive(w, &cy_capsense_context) ? "ACTIVE" : "-");
		for (uint32_t s = 0; s < wd->numSns; s++) {
			const cy_stc_capsense_sensor_context_t *sns = &wd->ptrSnsContext[s];

			shell_print(sh, "  sns%u raw %5u bsln %5u diff %5u", s, sns->raw, sns->bsln,
				    sns->diff);
		}
	}
	return 0;
}

SHELL_CMD_REGISTER(capsense, NULL, "Show CapSense widget status and sensor counts", cmd_capsense);

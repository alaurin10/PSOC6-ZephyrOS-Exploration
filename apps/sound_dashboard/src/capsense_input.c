/*
 * CapSense buttons and slider -> zbus.
 *
 *   input_chan  live touch state (buttons, slider position), on every change
 *   ctrl_chan   commands for ui_ctrl:
 *                 BTN0 touch          -> next view
 *                 BTN1 short touch    -> next colour scheme (on release)
 *                 BTN1 held >= 800 ms -> freeze / unfreeze the display
 *                 slider              -> sets the parameter selected by ui_chan's
 *                                        slider_param (0..100, left to right)
 *
 * Infineon's CapSense middleware does the sensing; this file replaces the
 * ModusToolbox BSP init (clock divider, pins, interrupt) with Zephyr code:
 *
 *   capsense thread: start scan -> wait for end-of-scan semaphore (given from
 *   the CSD interrupt) -> process -> act on changes -> sleep -> repeat.
 *
 * Adapted from apps/rtos_blinky/src/capsense_input.c.
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
#include "app_watchdog.h"
#include "capsense_input.h"
#include "channels.h"

LOG_MODULE_REGISTER(capsense, LOG_LEVEL_INF);

/* Below the UI and audio threads but above shell/logging: the middleware's
 * calibration busy-waits, which must never starve them. Scanning itself
 * sleeps on a semaphore.
 */
#define CS_PRIORITY       12
#define CS_STACK_SIZE     2048
#define CS_SCAN_PERIOD_MS 20  /* 50 scans per second */
#define CS_IRQ_PRIORITY   3

#define LONG_PRESS_MS     800
#define PARAM_STEP        2   /* only send a new slider value if it moved by >= 2 % */

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

static atomic_t scans;
static atomic_t commands_sent;

static void send_cmd(uint8_t cmd, uint8_t param, int32_t value)
{
	struct ctrl_msg m = {
		.source = CTRL_SRC_CAPSENSE,
		.cmd = cmd,
		.param = param,
		.value = value,
	};

	if (zbus_chan_pub(&ctrl_chan, &m, K_MSEC(20)) == 0) {
		atomic_inc(&commands_sent);
	} else {
		LOG_WRN("Command %u dropped", cmd);
	}
}

static void handle_buttons(bool btn0, bool btn1)
{
	static bool btn0_was, btn1_was, btn1_long_fired;
	static int64_t btn1_down_ms;

	/* Act on the touch, not while it's held. */
	if (btn0 && !btn0_was) {
		send_cmd(CTRL_VIEW_NEXT, 0, 0);
	}

	/* BTN1: a short touch acts on release; holding it acts once, at the threshold. */
	if (btn1 && !btn1_was) {
		btn1_down_ms = k_uptime_get();
		btn1_long_fired = false;
	} else if (btn1 && !btn1_long_fired && k_uptime_get() - btn1_down_ms >= LONG_PRESS_MS) {
		btn1_long_fired = true;
		send_cmd(CTRL_FREEZE_TOGGLE, 0, 0);
	} else if (!btn1 && btn1_was && !btn1_long_fired) {
		send_cmd(CTRL_COLOUR_NEXT, 0, 0);
	}

	btn0_was = btn0;
	btn1_was = btn1;
}

static void handle_slider(int16_t pos)
{
	static int32_t last_sent = -1;

	if (pos < 0) {
		last_sent = -1; /* the next touch always sends */
		return;
	}

	/* Which parameter the slider sets is part of the UI state; the channel
	 * always holds the latest copy, so just read it.
	 */
	struct ui_msg ui;

	if (zbus_chan_read(&ui_chan, &ui, K_MSEC(5)) != 0) {
		return;
	}

	int32_t value = (int32_t)pos * UI_PARAM_MAX / INPUT_SLIDER_MAX;

	/* Sliding produces a new position every scan; skip tiny changes so the
	 * command channel isn't flooded. Settings save once, after the last change.
	 */
	if (last_sent < 0 || abs(value - last_sent) >= PARAM_STEP ||
	    (value != last_sent && (value == 0 || value == UI_PARAM_MAX))) {
		send_cmd(CTRL_PARAM_SET, ui.slider_param, value);
		last_sent = value;
	}
}

static void handle_inputs(void)
{
	static struct input_msg last = {.slider = -1};
	const cy_stc_capsense_touch_t *touch =
		Cy_CapSense_GetTouchInfo(CY_CAPSENSE_LINEARSLIDER0_WDGT_ID, &cy_capsense_context);
	struct input_msg now = {
		.btn0 = Cy_CapSense_IsWidgetActive(CY_CAPSENSE_BUTTON0_WDGT_ID,
						   &cy_capsense_context) != 0,
		.btn1 = Cy_CapSense_IsWidgetActive(CY_CAPSENSE_BUTTON1_WDGT_ID,
						   &cy_capsense_context) != 0,
		.slider = touch->numPosition ? (int16_t)touch->ptrPosition[0].x : -1,
	};

	handle_buttons(now.btn0, now.btn1);
	handle_slider(now.slider);

	if (now.btn0 != last.btn0 || now.btn1 != last.btn1 || now.slider != last.slider) {
		now.seq = last.seq + 1;
		(void)zbus_chan_pub(&input_chan, &now, K_MSEC(20));
		last = now;
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
	app_watchdog_enable(WDT_CLIENT_CAPSENSE);
	LOG_INF("CapSense ready: BTN0 = next view, BTN1 = colour (hold = freeze), "
		"slider = parameter");

	while (true) {
		app_watchdog_alive(WDT_CLIENT_CAPSENSE);

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
		0, SYS_FOREVER_MS);

void capsense_input_start(void)
{
	k_thread_start(capsense_tid);
}

/* ---- Shell: "capsense" ------------------------------------------------------- */

static const char *const wd_names[CY_CAPSENSE_WIDGET_COUNT] = {"BTN0", "BTN1", "SLIDER"};

static int cmd_capsense(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct input_msg in;

	(void)zbus_chan_read(&input_chan, &in, K_MSEC(100));
	shell_print(sh, "%u scans, %u commands sent, slider %s%d", (uint32_t)atomic_get(&scans),
		    (uint32_t)atomic_get(&commands_sent), in.slider < 0 ? "not touched " : "at ",
		    in.slider < 0 ? 0 : in.slider);

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

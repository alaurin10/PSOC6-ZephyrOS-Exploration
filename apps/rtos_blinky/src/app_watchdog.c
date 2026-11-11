/*
 * Hardware watchdog and reset-cause reporting.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <cmsis_core.h>
#include <cy_syslib.h>

#include "app_watchdog.h"

LOG_MODULE_REGISTER(app_watchdog, LOG_LEVEL_INF);

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1; /* -1: watchdog not running */

void app_watchdog_report_reset_cause(void)
{
	/* Zephyr's hwinfo reset-cause API isn't implemented for PSoC 6 in this tree,
	 * so read the SRSS reset-cause register through the Infineon PDL directly.
	 */
	uint32_t reason = Cy_SysLib_GetResetReason();

	if (reason & CY_SYSLIB_RESET_HWWDT) {
		LOG_WRN("Last reset was caused by the WATCHDOG");
	} else {
		LOG_INF("Reset cause: 0x%08x", reason);
	}
	/* Clear it so the next boot reports its own cause, not this one. */
	Cy_SysLib_ClearResetReason();
}

void app_watchdog_start(void)
{
	/* The PSoC 6 WDT keeps counting while the CPU is halted at a breakpoint,
	 * so leave it off while debug is enabled (DHCSR.C_DEBUGEN). Only the
	 * debugger can set this bit and software can't clear it; a soft reset
	 * (e.g. after flashing) keeps it, an XRES (RESET button) or power cycle clears it.
	 */
	if (DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) {
		LOG_WRN("Debug enabled: watchdog not started. Press RESET (SW1) to arm it.");
		return;
	}

	if (!device_is_ready(wdt)) {
		LOG_ERR("Watchdog device not ready");
		return;
	}

	const struct wdt_timeout_cfg cfg = {
		.window.max = APP_WDT_TIMEOUT_MS,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int channel = wdt_install_timeout(wdt, &cfg);

	if (channel < 0) {
		LOG_ERR("Watchdog install failed (%d)", channel);
		return;
	}

	int ret = wdt_setup(wdt, 0);

	if (ret < 0) {
		LOG_ERR("Watchdog setup failed (%d)", ret);
		return;
	}

	wdt_channel = channel;
	LOG_INF("Watchdog started (%d ms timeout)", APP_WDT_TIMEOUT_MS);
}

void app_watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		wdt_feed(wdt, wdt_channel);
	}
}

/*
 * Hardware watchdog, liveness supervisor and reset-cause reporting.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <cmsis_core.h>
#include <cy_syslib.h>

#include "app_watchdog.h"

LOG_MODULE_REGISTER(app_watchdog, LOG_LEVEL_INF);

#define SUPERVISOR_PRIORITY 1 /* above every client, so it can't be starved by them */
#define SUPERVISOR_STACK    768
#define STALL_MAGIC         0x57415443u /* "WATC" */

static const char *const client_names[WDT_CLIENT_COUNT] = {
	"ui", "capsense", "audio", "web", "datalog",
};

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1; /* -1: hardware watchdog not running */

static atomic_t enabled_mask;
static atomic_t alive_mask;
static atomic_t feeds;

/*
 * Survives a watchdog reset (not a power cycle): which clients were stalled
 * when the supervisor stopped feeding, so the next boot can name them.
 */
static __noinit struct {
	uint32_t magic;
	uint32_t mask;
} stall_record;

void app_watchdog_report_reset_cause(void)
{
	/* Zephyr's hwinfo reset-cause API isn't implemented for PSoC 6 in this tree,
	 * so read the SRSS reset-cause register through the Infineon PDL directly.
	 */
	uint32_t reason = Cy_SysLib_GetResetReason();

	if (reason & CY_SYSLIB_RESET_HWWDT) {
		LOG_WRN("Last reset was caused by the WATCHDOG");
		if (stall_record.magic == STALL_MAGIC) {
			for (int i = 0; i < WDT_CLIENT_COUNT; i++) {
				if (stall_record.mask & BIT(i)) {
					LOG_WRN("  stalled thread: %s", client_names[i]);
				}
			}
		}
	} else {
		LOG_INF("Reset cause: 0x%08x", reason);
	}
	stall_record.magic = 0;
	/* Clear it so the next boot reports its own cause, not this one. */
	Cy_SysLib_ClearResetReason();
}

void app_watchdog_enable(enum app_wdt_client client)
{
	atomic_set_bit(&alive_mask, client);
	atomic_set_bit(&enabled_mask, client);
}

void app_watchdog_disable(enum app_wdt_client client)
{
	atomic_clear_bit(&enabled_mask, client);
}

void app_watchdog_alive(enum app_wdt_client client)
{
	atomic_set_bit(&alive_mask, client);
}

static void supervisor_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t reported = 0;

	while (true) {
		k_msleep(APP_WDT_CHECK_MS);

		uint32_t enabled = (uint32_t)atomic_get(&enabled_mask);
		uint32_t missing = enabled & ~(uint32_t)atomic_get(&alive_mask);

		if (missing == 0) {
			atomic_clear(&alive_mask);
			atomic_inc(&feeds);
			reported = 0;
			stall_record.magic = 0;
			if (wdt_channel >= 0) {
				wdt_feed(wdt, wdt_channel);
			}
			continue;
		}

		/* Not feeding: the hardware resets the chip within the timeout.
		 * Remember (and log, once) who is late.
		 */
		stall_record.mask = missing;
		stall_record.magic = STALL_MAGIC;
		if (missing != reported) {
			reported = missing;
			for (int i = 0; i < WDT_CLIENT_COUNT; i++) {
				if (missing & BIT(i)) {
					LOG_ERR("Thread '%s' missed its watchdog check-in",
						client_names[i]);
				}
			}
		}
	}
}

K_THREAD_DEFINE(wdt_sup_tid, SUPERVISOR_STACK, supervisor_thread, NULL, NULL, NULL,
		SUPERVISOR_PRIORITY, 0, SYS_FOREVER_MS);

static void hw_watchdog_start(void)
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

void app_watchdog_start(void)
{
	hw_watchdog_start();
	/* The supervisor runs even without the hardware watchdog, to report stalls. */
	k_thread_start(wdt_sup_tid);
}

static int cmd_watchdog(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint32_t enabled = (uint32_t)atomic_get(&enabled_mask);
	uint32_t alive = (uint32_t)atomic_get(&alive_mask);

	shell_print(sh, "Hardware watchdog %s, %u feeds", wdt_channel >= 0 ? "running" : "off",
		    (uint32_t)atomic_get(&feeds));
	for (int i = 0; i < WDT_CLIENT_COUNT; i++) {
		shell_print(sh, "  %-9s %s", client_names[i],
			    !(enabled & BIT(i)) ? "-" : (alive & BIT(i)) ? "checked in" : "waiting");
	}
	return 0;
}

SHELL_CMD_REGISTER(watchdog, NULL, "Show watchdog supervisor status", cmd_watchdog);

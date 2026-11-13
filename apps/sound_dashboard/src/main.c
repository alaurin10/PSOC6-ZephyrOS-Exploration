/*
 * Sound-reactive Wi-Fi dashboard for the CY8CPROTO-062-4343W.
 *
 * Modules (each one publishes or observes zbus channels, see channels.h):
 * - thermistor.c:     samples RT1 once per second, publishes temp_chan.
 * - capsense_input.c: buttons and slider, publishes input_chan and ctrl_chan.
 * - audio_capture.c:  PDM microphone stream and analysis (audio_dsp.c).
 * - ui_ctrl.c:        dashboard state machine, owns ui_chan.
 * - wifi_mgr.c:       Wi-Fi from stored credentials, DHCP, SNTP; publishes net_chan.
 * - web_server.c:     HTTP + WebSocket dashboard (pages in web/), mDNS.
 * - datalog.c:        CSV log on the QSPI flash (LittleFS), observes most channels.
 * - app_settings.c:   persisted UI state.
 * - app_watchdog.c:   hardware watchdog fed by a liveness supervisor.
 * - bus_monitor.c:    "bus" shell diagnostics.
 * - main.c (here):    startup order.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_settings.h"
#include "app_watchdog.h"
#include "audio_capture.h"
#include "capsense_input.h"
#include "datalog.h"
#include "thermistor.h"
#include "ui_ctrl.h"
#include "web_server.h"
#include "wifi_mgr.h"

LOG_MODULE_REGISTER(sound_dashboard, LOG_LEVEL_INF);

int main(void)
{
	int ret;

	LOG_INF("Sound dashboard on %s", CONFIG_BOARD);
	app_watchdog_report_reset_cause();

	/* Load saved settings before starting the threads that use them. */
	ret = app_settings_init();
	if (ret < 0) {
		LOG_ERR("Settings unavailable (%d), using defaults", ret);
	}

	app_watchdog_start();

	ret = ui_ctrl_start();
	if (ret < 0) {
		LOG_ERR("UI controller failed (%d)", ret);
	}

	ret = thermistor_init();
	if (ret < 0) {
		LOG_ERR("Thermistor init failed (%d)", ret);
	}

	capsense_input_start();
	audio_capture_start();
	datalog_start();

	ret = wifi_mgr_start();
	if (ret < 0) {
		LOG_ERR("Wi-Fi manager failed (%d)", ret);
	}

	ret = web_server_start();
	if (ret < 0) {
		LOG_ERR("Web server failed (%d)", ret);
	}

	LOG_INF("Started. Try 'ui', 'audio', 'datalog', 'wifimgr', 'web', 'temp', 'capsense', "
		"'bus watch on'.");

	/* Returning from main() just ends the main thread; the others keep running. */
	return 0;
}

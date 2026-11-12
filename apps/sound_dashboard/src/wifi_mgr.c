/*
 * Wi-Fi station manager: stored credentials, reconnects, DHCP, SNTP.
 *
 *   IDLE        no credentials stored: check again every few seconds, so
 *               "wifi cred add ..." in the shell is picked up by itself
 *   CONNECTING  connect request sent; on failure or disconnect, retry after
 *               2, 4, 8 ... 60 s
 *   CONNECTED   associated, waiting for DHCP
 *   ONLINE      has an IPv4 address: sync the clock (SNTP), poll the RSSI
 *
 * Network events arrive in the net_mgmt thread; the handler only records
 * them and queues work, and everything else runs on this module's own work
 * queue (connect requests and SNTP can block for a while).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h> /* after net_if.h: it uses struct net_if */
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/wifi_credentials.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "app_time.h"
#include "channels.h"
#include "wifi_mgr.h"

LOG_MODULE_REGISTER(wifi_mgr, LOG_LEVEL_INF);

#define WQ_PRIORITY        10
#define WQ_STACK_SIZE      3072
#define CRED_POLL          K_SECONDS(5)
#define BACKOFF_MIN_S      2
#define BACKOFF_MAX_S      60
#define RSSI_PERIOD        K_SECONDS(10)
#define SNTP_SERVER        "pool.ntp.org"
#define SNTP_TIMEOUT_MS    3000
#define SNTP_RETRY         K_SECONDS(30)
#define SNTP_RESYNC        K_HOURS(6)

#define EVENTS_WIFI (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)
#define EVENTS_IPV4 (NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL)

static K_THREAD_STACK_DEFINE(wq_stack, WQ_STACK_SIZE);
static struct k_work_q wq;

static struct net_if *iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

/* Events recorded by the net_mgmt handler, processed on the work queue. */
enum { EV_CONNECTED, EV_CONNECT_FAILED, EV_DISCONNECTED, EV_IP_ADDED, EV_IP_REMOVED };
static atomic_t pending;

/* Only touched on the work queue. */
static struct net_msg state;
static uint32_t backoff_s = BACKOFF_MIN_S;
static uint32_t attempts;
/* A connect request is out and its result hasn't arrived. Never send a second
 * one meanwhile: when the earlier one fails, the driver marks the interface
 * dormant again, even if the later one has connected.
 */
static bool in_flight;

static void connect_handler(struct k_work *work);
static void event_handler(struct k_work *work);
static void rssi_handler(struct k_work *work);
static void sntp_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(connect_work, connect_handler);
static K_WORK_DEFINE(event_work, event_handler);
static K_WORK_DELAYABLE_DEFINE(rssi_work, rssi_handler);
static K_WORK_DELAYABLE_DEFINE(sntp_work, sntp_handler);

static const char *const state_names[] = {"idle", "connecting", "connected", "online"};

static void publish(void)
{
	(void)zbus_chan_pub(&net_chan, &state, K_MSEC(100));
}

static void set_state(enum net_state s)
{
	if (state.state != s) {
		LOG_INF("Wi-Fi %s -> %s", state_names[state.state], state_names[s]);
		state.state = s;
		if (s != NET_STATE_ONLINE) {
			state.ipv4[0] = '\0';
		}
		if (s < NET_STATE_CONNECTED) {
			state.rssi = 0;
		}
		publish();
	}
}

static void retry_later(void)
{
	LOG_INF("Retrying in %u s", backoff_s);
	k_work_reschedule_for_queue(&wq, &connect_work, K_SECONDS(backoff_s));
	backoff_s = MIN(backoff_s * 2, BACKOFF_MAX_S);
}

/* Ask the interface for SSID and signal strength. */
static void update_link_info(void)
{
	struct wifi_iface_status st = {0};

	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &st, sizeof(st)) == 0 &&
	    st.state >= WIFI_STATE_ASSOCIATED) {
		strncpy(state.ssid, st.ssid, sizeof(state.ssid) - 1);
		state.rssi = (int8_t)CLAMP(st.rssi, INT8_MIN, 0);
	}
}

/* ---- Work items --------------------------------------------------------------- */

static void connect_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (state.state >= NET_STATE_CONNECTED || in_flight) {
		return;
	}
	if (wifi_credentials_is_empty()) {
		set_state(NET_STATE_IDLE);
		k_work_reschedule_for_queue(&wq, &connect_work, CRED_POLL);
		return;
	}

	set_state(NET_STATE_CONNECTING);
	attempts++;

	/* Tries the stored networks; the result arrives as an event. */
	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0);

	if (ret == 0 || ret == -EINPROGRESS) {
		in_flight = true;
	} else if (ret != -EALREADY) {
		LOG_WRN("Connect request failed (%d)", ret);
		retry_later();
	}
}

static void event_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	uint32_t ev = (uint32_t)atomic_clear(&pending);

	if (ev & (BIT(EV_CONNECT_FAILED) | BIT(EV_CONNECTED) | BIT(EV_DISCONNECTED))) {
		in_flight = false;
	}
	if (ev & BIT(EV_CONNECT_FAILED)) {
		set_state(NET_STATE_CONNECTING);
		retry_later();
	}
	if (ev & BIT(EV_CONNECTED)) {
		backoff_s = BACKOFF_MIN_S;
		update_link_info();
		LOG_INF("Joined '%s' (%d dBm), waiting for an address", state.ssid, state.rssi);
		set_state(NET_STATE_CONNECTED);
	}
	if (ev & BIT(EV_IP_ADDED)) {
		struct net_in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

		if (addr != NULL) {
			net_addr_ntop(NET_AF_INET, addr, state.ipv4, sizeof(state.ipv4));
		}
		update_link_info();
		LOG_INF("Online: http://%s/ (also http://%s.local/)", state.ipv4,
			CONFIG_NET_HOSTNAME);
		state.state = NET_STATE_ONLINE;
		publish();
		k_work_reschedule_for_queue(&wq, &sntp_work, K_NO_WAIT);
		k_work_reschedule_for_queue(&wq, &rssi_work, RSSI_PERIOD);
	}
	if (ev & BIT(EV_IP_REMOVED) && state.state == NET_STATE_ONLINE) {
		set_state(NET_STATE_CONNECTED);
	}
	if (ev & BIT(EV_DISCONNECTED)) {
		k_work_cancel_delayable(&rssi_work);
		set_state(NET_STATE_CONNECTING);
		retry_later();
	}
}

static void rssi_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (state.state < NET_STATE_CONNECTED) {
		return;
	}

	int8_t before = state.rssi;

	update_link_info();
	if (state.rssi != before) {
		publish();
	}
	k_work_reschedule_for_queue(&wq, &rssi_work, RSSI_PERIOD);
}

static void sntp_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (state.state != NET_STATE_ONLINE) {
		return;
	}

	struct sntp_time ts;
	int ret = sntp_simple(SNTP_SERVER, SNTP_TIMEOUT_MS, &ts);

	if (ret != 0) {
		LOG_WRN("SNTP from %s failed (%d)", SNTP_SERVER, ret);
		k_work_reschedule_for_queue(&wq, &sntp_work, SNTP_RETRY);
		return;
	}

	char now[32];

	app_time_set_unix_ms((int64_t)ts.seconds * 1000 +
			     (int64_t)(((uint64_t)ts.fraction * 1000) >> 32));
	app_time_format(k_uptime_get(), now, sizeof(now));
	LOG_INF("Clock set from %s: %s", SNTP_SERVER, now);
	k_work_reschedule_for_queue(&wq, &sntp_work, SNTP_RESYNC);
}

/* ---- net_mgmt events (net_mgmt thread: record and hand over) ------------------- */

static void net_event_cb(struct net_mgmt_event_callback *cb, uint64_t event,
			 struct net_if *ev_iface)
{
	ARG_UNUSED(ev_iface);

	switch (event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *st = cb->info;

		atomic_set_bit(&pending, st != NULL && st->status != 0 ? EV_CONNECT_FAILED
								       : EV_CONNECTED);
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		atomic_set_bit(&pending, EV_DISCONNECTED);
		break;
	case NET_EVENT_IPV4_ADDR_ADD:
		atomic_set_bit(&pending, EV_IP_ADDED);
		break;
	case NET_EVENT_IPV4_ADDR_DEL:
		atomic_set_bit(&pending, EV_IP_REMOVED);
		break;
	default:
		return;
	}
	k_work_submit_to_queue(&wq, &event_work);
}

int wifi_mgr_start(void)
{
	static const struct k_work_queue_config cfg = {.name = "wifi_mgr"};

	iface = net_if_get_first_wifi();
	if (iface == NULL) {
		LOG_ERR("No Wi-Fi interface");
		return -ENODEV;
	}

	k_work_queue_start(&wq, wq_stack, K_THREAD_STACK_SIZEOF(wq_stack), WQ_PRIORITY, &cfg);

	net_mgmt_init_event_callback(&wifi_cb, net_event_cb, EVENTS_WIFI);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, net_event_cb, EVENTS_IPV4);
	net_mgmt_add_event_callback(&ipv4_cb);

	/* DHCP runs from now on and restarts by itself whenever the link comes back. */
	net_dhcpv4_start(iface);

	publish();
	k_work_reschedule_for_queue(&wq, &connect_work, K_NO_WAIT);
	return 0;
}

/* ---- Shell: "wifimgr" ------------------------------------------------------------- */

static int cmd_wifimgr(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct net_msg m;
	char now[32];

	(void)zbus_chan_read(&net_chan, &m, K_MSEC(100));
	shell_print(sh, "State %s, SSID '%s', RSSI %d dBm, IPv4 %s, %u connect attempts",
		    state_names[m.state], m.ssid, m.rssi, m.ipv4[0] ? m.ipv4 : "-", attempts);
	if (iface != NULL) {
		static const char *const oper[] = {"unknown", "notpresent", "down",
						   "lowerlayerdown", "testing", "dormant", "up"};
		enum net_if_oper_state os = net_if_oper_state(iface);

		shell_print(sh, "Interface: oper %s, admin %s, carrier %s, dormant %s",
			    os < ARRAY_SIZE(oper) ? oper[os] : "?",
			    net_if_is_admin_up(iface) ? "up" : "down",
			    net_if_is_carrier_ok(iface) ? "on" : "off",
			    net_if_is_dormant(iface) ? "yes" : "no");
	}
	app_time_format(k_uptime_get(), now, sizeof(now));
	shell_print(sh, "Clock %s (%s)", now, app_time_valid() ? "from SNTP" : "not set");
	if (m.state == NET_STATE_IDLE) {
		shell_print(sh, "No network stored. Add one with:\n"
				"  wifimgr add \"<SSID>\" \"<passphrase>\"");
	}
	return 0;
}

/*
 * Decode \xHH and \\ escapes. The shell only accepts printable ASCII, but SSIDs
 * are bytes: an iPhone's default "Name’s iPhone" has a U+2019 apostrophe,
 * typed as "Name\xe2\x80\x99s iPhone".
 */
static int unescape(const char *in, char *out, size_t out_len)
{
	size_t n = 0;

	while (*in != '\0' && n < out_len) {
		if (in[0] == '\\' && in[1] == 'x' && isxdigit((unsigned char)in[2]) &&
		    isxdigit((unsigned char)in[3])) {
			char hex[3] = {in[2], in[3], '\0'};

			out[n++] = (char)strtoul(hex, NULL, 16);
			in += 4;
		} else if (in[0] == '\\' && in[1] == '\\') {
			out[n++] = '\\';
			in += 2;
		} else {
			out[n++] = *in++;
		}
	}
	return *in == '\0' ? (int)n : -ENAMETOOLONG;
}

static int cmd_wifimgr_add(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	char ssid[WIFI_SSID_MAX_LEN];
	int ssid_len = unescape(argv[1], ssid, sizeof(ssid));
	size_t psk_len = strlen(argv[2]);

	if (ssid_len <= 0) {
		shell_error(sh, "SSID must be 1..%d bytes", WIFI_SSID_MAX_LEN);
		return -EINVAL;
	}
	if (psk_len < WIFI_PSK_MIN_LEN || psk_len > WIFI_PSK_MAX_LEN) {
		shell_error(sh, "WPA2 passphrase must be %d..%d characters", WIFI_PSK_MIN_LEN,
			    WIFI_PSK_MAX_LEN);
		return -EINVAL;
	}

	int ret = wifi_credentials_set_personal(ssid, ssid_len, WIFI_SECURITY_TYPE_PSK, NULL, 0,
						argv[2], psk_len, 0, 0, 0);

	if (ret != 0) {
		shell_error(sh, "Storing the network failed (%d); 'wifi cred list' / "
				"'wifi cred delete' to make room", ret);
		return ret;
	}
	shell_print(sh, "Stored a %d-byte SSID; connecting", ssid_len);
	/* Try now (unless a request is already out), not at the next back-off step. */
	k_work_reschedule_for_queue(&wq, &connect_work, K_NO_WAIT);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(wifimgr_cmds,
	SHELL_CMD_ARG(add, NULL,
		      "Store a WPA2 network and connect: wifimgr add \"<SSID>\" \"<passphrase>\"\n"
		      "Non-ASCII SSID bytes as \\xHH, e.g. \"Ann\\xe2\\x80\\x99s iPhone\"",
		      cmd_wifimgr_add, 3, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(wifimgr, &wifimgr_cmds, "Wi-Fi connection status", cmd_wifimgr);

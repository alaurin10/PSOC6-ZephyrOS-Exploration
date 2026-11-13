/*
 * Web dashboard: Zephyr's HTTP server on port 80 with
 *
 *   /             index.html, app.js, style.css (gzipped into the firmware)
 *   /api/status   JSON snapshot of every channel
 *   /ws           WebSocket: live data out, commands in
 *
 * and a DNS-SD record, so the mDNS responder answers for http://psoc6.local/.
 *
 * Getting data out: a zbus listener on the data channels only marks a
 * channel dirty and wakes the web thread through an eventfd, so a publisher
 * (e.g. the audio thread) never waits for the network. The thread then reads
 * the latest message of each dirty channel and sends it to every client.
 * Spectrum frames go out with a zero timeout: a client that can't keep up
 * just misses frames. State messages get a short timeout; a client that
 * still can't take them is dropped (the page reconnects and gets a fresh
 * snapshot). While the UI is frozen, no spectrum frames are sent at all.
 *
 * Commands in: JSON from the browser is checked against an allow-list and
 * forwarded on ctrl_chan, like CapSense and shell commands; ui_ctrl checks
 * the values and owns the state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dns_sd.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/websocket.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zvfs/eventfd.h>

#include "app_time.h"
#include "app_watchdog.h"
#include "audio_capture.h"
#include "channels.h"
#include "datalog.h"
#include "ui_ctrl.h"
#include "web_server.h"

LOG_MODULE_REGISTER(web, LOG_LEVEL_INF);

#define WEB_PORT          80
#define WS_MAX_CLIENTS    3
#define WEB_PRIORITY      9
#define WEB_STACK_SIZE    4096
#define POLL_MS           (APP_WDT_CHECK_MS / 2)
#define STATE_TIMEOUT_MS  200
#define KEEPALIVE_MS      2000
#define TX_BUF_SIZE       1024
#define CMD_MAX_LEN       127 /* longest command a browser may send */
#define RX_MAX_PER_POLL   32  /* more messages than this in one poll is a flood */

BUILD_ASSERT(WS_MAX_CLIENTS < CONFIG_WEBSOCKET_MAX_CONTEXTS,
	     "CONFIG_WEBSOCKET_MAX_CONTEXTS needs one context per client plus one to refuse");

/* ---- Static files (generated from web/ by CMake) ---------------------------------- */

static const uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};
static const uint8_t app_js_gz[] = {
#include "app.js.gz.inc"
};
static const uint8_t style_css_gz[] = {
#include "style.css.gz.inc"
};

#define STATIC_GZ(_name, _data, _type)                                                             \
	static struct http_resource_detail_static _name = {                                        \
		.common = {                                                                        \
			.type = HTTP_RESOURCE_TYPE_STATIC,                                         \
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),                        \
			.content_encoding = "gzip",                                                \
			.content_type = _type,                                                     \
		},                                                                                 \
		.static_data = _data,                                                              \
		.static_data_len = sizeof(_data),                                                  \
	}

STATIC_GZ(index_detail, index_html_gz, "text/html");
STATIC_GZ(app_js_detail, app_js_gz, "text/javascript");
STATIC_GZ(style_css_detail, style_css_gz, "text/css");

/* ---- JSON helpers -------------------------------------------------------------------- */

/* Append a string with JSON escaping (SSIDs can contain anything). */
static int json_str(char *buf, size_t len, const char *s)
{
	int n = snprintk(buf, len, "\"");

	for (; *s != '\0' && n < (int)len - 8; s++) {
		if (*s == '"' || *s == '\\') {
			n += snprintk(buf + n, len - n, "\\%c", *s);
		} else if ((uint8_t)*s < 0x20) {
			n += snprintk(buf + n, len - n, "\\u%04x", *s);
		} else {
			buf[n++] = *s;
		}
	}
	n += snprintk(buf + n, len - n, "\"");
	return n;
}

static int fmt_temp(char *buf, size_t len)
{
	struct temp_msg m;

	(void)zbus_chan_read(&temp_chan, &m, K_MSEC(50));
	return snprintk(buf, len, "{\"t\":\"temp\",\"n\":%u,\"c\":%d}", m.seq, m.centi_c);
}

static int fmt_ui(char *buf, size_t len)
{
	struct ui_msg m;

	(void)zbus_chan_read(&ui_chan, &m, K_MSEC(50));
	return snprintk(buf, len,
			"{\"t\":\"ui\",\"n\":%u,\"view\":\"%s\",\"colour\":%u,\"freeze\":%u,"
			"\"ledclap\":%u,\"slider\":\"%s\",\"param\":{\"%s\":%u,\"%s\":%u,\"%s\":%u},"
			"\"src\":\"%s\"}",
			m.seq, ui_view_names[m.view % UI_VIEW_COUNT], m.colour, m.freeze,
			m.led_on_clap, ui_param_names[m.slider_param % UI_PARAM_COUNT],
			ui_param_names[0], m.param[0], ui_param_names[1], m.param[1],
			ui_param_names[2], m.param[2],
			ctrl_source_names[m.source % CTRL_SRC_COUNT]);
}

static int fmt_input(char *buf, size_t len)
{
	struct input_msg m;

	(void)zbus_chan_read(&input_chan, &m, K_MSEC(50));
	return snprintk(buf, len, "{\"t\":\"in\",\"n\":%u,\"b0\":%u,\"b1\":%u,\"s\":%d,\"max\":%d}",
			m.seq, m.btn0, m.btn1, m.slider, INPUT_SLIDER_MAX);
}

static int fmt_net(char *buf, size_t len)
{
	struct net_msg m;
	int n;

	(void)zbus_chan_read(&net_chan, &m, K_MSEC(50));
	n = snprintk(buf, len, "{\"t\":\"net\",\"state\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"ssid\":",
		     net_state_names[m.state % NET_STATE_COUNT], m.rssi, m.ipv4);
	n += json_str(buf + n, len - n, m.ssid);
	n += snprintk(buf + n, len - n, "}");
	return n;
}

static int fmt_event(char *buf, size_t len)
{
	struct audio_event_msg m;

	(void)zbus_chan_read(&audio_event_chan, &m, K_MSEC(50));
	if (m.seq == 0) {
		return 0;
	}
	return snprintk(buf, len, "{\"t\":\"ev\",\"n\":%u,\"e\":\"%s\",\"p\":%d}", m.seq,
			audio_event_names[m.type % AUDIO_EVT_COUNT], m.peak_cdb);
}

/* Spectrum frame, with the latest level: the browser's main animation tick. */
static int fmt_frame(char *buf, size_t len)
{
	struct audio_spectrum_msg s;
	struct audio_level_msg l;
	int n;

	(void)zbus_chan_read(&audio_spectrum_chan, &s, K_MSEC(50));
	(void)zbus_chan_read(&audio_level_chan, &l, K_MSEC(50));
	n = snprintk(buf, len, "{\"t\":\"f\",\"n\":%u,\"r\":%d,\"p\":%d,\"c\":%u,\"b\":[", s.seq,
		     l.rms_cdb, l.peak_cdb, l.clipped);
	for (int i = 0; i < s.n_bands && i < AUDIO_BANDS; i++) {
		n += snprintk(buf + n, len - n, i ? ",%u" : "%u", s.band_db[i]);
	}
	n += snprintk(buf + n, len - n, "]}");
	return n;
}

/* Sent once to each new client: what the data means, then the current state. */
static int fmt_hello(char *buf, size_t len)
{
	struct audio_spectrum_msg s;
	int n;

	(void)zbus_chan_read(&audio_spectrum_chan, &s, K_MSEC(50));
	n = snprintk(buf, len,
		     "{\"t\":\"hello\",\"host\":\"%s\",\"rate\":%u,\"fft\":%u,\"floor\":%d,"
		     "\"colours\":%d,\"views\":[\"%s\",\"%s\",\"%s\",\"%s\"],"
		     "\"params\":[\"%s\",\"%s\",\"%s\"],\"bands\":[",
		     CONFIG_NET_HOSTNAME, s.sample_rate_hz, s.fft_size, AUDIO_BAND_DB_MIN,
		     UI_COLOUR_COUNT, ui_view_names[0], ui_view_names[1], ui_view_names[2],
		     ui_view_names[3], ui_param_names[0], ui_param_names[1], ui_param_names[2]);
	for (int i = 0; i < AUDIO_BANDS; i++) {
		n += snprintk(buf + n, len - n, i ? ",%d" : "%d",
			      (int)audio_capture_band_center_hz(i));
	}
	n += snprintk(buf + n, len - n, "]}");
	return n;
}

/* ---- WebSocket clients ------------------------------------------------------------- */

struct ws_client {
	int sock;            /* -1 = free slot */
	bool greeted;
	uint32_t frames_sent;
	uint32_t frames_dropped;
	uint32_t commands;
	int64_t last_tx_ms;  /* uptime of the last message sent */
	uint8_t rx_len;      /* bytes of a command split over several reads */
	bool rx_skip;        /* discarding the rest of an oversized message */
	char rx[CMD_MAX_LEN + 1];
};

static struct ws_client clients[WS_MAX_CLIENTS] = {
	[0 ... WS_MAX_CLIENTS - 1] = {.sock = -1},
};
static K_MUTEX_DEFINE(clients_lock);
static int wake_fd = -1;
static atomic_t accepted;
static atomic_t rejected_cmds;

static void wake(void)
{
	if (wake_fd >= 0) {
		(void)zvfs_eventfd_write(wake_fd, 1);
	}
}

/* Called by the HTTP server thread once a client has upgraded to WebSocket. */
static int ws_setup(int ws_socket, struct http_request_ctx *request_ctx, void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	int slot = -1;

	k_mutex_lock(&clients_lock, K_FOREVER);
	for (int i = 0; i < WS_MAX_CLIENTS; i++) {
		if (clients[i].sock < 0) {
			clients[i] = (struct ws_client){.sock = ws_socket};
			slot = i;
			break;
		}
	}
	k_mutex_unlock(&clients_lock);

	if (slot < 0) {
		/* The server has already answered 101, so refuse with a close frame:
		 * 1013 "Try Again Later" lets the page say why.
		 */
		static const uint8_t busy[] = {1013 >> 8, 1013 & 0xff, 'b', 'u', 's', 'y'};

		LOG_WRN("WebSocket refused: %d clients already", WS_MAX_CLIENTS);
		(void)websocket_send_msg(ws_socket, busy, sizeof(busy), WEBSOCKET_OPCODE_CLOSE,
					 false, true, STATE_TIMEOUT_MS);
		return -ENOENT; /* the server closes the connection */
	}
	atomic_inc(&accepted);
	LOG_INF("WebSocket client %d connected", slot);
	wake();
	return 0;
}

static void drop_client(struct ws_client *c, const char *why)
{
	LOG_INF("WebSocket client %d gone (%s)", (int)(c - clients), why);
	(void)websocket_unregister(c->sock); /* closes the TCP connection too */
	c->sock = -1;
}

/* Send one message; frames may be dropped, state must arrive. */
static bool send_to(struct ws_client *c, const char *msg, int len, bool droppable)
{
	int ret = websocket_send_msg(c->sock, (const uint8_t *)msg, len,
				     WEBSOCKET_OPCODE_DATA_TEXT, false, true,
				     droppable ? 0 : STATE_TIMEOUT_MS);

	if (ret >= 0) {
		c->frames_sent += droppable;
		c->last_tx_ms = k_uptime_get();
		return true;
	}
	if (droppable && (ret == -EAGAIN || ret == -ETIMEDOUT)) {
		c->frames_dropped++;
		return true;
	}
	drop_client(c, ret == -ETIMEDOUT ? "too slow" : "send failed");
	return false;
}

static char tx_buf[TX_BUF_SIZE];

static void broadcast(int len, bool droppable)
{
	if (len <= 0) {
		return;
	}
	if (len >= (int)sizeof(tx_buf)) {
		LOG_ERR("Message truncated (%d bytes)", len);
		return;
	}
	for (int i = 0; i < WS_MAX_CLIENTS; i++) {
		if (clients[i].sock >= 0 && clients[i].greeted) {
			(void)send_to(&clients[i], tx_buf, len, droppable);
		}
	}
}

static bool greet(struct ws_client *c)
{
	int (*const fmts[])(char *, size_t) = {fmt_hello, fmt_ui,  fmt_net,
					       fmt_temp,  fmt_input, fmt_frame};

	for (size_t i = 0; i < ARRAY_SIZE(fmts); i++) {
		int len = fmts[i](tx_buf, sizeof(tx_buf));

		if (len > 0 && !send_to(c, tx_buf, len, false)) {
			return false;
		}
	}
	c->greeted = true;
	return true;
}

/* ---- Commands from the browser ------------------------------------------------------ */

struct web_cmd {
	const char *cmd;
	const char *name;
	int32_t value;
};

static const struct json_obj_descr web_cmd_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct web_cmd, cmd, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct web_cmd, name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct web_cmd, value, JSON_TOK_NUMBER),
};

enum { HAS_CMD = BIT(0), HAS_NAME = BIT(1), HAS_VALUE = BIT(2) };

/*
 * {"cmd":"view","name":"bars"|"next"}     {"cmd":"colour","value":2} / "name":"next"
 * {"cmd":"param","name":"gain","value":70} {"cmd":"slider","name":"smooth"}
 * {"cmd":"freeze","value":1}               {"cmd":"ledclap","value":0}
 * Anything else is rejected; ui_ctrl range-checks the values.
 */
static bool parse_command(char *json, size_t len, struct ctrl_msg *out)
{
	struct web_cmd c = {0};
	int fields = json_obj_parse(json, len, web_cmd_descr, ARRAY_SIZE(web_cmd_descr), &c);

	if (fields < 0 || !(fields & HAS_CMD)) {
		return false;
	}

	bool next = (fields & HAS_NAME) && strcmp(c.name, "next") == 0;
	int idx;

	*out = (struct ctrl_msg){.source = CTRL_SRC_WEB, .value = c.value};

	if (strcmp(c.cmd, "view") == 0 && (fields & HAS_NAME)) {
		idx = ui_name_index(ui_view_names, UI_VIEW_COUNT, c.name);
		out->cmd = next ? CTRL_VIEW_NEXT : CTRL_VIEW_SET;
		out->value = idx;
		return next || idx >= 0;
	}
	if (strcmp(c.cmd, "colour") == 0 && (next || (fields & HAS_VALUE))) {
		out->cmd = next ? CTRL_COLOUR_NEXT : CTRL_COLOUR_SET;
		return true;
	}
	if (strcmp(c.cmd, "param") == 0 && (fields & HAS_NAME) && (fields & HAS_VALUE)) {
		idx = ui_name_index(ui_param_names, UI_PARAM_COUNT, c.name);
		out->cmd = CTRL_PARAM_SET;
		out->param = (uint8_t)idx;
		return idx >= 0;
	}
	if (strcmp(c.cmd, "slider") == 0 && (fields & HAS_NAME)) {
		idx = ui_name_index(ui_param_names, UI_PARAM_COUNT, c.name);
		out->cmd = CTRL_SLIDER_PARAM_SET;
		out->value = idx;
		return idx >= 0;
	}
	if (strcmp(c.cmd, "freeze") == 0 && (fields & HAS_VALUE)) {
		out->cmd = CTRL_FREEZE_SET;
		return true;
	}
	if (strcmp(c.cmd, "ledclap") == 0 && (fields & HAS_VALUE)) {
		out->cmd = CTRL_LED_CLAP_SET;
		return true;
	}
	return false;
}

static void handle_command(struct ws_client *c)
{
	struct ctrl_msg msg;

	c->rx[c->rx_len] = '\0';
	if (parse_command(c->rx, c->rx_len, &msg) &&
	    zbus_chan_pub(&ctrl_chan, &msg, K_MSEC(50)) == 0) {
		c->commands++;
	} else {
		atomic_inc(&rejected_cmds);
		LOG_WRN("Rejected command from client %d: %s", (int)(c - clients), c->rx);
	}
}

/* Read everything a client has sent. All WebSockets of a resource share one
 * receive buffer (the resource's data_buffer), so no bytes may be left in it
 * when moving on to the next client: read until the socket is empty.
 */
static void receive_from(struct ws_client *c)
{
	for (int n = 0; n < RX_MAX_PER_POLL; n++) {
		uint32_t type = 0;
		uint64_t remaining = 0;
		int ret = websocket_recv_msg(c->sock, (uint8_t *)c->rx + c->rx_len,
					     CMD_MAX_LEN - c->rx_len, &type, &remaining, 0);

		if (ret == -EAGAIN) {
			return;
		}
		if (ret < 0 || (type & WEBSOCKET_FLAG_CLOSE)) {
			drop_client(c, ret < 0 ? "receive failed" : "closed");
			return;
		}
		if (!(type & WEBSOCKET_FLAG_TEXT) || c->rx_skip ||
		    (remaining > 0 && c->rx_len + ret >= CMD_MAX_LEN)) {
			/* Only short text commands are expected; skip anything else. */
			c->rx_skip = remaining > 0;
			c->rx_len = 0;
			continue;
		}
		c->rx_len += ret;
		if (remaining == 0) {
			handle_command(c);
			c->rx_len = 0;
		}
	}
	drop_client(c, "flooding");
}

/* ---- zbus: mark dirty, wake the thread ------------------------------------------ */

enum { D_FRAME, D_TEMP, D_UI, D_INPUT, D_NET, D_EVENT, D_COUNT };
static atomic_t dirty;

static void web_listener_cb(const struct zbus_channel *chan)
{
	int bit = chan == &audio_spectrum_chan ? D_FRAME
		: chan == &temp_chan	       ? D_TEMP
		: chan == &ui_chan	       ? D_UI
		: chan == &input_chan	       ? D_INPUT
		: chan == &net_chan	       ? D_NET
		: chan == &audio_event_chan    ? D_EVENT
					       : -1;

	if (bit >= 0) {
		atomic_set_bit(&dirty, bit);
		wake();
	}
}

ZBUS_LISTENER_DEFINE(web_lis, web_listener_cb);
ZBUS_CHAN_ADD_OBS(audio_spectrum_chan, web_lis, 5);
ZBUS_CHAN_ADD_OBS(temp_chan, web_lis, 5);
ZBUS_CHAN_ADD_OBS(ui_chan, web_lis, 5);
ZBUS_CHAN_ADD_OBS(input_chan, web_lis, 5);
ZBUS_CHAN_ADD_OBS(net_chan, web_lis, 5);
ZBUS_CHAN_ADD_OBS(audio_event_chan, web_lis, 5);

/* ---- The web thread ----------------------------------------------------------------- */

static void send_dirty(void)
{
	static int (*const fmts[D_COUNT])(char *, size_t) = {
		[D_FRAME] = fmt_frame, [D_TEMP] = fmt_temp,   [D_UI] = fmt_ui,
		[D_INPUT] = fmt_input, [D_NET] = fmt_net,     [D_EVENT] = fmt_event,
	};
	uint32_t bits = (uint32_t)atomic_clear(&dirty);
	struct ui_msg ui;

	(void)zbus_chan_read(&ui_chan, &ui, K_MSEC(50));

	/* State first, so a frame is drawn with the settings it was made with. */
	for (int d = D_COUNT - 1; d >= 0; d--) {
		if (!(bits & BIT(d)) || (d == D_FRAME && ui.freeze)) {
			continue;
		}
		broadcast(fmts[d](tx_buf, sizeof(tx_buf)), d == D_FRAME);
	}
}

/* Pages treat a few seconds of silence as a dead connection (one the board
 * dropped while its Wi-Fi was down looks open to the browser forever), so
 * a frozen display still gets a message every KEEPALIVE_MS.
 */
static void send_keepalives(void)
{
	static const char ka[] = "{\"t\":\"ka\"}";
	int64_t now = k_uptime_get();

	for (int i = 0; i < WS_MAX_CLIENTS; i++) {
		struct ws_client *c = &clients[i];

		if (c->sock >= 0 && c->greeted && now - c->last_tx_ms >= KEEPALIVE_MS) {
			(void)send_to(c, ka, sizeof(ka) - 1, true);
		}
	}
}

static void web_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct zsock_pollfd fds[1 + WS_MAX_CLIENTS];
	struct ws_client *owner[1 + WS_MAX_CLIENTS];

	app_watchdog_enable(WDT_CLIENT_WEB);

	while (true) {
		app_watchdog_alive(WDT_CLIENT_WEB);

		int nfds = 0;

		fds[nfds].fd = wake_fd;
		fds[nfds].events = ZSOCK_POLLIN;
		owner[nfds++] = NULL;

		k_mutex_lock(&clients_lock, K_FOREVER);
		for (int i = 0; i < WS_MAX_CLIENTS; i++) {
			if (clients[i].sock >= 0) {
				fds[nfds].fd = clients[i].sock;
				fds[nfds].events = ZSOCK_POLLIN;
				owner[nfds++] = &clients[i];
			}
		}
		k_mutex_unlock(&clients_lock);

		if (zsock_poll(fds, nfds, POLL_MS) < 0) {
			k_msleep(POLL_MS);
			continue;
		}

		k_mutex_lock(&clients_lock, K_FOREVER);
		if (fds[0].revents & ZSOCK_POLLIN) {
			zvfs_eventfd_t v;

			(void)zvfs_eventfd_read(wake_fd, &v);
		}
		for (int i = 1; i < nfds; i++) {
			struct ws_client *c = owner[i];

			if (c->sock != fds[i].fd) {
				continue; /* slot changed meanwhile */
			}
			if (fds[i].revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR | ZSOCK_POLLNVAL)) {
				drop_client(c, "hung up");
			} else if (fds[i].revents & ZSOCK_POLLIN) {
				receive_from(c);
			}
		}
		for (int i = 0; i < WS_MAX_CLIENTS; i++) {
			if (clients[i].sock >= 0 && !clients[i].greeted) {
				(void)greet(&clients[i]);
			}
		}
		send_dirty();
		send_keepalives();
		k_mutex_unlock(&clients_lock);
	}
}

K_THREAD_DEFINE(web_tid, WEB_STACK_SIZE, web_thread, NULL, NULL, NULL, WEB_PRIORITY, 0,
		SYS_FOREVER_MS);

/* ---- /api/status ------------------------------------------------------------------- */

static int status_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	static char body[TX_BUF_SIZE];
	static const struct {
		const char *key;
		int (*fmt)(char *, size_t);
	} parts[] = {
		{"ui", fmt_ui}, {"net", fmt_net}, {"temp", fmt_temp}, {"in", fmt_input},
		{"frame", fmt_frame},
	};
	char now[32];
	int n;

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	/* {"uptime_ms":..,"time":"..","ui":{..},"net":{..},...}: the same objects the
	 * WebSocket sends.
	 */
	app_time_format(k_uptime_get(), now, sizeof(now));
	n = snprintk(body, sizeof(body), "{\"uptime_ms\":%lld,\"time\":\"%s\"", k_uptime_get(),
		     now);
	for (size_t i = 0; i < ARRAY_SIZE(parts); i++) {
		n += snprintk(body + n, sizeof(body) - n, ",\"%s\":", parts[i].key);
		n += parts[i].fmt(body + n, sizeof(body) - n);
	}
	n += snprintk(body + n, sizeof(body) - n, "}");

	response_ctx->body = (const uint8_t *)body;
	response_ctx->body_len = MIN(n, (int)sizeof(body) - 1);
	response_ctx->final_chunk = true;
	return 0;
}

static struct http_resource_detail_dynamic status_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "application/json",
	},
	.cb = status_handler,
};

/* ---- /api/logs and /api/log: the data log ------------------------------------------ */

/* Value of `key` in a query string like "?file=log00001.csv&tail=10". */
static bool query_param(const uint8_t *q, size_t len, const char *key, char *out, size_t out_len)
{
	size_t key_len = strlen(key);
	size_t i = (len > 0 && q[0] == '?') ? 1 : 0;

	while (i < len) {
		size_t end = i;

		while (end < len && q[end] != '&') {
			end++;
		}
		if (end - i > key_len && memcmp(q + i, key, key_len) == 0 && q[i + key_len] == '=') {
			size_t n = MIN(end - i - key_len - 1, out_len - 1);

			memcpy(out, q + i + key_len + 1, n);
			out[n] = '\0';
			return true;
		}
		i = end + 1;
	}
	return false;
}

/* {"mounted":true,"current":"log00003.csv","free_kb":..,"total_kb":..,"sync_s":10,
 *  "files":[{"name":"log00001.csv","size":1048620},...]}
 */
static int logs_handler(struct http_client_ctx *client, enum http_transaction_status status,
			const struct http_request_ctx *request_ctx,
			struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	static char body[768];
	struct datalog_file files[CONFIG_APP_DATALOG_FILES + 2];
	struct datalog_stats st;
	int n_files;
	int n;

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	datalog_get_stats(&st);
	n_files = datalog_list(files, ARRAY_SIZE(files));
	n = snprintk(body, sizeof(body),
		     "{\"mounted\":%s,\"current\":\"%s\",\"free_kb\":%u,\"total_kb\":%u,"
		     "\"sync_s\":%d,\"files\":[",
		     st.mounted ? "true" : "false", st.current, (uint32_t)(st.free_bytes / 1024),
		     (uint32_t)(st.total_bytes / 1024), CONFIG_APP_DATALOG_SYNC_S);
	for (int i = 0; i < n_files; i++) {
		n += snprintk(body + n, sizeof(body) - n, "%s{\"name\":\"%s\",\"size\":%u}",
			      i ? "," : "", files[i].name, files[i].size);
	}
	n += snprintk(body + n, sizeof(body) - n, "]}");

	response_ctx->body = (const uint8_t *)body;
	response_ctx->body_len = MIN(n, (int)sizeof(body) - 1);
	response_ctx->final_chunk = true;
	return 0;
}

/*
 * GET /api/log?file=log00001.csv: the whole file, streamed in chunks.
 * GET /api/log?tail=N: the last N lines of the file being written.
 * The server lets one client at a time use a dynamic resource, so one set
 * of statics is enough.
 */
static struct fs_file_t dl_file;
static bool dl_open;
static char dl_buf[1536];

static void dl_close(void)
{
	if (dl_open) {
		fs_close(&dl_file);
		dl_open = false;
	}
}

static int log_reply(struct http_response_ctx *rsp, enum http_status code, const char *text)
{
	rsp->status = code;
	rsp->body = (const uint8_t *)text;
	rsp->body_len = strlen(text);
	rsp->final_chunk = true;
	return 0;
}

static int log_handler(struct http_client_ctx *client, enum http_transaction_status status,
		       const struct http_request_ctx *request_ctx,
		       struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	ssize_t n;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		dl_close();
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (!dl_open) {
		char name[DATALOG_NAME_LEN] = "";
		char path[32];
		char tail[8];
		struct datalog_stats st;

		if (query_param(request_ctx->data, request_ctx->data_len, "tail", tail,
				sizeof(tail))) {
			int len = datalog_tail(dl_buf, sizeof(dl_buf), CLAMP(atoi(tail), 1, 40),
					       false);

			if (len < 0) {
				return log_reply(response_ctx, HTTP_503_SERVICE_UNAVAILABLE,
						 "The data log isn't available\n");
			}
			response_ctx->body = (const uint8_t *)dl_buf;
			response_ctx->body_len = len;
			response_ctx->final_chunk = true;
			return 0;
		}

		(void)query_param(request_ctx->data, request_ctx->data_len, "file", name,
				  sizeof(name));
		if (datalog_path(name, path, sizeof(path)) < 0) {
			return log_reply(response_ctx, HTTP_404_NOT_FOUND,
					 "Use /api/log?file=logNNNNN.csv (see /api/logs)\n");
		}
		/* The file being written: include everything logged so far. */
		datalog_get_stats(&st);
		if (strcmp(name, st.current) == 0) {
			(void)datalog_flush(5000);
		}
		fs_file_t_init(&dl_file);
		if (fs_open(&dl_file, path, FS_O_READ) < 0) {
			return log_reply(response_ctx, HTTP_404_NOT_FOUND, "No such log file\n");
		}
		dl_open = true;
	}

	n = fs_read(&dl_file, dl_buf, sizeof(dl_buf));
	response_ctx->body = (const uint8_t *)dl_buf;
	response_ctx->body_len = MAX(n, 0);
	response_ctx->final_chunk = n < (ssize_t)sizeof(dl_buf);
	if (response_ctx->final_chunk) {
		dl_close();
	}
	return 0;
}

static struct http_resource_detail_dynamic logs_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "application/json",
	},
	.cb = logs_handler,
};

static struct http_resource_detail_dynamic log_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "text/csv",
	},
	.cb = log_handler,
};

/* ---- /api/rec.wav: the raw recording (CONFIG_APP_AUDIO_REC_SECONDS > 0) ------------- */

#if CONFIG_APP_AUDIO_REC_SECONDS > 0
#define WAV_HDR_LEN 44
#define WAV_CHUNK   4096

static void put_le(uint8_t *p, uint32_t v, int bytes)
{
	for (int i = 0; i < bytes; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static int rec_handler(struct http_client_ctx *client, enum http_transaction_status status,
		       const struct http_request_ctx *request_ctx,
		       struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	static uint8_t hdr[WAV_HDR_LEN];
	static size_t sent; /* PCM bytes sent so far */
	static bool started;
	size_t n;
	const int16_t *pcm;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		started = false;
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	pcm = audio_rec_get(&n);
	if (pcm == NULL) {
		static const char msg[] = "No finished recording: run \"mic rec\" first\n";

		response_ctx->status = HTTP_404_NOT_FOUND;
		response_ctx->body = (const uint8_t *)msg;
		response_ctx->body_len = sizeof(msg) - 1;
		response_ctx->final_chunk = true;
		return 0;
	}

	if (!started) {
		uint32_t bytes = n * sizeof(int16_t);
		uint32_t rate = CONFIG_APP_AUDIO_SAMPLE_RATE;

		/* RIFF/WAVE, PCM, mono, 16 bit, little endian like the samples. */
		memcpy(&hdr[0], "RIFF", 4);
		put_le(&hdr[4], 36 + bytes, 4);
		memcpy(&hdr[8], "WAVEfmt ", 8);
		put_le(&hdr[16], 16, 4);
		put_le(&hdr[20], 1, 2);
		put_le(&hdr[22], 1, 2);
		put_le(&hdr[24], rate, 4);
		put_le(&hdr[28], rate * 2, 4);
		put_le(&hdr[32], 2, 2);
		put_le(&hdr[34], 16, 2);
		memcpy(&hdr[36], "data", 4);
		put_le(&hdr[40], bytes, 4);
		started = true;
		sent = 0;
		response_ctx->body = hdr;
		response_ctx->body_len = sizeof(hdr);
		return 0;
	}

	size_t len = MIN(n * sizeof(int16_t) - sent, WAV_CHUNK);

	response_ctx->body = (const uint8_t *)pcm + sent;
	response_ctx->body_len = len;
	sent += len;
	response_ctx->final_chunk = sent == n * sizeof(int16_t);
	return 0;
}

static struct http_resource_detail_dynamic rec_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "audio/wav",
	},
	.cb = rec_handler,
};
#endif

/* ---- Service and resources ------------------------------------------------------------- */

static uint8_t ws_buf[512];

static struct http_resource_detail_websocket ws_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_WEBSOCKET,
		/* Upgrades start as an HTTP/1.1 GET */
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = ws_setup,
	.data_buffer = ws_buf,
	.data_buffer_len = sizeof(ws_buf),
};

static uint16_t web_port = WEB_PORT;

HTTP_SERVICE_DEFINE(dash_service, NULL, &web_port, CONFIG_HTTP_SERVER_MAX_CLIENTS, 4, NULL, NULL,
		    NULL);
HTTP_RESOURCE_DEFINE(index_res, dash_service, "/", &index_detail);
HTTP_RESOURCE_DEFINE(app_js_res, dash_service, "/app.js", &app_js_detail);
HTTP_RESOURCE_DEFINE(style_css_res, dash_service, "/style.css", &style_css_detail);
HTTP_RESOURCE_DEFINE(status_res, dash_service, "/api/status", &status_detail);
HTTP_RESOURCE_DEFINE(logs_res, dash_service, "/api/logs", &logs_detail);
HTTP_RESOURCE_DEFINE(log_res, dash_service, "/api/log", &log_detail);
#if CONFIG_APP_AUDIO_REC_SECONDS > 0
HTTP_RESOURCE_DEFINE(rec_res, dash_service, "/api/rec.wav", &rec_detail);
#endif
HTTP_RESOURCE_DEFINE(ws_res, dash_service, "/ws", &ws_detail);

/* "psoc6._http._tcp.local" for service browsers; psoc6.local itself is the hostname. */
DNS_SD_REGISTER_TCP_SERVICE(dash_dns_sd, CONFIG_NET_HOSTNAME, "_http", "local", DNS_SD_EMPTY_TXT,
			    WEB_PORT);

int web_server_start(void)
{
	wake_fd = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
	if (wake_fd < 0) {
		LOG_ERR("eventfd failed (%d)", errno);
		return -errno;
	}

	int ret = http_server_start();

	if (ret < 0) {
		LOG_ERR("HTTP server start failed (%d)", ret);
		return ret;
	}
	k_thread_start(web_tid);
	LOG_INF("Web dashboard on port %d", WEB_PORT);
	return 0;
}

/* ---- Shell: "web" ------------------------------------------------------------------------ */

static int cmd_web(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct net_msg net;

	(void)zbus_chan_read(&net_chan, &net, K_MSEC(100));
	shell_print(sh, "http://%s/ (http://%s.local/), %u WebSocket connections so far, "
		    "%u commands rejected", net.ipv4[0] ? net.ipv4 : "<no address>",
		    CONFIG_NET_HOSTNAME, (uint32_t)atomic_get(&accepted),
		    (uint32_t)atomic_get(&rejected_cmds));

	k_mutex_lock(&clients_lock, K_FOREVER);
	for (int i = 0; i < WS_MAX_CLIENTS; i++) {
		const struct ws_client *c = &clients[i];

		if (c->sock >= 0) {
			shell_print(sh, "  client %d: %u frames sent, %u dropped, %u commands", i,
				    c->frames_sent, c->frames_dropped, c->commands);
		}
	}
	k_mutex_unlock(&clients_lock);
	return 0;
}

SHELL_CMD_REGISTER(web, NULL, "Web dashboard clients", cmd_web);

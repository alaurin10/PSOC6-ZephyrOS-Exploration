/*
 * BLE temperature beacon + GATT Environmental Sensing service on the CYW4343W
 * radio (Murata 1DX module).
 *
 * The PSoC 6 runs the Zephyr Bluetooth host; the CYW4343W is the controller,
 * connected over UART2 using the HCI H4 protocol. bt_enable() first downloads
 * the controller firmware patch (bt_firmware.hcd, embedded in our image).
 *
 * Advertising: connectable, legacy, ~100-150 ms interval, containing
 *   - Complete Local Name "PSoC6 21.34C" (readable straight from a scanner list)
 *   - Service Data for Environmental Sensing (0x181A): sint16 temperature in
 *     0.01 C, the same encoding as the GATT Temperature characteristic (0x2A6E)
 *
 * GATT (after connecting): Environmental Sensing service with a Temperature
 * characteristic (read + notify). Apps such as nRF Connect know this
 * standard characteristic and show it decoded in degrees C.
 *
 * Only one connection is allowed. Connectable advertising stops when a
 * central connects; this thread restarts it after the disconnect.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/zbus/zbus.h>

#include "channels.h"

LOG_MODULE_REGISTER(ble_beacon, LOG_LEVEL_INF);

#define BLE_PRIORITY   9
#define BLE_STACK_SIZE 2048
#define NAME_PREFIX    "PSoC6"
#define TEMP_UNKNOWN   ((int16_t)0x8000) /* "value not known" for 0x2A6E */

/* Service data payload: 16-bit UUID (little endian) followed by the value. */
static uint8_t svc_data[4] = {BT_UUID_16_ENCODE(BT_UUID_ESS_VAL), 0x00, 0x00};
/* Sized for the worst case the compiler can see; the real name is <= 13 chars,
 * well within the 31-byte legacy advertising payload.
 */
static char name[32];

/* Not const: the name length changes with every update. */
static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, name, 0),
	BT_DATA(BT_DATA_SVC_DATA16, svc_data, sizeof(svc_data)),
};

/* Connectable, from the public (identity) address so nRF Connect shows the same
 * device after every reboot. 0xa0-0xf0 units of 0.625 ms = 100-150 ms.
 */
static const struct bt_le_adv_param *const adv_param =
	BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
			BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

/*
 * Subscriber, not listener: bt_le_adv_update_data() sends HCI commands and
 * blocks for the controller's reply, which must not happen inside the
 * publisher (the system work queue). Starts disabled so its queue doesn't
 * fill up while bt_enable() is still downloading firmware.
 */
ZBUS_SUBSCRIBER_DEFINE_WITH_ENABLE(ble_sub, 2, false);
ZBUS_CHAN_ADD_OBS(temp_chan, ble_sub, 4);

static atomic_t adv_updates;
static atomic_t adv_running;
static atomic_t adv_centi = ATOMIC_INIT(INT32_MIN);
static atomic_t connected;
static atomic_t notify_enabled;
static atomic_t notifications_sent;

/* ---- GATT: Environmental Sensing service --------------------------------- */

/* Current Temperature characteristic value, little endian sint16, 0.01 C. */
static int16_t gatt_temp = TEMP_UNKNOWN;

static ssize_t read_temp(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	uint8_t le[2];

	sys_put_le16((uint16_t)gatt_temp, le);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, le, sizeof(le));
}

/* The phone wrote the CCC descriptor: notifications switched on or off. */
static void temp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	bool on = value == BT_GATT_CCC_NOTIFY;

	atomic_set(&notify_enabled, on);
	LOG_INF("Temperature notifications %s", on ? "enabled" : "disabled");
}

/*
 * Attribute table, in order:
 *   [0] primary service declaration (0x181A)
 *   [1] characteristic declaration
 *   [2] characteristic value (0x2A6E)   <- notified
 *   [3] Client Characteristic Configuration descriptor (enables notify)
 */
BT_GATT_SERVICE_DEFINE(ess_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_ESS),
	BT_GATT_CHARACTERISTIC(BT_UUID_TEMPERATURE, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_temp, NULL, NULL),
	BT_GATT_CCC(temp_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

#define TEMP_VALUE_ATTR (&ess_svc.attrs[2])

/* ---- Connection callbacks (run in the Bluetooth RX thread) --------------- */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (err != 0) {
		LOG_WRN("Connection from %s failed (0x%02x)", addr, err);
		atomic_set(&adv_running, 0);
		return;
	}
	/* Legacy connectable advertising stops once a central connects. */
	atomic_set(&adv_running, 0);
	atomic_set(&connected, 1);
	LOG_INF("Connected: %s", addr);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	atomic_set(&connected, 0);
	atomic_set(&notify_enabled, 0);
	LOG_INF("Disconnected (reason 0x%02x), advertising resumes with the next sample",
		reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* ---- Advertising ---------------------------------------------------------- */

static void fill_payload(int32_t centi_c, bool valid)
{
	int16_t v = valid ? (int16_t)CLAMP(centi_c, INT16_MIN + 1, INT16_MAX) : TEMP_UNKNOWN;

	sys_put_le16((uint16_t)v, &svc_data[2]);
	gatt_temp = v;
	if (valid) {
		snprintk(name, sizeof(name), NAME_PREFIX " " CENTI_FMT "C", CENTI_ARGS(centi_c));
	} else {
		snprintk(name, sizeof(name), NAME_PREFIX " --C");
	}
	ad[1].data_len = strlen(name);
	atomic_set(&adv_centi, valid ? centi_c : INT32_MIN);
}

/* Start advertising if it isn't running and nobody is connected. */
static void ensure_advertising(void)
{
	static int last_err;

	if (atomic_get(&adv_running) || atomic_get(&connected)) {
		return;
	}

	int err = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), NULL, 0);

	if (err == 0) {
		atomic_set(&adv_running, 1);
		LOG_INF("Advertising as \"%s\"", name);
	} else if (err != last_err) {
		/* e.g. -ENOMEM right after a disconnect, until the connection object is
		 * freed; the next sample retries.
		 */
		LOG_WRN("Advertising start failed (%d), will retry", err);
	}
	last_err = err;
}

static void ble_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const struct zbus_channel *chan;
	struct temp_msg m;
	int64_t t0 = k_uptime_get();

	/* Synchronous: resets the controller, downloads its firmware, reads its features. */
	int err = bt_enable(NULL);

	if (err != 0) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return;
	}
	LOG_INF("Bluetooth ready in %lld ms", k_uptime_get() - t0);

	/* Start with the latest reading, if there is one. */
	bool have = zbus_chan_read(&temp_chan, &m, K_MSEC(100)) == 0 && m.seq != 0;

	fill_payload(m.centi_c, have);
	ensure_advertising();

	bt_addr_le_t addr;
	size_t count = 1;
	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_id_get(&addr, &count);
	bt_addr_le_to_str(&addr, addr_str, sizeof(addr_str));
	LOG_INF("Bluetooth address %s", addr_str);

	/* Now start receiving temperature notifications. */
	zbus_obs_set_enable(&ble_sub, true);

	/* One pass per temperature sample (~1 s). */
	while (zbus_sub_wait(&ble_sub, &chan, K_FOREVER) == 0) {
		if (zbus_chan_read(&temp_chan, &m, K_MSEC(100)) != 0) {
			continue;
		}
		fill_payload(m.centi_c, true);

		if (atomic_get(&connected) && atomic_get(&notify_enabled)) {
			uint8_t le[2];

			sys_put_le16((uint16_t)gatt_temp, le);
			/* NULL conn = every connected peer that enabled notifications. */
			if (bt_gatt_notify(NULL, TEMP_VALUE_ATTR, le, sizeof(le)) == 0) {
				atomic_inc(&notifications_sent);
			}
		}

		if (atomic_get(&adv_running)) {
			err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
			if (err == 0) {
				atomic_inc(&adv_updates);
			} else {
				LOG_WRN("Advertising update failed (%d)", err);
			}
		} else {
			ensure_advertising();
		}
	}
}

K_THREAD_DEFINE(ble_tid, BLE_STACK_SIZE, ble_thread, NULL, NULL, NULL, BLE_PRIORITY, 0, 0);

static int cmd_ble(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	bt_addr_le_t addr;
	size_t count = 1;
	char addr_str[BT_ADDR_LE_STR_LEN];
	int32_t c = (int32_t)atomic_get(&adv_centi);

	bt_id_get(&addr, &count);
	if (count == 0) {
		shell_print(sh, "Bluetooth not ready (yet)");
		return 0;
	}
	bt_addr_le_to_str(&addr, addr_str, sizeof(addr_str));
	shell_print(sh, "Address %s: %s, %s", addr_str,
		    atomic_get(&adv_running) ? "advertising" : "not advertising",
		    atomic_get(&connected) ? "connected" : "no connection");
	shell_print(sh, "%u advertising updates, %u notifications (notify %s)",
		    (uint32_t)atomic_get(&adv_updates), (uint32_t)atomic_get(&notifications_sent),
		    atomic_get(&notify_enabled) ? "on" : "off");
	if (c != INT32_MIN) {
		shell_print(sh, "Payload: name \"" NAME_PREFIX " " CENTI_FMT "C\", ESS 0x181A "
			    "temp 0x%04x (%d x 0.01 C)", CENTI_ARGS(c), (uint16_t)(int16_t)c, c);
	}
	return 0;
}

SHELL_CMD_REGISTER(ble, NULL, "Show BLE beacon/GATT status", cmd_ble);

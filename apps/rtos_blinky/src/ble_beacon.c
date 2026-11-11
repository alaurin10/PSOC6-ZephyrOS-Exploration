/*
 * BLE temperature beacon on the CYW4343W radio (Murata 1DX module).
 *
 * The PSoC 6 runs the Zephyr Bluetooth host; the CYW4343W is the controller,
 * connected over UART2 using the HCI H4 protocol. bt_enable() first downloads
 * the controller firmware patch (bt_firmware.hcd, embedded in our image).
 *
 * Advertising: non-connectable, legacy, ~100-150 ms interval, containing
 *   - Complete Local Name "PSoC6 21.34C" (readable straight from a scanner list)
 *   - Service Data for Environmental Sensing (0x181A): sint16 temperature in
 *     0.01 C, the same encoding as the GATT Temperature characteristic (0x2A6E)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
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

static void fill_adv(int32_t centi_c, bool valid)
{
	if (valid) {
		int16_t v = (int16_t)CLAMP(centi_c, INT16_MIN, INT16_MAX);

		sys_put_le16((uint16_t)v, &svc_data[2]);
		snprintk(name, sizeof(name), NAME_PREFIX " " CENTI_FMT "C", CENTI_ARGS(centi_c));
	} else {
		/* 0x8000 is the "value not known" marker for the Temperature characteristic. */
		sys_put_le16(0x8000, &svc_data[2]);
		snprintk(name, sizeof(name), NAME_PREFIX " --C");
	}
	ad[1].data_len = strlen(name);
	atomic_set(&adv_centi, valid ? centi_c : INT32_MIN);
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

	fill_adv(m.centi_c, have);

	/* Use the controller's public (identity) address, so nRF Connect shows a
	 * stable address instead of a new random one every reboot.
	 */
	err = bt_le_adv_start(BT_LE_ADV_NCONN_IDENTITY, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err != 0) {
		LOG_ERR("Advertising failed to start (%d)", err);
		return;
	}

	bt_addr_le_t addr;
	size_t count = 1;
	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_id_get(&addr, &count);
	bt_addr_le_to_str(&addr, addr_str, sizeof(addr_str));
	LOG_INF("Advertising as \"%s\" from %s", name, addr_str);
	atomic_set(&adv_running, 1);

	/* Now start receiving temperature notifications. */
	zbus_obs_set_enable(&ble_sub, true);

	while (zbus_sub_wait(&ble_sub, &chan, K_FOREVER) == 0) {
		if (zbus_chan_read(&temp_chan, &m, K_MSEC(100)) != 0) {
			continue;
		}
		fill_adv(m.centi_c, true);
		err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
		if (err != 0) {
			LOG_WRN("Advertising update failed (%d)", err);
		} else {
			atomic_inc(&adv_updates);
		}
	}
}

K_THREAD_DEFINE(ble_tid, BLE_STACK_SIZE, ble_thread, NULL, NULL, NULL, BLE_PRIORITY, 0, 0);

static int cmd_ble(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!atomic_get(&adv_running)) {
		shell_print(sh, "Not advertising (yet)");
		return 0;
	}

	bt_addr_le_t addr;
	size_t count = 1;
	char addr_str[BT_ADDR_LE_STR_LEN];
	int32_t c = (int32_t)atomic_get(&adv_centi);

	bt_id_get(&addr, &count);
	bt_addr_le_to_str(&addr, addr_str, sizeof(addr_str));
	shell_print(sh, "Advertising from %s, %u updates", addr_str,
		    (uint32_t)atomic_get(&adv_updates));
	if (c != INT32_MIN) {
		shell_print(sh, "Payload: name \"" NAME_PREFIX " " CENTI_FMT "C\", ESS 0x181A "
			    "temp 0x%04x (%d x 0.01 C)", CENTI_ARGS(c), (uint16_t)(int16_t)c, c);
	}
	return 0;
}

SHELL_CMD_REGISTER(ble, NULL, "Show BLE beacon status", cmd_ble);

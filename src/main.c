/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
//#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>

#include <zephyr/bluetooth/bluetooth.h> // Core Bluetooth stack init/control
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h> // GATT 


#include <zephyr/settings/settings.h>


/* SELF ADDED INCLUDES*/
#include <zephyr/logging/log.h>
#include "sp_state.h"
#include "BLE_GATT/sp_prov.h"
#include "BLE_GATT/sp_oper.h"

/* SELF ADDED FUNCS*/
LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define DEVICE_NAME             CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN         (sizeof(DEVICE_NAME) - 1)

/* SELF ADDED DEFINES */

#define BT_UUID_SP_PROV_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

static struct k_work adv_work;

/* SELF ADDED */
static bool streaming_enabled = false;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      BT_UUID_SP_PROV_SERVICE_VAL),
};

/* Deferred work handler that safely restarts BLE advertising from Zephyr workqueue context. */
static void adv_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int err;

	err = bt_le_adv_stop();
	if (err && err != -EALREADY) {
		LOG_ERR("Advertising stop failed (err %d)", err);
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}

	LOG_INF("Advertising successfully started");
}

/* Queues advertising start work instead of starting advertising directly from callback context. */
static void advertising_start(void)
{
	k_work_submit(&adv_work);
}

/* BLE connection callback; gives both GATT services a reference to the active connection. */
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed, err 0x%02x %s", err, bt_hci_err_to_str(err));
		return;
	}

	LOG_INF("Connected");
	sp_prov_connected(conn);
	sp_oper_connected(conn);
}

/* BLE disconnection callback; clears service sessions, stops streaming, and drops auth state. */
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected, reason 0x%02x %s", reason, bt_hci_err_to_str(reason));

	sp_prov_disconnected(conn);
	sp_oper_disconnected(conn);

	streaming_enabled = false;

	if (sp_state_get() == SP_STATE_AUTHENTICATED) {
		sp_state_set_provisioned_idle();
	}
}

/* Called when Zephyr has fully recycled the connection object; advertising can safely restart. */
static void recycled_cb(void)
{
	LOG_INF("Connection object available from previous conn. Disconnect is complete");
	advertising_start();
}

/* Optional BLE security-level callback used when legacy/sample security config is enabled. */
#ifdef CONFIG_BT_LBS_SECURITY_ENABLED
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		LOG_INF("Security changed: %s level %u", addr, level);
	} else {
		LOG_ERR("Security failed: %s level %u err %d %s", addr, level, err,
		       bt_security_err_to_str(err));
	}
}
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.recycled         = recycled_cb,
#ifdef CONFIG_BT_LBS_SECURITY_ENABLED
	.security_changed = security_changed,
#endif
};

/* Optional pairing callback that displays a passkey during BLE authentication. */
#if defined(CONFIG_BT_LBS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Passkey for %s: %06u", addr, passkey);
}

/* Optional pairing callback called when the central cancels authentication. */
static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing cancelled: %s", addr);
}

/* Optional pairing callback called after pairing completes successfully. */
static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}

/* Optional pairing callback called when pairing fails. */
static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing failed conn: %s, reason %d %s", addr, reason,
	       bt_security_err_to_str(reason));
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};
#else
static struct bt_conn_auth_cb conn_auth_callbacks;
static struct bt_conn_auth_info_cb conn_auth_info_callbacks;
#endif

static void app_prov_rx_handler(const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0U) {
		LOG_WRN("Provisioning RX: empty payload");
		return;
	}

	LOG_INF("Provisioning RX cmd=0x%02x len=%u", data[0], len);

	switch (sp_state_get()) {
	case SP_STATE_FACTORY_NEW:
		switch (data[0]) {
		case SP_PROV_CMD_GET_CHALLENGE:
			LOG_INF("Provisioning: challenge requested");
			break;

		case SP_PROV_CMD_SEND_PROOF:
			if (sp_prov_is_authenticated()) {
				LOG_INF("Provisioning: proof accepted");
			} else {
				LOG_WRN("Provisioning: proof not accepted");
			}
			break;

		case SP_PROV_CMD_SET_BLOB:
			if (sp_prov_is_blob_staged()) {
				LOG_INF("Provisioning: blob staged");
			} else {
				LOG_WRN("Provisioning: blob not staged");
			}
			break;

		case SP_PROV_CMD_COMMIT:
			if (sp_prov_is_authenticated() && sp_prov_is_blob_staged()) {
				LOG_INF("Provisioning: commit accepted, entering PROVISIONED_IDLE");
				streaming_enabled = false;
				sp_state_set_provisioned_idle();
				sp_prov_reset_session();
			} else {
				LOG_WRN("Provisioning: commit attempted without auth/blob");
			}
			break;

		default:
			LOG_WRN("Unknown provisioning command: 0x%02x", data[0]);
			break;
		}
		break;

	case SP_STATE_PROVISIONED_IDLE:
		LOG_WRN("Provisioning RX blocked in PROVISIONED_IDLE");
		break;

	case SP_STATE_AUTHENTICATED:
		LOG_WRN("Provisioning RX blocked in AUTHENTICATED");
		break;

	default:
		LOG_ERR("Invalid state");
		break;
	}
}

static void app_oper_auth_rx_handler(const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0U) {
		LOG_WRN("Operational auth RX: empty payload");
		return;
	}

	LOG_INF("Operational auth RX cmd=0x%02x len=%u", data[0], len);

	switch (sp_state_get()) {
	case SP_STATE_PROVISIONED_IDLE:
		switch (data[0]) {
		case SP_OPER_CMD_AUTH_GET_CHALLENGE:
			LOG_INF("Operational auth: challenge requested");
			break;

		case SP_OPER_CMD_AUTH_SEND_PROOF:
			if (sp_oper_is_authenticated()) {
				LOG_INF("Operational auth: proof accepted, entering AUTHENTICATED");
				sp_state_set_authenticated();
			} else {
				LOG_WRN("Operational auth: proof not accepted");
			}
			break;

		case SP_OPER_CMD_AUTH_LOGOUT:
			LOG_INF("Operational auth: logout while idle");
			break;

		default:
			LOG_WRN("Unknown operational auth command: 0x%02x", data[0]);
			break;
		}
		break;

	case SP_STATE_AUTHENTICATED:
		if (data[0] == SP_OPER_CMD_AUTH_LOGOUT) {
			LOG_INF("Operational auth: logout, returning to PROVISIONED_IDLE");
			streaming_enabled = false;
			sp_oper_reset_session();
			sp_state_set_provisioned_idle();
		} else {
			LOG_WRN("Operational auth command 0x%02x ignored in AUTHENTICATED", data[0]);
		}
		break;

	case SP_STATE_FACTORY_NEW:
		LOG_WRN("Operational auth blocked in FACTORY_NEW");
		break;

	default:
		LOG_ERR("Invalid state");
		break;
	}
}

static void app_oper_cmd_rx_handler(const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0U) {
		LOG_WRN("Operational command RX: empty payload");
		return;
	}

	if (sp_state_get() != SP_STATE_AUTHENTICATED) {
		LOG_WRN("Blocked operational command 0x%02x outside AUTHENTICATED", data[0]);
		return;
	}

	switch (data[0]) {
	case 0x01:
		streaming_enabled = true;
		LOG_INF("Operational command: start streaming");
		break;

	case 0x00:
		streaming_enabled = false;
		LOG_INF("Operational command: stop streaming");
		break;

	default:
		LOG_WRN("Unknown operational command: 0x%02x", data[0]);
		break;
	}
}

int main(void)
{
	int err;
	uint32_t counter = 0;

	sp_state_init(); // state machine for security

	LOG_INF("Starting Secure Provisioning custom BLE sample");

	if (IS_ENABLED(CONFIG_BT_LBS_SECURITY_ENABLED)) {
		err = bt_conn_auth_cb_register(&conn_auth_callbacks);
		if (err) {
			LOG_ERR("Failed to register authorization CBs.");
			return 0;
		}

		err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
		if (err) {
			LOG_ERR("Failed to register authorization info CBs.");
			return 0;
		}
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}

	LOG_INF("Bluetooth initialized");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	err = sp_prov_init(app_prov_rx_handler);
	if (err) {
		LOG_ERR("Failed to init provisioning service (err:%d)", err);
		return 0;
	}

	err = sp_oper_init(app_oper_auth_rx_handler, app_oper_cmd_rx_handler);
	if (err) {
		LOG_ERR("Failed to init operational service (err:%d)", err);
		return 0;
	}

	k_work_init(&adv_work, adv_work_handler);
	advertising_start();

	for (;;) {
		if (streaming_enabled) {
			uint8_t payload[4];

			payload[0] = (counter >> 0) & 0xFF;
			payload[1] = (counter >> 8) & 0xFF;
			payload[2] = (counter >> 16) & 0xFF;
			payload[3] = (counter >> 24) & 0xFF;

			err = sp_oper_send_telemetry(payload, sizeof(payload));
			if (err == -ENOTCONN) {
				LOG_INF("No BLE connection");
			} else if (err == -EACCES) {
				LOG_INF("Telemetry notifications not enabled");
			} else if (err) {
				LOG_WRN("sp_oper_send_telemetry failed: %d", err);
			} else {
				LOG_INF("Sent telemetry counter=%u", counter);
				counter++;
			}
		}
		k_sleep(K_MSEC(1000));
	}
}


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

#define BT_UUID_SP_OPER_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x12345688, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

// static bool app_button_state;
static struct k_work adv_work;

/* SELF ADDED */
// static struct bt_conn *current_conn; // connection handle for better control
static bool streaming_enabled = false;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      BT_UUID_SP_PROV_SERVICE_VAL,
		      BT_UUID_SP_OPER_SERVICE_VAL),
};

static void adv_work_handler(struct k_work *work)
{
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

static void advertising_start(void)
{
	k_work_submit(&adv_work);
}

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

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected, reason 0x%02x %s", reason, bt_hci_err_to_str(reason));

	sp_prov_disconnected(conn);
	sp_oper_disconnected(conn);
}

static void recycled_cb(void)
{
	LOG_INF("Connection object available from previous conn. Disconnect is complete");
	advertising_start();
}

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

#if defined(CONFIG_BT_LBS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Passkey for %s: %06u", addr, passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing cancelled: %s", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}

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

static bool is_operational_command(uint8_t cmd)
{
    return (cmd == 0x00) || (cmd == 0x01);
}

static bool is_provisioning_command(uint8_t cmd)
{
    return (cmd >= 0x10 && cmd <= 0x1F);
}

static bool is_auth_command(uint8_t cmd)
{
    return cmd == 0x20;
}

static void handle_provisioning_command(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    ARG_UNUSED(data);
    ARG_UNUSED(len);

    switch (cmd) {
    case 0x10:
        LOG_INF("Provisioning command received: mark provisioned");
		streaming_enabled = false;
        sp_state_set_provisioned_idle();
        break;

    default:
        LOG_WRN("Unknown provisioning command: 0x%02x", cmd);
        break;
    }
}

static void handle_operational_command(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    ARG_UNUSED(data);
    ARG_UNUSED(len);

    switch (cmd) {
    case 0x01:
        streaming_enabled = true;
        LOG_INF("Operational command: start streaming");
        break;

    case 0x00:
        streaming_enabled = false;
        LOG_INF("Operational command: stop streaming");
        break;

    default:
        LOG_WRN("Unknown operational command: 0x%02x", cmd);
        break;
    }
}

static void app_rx_handler(const uint8_t *data, uint16_t len)
{
    uint8_t cmd;

    if (data == NULL || len == 0U) {
        LOG_WRN("RX: empty payload");
        return;
    }

    cmd = data[0];

    LOG_INF("RX: state=%s cmd=0x%02x len=%u",
            sp_state_str(sp_state_get()), cmd, len);

    switch (sp_state_get()) {
    case SP_STATE_FACTORY_NEW:
        if (is_provisioning_command(cmd)) {
            handle_provisioning_command(cmd, data, len);
        } else {
            LOG_WRN("Blocked command 0x%02x in FACTORY_NEW", cmd);
        }
        break;

    case SP_STATE_PROVISIONED_IDLE:
        if (is_provisioning_command(cmd)) {
            handle_provisioning_command(cmd, data, len);
        } else if (is_auth_command(cmd)) {
        LOG_INF("Auth command received: entering AUTHENTICATED");
        sp_state_set_authenticated();
		} else {
            LOG_WRN("Blocked operational command 0x%02x in PROVISIONED_IDLE", cmd);
        }
        break;

    case SP_STATE_AUTHENTICATED:
        if (is_operational_command(cmd)) {
            handle_operational_command(cmd, data, len);
        } else if (is_provisioning_command(cmd)) {
            handle_provisioning_command(cmd, data, len);
        } else {
            LOG_WRN("Unknown command 0x%02x in AUTHENTICATED", cmd);
        }
        break;

    default:
        LOG_ERR("Invalid state");
        break;
    }
}

static void app_prov_rx_handler(const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0U) {
		LOG_WRN("Provisioning RX: empty payload");
		return;
	}

	LOG_INF("Provisioning RX cmd=0x%02x len=%u", data[0], len);
}

static void app_oper_auth_rx_handler(const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0U) {
		LOG_WRN("Operational auth RX: empty payload");
		return;
	}

	LOG_INF("Operational auth RX cmd=0x%02x len=%u", data[0], len);
}

static void app_prov_rx_handler(const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0U) {
		LOG_WRN("Provisioning RX: empty payload");
		return;
	}

	LOG_INF("Provisioning RX cmd=0x%02x len=%u", data[0], len);
}

static void app_oper_auth_rx_handler(const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0U) {
		LOG_WRN("Operational auth RX: empty payload");
		return;
	}

	LOG_INF("Operational auth RX cmd=0x%02x len=%u", data[0], len);
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

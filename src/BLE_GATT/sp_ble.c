#include "sp_ble.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(sp_ble, LOG_LEVEL_INF);

/*
 * Custom UUIDs
 *
 * Service UUID:      12345678-1234-5678-1234-56789abcdef0
 * TX Characteristic: 12345679-1234-5678-1234-56789abcdef0
 * RX Characteristic: 1234567a-1234-5678-1234-56789abcdef0
 */
#define BT_UUID_SP_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_SP_TX_VAL \
	BT_UUID_128_ENCODE(0x12345679, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_SP_RX_VAL \
	BT_UUID_128_ENCODE(0x1234567A, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

static struct bt_uuid_128 sp_service_uuid = BT_UUID_INIT_128(BT_UUID_SP_SERVICE_VAL);
static struct bt_uuid_128 sp_tx_uuid      = BT_UUID_INIT_128(BT_UUID_SP_TX_VAL);
static struct bt_uuid_128 sp_rx_uuid      = BT_UUID_INIT_128(BT_UUID_SP_RX_VAL);

/* Keep payload small for now. Increase later when you tune MTU/data length. */
#define SP_BLE_MAX_DATA_LEN 20

static struct bt_conn *current_conn;
static bool notify_enabled;
static sp_ble_rx_cb_t rx_callback;

static uint8_t tx_value[SP_BLE_MAX_DATA_LEN];
static uint8_t rx_value[SP_BLE_MAX_DATA_LEN];

/* Called when the central enables/disables notifications on TX characteristic */
static void tx_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("TX notifications %s", notify_enabled ? "enabled" : "disabled");
}

/* Called when central writes to RX characteristic */
static ssize_t write_rx(struct bt_conn *conn,
			const struct bt_gatt_attr *attr,
			const void *buf,
			uint16_t len,
			uint16_t offset,
			uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len > sizeof(rx_value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(rx_value, buf, len);
	LOG_INF("RX write: len=%u", len);

	if (rx_callback != NULL) {
		rx_callback(rx_value, len);
	}

	return len;
}

/*
 * Attribute layout:
 *   0: Primary service
 *   1: TX characteristic declaration
 *   2: TX characteristic value
 *   3: TX CCC
 *   4: RX characteristic declaration
 *   5: RX characteristic value
 */
BT_GATT_SERVICE_DEFINE(sp_svc,
	BT_GATT_PRIMARY_SERVICE(&sp_service_uuid),

	BT_GATT_CHARACTERISTIC(&sp_tx_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, tx_value),
	BT_GATT_CCC(tx_ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&sp_rx_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, write_rx, rx_value),
);

int sp_ble_init(sp_ble_rx_cb_t rx_cb)
{
	rx_callback = rx_cb;
	notify_enabled = false;
	current_conn = NULL;

	memset(tx_value, 0, sizeof(tx_value));
	memset(rx_value, 0, sizeof(rx_value));

	LOG_INF("Custom SP BLE service initialized");
	return 0;
}

int sp_ble_send(const uint8_t *data, uint16_t len)
{
	int err;

	if (data == NULL || len == 0U) {
		LOG_ERR("sp_ble_send: invalid args");
		return -EINVAL;
	}

	if (current_conn == NULL) {
		LOG_WRN("sp_ble_send: no connection");
		return -ENOTCONN;
	}

	if (!notify_enabled) {
		LOG_WRN("sp_ble_send: notify not enabled");
		return -EACCES;
	}

	if (len > sizeof(tx_value)) {
		LOG_ERR("sp_ble_send: message too large (%u)", len);
		return -EMSGSIZE;
	}

	memcpy(tx_value, data, len);

	LOG_INF("sp_ble_send: conn=%p len=%u", current_conn, len);

	err = bt_gatt_notify(current_conn, &sp_svc.attrs[2], tx_value, len);
	LOG_INF("sp_ble_send: bt_gatt_notify returned %d", err);

	return err;
}

bool sp_ble_is_notify_enabled(void)
{
	return notify_enabled;
}

void sp_ble_connected(struct bt_conn *conn)
{
	if (current_conn != NULL) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	current_conn = bt_conn_ref(conn);
	LOG_INF("BLE module stored connection reference");
}

void sp_ble_disconnected(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	notify_enabled = false;

	if (current_conn != NULL) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	LOG_INF("BLE module released connection reference");
}
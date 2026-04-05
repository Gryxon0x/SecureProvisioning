#include "sp_oper.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(sp_oper, LOG_LEVEL_INF);

/* UUIDs */
#define BT_UUID_SP_OPER_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x12345688, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_SP_OPER_AUTH_RX_VAL \
	BT_UUID_128_ENCODE(0x12345689, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_SP_OPER_AUTH_TX_VAL \
	BT_UUID_128_ENCODE(0x1234568A, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_SP_OPER_CMD_RX_VAL \
	BT_UUID_128_ENCODE(0x1234568B, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_SP_OPER_TELEM_TX_VAL \
	BT_UUID_128_ENCODE(0x1234568C, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

static struct bt_uuid_128 oper_service_uuid = BT_UUID_INIT_128(BT_UUID_SP_OPER_SERVICE_VAL);
static struct bt_uuid_128 auth_rx_uuid      = BT_UUID_INIT_128(BT_UUID_SP_OPER_AUTH_RX_VAL);
static struct bt_uuid_128 auth_tx_uuid      = BT_UUID_INIT_128(BT_UUID_SP_OPER_AUTH_TX_VAL);
static struct bt_uuid_128 cmd_rx_uuid       = BT_UUID_INIT_128(BT_UUID_SP_OPER_CMD_RX_VAL);
static struct bt_uuid_128 telem_tx_uuid     = BT_UUID_INIT_128(BT_UUID_SP_OPER_TELEM_TX_VAL);

#define SP_OPER_MAX_DATA_LEN 64

static struct bt_conn *current_conn;
static bool auth_tx_enabled;
static bool telem_tx_enabled;

static sp_oper_auth_rx_cb_t auth_callback;
static sp_oper_cmd_rx_cb_t cmd_callback;

static uint8_t auth_rx_value[SP_OPER_MAX_DATA_LEN];
static uint8_t auth_tx_value[SP_OPER_MAX_DATA_LEN];
static uint8_t cmd_rx_value[SP_OPER_MAX_DATA_LEN];
static uint8_t telem_tx_value[SP_OPER_MAX_DATA_LEN];

static void auth_tx_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	auth_tx_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Operational auth TX notifications %s", auth_tx_enabled ? "enabled" : "disabled");
}

static void telem_tx_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	telem_tx_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Operational telemetry notifications %s", telem_tx_enabled ? "enabled" : "disabled");
}

static ssize_t write_auth_rx(struct bt_conn *conn,
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

	if (len > sizeof(auth_rx_value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(auth_rx_value, buf, len);
	LOG_INF("Operational auth RX write: len=%u", len);

	if (auth_callback != NULL) {
		auth_callback(auth_rx_value, len);
	}

	return len;
}

static ssize_t write_cmd_rx(struct bt_conn *conn,
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

	if (len > sizeof(cmd_rx_value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(cmd_rx_value, buf, len);
	LOG_INF("Operational command RX write: len=%u", len);

	if (cmd_callback != NULL) {
		cmd_callback(cmd_rx_value, len);
	}

	return len;
}

/*
 * Attr layout:
 * 0 service
 * 1 auth RX decl
 * 2 auth RX value
 * 3 auth TX decl
 * 4 auth TX value
 * 5 auth TX CCC
 * 6 cmd RX decl
 * 7 cmd RX value
 * 8 telemetry TX decl
 * 9 telemetry TX value
 * 10 telemetry TX CCC
 */
BT_GATT_SERVICE_DEFINE(sp_oper_svc,
	BT_GATT_PRIMARY_SERVICE(&oper_service_uuid),

	BT_GATT_CHARACTERISTIC(&auth_rx_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, write_auth_rx, auth_rx_value),

	BT_GATT_CHARACTERISTIC(&auth_tx_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, auth_tx_value),
	BT_GATT_CCC(auth_tx_ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&cmd_rx_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, write_cmd_rx, cmd_rx_value),

	BT_GATT_CHARACTERISTIC(&telem_tx_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, telem_tx_value),
	BT_GATT_CCC(telem_tx_ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

int sp_oper_init(sp_oper_auth_rx_cb_t auth_cb, sp_oper_cmd_rx_cb_t cmd_cb)
{
	current_conn = NULL;
	auth_tx_enabled = false;
	telem_tx_enabled = false;
	auth_callback = auth_cb;
	cmd_callback = cmd_cb;

	memset(auth_rx_value, 0, sizeof(auth_rx_value));
	memset(auth_tx_value, 0, sizeof(auth_tx_value));
	memset(cmd_rx_value, 0, sizeof(cmd_rx_value));
	memset(telem_tx_value, 0, sizeof(telem_tx_value));

	LOG_INF("Operational service initialized");
	return 0;
}

void sp_oper_connected(struct bt_conn *conn)
{
	if (current_conn != NULL) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	current_conn = bt_conn_ref(conn);
	LOG_INF("Operational service stored connection");
}

void sp_oper_disconnected(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	auth_tx_enabled = false;
	telem_tx_enabled = false;

	if (current_conn != NULL) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	LOG_INF("Operational service released connection");
}

bool sp_oper_is_auth_tx_enabled(void)
{
	return auth_tx_enabled;
}

bool sp_oper_is_telemetry_enabled(void)
{
	return telem_tx_enabled;
}

int sp_oper_send_auth(const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0U) {
		return -EINVAL;
	}

	if (current_conn == NULL) {
		return -ENOTCONN;
	}

	if (!auth_tx_enabled) {
		return -EACCES;
	}

	if (len > sizeof(auth_tx_value)) {
		return -EMSGSIZE;
	}

	memcpy(auth_tx_value, data, len);

	return bt_gatt_notify(current_conn, &sp_oper_svc.attrs[4], auth_tx_value, len);
}

int sp_oper_send_telemetry(const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0U) {
		return -EINVAL;
	}

	if (current_conn == NULL) {
		return -ENOTCONN;
	}

	if (!telem_tx_enabled) {
		return -EACCES;
	}

	if (len > sizeof(telem_tx_value)) {
		return -EMSGSIZE;
	}

	memcpy(telem_tx_value, data, len);

	return bt_gatt_notify(current_conn, &sp_oper_svc.attrs[9], telem_tx_value, len);
}
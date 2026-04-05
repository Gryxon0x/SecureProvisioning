#include "sp_prov.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(sp_prov, LOG_LEVEL_INF);

/* UUIDs */
#define BT_UUID_SP_PROV_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_SP_PROV_STATE_VAL \
	BT_UUID_128_ENCODE(0x12345679, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_SP_PROV_RX_VAL \
	BT_UUID_128_ENCODE(0x1234567A, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_SP_PROV_TX_VAL \
	BT_UUID_128_ENCODE(0x1234567B, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

static struct bt_uuid_128 prov_service_uuid = BT_UUID_INIT_128(BT_UUID_SP_PROV_SERVICE_VAL);
static struct bt_uuid_128 prov_state_uuid   = BT_UUID_INIT_128(BT_UUID_SP_PROV_STATE_VAL);
static struct bt_uuid_128 prov_rx_uuid      = BT_UUID_INIT_128(BT_UUID_SP_PROV_RX_VAL);
static struct bt_uuid_128 prov_tx_uuid      = BT_UUID_INIT_128(BT_UUID_SP_PROV_TX_VAL);

#define SP_PROV_MAX_DATA_LEN 64

struct __packed sp_prov_state_msg {
	uint8_t state;
	uint8_t flags;
	uint16_t last_error;
};

static struct bt_conn *current_conn;
static bool tx_enabled;
static sp_prov_rx_cb_t rx_callback;

static struct sp_prov_state_msg state_value;
static uint8_t tx_value[SP_PROV_MAX_DATA_LEN];
static uint8_t rx_value[SP_PROV_MAX_DATA_LEN];

static ssize_t read_state(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr,
			  void *buf,
			  uint16_t len,
			  uint16_t offset)
{
	const struct sp_prov_state_msg *value = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static void tx_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	tx_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Provisioning TX notifications %s", tx_enabled ? "enabled" : "disabled");
}

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
	LOG_INF("Provisioning RX write: len=%u", len);

	if (rx_callback != NULL) {
		rx_callback(rx_value, len);
	}

	return len;
}

/*
 * Attr layout:
 * 0 service
 * 1 state characteristic declaration
 * 2 state characteristic value
 * 3 state CCC
 * 4 prov RX declaration
 * 5 prov RX value
 * 6 prov TX declaration
 * 7 prov TX value
 * 8 prov TX CCC
 */
BT_GATT_SERVICE_DEFINE(sp_prov_svc,
	BT_GATT_PRIMARY_SERVICE(&prov_service_uuid),

	BT_GATT_CHARACTERISTIC(&prov_state_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       read_state, NULL, &state_value),
	BT_GATT_CCC(tx_ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&prov_rx_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, write_rx, rx_value),

	BT_GATT_CHARACTERISTIC(&prov_tx_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, tx_value),
	BT_GATT_CCC(tx_ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

int sp_prov_init(sp_prov_rx_cb_t rx_cb)
{
	rx_callback = rx_cb;
	tx_enabled = false;
	current_conn = NULL;

	memset(&state_value, 0, sizeof(state_value));
	memset(tx_value, 0, sizeof(tx_value));
	memset(rx_value, 0, sizeof(rx_value));

	LOG_INF("Provisioning service initialized");
	return 0;
}

void sp_prov_connected(struct bt_conn *conn)
{
	if (current_conn != NULL) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	current_conn = bt_conn_ref(conn);
	LOG_INF("Provisioning service stored connection");
}

void sp_prov_disconnected(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	tx_enabled = false;

	if (current_conn != NULL) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	LOG_INF("Provisioning service released connection");
}

bool sp_prov_is_tx_enabled(void)
{
	return tx_enabled;
}

int sp_prov_send(const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0U) {
		return -EINVAL;
	}

	if (current_conn == NULL) {
		return -ENOTCONN;
	}

	if (!tx_enabled) {
		return -EACCES;
	}

	if (len > sizeof(tx_value)) {
		return -EMSGSIZE;
	}

	memcpy(tx_value, data, len);

	return bt_gatt_notify(current_conn, &sp_prov_svc.attrs[7], tx_value, len);
}

int sp_prov_send_state(uint8_t state, uint8_t flags, uint16_t last_error)
{
	int err;

	state_value.state = state;
	state_value.flags = flags;
	state_value.last_error = last_error;

	if (current_conn == NULL) {
		return -ENOTCONN;
	}

	if (!tx_enabled) {
		return -EACCES;
	}

	err = bt_gatt_notify(current_conn, &sp_prov_svc.attrs[2], &state_value, sizeof(state_value));
	return err;
}
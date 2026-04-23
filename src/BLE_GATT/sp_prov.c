#include "sp_prov.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
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

/* Provisioning session state */
static uint8_t prov_challenge[SP_PROV_CHALLENGE_LEN];
static bool prov_challenge_active;
static bool prov_authenticated;

static uint8_t staged_blob[SP_PROV_BLOB_MAX_LEN];
static uint16_t staged_blob_len;
static bool staged_blob_valid;

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

static int sp_prov_send_error(uint8_t cmd, uint8_t err_code)
{
	uint8_t msg[3];

	msg[0] = SP_PROV_EVT_ERROR;
	msg[1] = cmd;
	msg[2] = err_code;

	return sp_prov_send(msg, sizeof(msg));
}

static int sp_prov_send_ack(uint8_t cmd)
{
	uint8_t msg[2];

	msg[0] = SP_PROV_EVT_ACK;
	msg[1] = cmd;

	return sp_prov_send(msg, sizeof(msg));
}

static int sp_prov_send_commit_result(uint8_t status)
{
	uint8_t msg[2];

	msg[0] = SP_PROV_EVT_COMMIT_RESULT;
	msg[1] = status;

	return sp_prov_send(msg, sizeof(msg));
}

static int handle_get_challenge(void)
{
	uint8_t msg[1 + SP_PROV_CHALLENGE_LEN];

	sys_rand_get(prov_challenge, sizeof(prov_challenge));
	prov_challenge_active = true;
	prov_authenticated = false;
	staged_blob_valid = false;
	staged_blob_len = 0U;

	msg[0] = SP_PROV_EVT_CHALLENGE;
	memcpy(&msg[1], prov_challenge, sizeof(prov_challenge));

	LOG_INF("Provisioning challenge generated");
	return sp_prov_send(msg, sizeof(msg));
}

/*
 * Placeholder proof verification for now.
 * Real implementation should verify HMAC/bootstrap proof.
 */
static bool verify_proof_placeholder(const uint8_t *proof, uint16_t len)
{
	if (!prov_challenge_active) {
		return false;
	}

	/* Temporary rule for plumbing:
	 * accept proof if first byte is 0xA5 and len >= 1
	 */
	if (len >= 1U && proof[0] == 0xA5U) {
		return true;
	}

	return false;
}

static int handle_send_proof(const uint8_t *buf, uint16_t len)
{
	bool ok;

	if (len < 2U) {
		return sp_prov_send_error(SP_PROV_CMD_SEND_PROOF, 0x01);
	}

	ok = verify_proof_placeholder(&buf[1], len - 1U);
	if (!ok) {
		prov_authenticated = false;
		LOG_WRN("Provisioning proof rejected");
		return sp_prov_send_error(SP_PROV_CMD_SEND_PROOF, 0x02);
	}

	prov_authenticated = true;
	LOG_INF("Provisioning proof accepted");
	return sp_prov_send_ack(SP_PROV_CMD_SEND_PROOF);
}

static int handle_set_blob(const uint8_t *buf, uint16_t len)
{
	uint16_t blob_len = len - 1U;

	if (!prov_authenticated) {
		return sp_prov_send_error(SP_PROV_CMD_SET_BLOB, 0x03);
	}

	if (len < 2U) {
		return sp_prov_send_error(SP_PROV_CMD_SET_BLOB, 0x01);
	}

	if (blob_len > sizeof(staged_blob)) {
		return sp_prov_send_error(SP_PROV_CMD_SET_BLOB, 0x04);
	}

	memcpy(staged_blob, &buf[1], blob_len);
	staged_blob_len = blob_len;
	staged_blob_valid = true;

	LOG_INF("Provisioning blob staged: len=%u", blob_len);
	return sp_prov_send_ack(SP_PROV_CMD_SET_BLOB);
}

static int handle_commit(void)
{
	if (!prov_authenticated) {
		return sp_prov_send_error(SP_PROV_CMD_COMMIT, 0x03);
	}

	if (!staged_blob_valid) {
		return sp_prov_send_error(SP_PROV_CMD_COMMIT, 0x05);
	}

	LOG_INF("Provisioning commit accepted");
	return sp_prov_send_commit_result(0x00);
}

static int handle_rx_message(const uint8_t *buf, uint16_t len)
{
	uint8_t cmd;

	if (buf == NULL || len == 0U) {
		return -EINVAL;
	}

	cmd = buf[0];

	switch (cmd) {
	case SP_PROV_CMD_GET_CHALLENGE:
		return handle_get_challenge();

	case SP_PROV_CMD_SEND_PROOF:
		return handle_send_proof(buf, len);

	case SP_PROV_CMD_SET_BLOB:
		return handle_set_blob(buf, len);

	case SP_PROV_CMD_COMMIT:
		return handle_commit();

	default:
		LOG_WRN("Unknown provisioning command: 0x%02x", cmd);
		return sp_prov_send_error(cmd, 0x7F);
	}
}

static ssize_t write_rx(struct bt_conn *conn,
			const struct bt_gatt_attr *attr,
			const void *buf,
			uint16_t len,
			uint16_t offset,
			uint8_t flags)
{
	int err;

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
	err = handle_rx_message(rx_value, len);
	if (err) {
		LOG_WRN("Provisioning command handling returned %d", err);
	}

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

	memset(prov_challenge, 0, sizeof(prov_challenge));
	prov_challenge_active = false;
	prov_authenticated = false;

	memset(staged_blob, 0, sizeof(staged_blob));
	staged_blob_len = 0U;
	staged_blob_valid = false;

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

	sp_prov_reset_session();
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

bool sp_prov_is_challenge_active(void)
{
	return prov_challenge_active;
}

bool sp_prov_is_authenticated(void)
{
	return prov_authenticated;
}

bool sp_prov_is_blob_staged(void)
{
	return staged_blob_valid;
}

const uint8_t *sp_prov_get_challenge(void)
{
	return prov_challenge;
}

uint16_t sp_prov_get_challenge_len(void)
{
	return sizeof(prov_challenge);
}

const uint8_t *sp_prov_get_blob(void)
{
	return staged_blob;
}

uint16_t sp_prov_get_blob_len(void)
{
	return staged_blob_len;
}

void sp_prov_reset_session(void)
{
	memset(prov_challenge, 0, sizeof(prov_challenge));
	prov_challenge_active = false;
	prov_authenticated = false;

	memset(staged_blob, 0, sizeof(staged_blob));
	staged_blob_len = 0U;
	staged_blob_valid = false;
}
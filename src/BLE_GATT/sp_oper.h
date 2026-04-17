#ifndef SP_PROV_H_
#define SP_PROV_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/bluetooth/conn.h>

typedef void (*sp_oper_auth_rx_cb_t)(const uint8_t *data, uint16_t len);
typedef void (*sp_oper_cmd_rx_cb_t)(const uint8_t *data, uint16_t len);

enum {
	SP_OPER_CMD_AUTH_GET_CHALLENGE = 0x20,
	SP_OPER_CMD_AUTH_SEND_PROOF    = 0x21,
	SP_OPER_CMD_AUTH_LOGOUT        = 0x22,
};

enum {
	SP_OPER_EVT_AUTH_CHALLENGE     = 0x90,
	SP_OPER_EVT_AUTH_ACK           = 0x91,
	SP_OPER_EVT_AUTH_ERROR         = 0x92,
	SP_OPER_EVT_AUTH_RESULT        = 0x93,
};

#define SP_OPER_CHALLENGE_LEN 16
#define SP_OPER_PROOF_MAX_LEN 32

int sp_oper_init(sp_oper_auth_rx_cb_t auth_cb, sp_oper_cmd_rx_cb_t cmd_cb);

void sp_oper_connected(struct bt_conn *conn);
void sp_oper_disconnected(struct bt_conn *conn);

bool sp_oper_is_auth_tx_enabled(void);
bool sp_oper_is_telemetry_enabled(void);

int sp_oper_send_auth(const uint8_t *data, uint16_t len);
int sp_oper_send_telemetry(const uint8_t *data, uint16_t len);

/* New helpers */
bool sp_oper_is_challenge_active(void);
bool sp_oper_is_authenticated(void);

const uint8_t *sp_oper_get_challenge(void);
uint16_t sp_oper_get_challenge_len(void);

void sp_oper_reset_session(void);

#endif /* SP_OPER_H_ */
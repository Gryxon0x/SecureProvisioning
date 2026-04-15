#ifndef SP_PROV_H_
#define SP_PROV_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/bluetooth/conn.h>

typedef void (*sp_prov_rx_cb_t)(const uint8_t *data, uint16_t len);

enum {
	SP_PROV_CMD_GET_CHALLENGE = 0x10,
	SP_PROV_CMD_SEND_PROOF    = 0x11,
	SP_PROV_CMD_SET_BLOB      = 0x12,
	SP_PROV_CMD_COMMIT        = 0x13,
};

enum {
	SP_PROV_EVT_CHALLENGE     = 0x80,
	SP_PROV_EVT_ACK           = 0x81,
	SP_PROV_EVT_ERROR         = 0x82,
	SP_PROV_EVT_COMMIT_RESULT = 0x83,
};

int sp_prov_init(sp_prov_rx_cb_t rx_cb);

void sp_prov_connected(struct bt_conn *conn);
void sp_prov_disconnected(struct bt_conn *conn);

bool sp_prov_is_tx_enabled(void);
int sp_prov_send(const uint8_t *data, uint16_t len);
int sp_prov_send_state(uint8_t state, uint8_t flags, uint16_t last_error);

/* New helpers */
bool sp_prov_is_challenge_active(void);
bool sp_prov_is_authenticated(void);
bool sp_prov_is_blob_staged(void);

const uint8_t *sp_prov_get_challenge(void);
uint16_t sp_prov_get_challenge_len(void);

const uint8_t *sp_prov_get_blob(void);
uint16_t sp_prov_get_blob_len(void);

void sp_prov_reset_session(void);

#endif /* SP_PROV_H_ */
#ifndef SP_PROV_H_
#define SP_PROV_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/bluetooth/conn.h>

typedef void (*sp_oper_auth_rx_cb_t)(const uint8_t *data, uint16_t len);
typedef void (*sp_oper_cmd_rx_cb_t)(const uint8_t *data, uint16_t len);

int sp_prov_init(sp_prov_rx_cb_t rx_cb);

void sp_prov_connected(struct bt_conn *conn);
void sp_prov_disconnected(struct bt_conn *conn);

bool sp_prov_is_tx_enabled(void);
int sp_prov_send(const uint8_t *data, uint16_t len);
int sp_prov_send_state(uint8_t state, uint8_t flags, uint16_t last_error);

#endif /* SP_PROV_H_ */
#ifndef SP_BLE_H_
#define SP_BLE_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/bluetooth/conn.h>

typedef void (*sp_ble_rx_cb_t)(const uint8_t *data, uint16_t len);

int sp_ble_init(sp_ble_rx_cb_t rx_cb);
int sp_ble_send(const uint8_t *data, uint16_t len);
bool sp_ble_is_notify_enabled(void);

void sp_ble_connected(struct bt_conn *conn);
void sp_ble_disconnected(struct bt_conn *conn);

#endif /* SP_BLE_H_ */
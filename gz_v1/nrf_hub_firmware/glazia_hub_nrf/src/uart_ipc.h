#pragma once
#include <stdint.h>
#include <stddef.h>

/* Frame: [0xA5][TYPE][LEN_LO][LEN_HI][PAYLOAD...][CRC8-poly0x07]
 * Binary-compatible with ESP32-S3 nrf_ipc.c (same byte values, same CRC). */

/* Commands received from ESP32-S3 */
#define IPC_CMD_PING        0x00  /* no payload — reply with IPC_EVT_PONG */
#define IPC_CMD_NET_FORM    0x01
#define IPC_CMD_NET_STATUS  0x02
#define IPC_CMD_COMMISSION  0x03  /* eui64[8] + pskd[9 null-term] + timeout_s[u16-LE] */
#define IPC_CMD_SENSOR_DEL  0x04  /* eui64[8] */

/* Events sent to ESP32-S3 */
#define IPC_EVT_PONG           0x80  /* no payload — reply to IPC_CMD_PING */
#define IPC_EVT_NET_UP         0x81  /* channel[1] + panid[2-LE] */
#define IPC_EVT_NET_DOWN       0x82
#define IPC_EVT_SENSOR_JOINED  0x83  /* eui64[8] */
#define IPC_EVT_SENSOR_DATA    0x84  /* eui64[8] + JSON string (null-term) */
#define IPC_EVT_COMM_FAILED    0x85  /* eui64[8] */
#define IPC_EVT_SENSOR_LOST    0x86  /* eui64[8] — child aged out of the Thread child table */
#define IPC_EVT_SENSOR_ONLINE  0x87  /* eui64[8] — child (re)attached to the Thread child table */
#define IPC_EVT_CHILD_LIST     0x88  /* N × extaddr[8] — snapshot of the current Thread child table */

typedef void (*ipc_cmd_cb_t)(uint8_t type, const uint8_t *payload, uint16_t len);

void uart_ipc_init(ipc_cmd_cb_t cb);
void uart_ipc_send(uint8_t type, const uint8_t *payload, uint16_t len);

void ipc_send_net_up(uint8_t channel, uint16_t panid);
void ipc_send_net_down(void);
void ipc_send_sensor_joined(const uint8_t eui64[8]);
void ipc_send_sensor_data(const uint8_t eui64[8], const char *json);
void ipc_send_comm_failed(const uint8_t eui64[8]);
void ipc_send_sensor_lost(const uint8_t eui64[8]);
void ipc_send_sensor_online(const uint8_t eui64[8]);
void ipc_send_child_list(const uint8_t *extaddrs, int n_children);

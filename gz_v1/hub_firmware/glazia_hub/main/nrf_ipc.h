#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * IPC frame: [0xA5][TYPE][LEN_LO][LEN_HI][PAYLOAD...][CRC8-poly0x07]
 * Binary-compatible with gz_v2 uart_ipc (same byte values, same CRC).
 */

/* Commands: ESP32-S3 → nRF */
#define IPC_CMD_PING        0x00  /* no payload — expect IPC_EVT_PONG back */
#define IPC_CMD_NET_FORM    0x01
#define IPC_CMD_NET_STATUS  0x02
#define IPC_CMD_COMMISSION  0x03  /* eui64[8] + pskd[9 null-term] + timeout_s[u16-LE] */
#define IPC_CMD_SENSOR_DEL  0x04  /* eui64[8] */

/* Events: nRF → ESP32-S3 */
#define IPC_EVT_PONG           0x80  /* no payload — reply to IPC_CMD_PING */
#define IPC_EVT_NET_UP         0x81  /* channel[1] + panid[2-LE] */
#define IPC_EVT_NET_DOWN       0x82
#define IPC_EVT_SENSOR_JOINED  0x83  /* eui64[8] */
#define IPC_EVT_SENSOR_DATA    0x84  /* eui64[8] + JSON string (null-term) */
#define IPC_EVT_COMM_FAILED    0x85  /* eui64[8] */
#define IPC_EVT_SENSOR_LOST    0x86  /* eui64[8] — child aged out of the Thread child table */
#define IPC_EVT_SENSOR_ONLINE  0x87  /* eui64[8] — child (re)attached to the Thread child table */
#define IPC_EVT_CHILD_LIST     0x88  /* N × extaddr[8] — snapshot of the current Thread child table */

typedef void (*nrf_ipc_event_cb_t)(uint8_t type, const uint8_t *payload, uint16_t len);

void nrf_ipc_init(nrf_ipc_event_cb_t cb);
void nrf_ipc_send(uint8_t type, const uint8_t *payload, uint16_t len);

void ipc_cmd_ping(void);
void ipc_cmd_net_form(void);
void ipc_cmd_net_status(void);
void ipc_cmd_commission(const uint8_t eui64[8], const char *pskd, uint16_t timeout_s);
void ipc_cmd_sensor_del(const uint8_t eui64[8]);

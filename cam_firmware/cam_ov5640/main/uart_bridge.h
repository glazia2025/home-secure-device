#pragma once

#include <stdint.h>

/* IPC protocol message types — must match hub's cam_uart.h exactly */
#define CAM_MSG_IDLE           0x00
#define CAM_MSG_WEBRTC_START   0x01  /* hub→cam: JSON {ssid,pass,turn_user,turn_psw} */
#define CAM_MSG_STOP           0x02  /* hub→cam: no payload */
#define CAM_MSG_OFFER          0x03  /* cam→hub: JSON {"type":"offer","sdp":{...}} */
#define CAM_MSG_ANSWER         0x04  /* hub→cam: raw SDP string */
#define CAM_MSG_ICE_FROM_CAM   0x05  /* cam→hub: JSON {"type":"ice-candidate",...} */
#define CAM_MSG_ICE_TO_CAM     0x06  /* hub→cam: raw ICE candidate string */
#define CAM_MSG_LINK_TEST      0x07  /* cam→hub: periodic UART health probe */

#define CAM_UART_MAX_PL        4092  /* max payload per frame */

void uart_bridge_start(void);

/* Send a signaling message to the hub over UART.
 * payload is copied internally; caller owns it after this returns. */
void uart_bridge_send_msg(uint8_t type, const char *payload, uint16_t len);

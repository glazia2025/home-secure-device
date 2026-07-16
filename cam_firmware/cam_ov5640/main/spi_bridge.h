#pragma once

#include <stdint.h>

/* SPI protocol message types — must match hub's cam_spi.h exactly */
#define CAM_MSG_IDLE           0x00
#define CAM_MSG_WEBRTC_START   0x01  /* hub→cam: JSON {ssid,pass,turn_user,turn_psw} */
#define CAM_MSG_STOP           0x02  /* hub→cam: no payload */
#define CAM_MSG_OFFER          0x03  /* cam→hub: JSON {"type":"offer","sdp":{...}} */
#define CAM_MSG_ANSWER         0x04  /* hub→cam: raw SDP string */
#define CAM_MSG_ICE_FROM_CAM   0x05  /* cam→hub: JSON {"type":"ice-candidate",...} */
#define CAM_MSG_ICE_TO_CAM     0x06  /* hub→cam: raw ICE candidate string */

#define CAM_SPI_MSG_SIZE       4096  /* fixed transaction size */

void spi_bridge_start(void);

/* Queue a signaling message to send to hub on the next SPI transaction.
 * Called from webrtc_cam when it has an offer or ICE candidate ready.
 * payload is copied internally; caller owns it after this returns. */
void spi_bridge_queue_msg(uint8_t type, const char *payload, uint16_t len);

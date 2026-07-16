#pragma once

#include <stdint.h>

/* SPI protocol message types (hub↔cam_esp, same values on both sides) */
#define CAM_MSG_IDLE           0x00  /* no message */
#define CAM_MSG_WEBRTC_START   0x01  /* hub→cam: JSON {ssid,pass,turn_user,turn_psw} */
#define CAM_MSG_STOP           0x02  /* hub→cam: no payload */
#define CAM_MSG_OFFER          0x03  /* cam→hub: JSON {"type":"offer","sdp":{...}} */
#define CAM_MSG_ANSWER         0x04  /* hub→cam: raw SDP string */
#define CAM_MSG_ICE_FROM_CAM   0x05  /* cam→hub: JSON {"type":"ice-candidate",...} */
#define CAM_MSG_ICE_TO_CAM     0x06  /* hub→cam: raw ICE candidate string */

#define CAM_SPI_MSG_SIZE       4096  /* fixed transaction size, both directions */

void cam_spi_init(void);
void cam_spi_webrtc_start(const char *ssid, const char *pass,
                           const char *turn_user, const char *turn_psw);
void cam_spi_webrtc_stop(void);
void cam_spi_relay_answer(const char *sdp_str);
void cam_spi_relay_ice_to_cam(const char *cand_str);
void cam_spi_resend_pending_offer(void);

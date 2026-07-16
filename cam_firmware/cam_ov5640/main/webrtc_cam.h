#pragma once

/* Called once at boot: allocates task stacks in PSRAM, starts cert pre-generation. */
void webrtc_cam_init(void);

/* Called by spi_bridge when CAM_MSG_WEBRTC_START is received.
 * json: heap-allocated, null-terminated; callee frees it via strtok / cJSON. */
void webrtc_cam_start_from_json(const char *json);

/* Called by spi_bridge when CAM_MSG_ANSWER is received (raw SDP string). */
void webrtc_cam_on_answer(const char *sdp_str, int len);

/* Called by spi_bridge when CAM_MSG_ICE_TO_CAM is received (raw candidate string). */
void webrtc_cam_on_ice(const char *cand_str, int len);

/* Called by spi_bridge when CAM_MSG_STOP is received, or on WS disconnect. */
void webrtc_cam_stop(void);

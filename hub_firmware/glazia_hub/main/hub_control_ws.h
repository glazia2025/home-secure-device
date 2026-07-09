#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t hub_control_ws_start(void);
void hub_control_ws_stop(void);

/* Send an arbitrary JSON string over the hub control WebSocket.
 * Used by webrtc_stream to relay SDP offer and ICE candidates. */
void hub_control_ws_send_json(const char *json_str);

/* Send raw binary data over the hub control WebSocket.
 * Used by mjpeg_transport_send_frame — call only through that abstraction. */
esp_err_t hub_control_ws_send_bin(const uint8_t *data, size_t len, TickType_t ticks_to_wait);

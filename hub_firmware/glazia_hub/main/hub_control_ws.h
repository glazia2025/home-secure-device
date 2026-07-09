#pragma once

#include "esp_err.h"

esp_err_t hub_control_ws_start(void);
void hub_control_ws_stop(void);

/* Send an arbitrary JSON string over the hub control WebSocket.
 * Used by webrtc_stream to relay SDP offer and ICE candidates. */
void hub_control_ws_send_json(const char *json_str);

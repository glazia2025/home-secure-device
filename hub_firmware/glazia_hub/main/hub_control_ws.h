#pragma once

#include "esp_err.h"

esp_err_t hub_control_ws_start(void);
void hub_control_ws_stop(void);
void hub_control_ws_send_camera_status(const char *stream_session_id,
                                       const char *status,
                                       const char *error);

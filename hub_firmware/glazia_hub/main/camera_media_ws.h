#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t camera_media_ws_connect(const char *stream_session_id);
esp_err_t camera_media_ws_send_frame(const uint8_t *jpg, size_t jpg_len);
void camera_media_ws_stop(void);
bool camera_media_ws_is_connected(void);

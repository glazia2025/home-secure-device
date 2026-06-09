#pragma once

#include "esp_err.h"

esp_err_t camera_stream_start(const char *stream_session_id);
void camera_stream_stop(void);

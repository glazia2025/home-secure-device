#pragma once
#include "esp_err.h"
#include "esp_camera.h"

esp_err_t camera_core_init(void);
esp_err_t camera_core_init_webrtc(void);  /* YUYV, QVGA, PSRAM, 2 buffers */
void camera_core_deinit(void);

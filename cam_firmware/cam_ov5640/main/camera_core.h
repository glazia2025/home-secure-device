#pragma once
#include "esp_err.h"
#include "esp_camera.h"

esp_err_t camera_core_init(void);
void camera_core_deinit(void);

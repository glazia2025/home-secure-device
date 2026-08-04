#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t hub_sensor_init(void);
bool hub_sensor_get_latest(float *out_temp_c, float *out_hum_pct);

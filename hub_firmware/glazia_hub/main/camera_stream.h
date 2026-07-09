#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Idempotent — safe to call when camera is already running. */
esp_err_t camera_ensure_init(void);

/* No-op if camera is not initialized. */
void camera_deinit(void);

bool camera_is_initialized(void);

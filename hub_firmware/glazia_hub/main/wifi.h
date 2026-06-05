#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Initialize ESP-NETIF and the default event loop before app peripherals.
esp_err_t wifi_platform_init(void);

// Connect to WiFi using given credentials
// Calls api_register_hub() automatically on success
void wifi_connect(const char *ssid, const char *password);

// Fingerprint-gated offline mode helpers. Keep TFT/app task alive while
// stopping network radios used by hub operation.
void wifi_enter_offline_mode(void);
bool wifi_resume_from_offline_mode(void);

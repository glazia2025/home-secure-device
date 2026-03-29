#pragma once

// Connect to WiFi using given credentials
// Calls api_register_hub() automatically on success
void wifi_connect(const char *ssid, const char *password);

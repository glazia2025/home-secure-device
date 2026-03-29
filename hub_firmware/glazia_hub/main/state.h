#pragma once

#include <stdbool.h>
#include <stdint.h>

// ── Hub operating modes ───────────────────────────────────────────────────
typedef enum {
    MODE_IDLE,            // Just booted, waiting for button press
    MODE_HUB_PAIRING,     // 1st button press — BLE on, waiting for WiFi creds
    MODE_WIFI_CONNECTING, // Got creds, connecting to WiFi
    MODE_REGISTERING,     // On WiFi, calling /api/device/hubs/register
    MODE_OPERATIONAL,     // Registered, normal operation
    MODE_SENSOR_PAIRING,  // 2nd button press — WebSocket open, waiting for sensor MAC
} hub_mode_t;

// ── Global state ──────────────────────────────────────────────────────────
extern hub_mode_t g_mode;

// WiFi credentials received over BLE
extern char g_wifi_ssid[64];
extern char g_wifi_password[64];

// Provisioning token received over BLE
extern char g_provisioning_token[64];

// Hub identity (received from server after registration)
extern char g_hub_mac[18];       // "AA:BB:CC:DD:EE:FF"
extern char g_hub_secret[128];   // returned by /register
extern char g_home_id[64];       // returned by /register
extern char g_home_name[64];     // returned by /register

// Server config
#define SERVER_IP    "10.182.205.6"
#define SERVER_PORT  8000
#define SERVER_BASE  "http://10.182.205.6:8000"
#define WS_URI       "ws://10.182.205.6:8000/api/device/hubs/ws"

// Device API key
#define DEVICE_API_KEY "glazia-dev-key"

// GPIO pins
#define BUTTON_GPIO   9
#define TFT_CS        18
#define TFT_RST       19
#define TFT_DC        20
#define TFT_MOSI      21
#define TFT_CLK       22
#define TFT_BL        -1   // tied to 3V3, no GPIO control needed

// BLE
#define BLE_DEVICE_NAME "GlaziaHub"

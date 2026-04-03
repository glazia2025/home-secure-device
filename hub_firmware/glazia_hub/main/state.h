#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MODE_IDLE,
    MODE_HUB_PAIRING,
    MODE_WIFI_CONNECTING,
    MODE_REGISTERING,
    MODE_OPERATIONAL,
    MODE_SENSOR_PAIRING,
} hub_mode_t;

// ── Global state ──────────────────────────────────────────────────────────
extern hub_mode_t g_mode;
extern char g_wifi_ssid[64];
extern char g_wifi_password[64];
extern char g_provisioning_token[64];
extern char g_hub_mac[18];
extern char g_hub_secret[128];
extern char g_home_id[64];
extern char g_home_name[64];

// ── Server Config ─────────────────────────────────────────────────────────
#define SERVER_IP      "10.182.205.6"
#define SERVER_PORT    8000
#define SERVER_BASE    "http://10.182.205.6:8000"
#define WS_URI         "ws://10.182.205.6:8000/api/device/hubs/ws"
#define DEVICE_API_KEY "glazia-dev-key"
#define BLE_DEVICE_NAME "GlaziaHub"

// ── ESP32-S3 GPIO PINS ────────────────────────────────────────────────────
#define BUTTON_GPIO   21   // General purpose GPIO, safe on ESP32-S3-N16R8

// Display pins (Ignoring since not connected, but keeping safe values)
#define TFT_CS        1
#define TFT_RST       2
#define TFT_DC        3
#define TFT_MOSI      4
#define TFT_CLK       5
#define TFT_BL        -1

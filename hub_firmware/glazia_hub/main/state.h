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
extern char g_user_name[64];

// Pending sensor pairing — populated by api_fetch_sensor_pairing(), consumed by espnow
extern char g_pending_sensor_mac[18];    // e.g. "AA:BB:CC:DD:EE:FF"
extern char g_pending_provision_key[33]; // 32-char hex of 16-byte LMK

// ── Server Config ─────────────────────────────────────────────────────────
#define SERVER_IP      "10.208.134.6"
#define SERVER_PORT    3000
#define SERVER_BASE    "http://10.208.134.6:3000"
#define DEVICE_API_KEY "glazia-device-dev-key"
#define BLE_DEVICE_NAME "GlaziaHub"

// ── ESP-NOW Security ──────────────────────────────────────────────────────
// PMK: 16-byte Primary Master Key — must match on hub and all sensors.
// The per-sensor LMK (provision_key) is delivered out-of-band (BLE→server→HTTP).
#define GLAZIA_ESP_NOW_PMK  "glz!dev.pmk.2024"   // exactly 16 bytes

// ── ESP32-S3 GPIO PINS ────────────────────────────────────────────────────
#define BUTTON_GPIO   21   // General purpose GPIO, safe on ESP32-S3-N16R8

// Display — S3 talks to ESP32-C6 over UART; C6 drives the TFT via LVGL
// Wire: S3 GPIO17 → C6 GPIO9 (RX)
//       S3 GPIO18 ← C6 GPIO14 (TX, optional — only needed for ALIVE heartbeat)
//       GND shared between both boards
#define DISP_UART_TX  17
#define DISP_UART_RX  18

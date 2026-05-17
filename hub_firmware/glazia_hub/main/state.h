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
#define SERVER_IP      "10.148.212.6"
#define SERVER_PORT    3000
#define SERVER_BASE    "http://10.148.212.6:3000"
#define DEVICE_API_KEY "glazia-device-dev-key"
#define BLE_DEVICE_NAME "GlaziaHub"

// ── ESP-NOW Security ──────────────────────────────────────────────────────
// PMK: 16-byte Primary Master Key — must match on hub and all sensors.
// The per-sensor LMK (provision_key) is delivered out-of-band (BLE→server→HTTP).
#define GLAZIA_ESP_NOW_PMK  "glz!dev.pmk.2024"   // exactly 16 bytes

// ── ESP32-S3 GPIO PINS ────────────────────────────────────────────────────
#define BUTTON_GPIO   21   // General purpose GPIO, safe on ESP32-S3-N16R8

// Display — S3 drives ILI9341 TFT directly over SPI (no C6 bridge)
// GPIO4 (MOSI) is intentionally off the SPI2 IOMUX to force GPIO-matrix routing.
// Wire: MOSI=GPIO4, SCK=GPIO12, CS=GPIO10, DC=GPIO13, RST=GPIO14
//       LED/BL → 3.3 V, GND shared
#define LCD_PIN_MOSI   4
#define LCD_PIN_SCK   12
#define LCD_PIN_CS    10
#define LCD_PIN_DC    13
#define LCD_PIN_RST   14

// Touch — XPT2046 resistive touch controller on same SPI2 bus as LCD
// T_CLK shares LCD_PIN_SCK (GPIO12), T_DIN shares LCD_PIN_MOSI (GPIO4)
// Wire: T_OUT→GPIO2, T_CS→GPIO5, T_IRQ→GPIO17
//       (set TOUCH_PIN_IRQ to -1 if T_IRQ left unwired)
#define TOUCH_PIN_MISO  2   // T_OUT
#define TOUCH_PIN_CS     5   // T_CS
#define TOUCH_PIN_IRQ   17   // T_IRQ

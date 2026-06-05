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
    MODE_FINGERPRINT_ENROLL,
    MODE_FINGERPRINT_VERIFY,
    MODE_OFFLINE,
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
#define SERVER_IP      "10.242.127.6"
#define SERVER_PORT    3000
#define SERVER_BASE    "http://10.242.127.6:3000"
#define DEVICE_API_KEY "glazia-device-dev-key"
#define BLE_DEVICE_NAME "GlaziaHub"

// ── ESP-NOW Security ──────────────────────────────────────────────────────
// PMK: 16-byte Primary Master Key — must match on hub and all sensors.
// The per-sensor LMK (provision_key) is delivered out-of-band (BLE→server→HTTP).
#define GLAZIA_ESP_NOW_PMK  "glz!dev.pmk.2024"   // exactly 16 bytes

// ── ESP32-S3 GPIO PINS ────────────────────────────────────────────────────
#define BUTTON_GPIO   19   // Active-low button; GPIO19 is USB D+ on many S3 boards

// Display — S3 drives ILI9341 TFT directly over SPI (no C6 bridge)
// Wire: MOSI=GPIO38, SCK=GPIO39, CS=GPIO40, DC=GPIO41, RST=GPIO42
//       LED/BL → 3.3 V, GND shared
#define LCD_PIN_MOSI  38
#define LCD_PIN_SCK   39
#define LCD_PIN_CS    40
#define LCD_PIN_DC    41
#define LCD_PIN_RST   42

// Touch — XPT2046 resistive touch controller on same SPI2 bus as LCD
// T_CLK shares LCD_PIN_SCK (GPIO39), T_DIN shares LCD_PIN_MOSI (GPIO38)

#define TOUCH_PIN_MISO 47   // T_OUT
#define TOUCH_PIN_CS   48   // T_CS
#define TOUCH_PIN_IRQ  21   // T_IRQ

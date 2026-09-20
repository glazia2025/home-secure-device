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

// TURN credentials — loaded from NVS; fall back to compile-time defaults below
extern char g_turn_user[64];
extern char g_turn_psw[64];

// ── Firmware Version ──────────────────────────────────────────────────────
#define HUB_FIRMWARE_VERSION "1.0.0"

// ── Server Config ─────────────────────────────────────────────────────────
#define SERVER_IP      "home-secure.glazia.in"
#define SERVER_PORT    443
#define SERVER_BASE    "https://home-secure.glazia.in"
#define DEVICE_API_KEY "replace-this-too"
#define BLE_DEVICE_NAME "GlaziaHub"

// ── TURN server credentials (dev defaults — rotate before production) ─────
#define TURN_DEFAULT_USER "xio"
#define TURN_DEFAULT_PSW  "xio@1234"

// ── ESP-NOW Security ──────────────────────────────────────────────────────
// PMK: 16-byte Primary Master Key — must match on hub and all sensors.
// The per-sensor LMK (provision_key) is delivered out-of-band (BLE→server→HTTP).
#define GLAZIA_ESP_NOW_PMK  "glz!dev.pmk.2024"   // exactly 16 bytes

// ── nRF Thread co-chip IPC ────────────────────────────────────────────────
// Set to 1 to restore original ESP-NOW mesh; 0 = nrf_test Thread mesh.
#define CONFIG_ESPNOW_ENABLE  0
// Console is on USB-Serial-JTAG (sdkconfig), freeing UART0 for nRF IPC.
// FireBeetle 2 ESP32-P4 header pins: silk "37/T" (GPIO37) = hub TX, "38/R" (GPIO38) = hub RX.
// nRF XIAO: D0(P0.02)=TX → GPIO38(P4 RX); D1(P0.03)=RX ← GPIO37(P4 TX).
#define NRF_UART_NUM          UART_NUM_0
#define NRF_UART_TX_GPIO      37
#define NRF_UART_RX_GPIO      38
#define NRF_UART_BAUD         115200

// ── ESP32-P4 GPIO PINS (FireBeetle 2 ESP32-P4 V1.0) ───────────────────────
// The display is a native MIPI-DSI panel (Waveshare 7-DSI-TOUCH-A, ILI9881C
// 720x1280 + GT911 touch) driven directly by the P4 — see display.c. Its DSI
// lanes are dedicated P4 pins; the GT911 touch + backlight (0x45) ride the
// shared board I2C (I2C_NUM_0, GPIO7/8 — see display_power.c). No display SPI/
// UART pins here (the old ESP32-S3 coprocessor + UART3 bridge were removed).
//
// Peripheral pin map (also see each driver's local #defines):
//   nRF Thread IPC   UART0  TX=37  RX=38   (this block)
//   Camera OV5647    MIPI-CSI (native)     (camera_csi.c)
//   Display (DSI)    MIPI-DSI (native); touch/backlight I2C0 GPIO7/8 (display.c)
//   PMS7003 (AQI)    UART1  RX=20 (TX unused)  (aqi_sensor.c — SET/RESET tied 3.3V)
//   R307 fingerprint UART2  TX=4   RX=5    (fingerprint.c)
//   Door lock relay  GPIO50                (door_lock.c)
//   DHT22 (ambient)  GPIO52                (hub_sensor.c)

// ── OV5647 camera on MIPI-CSI (camera_csi.c) ──────────────────────────────
// The CSI D+/D-/CLK lanes are dedicated P4 pins (not GPIO-muxable). Only the
// sensor CONTROL lines below are board-specific. Values are FireBeetle-2-P4 /
// ESP-IDF reference defaults — VERIFY against your board before flashing.
//   SCCB is the sensor's I2C control bus. XCLK is board-generated on the
//   FireBeetle-2-P4 camera connector, so CAM_XCLK_GPIO = -1 (no MCU clock out);
//   set it to a GPIO if your board expects the P4 to drive XCLK.
#define CAM_SCCB_I2C_PORT        0        /* I2C_NUM_0 */    // verify against your board
#define CAM_SCCB_SDA_GPIO        7                            // verify against your board
#define CAM_SCCB_SCL_GPIO        8                            // verify against your board
#define CAM_SCCB_FREQ_HZ         100000
#define CAM_RESET_GPIO           (-1)                         // verify against your board
#define CAM_PWDN_GPIO            (-1)                         // verify against your board
#define CAM_XCLK_GPIO            (-1)                         // board-generated; verify
#define CAM_LDO_CHAN_ID          3        /* MIPI D-PHY LDO */// verify against your board
#define CAM_LDO_VOLTAGE_MV       2500                         // verify against your board
#define CAM_CSI_LANE_NUM         2
#define CAM_CSI_LANE_BITRATE_MBPS 200

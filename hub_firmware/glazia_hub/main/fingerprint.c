/**
 * fingerprint.c — R307 Capacitive Fingerprint Sensor Driver
 *
 * UART2-based driver for the R307 fingerprint sensor module.
 * Implements the Zhejiang ZLG binary packet protocol for enrollment and verification.
 * Stores one master fingerprint template in the R307's onboard flash (page 1).
 */

#include "fingerprint.h"
#include "nvs_storage.h"
#include "state.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <stddef.h>

static const char *TAG = "FINGERPRINT";

/* ── Configuration ────────────────────────────────────────────────────────── */
#define FP_UART_NUM          UART_NUM_2
#define FP_TX_PIN            43
#define FP_RX_PIN            44
#define FP_BAUD_RATE         57600
#define FP_BUF_SIZE          256
#define FP_RX_TIMEOUT_MS     2000
#define FP_CMD_TIMEOUT_MS    5000
#define FP_ENROLL_SLOT       1  /* Store master template in slot 1 */

/* ── R307 Protocol Constants ─────────────────────────────────────────────── */
#define FP_HEADER_1          0xEF
#define FP_HEADER_2          0x01
#define FP_ADDR              {0x00, 0x00, 0x00, 0x00}
#define FP_PKT_CMD           0x01  /* Packet identifier: command */
#define FP_PKT_DATA          0x07  /* Packet identifier: data */
#define FP_PKT_ACK           0x07  /* Acknowledgement (same as data) */
#define FP_PKT_ERROR         0x08  /* Error response */

/* ── R307 Command Codes ──────────────────────────────────────────────────── */
#define CMD_GET_IMAGE        0x01
#define CMD_IMG2TZ           0x02
#define CMD_MATCH            0x03
#define CMD_SEARCH           0x04
#define CMD_REG_MODEL        0x05
#define CMD_STORE_CHAR       0x06
#define CMD_LOAD_CHAR        0x07
#define CMD_DELETE_CHAR      0x08
#define CMD_EMPTY            0x0D
#define CMD_GET_PARAM        0x0E
#define CMD_SET_PARAM        0x0F

/* ── Response Confirmation Codes ────────────────────────────────────────── */
#define CONFIRM_OK           0x00
#define CONFIRM_FAIL         0x01
#define CONFIRM_TIMEOUT      0x02

/* ── Global State ────────────────────────────────────────────────────────── */
static bool                  s_initialized = false;
static fp_display_cb         s_display_cb = NULL;
static QueueHandle_t         s_uart_queue = NULL;

/* ── Helper: Display Prompt (safe to call anytime) ────────────────────────── */
static inline void fp_display(const char *msg)
{
    if (s_display_cb) {
        s_display_cb(msg);
    }
}

/* ── UART Packet Structure ────────────────────────────────────────────────── */
typedef struct {
    uint8_t header[2];       /* 0xEF 0x01 */
    uint8_t address[4];      /* 00 00 00 00 */
    uint8_t pid;             /* 0x01 = command, 0x07 = data/ack */
    uint8_t length[2];       /* big-endian: length of data + pid + checksum */
    uint8_t data[128];       /* command + parameters */
    uint8_t checksum[2];     /* big-endian */
} fp_packet_t;

/**
 * Calculate checksum (sum of all bytes from pid through data, big-endian).
 */
static uint16_t fp_checksum(const uint8_t *buf, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += buf[i];
    }
    return sum;
}

/**
 * Build and send a command packet.
 * cmd: command code (CMD_GET_IMAGE, etc.)
 * data: optional command parameters
 * data_len: length of parameters
 */
static esp_err_t fp_send_cmd(uint8_t cmd, const uint8_t *data, size_t data_len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t pkt[FP_BUF_SIZE];
    size_t pkt_len = 0;

    pkt[pkt_len++] = FP_HEADER_1;
    pkt[pkt_len++] = FP_HEADER_2;
    pkt[pkt_len++] = 0x00;
    pkt[pkt_len++] = 0x00;
    pkt[pkt_len++] = 0x00;
    pkt[pkt_len++] = 0x00;
    pkt[pkt_len++] = FP_PKT_CMD;

    /* Length = 1 (cmd) + data_len + 2 (checksum) */
    uint16_t len = 1 + data_len + 2;
    pkt[pkt_len++] = (len >> 8) & 0xFF;
    pkt[pkt_len++] = len & 0xFF;

    /* Append command code */
    pkt[pkt_len++] = cmd;

    /* Append parameters */
    if (data_len > 0) {
        memcpy(&pkt[pkt_len], data, data_len);
        pkt_len += data_len;
    }

    /* Calculate and append checksum (from pid to end of data) */
    uint16_t cs = fp_checksum(&pkt[6], 1 + data_len);
    pkt[pkt_len++] = (cs >> 8) & 0xFF;
    pkt[pkt_len++] = cs & 0xFF;

    /* Send to UART */
    uart_write_bytes(FP_UART_NUM, (const char *)pkt, pkt_len);
    ESP_LOGD(TAG, "Sent cmd 0x%02X, len=%u", cmd, pkt_len);

    return ESP_OK;
}

/**
 * Receive and parse a response packet.
 * Returns confirmation code (CONFIRM_OK, CONFIRM_FAIL, etc.) or negative on timeout.
 * out_data: optional buffer for response data (beyond confirmation code).
 * out_len: optional output length of data received.
 */
static int fp_recv_response(uint8_t *out_data, size_t *out_len)
{
    if (!s_initialized) return -1;

    uint8_t buf[FP_BUF_SIZE];
    size_t idx = 0;
    int64_t deadline_ms = esp_log_timestamp() + FP_CMD_TIMEOUT_MS;

    /* Read entire packet (header + address + pid + length + data + checksum) */
    while (idx < FP_BUF_SIZE) {
        int bytes = uart_read_bytes(FP_UART_NUM, &buf[idx], 1, pdMS_TO_TICKS(100));
        if (bytes < 0) {
            if (esp_log_timestamp() > deadline_ms) {
                ESP_LOGW(TAG, "Response timeout");
                return -1;
            }
            continue;
        }

        idx += bytes;

        /* Wait for at least header (2) + address (4) + pid (1) + length (2) = 9 bytes */
        if (idx < 9) continue;

        /* Check header */
        if (buf[0] != FP_HEADER_1 || buf[1] != FP_HEADER_2) {
            ESP_LOGW(TAG, "Bad header: %02X %02X", buf[0], buf[1]);
            return -1;
        }

        /* Parse length field */
        uint16_t pkt_len = ((uint16_t)buf[8] << 8) | buf[9];
        size_t total_needed = 10 + pkt_len;

        if (idx >= total_needed) {
            /* Full packet received */
            uint8_t confirm = buf[10];  /* First byte of data is confirmation */
            ESP_LOGD(TAG, "Response confirm=0x%02X, pkt_len=%u", confirm, pkt_len);

            if (out_data && pkt_len > 1) {
                size_t data_len = pkt_len - 1 - 2;  /* Exclude confirm + checksum */
                if (data_len > 0 && out_len) {
                    memcpy(out_data, &buf[11], data_len);
                    *out_len = data_len;
                }
            }

            return (int)confirm;
        }

        if (idx >= total_needed) break;
    }

    ESP_LOGW(TAG, "Packet too large or incomplete");
    return -1;
}

/**
 * Send command and wait for response.
 * Returns confirmation code on success, or negative on error.
 */
static int fp_cmd(uint8_t cmd, const uint8_t *data, size_t data_len,
                  uint8_t *out_data, size_t *out_len)
{
    fp_send_cmd(cmd, data, data_len);
    return fp_recv_response(out_data, out_len);
}

/**
 * Single-argument command helper (common case: command + 1 parameter byte).
 */
static int fp_cmd1(uint8_t cmd, uint8_t param)
{
    return fp_cmd(cmd, &param, 1, NULL, NULL);
}

/* ── Fingerprint Enrollment ──────────────────────────────────────────────── */

esp_err_t fp_enroll(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    fp_display("Place finger to enroll");
    vTaskDelay(pdMS_TO_TICKS(500));

    /* First scan: Get image and convert to template in buffer 1 */
    for (int attempt = 0; attempt < 5; attempt++) {
        fp_display("Scanning... (1/2)");
        int conf = fp_cmd1(CMD_GET_IMAGE, 0);
        if (conf == CONFIRM_OK) {
            conf = fp_cmd1(CMD_IMG2TZ, 1);  /* Buffer 1 */
            if (conf == CONFIRM_OK) {
                ESP_LOGI(TAG, "First scan successful");
                fp_display("Scan 1 complete.\nPlace finger again.");
                vTaskDelay(pdMS_TO_TICKS(1500));
                break;
            }
        }
        if (attempt < 4) {
            fp_display("No finger. Try again.");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Second scan: Get image and convert to template in buffer 2 */
    for (int attempt = 0; attempt < 5; attempt++) {
        fp_display("Scanning... (2/2)");
        int conf = fp_cmd1(CMD_GET_IMAGE, 0);
        if (conf == CONFIRM_OK) {
            conf = fp_cmd1(CMD_IMG2TZ, 2);  /* Buffer 2 */
            if (conf == CONFIRM_OK) {
                ESP_LOGI(TAG, "Second scan successful");
                break;
            }
        }
        if (attempt < 4) {
            fp_display("No finger. Try again.");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Combine buffers into model */
    int conf = fp_cmd1(CMD_REG_MODEL, 0);
    if (conf != CONFIRM_OK) {
        ESP_LOGE(TAG, "RegModel failed: 0x%02X", conf);
        fp_display("Enrollment failed.");
        return ESP_FAIL;
    }

    /* Store to R307 onboard flash, page 1 (master template) */
    uint8_t store_params[] = {FP_ENROLL_SLOT >> 8, FP_ENROLL_SLOT & 0xFF};
    conf = fp_cmd(CMD_STORE_CHAR, store_params, sizeof(store_params), NULL, NULL);
    if (conf != CONFIRM_OK) {
        ESP_LOGE(TAG, "StoreChar failed: 0x%02X", conf);
        fp_display("Storage failed.");
        return ESP_FAIL;
    }

    /* Mark as enrolled in NVS */
    nvs_save_fp_enrolled(true);

    ESP_LOGI(TAG, "Enrollment successful, template stored in slot %d", FP_ENROLL_SLOT);
    fp_display("Enrollment complete!");
    vTaskDelay(pdMS_TO_TICKS(1500));

    return ESP_OK;
}

/* ── Fingerprint Verification ────────────────────────────────────────────── */

esp_err_t fp_verify(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!fp_is_enrolled()) {
        fp_display("No fingerprint enrolled.");
        return ESP_FAIL;
    }

    fp_display("Place your finger...");

    /* Capture and convert to buffer 1 */
    for (int attempt = 0; attempt < 3; attempt++) {
        int conf = fp_cmd1(CMD_GET_IMAGE, 0);
        if (conf == CONFIRM_OK) {
            conf = fp_cmd1(CMD_IMG2TZ, 1);
            if (conf == CONFIRM_OK) {
                fp_display("Verifying...");
                break;
            }
        }
        if (attempt < 2) {
            fp_display("Not detected. Try again.");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Search against all stored templates (master is at slot 1) */
    uint8_t search_params[] = {
        0x00, 0x01,      /* Start page 0, search 1 template */
        0x00, 0x7F,      /* Search up to page 127 (whole database) */
    };
    uint8_t search_data[4];
    size_t search_data_len = sizeof(search_data);

    int conf = fp_cmd(CMD_SEARCH, search_params, sizeof(search_params),
                      search_data, &search_data_len);

    if (conf == CONFIRM_OK && search_data_len >= 4) {
        uint16_t match_slot = ((uint16_t)search_data[0] << 8) | search_data[1];
        uint16_t match_score = ((uint16_t)search_data[2] << 8) | search_data[3];
        ESP_LOGI(TAG, "Match found: slot=%u, score=%u", match_slot, match_score);
        fp_display("Access granted!");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "No match found");
    fp_display("Access denied.");
    vTaskDelay(pdMS_TO_TICKS(1500));
    return ESP_FAIL;
}

/* ── NVS Integration ─────────────────────────────────────────────────────── */

bool fp_is_enrolled(void)
{
    return nvs_load_fp_enrolled();
}

/* ── Initialization ──────────────────────────────────────────────────────── */

esp_err_t fp_init(void)
{
    if (s_initialized) return ESP_OK;

    /* Configure UART2 */
    uart_config_t cfg = {
        .baud_rate = FP_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_ODD,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    esp_err_t err = uart_param_config(FP_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(FP_UART_NUM, FP_TX_PIN, FP_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(FP_UART_NUM, FP_BUF_SIZE, 0, 10, &s_uart_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Fingerprint sensor initialized on UART%d", FP_UART_NUM);

    return ESP_OK;
}

void fp_set_display_cb(fp_display_cb cb)
{
    s_display_cb = cb;
    ESP_LOGI(TAG, "Display callback registered");
}

/* ── Fingerprint Verification Task ───────────────────────────────────────── */

void fingerprint_verify_task(void *arg)
{
    lv_obj_t *sw = (lv_obj_t *)arg;

    ESP_LOGI(TAG, "Fingerprint verification task started");

    /* Perform fingerprint verification */
    esp_err_t result = fp_verify();

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Fingerprint verification successful");
        g_mode = MODE_OPERATIONAL;
        /* Toggle remains OFF (verified successfully) */
    } else {
        ESP_LOGW(TAG, "Fingerprint verification failed");
        g_mode = MODE_OPERATIONAL;
        /* Revert toggle back to ON if it's still OFF */
        if (lvgl_port_lock(pdMS_TO_TICKS(100))) {
            if (!lv_obj_has_state(sw, LV_STATE_CHECKED)) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            }
            lvgl_port_unlock();
        }
    }

    vTaskDelete(NULL);
}

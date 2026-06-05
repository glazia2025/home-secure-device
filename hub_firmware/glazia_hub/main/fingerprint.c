/**
 * R307 fingerprint sensor driver.
 *
 * Protocol and capture flow are adapted from hub_firmware/fs_test/main/fs_test.c,
 * which is the verified working ESP-IDF sample for this hardware.
 *
 * Wiring for this prototype:
 *   R307 TX -> ESP32-S3 GPIO2 (UART2 RX)
 *   R307 RX -> ESP32-S3 GPIO1 (UART2 TX)
 */

#include "fingerprint.h"
#include "display.h"
#include "nvs_storage.h"
#include "state.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "FINGERPRINT";

#define R307_UART_NUM UART_NUM_2
#define R307_TX_PIN 1
#define R307_RX_PIN 2
#define R307_BAUD_RATE 57600
#define R307_RX_BUF_SIZE 256
#define R307_CMD_TIMEOUT_MS 5000

#define R307_HEADER_1 0xEF
#define R307_HEADER_2 0x01
#define R307_PKT_COMMAND 0x01
#define R307_PKT_ACK 0x07

#define R307_CMD_GEN_IMG 0x01
#define R307_CMD_IMG2TZ 0x02
#define R307_CMD_SEARCH 0x04
#define R307_CMD_REG_MODEL 0x05
#define R307_CMD_STORE 0x06

#define R307_OK 0x00
#define R307_ERR_PACKET 0x01
#define R307_ERR_NO_FINGER 0x02
#define R307_ERR_IMAGE_FAIL 0x03
#define R307_ERR_IMG2TZ_MESSY 0x06
#define R307_ERR_IMG2TZ_FEW_POINTS 0x07
#define R307_ERR_NO_MATCH 0x09
#define R307_ERR_REG_MODEL_FAIL 0x0A
#define R307_ERR_BAD_PAGE 0x0B
#define R307_ERR_INVALID_RANGE 0x17
#define R307_ERR_FLASH_WRITE 0x18

#define FP_ENROLL_SLOT 1
#define FP_ID "id_01"
#define FP_MAX_PRINTS 5
#define CAPTURE_RETRY_DELAY_MS 500
#define FP_SCAN_START_DELAY_MS 5000
#define FP_ENROLL_WINDOW_MS 15000
#define FP_VERIFY_WINDOW_MS 10000
#define FP_RETRY_DELAY_MS 2500
#define FP_VERIFY_ATTEMPTS 3
#define FP_ENROLL_SECOND_CAPTURE_DELAY_MS 1500
#define FP_SUCCESS_RESULT_HOLD_MS 900

typedef struct {
    uint8_t confirm;
    uint8_t data[32];
    size_t data_len;
} r307_response_t;

static bool s_initialized = false;
static fp_display_cb s_display_cb = NULL;
static TaskHandle_t s_enroll_task = NULL;

static void fp_display(const char *msg)
{
    if (s_display_cb) {
        s_display_cb(msg);
    }
}

static bool tick_before(TickType_t now, TickType_t deadline)
{
    return (int32_t)(deadline - now) > 0;
}

static void delay_until_tick(TickType_t deadline)
{
    TickType_t now = xTaskGetTickCount();
    if (tick_before(now, deadline)) {
        vTaskDelay(deadline - now);
    }
}

static void update_window_progress(TickType_t start, TickType_t deadline)
{
    TickType_t now = xTaskGetTickCount();
    if (!tick_before(now, deadline)) {
        display_fingerprint_progress(100);
        return;
    }

    TickType_t total = deadline - start;
    TickType_t elapsed = now - start;
    uint8_t percent = total > 0 ? (uint8_t)((elapsed * 100) / total) : 100;
    display_fingerprint_progress(percent);
}

static void delay_until_tick_with_progress(TickType_t start, TickType_t deadline, TickType_t wait_until)
{
    while (tick_before(xTaskGetTickCount(), wait_until)) {
        update_window_progress(start, deadline);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static uint16_t r307_checksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;

    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }

    return sum;
}

static const char *r307_confirm_name(uint8_t confirm)
{
    switch (confirm) {
    case R307_OK:
        return "ok";
    case R307_ERR_PACKET:
        return "packet receive error";
    case R307_ERR_NO_FINGER:
        return "no finger detected";
    case R307_ERR_IMAGE_FAIL:
        return "failed to collect finger image";
    case R307_ERR_IMG2TZ_MESSY:
        return "image too messy for character file";
    case R307_ERR_IMG2TZ_FEW_POINTS:
        return "too few feature points";
    case R307_ERR_NO_MATCH:
        return "no matching fingerprint";
    case R307_ERR_REG_MODEL_FAIL:
        return "failed to combine character files";
    case R307_ERR_BAD_PAGE:
        return "template page is out of range";
    case R307_ERR_INVALID_RANGE:
        return "invalid search range";
    case R307_ERR_FLASH_WRITE:
        return "flash write failed";
    default:
        return "unknown confirmation code";
    }
}

static esp_err_t r307_uart_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = R307_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_LOGI(TAG, "R307 UART%d driver install starting", R307_UART_NUM);
    esp_err_t err = uart_driver_install(R307_UART_NUM, R307_RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "R307 UART%d driver install complete", R307_UART_NUM);

    ESP_LOGI(TAG, "R307 UART%d param config starting", R307_UART_NUM);
    err = uart_param_config(R307_UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "R307 UART%d param config complete", R307_UART_NUM);

    ESP_LOGI(TAG, "R307 UART%d pin config starting", R307_UART_NUM);
    err = uart_set_pin(R307_UART_NUM, R307_TX_PIN, R307_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART pin config failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "R307 UART%d pin config complete", R307_UART_NUM);

    ESP_LOGI(TAG, "R307 UART%d initialized: ESP TX=GPIO%d ESP RX=GPIO%d baud=%d",
             R307_UART_NUM, R307_TX_PIN, R307_RX_PIN, R307_BAUD_RATE);
    return ESP_OK;
}

static esp_err_t r307_send_cmd(uint8_t cmd, const uint8_t *params, size_t params_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (params_len > 32) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t pkt[48];
    size_t idx = 0;

    pkt[idx++] = R307_HEADER_1;
    pkt[idx++] = R307_HEADER_2;
    pkt[idx++] = 0xFF;
    pkt[idx++] = 0xFF;
    pkt[idx++] = 0xFF;
    pkt[idx++] = 0xFF;
    pkt[idx++] = R307_PKT_COMMAND;

    const uint16_t pkt_len = (uint16_t)(1 + params_len + 2);
    pkt[idx++] = (uint8_t)(pkt_len >> 8);
    pkt[idx++] = (uint8_t)(pkt_len & 0xFF);
    pkt[idx++] = cmd;

    if (params_len > 0) {
        memcpy(&pkt[idx], params, params_len);
        idx += params_len;
    }

    const uint16_t checksum = r307_checksum(&pkt[6], 4 + params_len);
    pkt[idx++] = (uint8_t)(checksum >> 8);
    pkt[idx++] = (uint8_t)(checksum & 0xFF);

    uart_flush_input(R307_UART_NUM);

    const int written = uart_write_bytes(R307_UART_NUM, (const char *)pkt, idx);
    if (written != (int)idx) {
        ESP_LOGE(TAG, "UART write failed for cmd 0x%02X: wrote %d/%u",
                 cmd, written, (unsigned)idx);
        return ESP_FAIL;
    }

    ESP_LOG_BUFFER_HEXDUMP(TAG, pkt, idx, ESP_LOG_DEBUG);
    return ESP_OK;
}

static esp_err_t r307_recv_ack(r307_response_t *response)
{
    uint8_t header[9];
    int got = uart_read_bytes(R307_UART_NUM, header, sizeof(header),
                              pdMS_TO_TICKS(R307_CMD_TIMEOUT_MS));
    if (got != (int)sizeof(header)) {
        ESP_LOGW(TAG, "R307 ACK header timeout (%d/%u bytes)", got, (unsigned)sizeof(header));
        return ESP_ERR_TIMEOUT;
    }

    if (header[0] != R307_HEADER_1 || header[1] != R307_HEADER_2 ||
        header[6] != R307_PKT_ACK) {
        ESP_LOGW(TAG, "Bad ACK header: %02X %02X pid=%02X", header[0], header[1], header[6]);
        return ESP_FAIL;
    }

    const uint16_t pkt_len = ((uint16_t)header[7] << 8) | header[8];
    if (pkt_len < 3 || pkt_len > R307_RX_BUF_SIZE) {
        ESP_LOGW(TAG, "Invalid ACK packet length: %u", pkt_len);
        return ESP_FAIL;
    }

    uint8_t body[R307_RX_BUF_SIZE];
    got = uart_read_bytes(R307_UART_NUM, body, pkt_len, pdMS_TO_TICKS(R307_CMD_TIMEOUT_MS));
    if (got != pkt_len) {
        ESP_LOGW(TAG, "R307 ACK body timeout (%d/%u bytes)", got, pkt_len);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t checksum_data[R307_RX_BUF_SIZE + 3];
    checksum_data[0] = header[6];
    checksum_data[1] = header[7];
    checksum_data[2] = header[8];
    memcpy(&checksum_data[3], body, pkt_len - 2);

    const uint16_t expected = r307_checksum(checksum_data, 3 + pkt_len - 2);
    const uint16_t received = ((uint16_t)body[pkt_len - 2] << 8) | body[pkt_len - 1];
    if (expected != received) {
        ESP_LOGW(TAG, "ACK checksum mismatch expected=%04X received=%04X", expected, received);
        return ESP_FAIL;
    }

    response->confirm = body[0];
    response->data_len = pkt_len - 3;
    if (response->data_len > sizeof(response->data)) {
        response->data_len = sizeof(response->data);
    }
    if (response->data_len > 0) {
        memcpy(response->data, &body[1], response->data_len);
    }

    ESP_LOGD(TAG, "R307 confirm=0x%02X (%s)", response->confirm,
             r307_confirm_name(response->confirm));
    return ESP_OK;
}

static esp_err_t r307_cmd(uint8_t cmd, const uint8_t *params, size_t params_len,
                          r307_response_t *response)
{
    r307_response_t local_response = {0};
    if (response == NULL) {
        response = &local_response;
    }

    esp_err_t err = r307_send_cmd(cmd, params, params_len);
    if (err != ESP_OK) {
        return err;
    }

    return r307_recv_ack(response);
}

static uint8_t r307_cmd_confirm(uint8_t cmd, const uint8_t *params, size_t params_len)
{
    r307_response_t response = {0};
    esp_err_t err = r307_cmd(cmd, params, params_len, &response);
    if (err != ESP_OK) {
        return 0xFF;
    }

    return response.confirm;
}

static bool r307_capture_to_buffer_until(uint8_t buffer_id, TickType_t start, TickType_t deadline)
{
    int attempt = 1;
    while (tick_before(xTaskGetTickCount(), deadline)) {
        update_window_progress(start, deadline);
        uint8_t confirm = r307_cmd_confirm(R307_CMD_GEN_IMG, NULL, 0);
        if (confirm == R307_OK) {
            const uint8_t params[] = {buffer_id};
            confirm = r307_cmd_confirm(R307_CMD_IMG2TZ, params, sizeof(params));
            if (confirm == R307_OK) {
                ESP_LOGI(TAG, "Captured image into character buffer %u", buffer_id);
                return true;
            }

            ESP_LOGW(TAG, "Image conversion failed: 0x%02X (%s)",
                     confirm, r307_confirm_name(confirm));
            return false;
        }

        if (confirm != R307_ERR_NO_FINGER) {
            ESP_LOGW(TAG, "Capture attempt %d failed: 0x%02X (%s)",
                     attempt, confirm, r307_confirm_name(confirm));
        } else if (attempt == 1 || attempt == 5 || attempt == 9) {
            ESP_LOGI(TAG, "No finger detected yet; keep finger placed on sensor");
        }

        vTaskDelay(pdMS_TO_TICKS(CAPTURE_RETRY_DELAY_MS));
        attempt++;
    }

    ESP_LOGW(TAG, "Timed out waiting for a usable fingerprint image");
    return false;
}

static esp_err_t fp_enroll_once(uint16_t slot)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t scan_start = start + pdMS_TO_TICKS(FP_SCAN_START_DELAY_MS);
    const TickType_t deadline = start + pdMS_TO_TICKS(FP_ENROLL_WINDOW_MS);
    esp_err_t result = ESP_FAIL;

    ESP_LOGI(TAG, "Enrollment target slot: %u", slot);
    display_fingerprint_phase("Registering your fingerprint", "Place your finger on the sensor");
    delay_until_tick_with_progress(start, deadline, scan_start);
    display_fingerprint_phase("Registering your fingerprint", "Processing...");

    if (!r307_capture_to_buffer_until(1, start, deadline)) {
        goto done;
    }

    const TickType_t second_capture_at = xTaskGetTickCount() +
                                         pdMS_TO_TICKS(FP_ENROLL_SECOND_CAPTURE_DELAY_MS);
    if (!tick_before(second_capture_at, deadline)) {
        goto done;
    }
    delay_until_tick_with_progress(start, deadline, second_capture_at);
    display_fingerprint_phase("Registering your fingerprint", "Processing...");

    if (!r307_capture_to_buffer_until(2, start, deadline)) {
        goto done;
    }

    uint8_t confirm = r307_cmd_confirm(R307_CMD_REG_MODEL, NULL, 0);
    if (confirm != R307_OK) {
        ESP_LOGE(TAG, "RegModel failed: 0x%02X (%s)", confirm, r307_confirm_name(confirm));
        goto done;
    }

    const uint8_t store_params[] = {
        0x01,
        (uint8_t)(slot >> 8),
        (uint8_t)(slot & 0xFF),
    };
    confirm = r307_cmd_confirm(R307_CMD_STORE, store_params, sizeof(store_params));
    if (confirm != R307_OK) {
        ESP_LOGE(TAG, "Store failed: 0x%02X (%s)", confirm, r307_confirm_name(confirm));
        goto done;
    }

    ESP_LOGI(TAG, "Enrollment complete. Stored fingerprint in R307 slot %u", slot);
    result = ESP_OK;

done:
    delay_until_tick(deadline);
    display_fingerprint_progress(100);
    if (result == ESP_OK) {
        display_fingerprint_phase("Registering your fingerprint", "Registration completed");
    } else {
        display_fingerprint_phase("Registering your fingerprint", "Denied");
    }
    return result;
}

esp_err_t fp_enroll(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Fingerprint enrollment requested before driver init");
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t slots[FP_MAX_PRINTS] = {0};
    uint8_t count = nvs_load_fingerprints(slots, FP_MAX_PRINTS);
    ESP_LOGI(TAG, "Fingerprint enrollment requested: existing=%u max=%u", count, FP_MAX_PRINTS);
    if (count >= FP_MAX_PRINTS) {
        ESP_LOGW(TAG, "Fingerprint enrollment blocked: limit reached");
        fp_display("Fingerprint limit reached");
        return ESP_FAIL;
    }

    uint16_t slot = (uint16_t)(count + 1);
    while (count < FP_MAX_PRINTS) {
        esp_err_t err = fp_enroll_once(slot);
        if (err == ESP_OK) {
            slots[count++] = slot;
            nvs_save_fingerprints(count, slots);
            char id[8];
            snprintf(id, sizeof(id), "id_%02u", count);
            nvs_save_fingerprint(true, id, slot);
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(FP_RETRY_DELAY_MS));
    }

    return ESP_FAIL;
}

static bool fp_slot_is_authorized(const uint16_t *slots, uint8_t count, uint16_t slot)
{
    for (uint8_t i = 0; i < count; i++) {
        if (slots[i] == slot) {
            return true;
        }
    }

    return false;
}

static esp_err_t fp_verify_once(int attempt_num, bool admin_only)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t scan_start = start + pdMS_TO_TICKS(FP_SCAN_START_DELAY_MS);
    const TickType_t deadline = start + pdMS_TO_TICKS(FP_VERIFY_WINDOW_MS);
    esp_err_t result = ESP_FAIL;
    const char *phase = admin_only
        ? "Admin User Only\nVerifying your fingerprint"
        : "Verifying your fingerprint";

    display_fingerprint_phase(phase, "Place your finger on the sensor");

    delay_until_tick_with_progress(start, deadline, scan_start);
    display_fingerprint_phase(phase, "Processing...");

    if (!r307_capture_to_buffer_until(1, start, deadline)) {
        goto done;
    }

    uint16_t slots[FP_MAX_PRINTS] = {0};
    uint8_t count = nvs_load_fingerprints(slots, FP_MAX_PRINTS);
    if (count == 0) {
        ESP_LOGW(TAG, "No fingerprint slots loaded from NVS on verify attempt %d", attempt_num);
        goto done;
    }

    uint16_t min_slot = slots[0];
    uint16_t max_slot = slots[0];
    uint8_t authorized_count = count;
    if (admin_only) {
        authorized_count = 1;
        ESP_LOGI(TAG, "Admin fingerprint slot=%u", slots[0]);
    } else {
        for (uint8_t i = 0; i < count; i++) {
            ESP_LOGI(TAG, "Authorized fingerprint slot[%u]=%u", i, slots[i]);
            if (slots[i] < min_slot) min_slot = slots[i];
            if (slots[i] > max_slot) max_slot = slots[i];
        }
    }

    const uint16_t page_count = (uint16_t)(max_slot - min_slot + 1);
    const uint8_t search_params[] = {
        0x01,
        (uint8_t)(min_slot >> 8),
        (uint8_t)(min_slot & 0xFF),
        (uint8_t)(page_count >> 8),
        (uint8_t)(page_count & 0xFF),
    };

    ESP_LOGI(TAG, "Searching fingerprint slots %u..%u (%u page(s)) on attempt %d",
             min_slot, max_slot, page_count, attempt_num);

    r307_response_t response = {0};
    esp_err_t err = r307_cmd(R307_CMD_SEARCH, search_params, sizeof(search_params), &response);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Search failed for slots %u..%u: %s",
                 min_slot, max_slot, esp_err_to_name(err));
        goto done;
    }

    if (response.confirm == R307_OK && response.data_len >= 4) {
        const uint16_t matched_slot = ((uint16_t)response.data[0] << 8) | response.data[1];
        const uint16_t score = ((uint16_t)response.data[2] << 8) | response.data[3];
        ESP_LOGI(TAG, "Match found: slot=%u score=%u", matched_slot, score);
        if (fp_slot_is_authorized(slots, authorized_count, matched_slot)) {
            result = ESP_OK;
        } else {
            ESP_LOGW(TAG, "Matched fingerprint slot %u is not authorized for %s",
                     matched_slot, admin_only ? "admin action" : "this action");
        }
        goto done;
    }

    ESP_LOGW(TAG, "Search result slots %u..%u: 0x%02X (%s)",
             min_slot, max_slot, response.confirm, r307_confirm_name(response.confirm));

done:
    delay_until_tick(deadline);
    display_fingerprint_progress(100);
    if (result == ESP_OK) {
        display_fingerprint_phase(phase, "Access granted");
    } else {
        display_fingerprint_phase(phase, "Denied");
    }
    return result;
}

static esp_err_t fp_verify_with_scope(bool admin_only)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Fingerprint verification requested before driver init");
        return ESP_ERR_INVALID_STATE;
    }
    if (!fp_is_enrolled()) {
        ESP_LOGW(TAG, "Fingerprint verification blocked: no fingerprint enrolled");
        fp_display("No fingerprint enrolled");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fingerprint verification started: attempts=%d admin_only=%d",
             FP_VERIFY_ATTEMPTS, admin_only ? 1 : 0);
    for (int attempt = 1; attempt <= FP_VERIFY_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "Fingerprint verification attempt %d/%d", attempt, FP_VERIFY_ATTEMPTS);
        esp_err_t err = fp_verify_once(attempt, admin_only);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Fingerprint verification succeeded on attempt %d", attempt);
            vTaskDelay(pdMS_TO_TICKS(FP_SUCCESS_RESULT_HOLD_MS));
            return ESP_OK;
        }

        if (attempt < FP_VERIFY_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(FP_RETRY_DELAY_MS));
        }
    }

    ESP_LOGW(TAG, "Fingerprint verification failed after %d attempts", FP_VERIFY_ATTEMPTS);
    return ESP_FAIL;
}

esp_err_t fp_verify(void)
{
    return fp_verify_with_scope(false);
}

esp_err_t fp_verify_admin(void)
{
    return fp_verify_with_scope(true);
}

bool fp_is_enrolled(void)
{
    return nvs_load_fp_enrolled();
}

esp_err_t fp_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = r307_uart_init();
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

void fp_set_display_cb(fp_display_cb cb)
{
    s_display_cb = cb;
}

static void fp_enroll_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Fingerprint enrollment task started");
    g_mode = MODE_FINGERPRINT_ENROLL;
    display_show_fingerprint_screen("Registration", "Place your finger on the sensor");

    esp_err_t err = fp_enroll();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Fingerprint enrollment complete");
    } else {
        ESP_LOGW(TAG, "Fingerprint enrollment failed");
    }

    g_mode = MODE_OPERATIONAL;
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1200));
        display_show_dashboard(true);
    }
    s_enroll_task = NULL;
    vTaskDelete(NULL);
}

bool fp_start_enroll_if_needed(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Fingerprint enrollment skipped; driver not initialized");
        return false;
    }

    if (fp_is_enrolled()) {
        ESP_LOGI(TAG, "Fingerprint already enrolled");
        return false;
    }

    if (s_enroll_task != NULL) {
        ESP_LOGI(TAG, "Fingerprint enrollment already running");
        return true;
    }

    if (xTaskCreate(fp_enroll_task, "fp_enroll", 4096, NULL, 5, &s_enroll_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fingerprint enrollment task");
        s_enroll_task = NULL;
        return false;
    }

    return true;
}

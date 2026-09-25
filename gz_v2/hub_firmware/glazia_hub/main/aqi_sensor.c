#include "aqi_sensor.h"

#include "display.h"
#include "state.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AQI_SENSOR";

// FireBeetle 2 ESP32-P4: PMS7003 laser particulate sensor on UART1.
// The PMS7003 is 3.3 V TTL (safe direct to P4 GPIO); VCC is 5 V for fan/laser.
// SET (PIN10) and RESET (PIN5) are tied to 3.3 V, so the sensor stays in normal
// active mode and streams 32-byte frames automatically at 9600 8N1 — we only
// read. PMS TX (PIN9) → P4 GPIO20 carries the data; the P4 does not transmit
// (no TX pin mapped). GPIO20 is the freed MQ135 ADC pin; GPIO33 (the old
// UART-camera pin) is not reliably broken out on this board's header.
#define PMS_UART_NUM            UART_NUM_1
#define PMS_RX_GPIO             49   // P4 RX  <- PMS TX (PIN9), data
#define PMS_BAUD                9600
#define PMS_FRAME_LEN           32   // 0x42 0x4D + 28-byte body + 2-byte checksum
#define PMS_RX_BUF              256

#define AQI_TASK_STACK          3072
#define AQI_TASK_PRIORITY       4
#define AQI_WARMUP_MS           3000  // fan/laser needs ~3 s to stabilize
#define AQI_UPDATE_DELAY_MS     1000
#define PMS_READ_TIMEOUT_MS     2000

static TaskHandle_t s_task_handle;

static bool aqi_sensor_should_publish(void)
{
    switch (g_mode) {
    case MODE_REGISTERING:
    case MODE_OPERATIONAL:
    case MODE_SENSOR_PAIRING:
    case MODE_FINGERPRINT_ENROLL:
    case MODE_FINGERPRINT_VERIFY:
        return true;
    default:
        return false;
    }
}

// Read one PMS7003 data frame and extract the atmospheric PM2.5 value (µg/m³).
// Byte-aligns on the 0x42 0x4D start header, then reads the remaining 30 bytes
// and verifies the 16-bit checksum (sum of bytes 0..29). Returns ESP_OK and
// sets *pm2_5 on a valid frame; ESP_ERR_TIMEOUT / ESP_ERR_INVALID_CRC otherwise.
static esp_err_t read_pms_frame(uint16_t *pm2_5)
{
    uint8_t byte = 0;

    // Hunt for the 0x42 0x4D start-of-frame magic.
    for (;;) {
        int got = uart_read_bytes(PMS_UART_NUM, &byte, 1,
                                  pdMS_TO_TICKS(PMS_READ_TIMEOUT_MS));
        if (got != 1) {
            return ESP_ERR_TIMEOUT;
        }
        if (byte != 0x42) {
            continue;
        }
        got = uart_read_bytes(PMS_UART_NUM, &byte, 1,
                              pdMS_TO_TICKS(PMS_READ_TIMEOUT_MS));
        if (got != 1) {
            return ESP_ERR_TIMEOUT;
        }
        if (byte == 0x4D) {
            break;
        }
        // Not the second magic byte; if it was another 0x42, the loop will
        // re-check it as a candidate start on the next iteration.
    }

    // Read the remaining 30 bytes of the frame (body + checksum).
    uint8_t body[PMS_FRAME_LEN - 2];
    int got = uart_read_bytes(PMS_UART_NUM, body, sizeof(body),
                              pdMS_TO_TICKS(PMS_READ_TIMEOUT_MS));
    if (got != (int)sizeof(body)) {
        return ESP_ERR_TIMEOUT;
    }

    // Checksum = sum of all bytes from the header through the last data byte.
    uint16_t sum = 0x42 + 0x4D;
    for (int i = 0; i < (int)sizeof(body) - 2; i++) {
        sum += body[i];
    }
    uint16_t frame_cksum = ((uint16_t)body[sizeof(body) - 2] << 8) |
                           body[sizeof(body) - 1];
    if (sum != frame_cksum) {
        return ESP_ERR_INVALID_CRC;
    }

    // body[] indexing (frame byte - 2): atmospheric PM2.5 is frame bytes 12..13.
    //   frame[4..5]   PM1.0 CF=1        frame[10..11] PM1.0 atmospheric
    //   frame[6..7]   PM2.5 CF=1        frame[12..13] PM2.5 atmospheric
    //   frame[8..9]   PM10  CF=1        frame[14..15] PM10  atmospheric
    *pm2_5 = ((uint16_t)body[10] << 8) | body[11];
    return ESP_OK;
}

// US-EPA PM2.5 (µg/m³) → AQI, piecewise-linear over the standard breakpoints.
// AQI = (AQIhi-AQIlo)/(BPhi-BPlo)*(C-BPlo)+AQIlo, clamped to 0..500.
static float pm25_to_aqi(uint16_t pm)
{
    struct {
        float c_lo, c_hi;
        float a_lo, a_hi;
    } static const bp[] = {
        {   0.0f,  12.0f,   0.0f,  50.0f },
        {  12.1f,  35.4f,  51.0f, 100.0f },
        {  35.5f,  55.4f, 101.0f, 150.0f },
        {  55.5f, 150.4f, 151.0f, 200.0f },
        { 150.5f, 250.4f, 201.0f, 300.0f },
        { 250.5f, 350.4f, 301.0f, 400.0f },
        { 350.5f, 500.4f, 401.0f, 500.0f },
    };

    float c = (float)pm;
    for (int i = 0; i < (int)(sizeof(bp) / sizeof(bp[0])); i++) {
        if (c <= bp[i].c_hi) {
            float aqi = (bp[i].a_hi - bp[i].a_lo) / (bp[i].c_hi - bp[i].c_lo) *
                            (c - bp[i].c_lo) +
                        bp[i].a_lo;
            if (aqi < 0.0f) aqi = 0.0f;
            if (aqi > 500.0f) aqi = 500.0f;
            return aqi;
        }
    }
    return 500.0f;  // Above the top breakpoint → hazardous ceiling.
}

static const char *aqi_classifier(float aqi)
{
    if (aqi < 51.0f) return "good";
    if (aqi < 101.0f) return "nominal";
    if (aqi < 201.0f) return "moderate";
    if (aqi < 301.0f) return "poor";
    if (aqi < 401.0f) return "very_poor";
    return "severe";
}

static void aqi_sensor_task(void *arg)
{
    (void)arg;

    // Let the fan/laser spin up and readings settle before publishing.
    vTaskDelay(pdMS_TO_TICKS(AQI_WARMUP_MS));
    uart_flush_input(PMS_UART_NUM);

    while (true) {
        uint16_t pm2_5 = 0;
        esp_err_t err = read_pms_frame(&pm2_5);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "PMS7003 frame read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(AQI_UPDATE_DELAY_MS));
            continue;
        }

        float aqi = pm25_to_aqi(pm2_5);
        ESP_LOGD(TAG, "PM2.5=%u ug/m3 -> AQI=%.0f", (unsigned)pm2_5, aqi);

        if (aqi_sensor_should_publish()) {
            display_update_aqi(aqi, aqi_classifier(aqi), pm2_5);
        }

        // The sensor streams every ~2.3 s (stable) to ~0.2–0.8 s (fast); this
        // paces publishing without dropping the alignment we just achieved.
        vTaskDelay(pdMS_TO_TICKS(AQI_UPDATE_DELAY_MS));
    }
}

esp_err_t aqi_sensor_init(void)
{
    if (s_task_handle) {
        return ESP_OK;
    }

    esp_err_t err = uart_driver_install(PMS_UART_NUM, PMS_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMS7003 UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    uart_config_t cfg = {
        .baud_rate  = PMS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_param_config(PMS_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMS7003 UART param config failed: %s", esp_err_to_name(err));
        uart_driver_delete(PMS_UART_NUM);
        return err;
    }

    // TX unmapped: active mode is receive-only, so we never transmit.
    err = uart_set_pin(PMS_UART_NUM, UART_PIN_NO_CHANGE, PMS_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMS7003 UART set pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(PMS_UART_NUM);
        return err;
    }

    if (xTaskCreatePinnedToCore(aqi_sensor_task, "aqi_sensor", AQI_TASK_STACK, NULL,
                                AQI_TASK_PRIORITY, &s_task_handle, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PMS7003 task");
        uart_driver_delete(PMS_UART_NUM);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "PMS7003 AQI task started on UART%d (RX=GPIO%d, TX=unused @ %d baud)",
             (int)PMS_UART_NUM, PMS_RX_GPIO, PMS_BAUD);
    return ESP_OK;
}

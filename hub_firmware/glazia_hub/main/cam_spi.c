#include "cam_spi.h"
#include "hub_control_ws.h"
#include "state.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "CAM_SPI";

/* ── Pins ──────────────────────────────────────────────────────────────────── */
#define CAM_SPI_HOST    SPI3_HOST
#define CAM_SPI_SCLK    4
#define CAM_SPI_MOSI    17
#define CAM_SPI_MISO    18
#define CAM_SPI_CS      16
#define CAM_SPI_DRDY    15          /* input: cam DRDY output, POSEDGE interrupt */
#define CAM_SPI_CLK_HZ  (8 * 1000 * 1000)

/* ── Transfer sizes ────────────────────────────────────────────────────────── */
#define SPI_CMD_SIZE    32          /* hub→cam command transaction (cam IDLE state) */
#define SPI_TRANS_SIZE  5120        /* must match SPI_TRANS_SIZE in spi_bridge.c */

/* ── Command encoding (first 4 MOSI bytes) ────────────────────────────────── */
static const uint8_t CMD_START[4]    = {0xCA, 0x01, 0x00, 0x00};
static const uint8_t CMD_STOP[4]     = {0xCA, 0x00, 0x00, 0x00};

/* ── Module state ──────────────────────────────────────────────────────────── */
static spi_device_handle_t  s_spi_dev       = NULL;
static volatile bool        s_proxy_running  = false;
static volatile bool        s_stop_requested = false;
static TaskHandle_t         s_proxy_task     = NULL;

/* BSS (internal SRAM) — DMA-capable without heap_caps */
static uint8_t s_cmd_buf[SPI_CMD_SIZE];
static uint8_t s_tx_zero_buf[SPI_TRANS_SIZE];  /* all-zeros MOSI for normal frame reads */

/* Pre-allocated at cam_spi_init() before WiFi/WS take internal SRAM */
static uint8_t *s_frame_buf = NULL;

/* ── DRDY rising-edge ISR → task notification ──────────────────────────────── */
static void IRAM_ATTR drdy_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    if (s_proxy_task) {
        vTaskNotifyGiveFromISR(s_proxy_task, &hp);
        portYIELD_FROM_ISR(hp);
    }
}

/* ── Frame proxy task ──────────────────────────────────────────────────────── */
static void frame_proxy_task(void *arg)
{
    if (!s_frame_buf) {
        ESP_LOGE(TAG, "No DMA frame buf — cam_spi_init() failed to alloc");
        s_proxy_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Frame proxy task started");

    TickType_t last_drdy_tick = xTaskGetTickCount();

    while (s_proxy_running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));

        /* Only skip the SPI cycle when no frame is pending (DRDY LOW).
         * If DRDY is HIGH the coprocessor is waiting — fall through to
         * send CMD_STOP in MOSI, then the s_stop_requested block breaks. */
        if (!s_proxy_running && gpio_get_level(CAM_SPI_DRDY) == 0) {
            break;
        }

        if (!gpio_get_level(CAM_SPI_DRDY)) {
            /* Watchdog: coprocessor returned to IDLE after 3× hub timeout — re-trigger */
            if ((xTaskGetTickCount() - last_drdy_tick) > pdMS_TO_TICKS(5000)) {
                ESP_LOGW(TAG, "No DRDY for 5 s — re-sending START to coprocessor");
                cam_spi_send_start();
                last_drdy_tick = xTaskGetTickCount();
            }
            continue;
        }

        last_drdy_tick = xTaskGetTickCount();

        /* 2 ms settle: let slave DMA hardware stabilise after DRDY rises */
        vTaskDelay(pdMS_TO_TICKS(2));

        /* Full-duplex: MOSI carries CMD_STOP when stopping, zeros otherwise.
         * The coprocessor checks rx_buf[0:4] for CMD_STOP after each transfer. */
        if (s_stop_requested) {
            memcpy(s_tx_zero_buf, CMD_STOP, 4);
        }

        spi_transaction_t t = {
            .length    = (size_t)SPI_TRANS_SIZE * 8,
            .rxlength  = (size_t)SPI_TRANS_SIZE * 8,
            .tx_buffer = s_tx_zero_buf,
            .rx_buffer = s_frame_buf,
        };
        esp_err_t err = spi_device_polling_transmit(s_spi_dev, &t);

        if (s_stop_requested) {
            memset(s_tx_zero_buf, 0, 4);   /* restore zeros for next session */
            s_proxy_running = false;
            break;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SPI frame rx failed: %s", esp_err_to_name(err));
            continue;
        }

        uint32_t frame_len;
        memcpy(&frame_len, s_frame_buf, sizeof(frame_len));

        if (frame_len == 0 || frame_len > (uint32_t)(SPI_TRANS_SIZE - 4)) {
            ESP_LOGW(TAG, "Invalid frame_len=%" PRIu32 ", discarding", frame_len);
            continue;
        }

        /* JPEG SOI = 0xFF 0xD8, always followed by 0xFF (next marker byte).
         * Checking 3 bytes catches SPI-corrupted frames that accidentally have
         * the right first 2 bytes but wrong continuation. */
        if (s_frame_buf[4] != 0xFF || s_frame_buf[5] != 0xD8 || s_frame_buf[6] != 0xFF) {
            ESP_LOGW(TAG, "Bad JPEG header (0x%02X 0x%02X 0x%02X), discarding",
                     s_frame_buf[4], s_frame_buf[5], s_frame_buf[6]);
            continue;
        }

        esp_err_t ws_err = hub_control_ws_send_bin(s_frame_buf + 4, (size_t)frame_len,
                                                    pdMS_TO_TICKS(500));
        if (ws_err == ESP_OK) {
            static uint32_t s_frame_count = 0;
            if (++s_frame_count % 30 == 0) {
                ESP_LOGI(TAG, "Frame WS sent (%" PRIu32 " bytes, count=%" PRIu32 ")",
                         frame_len, s_frame_count);
            }
        } else {
            ESP_LOGW(TAG, "Frame WS send failed: %s", esp_err_to_name(ws_err));
        }
    }

    ESP_LOGI(TAG, "Frame proxy task stopped");
    s_proxy_task = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void cam_spi_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = CAM_SPI_MOSI,
        .miso_io_num     = CAM_SPI_MISO,
        .sclk_io_num     = CAM_SPI_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = SPI_TRANS_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CAM_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .mode           = 0,
        .clock_speed_hz = CAM_SPI_CLK_HZ,
        .spics_io_num   = CAM_SPI_CS,
        .queue_size     = 1,
        /* full-duplex: no SPI_DEVICE_HALFDUPLEX flag */
    };
    ESP_ERROR_CHECK(spi_bus_add_device(CAM_SPI_HOST, &dev, &s_spi_dev));

    /* DRDY: input with pull-down, rising-edge interrupt */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CAM_SPI_DRDY),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* ISR service may already be installed by another driver — ignore if so */
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(CAM_SPI_DRDY, drdy_isr, NULL));

    /* Pre-allocate DMA frame buffer now, before WiFi/WS consume internal SRAM.
     * After WSS connects, internal_largest drops to ~7936 B — too small for 4096+
     * task stack if we wait until viewer-ready. */
    s_frame_buf = heap_caps_malloc(SPI_TRANS_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_frame_buf) {
        ESP_LOGE(TAG, "DMA frame buf alloc failed at init (need %d bytes internal)",
                 SPI_TRANS_SIZE);
    } else {
        ESP_LOGI(TAG, "SPI master initialized (SPI3 SCLK=%d MOSI=%d MISO=%d CS=%d DRDY_IN=%d) — DMA buf %d B",
                 CAM_SPI_SCLK, CAM_SPI_MOSI, CAM_SPI_MISO, CAM_SPI_CS, CAM_SPI_DRDY, SPI_TRANS_SIZE);
    }
}

void cam_spi_send_start(void)
{
    if (!s_spi_dev) return;

    memcpy(s_cmd_buf, CMD_START, 4);
    memset(s_cmd_buf + 4, 0, SPI_CMD_SIZE - 4);

    spi_transaction_t t = {
        .length    = SPI_CMD_SIZE * 8,
        .tx_buffer = s_cmd_buf,
        .rx_buffer = NULL,
    };
    esp_err_t err = spi_device_transmit(s_spi_dev, &t);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sent START to cam coprocessor via SPI");
    } else {
        ESP_LOGW(TAG, "START cmd failed: %s", esp_err_to_name(err));
    }
}

void cam_spi_send_stop(void)
{
    /* STOP is embedded in MOSI during the next full-duplex frame transaction */
    s_stop_requested = true;
    ESP_LOGI(TAG, "STOP requested — will embed CMD_STOP on next frame transaction");
}

void cam_spi_proxy_start(void)
{
    if (s_proxy_running) return;

    /* Wait for any previous proxy task to fully exit before spawning a new one.
     * viewer-gone → viewer-ready can arrive 100ms apart; the old task may still
     * be blocked in ulTaskNotifyTake. Without this wait, both tasks hold internal
     * SRAM simultaneously and the second alloc fails. */
    TickType_t wait_start = xTaskGetTickCount();
    while (s_proxy_task != NULL) {
        if ((xTaskGetTickCount() - wait_start) > pdMS_TO_TICKS(500)) {
            ESP_LOGW(TAG, "Old proxy task still alive after 500 ms — aborting start");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_stop_requested = false;
    s_proxy_running  = true;

    /* 4096 stack: hub_control_ws_send_bin() queues to WS ring buffer; TLS encryption
     * runs in the hub_ws task, not here. Raise to 6144 if stack overflow panic occurs. */
    BaseType_t rc = xTaskCreate(frame_proxy_task, "cam_proxy", 4096, NULL, 5, &s_proxy_task);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "cam_proxy xTaskCreate failed (OOM) internal_largest=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_proxy_running = false;
        s_proxy_task    = NULL;
        return;
    }
    ESP_LOGI(TAG, "Frame proxy started (WS binary transport)");
}

void cam_spi_proxy_stop(void)
{
    s_stop_requested = true;
    s_proxy_running  = false;
    if (s_proxy_task) {
        xTaskNotifyGive(s_proxy_task);   /* wake from ulTaskNotifyTake immediately */
    }
    ESP_LOGI(TAG, "Frame proxy stopped");
}

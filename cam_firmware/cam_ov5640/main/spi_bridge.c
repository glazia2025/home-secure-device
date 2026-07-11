#include "spi_bridge.h"
#include "camera_core.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "SPI_BRIDGE";

/* ── Pins ──────────────────────────────────────────────────────────────────── */
#define CAM_SPI_HOST    SPI2_HOST
#define CAM_SPI_SCLK    14
#define CAM_SPI_MOSI    1       /* receives commands from hub */
#define CAM_SPI_MISO    2       /* sends JPEG data to hub */
#define CAM_SPI_CS      21
#define CAM_SPI_DRDY    3       /* output: HIGH = frame ready for hub to clock out */

/* ── Transfer sizes ────────────────────────────────────────────────────────── */
#define SPI_CMD_SIZE    32
#define SPI_TRANS_SIZE  5120    /* must match SPI_TRANS_SIZE in cam_spi.c (hub master) */

/* ── Command magic bytes (checked in MOSI rx_buffer[0..3]) ────────────────── */
static const uint8_t CMD_STOP[4] = {0xCA, 0x00, 0x00, 0x00};

/* ── Command receive buffer (BSS = internal SRAM = DMA-capable) ────────────── */
static uint8_t s_cmd_rx_buf[SPI_CMD_SIZE];

/* ── SPI slave bus/device config (stored for recovery re-init) ─────────────── */
static spi_bus_config_t s_bus = {
    .mosi_io_num     = CAM_SPI_MOSI,
    .miso_io_num     = CAM_SPI_MISO,
    .sclk_io_num     = CAM_SPI_SCLK,
    .quadwp_io_num   = -1,
    .quadhd_io_num   = -1,
    .max_transfer_sz = SPI_TRANS_SIZE,
};
static spi_slave_interface_config_t s_slave = {
    .mode         = 0,
    .spics_io_num = CAM_SPI_CS,
    .queue_size   = 1,
    .flags        = 0,
};

static esp_err_t slave_init(void)
{
    return spi_slave_initialize(CAM_SPI_HOST, &s_bus, &s_slave, SPI_DMA_CH_AUTO);
}

static void slave_deinit(void)
{
    spi_slave_free(CAM_SPI_HOST);
}

/* ── Main SPI listener task ────────────────────────────────────────────────── */
static void spi_listener_task(void *arg)
{
    ESP_ERROR_CHECK(slave_init());

    gpio_set_direction(CAM_SPI_DRDY, GPIO_MODE_OUTPUT);
    gpio_set_level(CAM_SPI_DRDY, 0);

    /* Allocate persistent DMA frame buffers once for the lifetime of the task */
    uint8_t *tx_buf = heap_caps_malloc(SPI_TRANS_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *rx_buf = heap_caps_malloc(SPI_TRANS_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!tx_buf || !rx_buf) {
        ESP_LOGE(TAG, "DMA frame buffer alloc failed (need %d bytes internal)", SPI_TRANS_SIZE);
        heap_caps_free(tx_buf);
        heap_caps_free(rx_buf);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SPI slave initialized (SPI2 SCLK=%d MOSI=%d MISO=%d CS=%d DRDY=%d)",
             CAM_SPI_SCLK, CAM_SPI_MOSI, CAM_SPI_MISO, CAM_SPI_CS, CAM_SPI_DRDY);

    while (1) {
        /* ── IDLE: wait for a start command from hub ────────────────────── */
        memset(s_cmd_rx_buf, 0, SPI_CMD_SIZE);
        spi_slave_transaction_t cmd_trans = {
            .length    = SPI_CMD_SIZE * 8,
            .rx_buffer = s_cmd_rx_buf,
            .tx_buffer = NULL,
        };
        spi_slave_transmit(CAM_SPI_HOST, &cmd_trans, portMAX_DELAY);

        /* Validate start magic: {0xCA, 0x01, ...} */
        if (s_cmd_rx_buf[0] != 0xCA || s_cmd_rx_buf[1] != 0x01) {
            ESP_LOGD(TAG, "Ignoring unknown cmd (byte0=0x%02X byte1=0x%02X)",
                     s_cmd_rx_buf[0], s_cmd_rx_buf[1]);
            continue;
        }
        ESP_LOGI(TAG, "Received START command");

        esp_err_t cam_err = camera_core_init();
        if (cam_err != ESP_OK) {
            ESP_LOGE(TAG, "Camera init failed (0x%x) — returning to IDLE", cam_err);
            continue;
        }

        /* ── SENDING: stream frames until stop or hub timeout ───────────── */
        int no_resp_count = 0;
        bool sending = true;

        while (sending) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            if (fb->len + 4 > SPI_TRANS_SIZE) {
                ESP_LOGW(TAG, "Frame too large (%d bytes), skipping", (int)fb->len);
                esp_camera_fb_return(fb);
                continue;
            }

            /* Build DMA tx_buf: [4-byte frame_len LE] + [JPEG bytes] + [zero padding] */
            uint32_t flen = (uint32_t)fb->len;
            memcpy(tx_buf, &flen, 4);
            memcpy(tx_buf + 4, fb->buf, fb->len);
            memset(tx_buf + 4 + fb->len, 0, SPI_TRANS_SIZE - 4 - fb->len);
            esp_camera_fb_return(fb);   /* safe: JPEG is now in DMA tx_buf */

            /* Queue slave tx BEFORE raising DRDY — hardware must be armed first */
            spi_slave_transaction_t frame_trans = {
                .length    = (size_t)SPI_TRANS_SIZE * 8,
                .tx_buffer = tx_buf,
                .rx_buffer = rx_buf,    /* captures hub MOSI for stop-command detection */
            };
            esp_err_t q_err = spi_slave_queue_trans(CAM_SPI_HOST, &frame_trans,
                                                     pdMS_TO_TICKS(100));
            if (q_err != ESP_OK) {
                ESP_LOGW(TAG, "spi_slave_queue_trans failed: %s", esp_err_to_name(q_err));
                continue;
            }

            gpio_set_level(CAM_SPI_DRDY, 1);   /* signal hub: frame ready */

            bool trans_done = false;
            while (!trans_done && sending) {
                spi_slave_transaction_t *result = NULL;
                esp_err_t res_err = spi_slave_get_trans_result(CAM_SPI_HOST, &result,
                                                                pdMS_TO_TICKS(1000));
                if (res_err == ESP_ERR_TIMEOUT) {
                    no_resp_count++;
                    ESP_LOGW(TAG, "No hub response (%d/3) — hub gone?", no_resp_count);
                    if (no_resp_count >= 3) {
                        ESP_LOGW(TAG, "Hub unresponsive — stopping camera");
                        slave_deinit();
                        ESP_ERROR_CHECK(slave_init());
                        sending = false;
                        break;
                    }
                    /* keep waiting for the same queued transaction to complete */
                } else if (res_err == ESP_OK) {
                    trans_done = true;
                    no_resp_count = 0;
                } else {
                    ESP_LOGE(TAG, "SPI trans error: %s", esp_err_to_name(res_err));
                    sending = false;
                    break;
                }
            }
            gpio_set_level(CAM_SPI_DRDY, 0);

            if (!sending) continue;

            ESP_LOGD(TAG, "DRDY LOW — hub clocked frame out (frame_len=%" PRIu32 ")", flen);

            /* Check if hub embedded a stop command in its MOSI bytes */
            if (memcmp(rx_buf, CMD_STOP, 4) == 0) {
                ESP_LOGI(TAG, "Received STOP from hub");
                sending = false;
            }

            /* Frame timing diagnostic: log actual camera output rate every 30 frames */
            {
                static uint32_t s_frame_count = 0;
                static TickType_t s_last_log_tick = 0;
                TickType_t now = xTaskGetTickCount();
                if (s_last_log_tick == 0) s_last_log_tick = now;
                if (++s_frame_count % 30 == 0) {
                    ESP_LOGI(TAG, "Frames sent: %" PRIu32 " (last 30 in %u ms)",
                             s_frame_count,
                             (unsigned)((now - s_last_log_tick) * portTICK_PERIOD_MS));
                    s_last_log_tick = now;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));   /* camera JPEG engine is rate limiter; 10ms is a safe floor */
        }

        camera_core_deinit();
        ESP_LOGI(TAG, "Stopped — returning to IDLE");
    }

    /* Unreachable in normal operation */
    heap_caps_free(tx_buf);
    heap_caps_free(rx_buf);
    vTaskDelete(NULL);
}

void spi_bridge_start(void)
{
    xTaskCreate(spi_listener_task, "spi_listener", 5120, NULL, 5, NULL);
}

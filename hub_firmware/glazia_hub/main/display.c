#include "display.h"
#include "state.h"
#include "font.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "DISPLAY";

#define LCD_W 320
#define LCD_H 240

#define COLOR_WHITE  0xFFFF
#define COLOR_BLACK  0x0000
#define COLOR_ORANGE 0xFD20

static spi_device_handle_t spi;

// Framebuffer — 320*240*2 = 153600 bytes, put in DRAM
static uint16_t fb[LCD_W * LCD_H];

// ── Low level ─────────────────────────────────────────────────────────────

static void send_cmd(uint8_t cmd)
{
    gpio_set_level(TFT_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(spi, &t);
}

static void send_data(const uint8_t *data, int len)
{
    if (len == 0) return;
    gpio_set_level(TFT_DC, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(spi, &t);
}

static void send_u8(uint8_t d) { send_data(&d, 1); }

// ── ILI9341 init ─────────────────────────────────────────────────────────

static void ili9341_init(void)
{
    gpio_set_level(TFT_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TFT_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    send_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(100)); // SW reset
    send_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120)); // Sleep out

    send_cmd(0x3A); send_u8(0x55);  // 16-bit color
    send_cmd(0x36); send_u8(0x48);  // landscape, BGR
    send_cmd(0x29);                 // Display on
    vTaskDelay(pdMS_TO_TICKS(25));
}

// ── Set draw window ───────────────────────────────────────────────────────

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    send_cmd(0x2A);
    uint8_t col[] = {x0>>8, x0&0xFF, x1>>8, x1&0xFF};
    send_data(col, 4);

    send_cmd(0x2B);
    uint8_t row[] = {y0>>8, y0&0xFF, y1>>8, y1&0xFF};
    send_data(row, 4);

    send_cmd(0x2C);
}

// ── Flush framebuffer to screen ───────────────────────────────────────────

static void fb_flush(void)
{
    set_window(0, 0, LCD_W - 1, LCD_H - 1);
    gpio_set_level(TFT_DC, 1);

    // Send in chunks (max SPI transfer is limited)
    const int CHUNK = LCD_W * 16;  // 16 lines at a time
    int total = LCD_W * LCD_H;
    int sent  = 0;
    while (sent < total) {
        int n = total - sent;
        if (n > CHUNK) n = CHUNK;
        spi_transaction_t t = {
            .length    = n * 16,
            .tx_buffer = &fb[sent],
        };
        spi_device_polling_transmit(spi, &t);
        sent += n;
    }
}

// ── Draw char into framebuffer ────────────────────────────────────────────

static void fb_char(int x, int y, char c, uint16_t color, uint8_t scale)
{
    const uint8_t *bm = font_get(c);
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 5; col++) {
            if (bm[col] & (1 << row)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col*scale + sx;
                        int py = y + row*scale + sy;
                        if (px < LCD_W && py < LCD_H)
                            fb[py * LCD_W + px] = (color >> 8) | (color << 8); // swap endian
                    }
            }
        }
    }
}

static void fb_str(int x, int y, const char *s, uint16_t color, uint8_t scale)
{
    while (*s) { fb_char(x, y, *s++, color, scale); x += 6 * scale; }
}

// ── Public API ────────────────────────────────────────────────────────────

void display_init(void)
{
    gpio_set_direction(TFT_DC,  GPIO_MODE_OUTPUT);
    gpio_set_direction(TFT_RST, GPIO_MODE_OUTPUT);

    spi_bus_config_t bus = {
        .mosi_io_num   = TFT_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = TFT_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 16 * 2,
    };
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = TFT_CS,
        .queue_size     = 7,
    };
    spi_bus_add_device(SPI2_HOST, &dev, &spi);

    ili9341_init();
    memset(fb, 0x00, sizeof(fb));
    fb_flush();

    ESP_LOGI(TAG, "Display initialised");
}

void display_show(const char *line1, const char *line2)
{
    memset(fb, 0x00, sizeof(fb));
    fb_str(10, 70,  line1, COLOR_WHITE,  3);
    fb_str(10, 140, line2, COLOR_ORANGE, 2);
    fb_flush();
    ESP_LOGI(TAG, "Display: [%s] [%s]", line1, line2);
}

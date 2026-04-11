/**
 * C6 Display Slave
 * ─────────────────
 * Fixed LVGL layout for Glazia Hub:
 *   - "Glazia Hub" title
 *   - Status box (updatable)
 *   - Critical toggle button (pink)
 *   - 4 sensor boxes with rounded corners (empty for now)
 *
 * UART command protocol (newline-terminated, 115200 baud):
 *   STATUS:line1|line2     update status box, | = newline
 *   FILL:R,G,B             fill screen with colour (debug)
 *   CLEAR                  black screen (debug)
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "c6_display";

/* ── TFT pins (C6) ───────────────────────────────────────────────────────── */
#define PIN_CS    18
#define PIN_RST   19
#define PIN_DC    20
#define PIN_MOSI   7
#define PIN_SCK    6

/* ── UART to S3 ──────────────────────────────────────────────────────────── */
#define UART_PORT   UART_NUM_1
#define UART_RX_PIN  9
#define UART_TX_PIN 14
#define UART_BAUD   115200
#define UART_BUF    512

/* ── Display resolution (portrait) ──────────────────────────────────────── */
#define LCD_H_RES        240
#define LCD_V_RES        320
#define LCD_SPI_HOST     SPI2_HOST
#define LCD_PIXEL_CLK_HZ (10 * 1000 * 1000)
#define LCD_CMD_BITS     8
#define LCD_PARAM_BITS   8
#define DRAW_BUF_LINES   50

static esp_lcd_panel_io_handle_t io_handle;
static esp_lcd_panel_handle_t    panel_handle;
static lv_disp_t                *disp;

/* ── Updatable widget references ─────────────────────────────────────────── */
static lv_obj_t *status_label = NULL;

/* ── LCD init ────────────────────────────────────────────────────────────── */
static void lcd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_DC,
        .cs_gpio_num       = PIN_CS,
        .pclk_hz           = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_endian     = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel_handle));

    esp_lcd_panel_reset(panel_handle);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_lcd_panel_init(panel_handle);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_invert_color(panel_handle, false);
    esp_lcd_panel_mirror(panel_handle, true, false);
    esp_lcd_panel_set_gap(panel_handle, 0, 0);
    esp_lcd_panel_disp_on_off(panel_handle, true);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ── LVGL init ───────────────────────────────────────────────────────────── */
static void lvgl_init(void)
{
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = panel_handle,
        .buffer_size   = LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .flags         = { .buff_dma = true },
    };
    disp = lvgl_port_add_disp(&disp_cfg);
}

/* ── Build fixed layout ──────────────────────────────────────────────────── */
static void build_layout(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    // ── Title ─────────────────────────────────────────────────────────
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Glazia Hub");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // ── Status box ────────────────────────────────────────────────────
    lv_obj_t *status_box = lv_obj_create(scr);
    lv_obj_set_pos(status_box, 10, 48);
    lv_obj_set_size(status_box, 220, 88);
    lv_obj_set_style_bg_color(status_box, lv_color_make(18, 18, 18), LV_PART_MAIN);
    lv_obj_set_style_border_color(status_box, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(status_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(status_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(status_box, 8, LV_PART_MAIN);
    lv_obj_clear_flag(status_box, LV_OBJ_FLAG_SCROLLABLE);

    status_label = lv_label_create(status_box);
    lv_label_set_text(status_label, "Starting...");
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_label, 204);
    lv_obj_set_style_text_color(status_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);

    // ── Critical toggle button (pink) ─────────────────────────────────
    lv_obj_t *crit_btn = lv_obj_create(scr);
    lv_obj_set_pos(crit_btn, 10, 148);
    lv_obj_set_size(crit_btn, 220, 30);
    lv_obj_set_style_bg_color(crit_btn, lv_color_make(255, 160, 160), LV_PART_MAIN);
    lv_obj_set_style_border_width(crit_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(crit_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(crit_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(crit_btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *crit_lbl = lv_label_create(crit_btn);
    lv_label_set_text(crit_lbl, "Critical Toggle");
    lv_obj_set_style_text_color(crit_lbl, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(crit_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(crit_lbl, LV_ALIGN_CENTER, 0, 0);

    // ── Sensor boxes (2x2 grid, rounded corners, empty) ───────────────
    // x: 10, 130  y: 190, 258   size: 100x60   radius: 10
    const int bx[4] = {10, 130, 10, 130};
    const int by[4] = {190, 190, 258, 258};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *box = lv_obj_create(scr);
        lv_obj_set_pos(box, bx[i], by[i]);
        lv_obj_set_size(box, 100, 60);
        lv_obj_set_style_bg_color(box, lv_color_make(22, 22, 22), LV_PART_MAIN);
        lv_obj_set_style_border_color(box, lv_color_make(70, 70, 70), LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(box, 10, LV_PART_MAIN);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Layout built");
}

/* ── Command handlers ────────────────────────────────────────────────────── */

// STATUS:line1|line2  — update the status box, | becomes newline
static void cmd_status(const char *msg)
{
    if (!status_label) return;

    char text[128] = {0};
    strncpy(text, msg, sizeof(text) - 1);
    for (char *p = text; *p; p++) {
        if (*p == '|') *p = '\n';
    }

    lvgl_port_lock(0);
    lv_label_set_text(status_label, text);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    lvgl_port_unlock();
}

static void cmd_fill(uint8_t r, uint8_t g, uint8_t b)
{
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_set_style_bg_color(scr, lv_color_make(r, g, b), LV_PART_MAIN);
    lvgl_port_unlock();
}

/* ── Command parser ──────────────────────────────────────────────────────── */
static void handle_command(char *line)
{
    ESP_LOGI(TAG, "cmd: %s", line);

    if (strncmp(line, "STATUS:", 7) == 0) {
        cmd_status(line + 7);
    } else if (strncmp(line, "FILL:", 5) == 0) {
        int r = 0, g = 0, b = 0;
        sscanf(line + 5, "%d,%d,%d", &r, &g, &b);
        cmd_fill((uint8_t)r, (uint8_t)g, (uint8_t)b);
    } else if (strcmp(line, "CLEAR") == 0) {
        cmd_status("...");
    } else {
        ESP_LOGW(TAG, "unknown command: %s", line);
    }
}

/* ── UART receiver task ──────────────────────────────────────────────────── */
static void uart_task(void *arg)
{
    char buf[256];
    int  pos = 0;
    uint8_t ch;

    while (1) {
        if (uart_read_bytes(UART_PORT, &ch, 1, portMAX_DELAY) > 0) {
            if (ch == '\n' || ch == '\r') {
                if (pos > 0) {
                    buf[pos] = '\0';
                    handle_command(buf);
                    pos = 0;
                }
            } else if (pos < (int)sizeof(buf) - 1) {
                buf[pos++] = (char)ch;
            }
        }
    }
}

/* ── UART heartbeat ──────────────────────────────────────────────────────── */
static void uart_alive_task(void *arg)
{
    while (1) {
        uart_write_bytes(UART_PORT, "ALIVE\n", 6);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "C6 display slave starting");

    lcd_init();
    lvgl_init();

    uart_config_t uart_cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    build_layout();

    xTaskCreate(uart_task,       "uart_rx",  4096, NULL, 5, NULL);
    xTaskCreate(uart_alive_task, "uart_tx",  2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "Ready — listening on UART1 RX=GPIO%d", UART_RX_PIN);
}

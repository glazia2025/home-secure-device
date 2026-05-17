/**
 * display.c — ILI9341 TFT driven directly from ESP32-S3 over SPI + LVGL.
 *
 * Replaces the old UART-to-C6 bridge. The public API (display_show,
 * display_hub_location, display_sensor_location, display_sensor_list)
 * is identical — callers in wifi.c, button.c, api_client.c etc. need
 * no changes.
 *
 * Layout (240 × 320, portrait):
 *   [0]  Header      — Glazia logo + "GLAZIA / Hub"       y: 0–67
 *   [1]  Status panel — status text + location             y: 78–147
 *   [2]  Critical Toggle (always ON while powered)         y: 147–182
 *   [3]  Sensor list  — scrollable paired-sensor table     y: 192–311
 */
#include "display.h"
#include "espnow.h"
#include "misc/lv_area.h"
#include "state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "DISPLAY";

extern const lv_img_dsc_t img_glazia_logo;

/* ── Display constants ───────────────────────────────────────────────────── */
#define LCD_H_RES          240
#define LCD_V_RES          320
#define LCD_SPI_HOST       SPI2_HOST
#define LCD_PIXEL_CLK_HZ   (10 * 1000 * 1000)
#define LCD_CMD_BITS       8
#define LCD_PARAM_BITS     8
#define DRAW_BUF_LINES     50

/* ── LVGL object handles (set once in display_init, read-only after) ─────── */
static lv_obj_t *s_status_label   = NULL;
static lv_obj_t *s_location_label = NULL;
static lv_obj_t *s_sensor_label   = NULL;
static lv_obj_t *s_critical_sw    = NULL;

/* ── XPT2046 touch — raw SPI device (no registry component needed) ───────── */
static spi_device_handle_t s_tp_spi = NULL;
static lv_indev_t         *s_tp_indev = NULL;
static lv_indev_drv_t      s_tp_drv;
/* ── Hardware init ───────────────────────────────────────────────────────── */

/*
 * XPT2046 calibration constants — derived from observed raw ADC min/max.
 *
 * On this panel the axes are physically swapped relative to the ILI9341:
 *   0xD0 (XPT2046 "X") → measures the VERTICAL position (screen Y)
 *   0x90 (XPT2046 "Y") → measures the HORIZONTAL position (screen X)
 *
 * The LCD is initialised with mirror(true, false) — X axis is flipped —
 * so the touch horizontal coordinate must also be mirrored.
 *
 * Adjust these if the touch boundary feels off after calibration:
 */
#define TP_X_RAW_MIN   0    /* 0x90 reading at the left   edge of the panel */
#define TP_X_RAW_MAX   1300    /* 0x90 reading at the right  edge of the panel (corrected for this hardware) */
#define TP_Y_RAW_MIN  1173   /* 0xD0 reading at the top edge of the panel */
#define TP_Y_RAW_MAX  2465    /* 0xD0 reading at the bottom edge of the panel */

/*
 * XPT2046 single-channel read.
 * cmd: 0xD0 = X axis, 0x90 = Y axis  (12-bit differential mode)
 * Protocol: send 1 command byte + 2 zero bytes; result is in bytes 1–2,
 * bits [14:3], so right-shift by 3 to get the 12-bit ADC value (0–4095).
 */
static uint16_t xpt2046_read_raw(uint8_t cmd)
{
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0x00, 0x00, 0x00};
    spi_transaction_t t = {
        .length    = 24,        /* 3 bytes */
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(s_tp_spi, &t);
    uint16_t result = (((uint16_t)rx[1] << 8) | rx[2]) >> 3;

    return result;
}

/*
 * Average 8 successive reads of the same channel.
 * XPT2046 is noisy; a single read can jump ~30 px, which LVGL's click
 * detector treats as a drag and never fires LV_EVENT_CLICKED on the switch.
 * Averaging stabilises the reported position to within ~3 px.
 */
static uint16_t xpt2046_read_avg(uint8_t cmd)
{
    uint32_t sum = 0;
    for (int i = 0; i < 6; i++) {
        sum += xpt2046_read_raw(cmd);
    }
    return (uint16_t)(sum / 6);
}

/*
 * LVGL input-device read callback — called from the LVGL task on every tick.
 * T_IRQ is active-low: LOW = panel touched.
 */

static void xpt2046_read_cb(lv_indev_drv_t *drv,
                            lv_indev_data_t *data)
{
    LV_UNUSED(drv);

    static bool s_last_pressed = false;

    bool pressed =
        (gpio_get_level(TOUCH_PIN_IRQ) == 0);


    if (pressed) {

        /*
         * 0xD0 -> vertical
         * 0x90 -> horizontal
         */

        uint16_t raw_phys_y =
            xpt2046_read_avg(0xD0);

        uint16_t raw_phys_x =
            xpt2046_read_avg(0x90);

        if (raw_phys_x > 3500 || raw_phys_y > 3500) { data->state = LV_INDEV_STATE_RELEASED; return; }

        /*
         * Clamp
         */

        if (raw_phys_x < TP_X_RAW_MIN)
            raw_phys_x = TP_X_RAW_MIN;

        if (raw_phys_x > TP_X_RAW_MAX)
            raw_phys_x = TP_X_RAW_MAX;

        if (raw_phys_y < TP_Y_RAW_MIN)
            raw_phys_y = TP_Y_RAW_MIN;

        if (raw_phys_y > TP_Y_RAW_MAX)
            raw_phys_y = TP_Y_RAW_MAX;

        /*
         * Interpolate
         */

        lv_coord_t px =
            (lv_coord_t)(
                ((uint32_t)(raw_phys_x - TP_X_RAW_MIN)
                * (LCD_H_RES - 1))
                / (TP_X_RAW_MAX - TP_X_RAW_MIN)
            );

        lv_coord_t py =
            (lv_coord_t)(
                ((uint32_t)(raw_phys_y - TP_Y_RAW_MIN)
                * (LCD_V_RES - 1))
                / (TP_Y_RAW_MAX - TP_Y_RAW_MIN)
            );

        /*
         * LCD mirrored in X
         */

        px = (LCD_H_RES - 1) - px;


        /*
            * Final clamp
         */

        if (px < 0)
            px = 0;

        if (px >= LCD_H_RES)
            px = LCD_H_RES - 1;

        if (py < 0)
            py = 0;

        if (py >= LCD_V_RES)
            py = LCD_V_RES - 1;

        data->point.x = px;
        data->point.y = py;

        static uint32_t dbg_counter = 0;

        if ((dbg_counter++ % 10) == 0) {
            ESP_LOGI(TAG,
                     "TOUCH raw=(%u,%u) px=(%d,%d)",
                     raw_phys_x,
                     raw_phys_y,
                     px,
                     py);
        }
    }

    if (!pressed && s_last_pressed) {
        ESP_LOGI(TAG, "TOUCH released");
    }

    s_last_pressed = pressed;

    data->state =
        pressed
        ? LV_INDEV_STATE_PRESSED
        : LV_INDEV_STATE_RELEASED;
}

/*
 * Critical toggle event callback — fires on LV_EVENT_VALUE_CHANGED.
 * Currently just logs toggle state. No fingerprint verification for now.
 */
static void critical_toggle_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e); ESP_LOGI(TAG, "Switch event code=%d", code);
    lv_obj_t *sw = lv_event_get_target(e);
    bool is_on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (is_on) {
        ESP_LOGI(TAG, "Critical Toggle: ON");
    } else {
        ESP_LOGI(TAG, "Critical Toggle: OFF");
    }
}

static void lcd_hw_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = TOUCH_PIN_MISO,
        .sclk_io_num     = LCD_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = LCD_PIN_DC,
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_endian     = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));

    esp_lcd_panel_reset(panel);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_lcd_panel_init(panel);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_invert_color(panel, false);
    esp_lcd_panel_mirror(panel, true, false);
    esp_lcd_panel_set_gap(panel, 0, 0);
    esp_lcd_panel_disp_on_off(panel, true);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* T_IRQ: active-low open-drain output from XPT2046 — pull up internally */
    gpio_config_t irq_cfg = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_IRQ,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&irq_cfg));

    /* Register XPT2046 as a separate SPI device on SPI2 */
    spi_device_interface_config_t tp_devcfg = {
        .clock_speed_hz = 1000 * 1000,  // 1 MHz
        .mode           = 0,                 // SPI mode 0 (CPOL=0, CPHA=0)
        .spics_io_num   = TOUCH_PIN_CS,      // GPIO5
        .queue_size     = 1,
        .flags          = 0,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_SPI_HOST, &tp_devcfg, &s_tp_spi));

    /* Hand off to LVGL port */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .flags         = { .buff_dma = true },
    };
    lvgl_port_add_disp(&disp_cfg);
}

/* ── UI build (called once LVGL task is ready) ───────────────────────────── */

static void build_ui(lv_disp_t *disp)
{
    lv_theme_t *theme = lv_theme_default_init(disp,
        lv_palette_main(LV_PALETTE_RED),
        lv_palette_main(LV_PALETTE_GREY),
        true, LV_FONT_DEFAULT);
    lv_disp_set_theme(disp, theme);

    /* Root screen — red background */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, LCD_H_RES, LCD_V_RES);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_make(240, 240, 240), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    /* ── Header ──────────────────────────────────────────────────────────── */
    {
        /* Logo on the left — 350×350 source, zoomed to ~55px square */
        lv_obj_t *logo = lv_img_create(scr);
        lv_img_set_src(logo, &img_glazia_logo);
        lv_img_set_zoom(logo, 44);
        lv_img_set_pivot(logo, 0, 0);   /* scale from top-left */
        lv_obj_align(logo, LV_ALIGN_TOP_LEFT, 10, 6);

        /* "GLAZIA / Hub" text on the right — larger size for emphasis */
        lv_obj_t *t = lv_label_create(scr);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_26, LV_PART_MAIN);
        lv_obj_set_style_text_color(t, lv_color_black(), LV_PART_MAIN);
        lv_label_set_text(t, "GLAZIA");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 14, 8);

        lv_obj_t *s = lv_label_create(scr);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(s, lv_color_black(), LV_PART_MAIN);
        lv_label_set_text(s, "Hub");
        lv_obj_align(s, LV_ALIGN_TOP_MID, 14, 42);
    }

    /* ── Status / sensor-data panel ─────────────────────────────────────── */
    {
        lv_obj_t *panel = lv_obj_create(scr);
        lv_obj_set_pos(panel, 8, 68);
        lv_obj_set_size(panel, 224, 70);
        lv_obj_set_style_bg_color(panel, lv_color_make(20, 20, 20), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(panel, lv_color_make(238, 28, 37), LV_PART_MAIN);
        lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(panel, 8, LV_PART_MAIN);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        /* Status text — updated by display_show() */
        s_status_label = lv_label_create(panel);
        lv_label_set_text(s_status_label, "Starting up...");
        lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_status_label, 208);
        lv_obj_set_style_text_color(s_status_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

        /* Location / source — updated by display_hub_location() / display_sensor_location() */
        s_location_label = lv_label_create(panel);
        lv_label_set_text(s_location_label, "Location: --");
        lv_label_set_long_mode(s_location_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_location_label, 208);
        lv_obj_set_style_text_color(s_location_label, lv_color_make(200, 200, 200), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_location_label, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_align(s_location_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    /* ── Critical toggle (always ON while hub is powered) ────────────────── */
    {
        s_critical_sw = lv_switch_create(scr);
        lv_obj_set_size(s_critical_sw, 52, 26);
        lv_obj_align(s_critical_sw, LV_ALIGN_TOP_MID, 0, 144);
        lv_obj_set_ext_click_area(s_critical_sw, 40);  /* extend hitbox 40px in all directions */
        lv_obj_add_flag(
            s_critical_sw,
            LV_OBJ_FLAG_PRESS_LOCK
        );

        lv_obj_add_state(
            s_critical_sw,
            LV_STATE_CHECKED
        );
        lv_obj_add_event_cb(s_critical_sw, critical_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(s_critical_sw, critical_toggle_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_style_bg_color(s_critical_sw, lv_color_make(238, 28, 37),
                                   LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(s_critical_sw, LV_OPA_COVER,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_critical_sw, lv_color_white(),
                                   LV_PART_KNOB | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_critical_sw, LV_OPA_COVER,
                                  LV_PART_KNOB | LV_STATE_DEFAULT);

        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "Critical Toggle");
        lv_obj_set_style_text_color(lbl, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 175);
    }

    /* ── Sensor list panel — scrollable ──────────────────────────────────── */
    {
        lv_obj_t *panel = lv_obj_create(scr);
        lv_obj_set_pos(panel, 8, 192);
        lv_obj_set_size(panel, 224, 120);
        lv_obj_set_style_bg_color(panel, lv_color_make(20, 20, 20), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(panel, lv_color_make(238, 28, 37), LV_PART_MAIN);
        lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(panel, 8, LV_PART_MAIN);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        s_sensor_label = lv_label_create(panel);
        lv_label_set_text(s_sensor_label,
                          "No sensors paired yet.\nPress hub button to pair.");
        lv_label_set_long_mode(s_sensor_label, LV_LABEL_LONG_WRAP);
        lv_label_set_recolor(s_sensor_label, true);
        lv_obj_set_width(s_sensor_label, 208);
        lv_obj_set_style_text_color(s_sensor_label,
                                    lv_color_make(200, 200, 200), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_sensor_label,
                                   &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_align(s_sensor_label, LV_ALIGN_TOP_LEFT, 0, 4);
    }

    lv_scr_load(scr);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void display_init(void)
{
    lcd_hw_init();

    /* Wait up to 2 s for the LVGL task to be ready before building the UI. */
    if (!lvgl_port_lock(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "LVGL lock timed out — display not ready");
        return;
    }
    build_ui(lv_disp_get_default());

    /* Register XPT2046 as LVGL pointer input device inside the lock */
    lv_indev_drv_init(&s_tp_drv);
    s_tp_drv.type         = LV_INDEV_TYPE_POINTER;
    s_tp_drv.read_cb      = xpt2046_read_cb;
    s_tp_drv.scroll_limit = 50;
    s_tp_indev = lv_indev_drv_register(&s_tp_drv);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Display ready — SPI direct, MOSI=GPIO%d, touch enabled", LCD_PIN_MOSI);
}

/* Thread-safe label update helper */
static void label_set(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    lv_label_set_text(label, text);
    lvgl_port_unlock();
}

void display_show(const char *line1, const char *line2)
{
    char buf[128];
    if (line2 && line2[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s\n%s", line1, line2);
    } else {
        snprintf(buf, sizeof(buf), "%s", line1);
    }
    label_set(s_status_label, buf);
    ESP_LOGI(TAG, "Display: [%s] [%s]", line1, line2 ? line2 : "");
}

void display_hub_location(const char *home_name)
{
    if (!home_name) return;
    char buf[72];
    snprintf(buf, sizeof(buf), "Hub: %s", home_name);
    label_set(s_location_label, buf);
}

void display_sensor_location(const char *mac_str)
{
    if (!mac_str) return;
    char buf[72];
    snprintf(buf, sizeof(buf), "From: %s", mac_str);
    label_set(s_location_label, buf);
}

void display_sensor_list(void)
{
    if (!s_sensor_label) return;

    char sensors[140];
    espnow_get_sensor_list_str(sensors, sizeof(sensors));

    /* Build "Name : #00FF00 Online#" / "#FF0000 Offline#" per entry */
    char out[512];
    int  pos = 0;
    char buf[150];
    strncpy(buf, sensors, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *entry = buf;
    int   count = 0;

    while (entry && *entry && pos < (int)sizeof(out) - 1) {
        char *next   = strchr(entry, ';');
        if (next) *next++ = '\0';

        char *name   = entry;
        char *loc    = strchr(name, '|');
        char *status = NULL;
        if (loc) {
            *loc++ = '\0';
            status = strchr(loc, '|');
            if (status) *status++ = '\0';
        }

        bool online = (status && strcmp(status, "ON") == 0);
        pos += snprintf(out + pos, sizeof(out) - pos,
                        "%s : %s\n",
                        name ? name : "?",
                        online ? "#00FF00 Online#" : "#FF0000 Offline#");
        count++;
        entry = next;
    }

    if (count == 0) {
        strncpy(out, "No sensors paired.\nPress hub button to pair.",
                sizeof(out) - 1);
    } else {
        int len = (int)strlen(out);
        if (len > 0 && out[len - 1] == '\n') out[len - 1] = '\0';
    }

    label_set(s_sensor_label, out);
    ESP_LOGI(TAG, "Display sensor list: %s", sensors[0] ? sensors : "(empty)");
}

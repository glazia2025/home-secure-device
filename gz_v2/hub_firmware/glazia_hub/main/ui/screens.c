#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../state.h"
#include "screens.h"
#include "images.h"
#include "fonts.h"
#include "actions.h"
#include "vars.h"
#include "styles.h"
#include "ui.h"
#include "widgets/lv_label.h"
#include "extra/libs/qrcode/qrcodegen.h"

objects_t objects;

/* Render a QR into an lv_qrcode canvas using qrcodegen_encodeText() instead of
 * LVGL's lv_qrcode_update(), which hardcodes qrcodegen_encodeBinary() (byte
 * mode). For a hub MAC like "30:ED:A0:EA:18:AE" every character is in the QR
 * alphanumeric set, so encodeText picks alphanumeric mode → version 1 (21×21)
 * with larger modules, versus byte mode's version 2 (25×25). The bigger modules
 * are what make it scannable on the backlit TFT (a plain string-to-QR site
 * produces the same version-1 code, which scans; byte mode did not).
 * The canvas is INDEXED_1BIT: palette index 1 = light, 0 = dark (set by
 * lv_qrcode_create), matching lv_qrcode's own convention. */
#define HUB_QR_MAX_VERSION 6
static void hub_qr_render(lv_obj_t *canvas, const char *text)
{
    if (!text || !text[0]) return;

    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(HUB_QR_MAX_VERSION)];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(HUB_QR_MAX_VERSION)];
    bool ok = qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, HUB_QR_MAX_VERSION,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) return;

    lv_img_dsc_t *img = lv_canvas_get_img(canvas);
    lv_coord_t obj_w = img->header.w;
    int qr_size = qrcodegen_getSize(qr);
    int scale = obj_w / qr_size;
    if (scale < 1) scale = 1;
    int margin = (obj_w - qr_size * scale) / 2;

    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);

    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            lv_color_t c = qrcodegen_getModule(qr, x, y) ? lv_color_black() : lv_color_white();
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    lv_canvas_set_px_color(canvas, margin + x * scale + dx,
                                           margin + y * scale + dy, c);
                }
            }
        }
        /* Yield every 3 rows so the idle task can reset the watchdog timer.
         * The outer loop is only ~21 iterations, so the total extra delay is
         * at most 7 × 10 ms = 70 ms — well within any WDT budget. */
        if ((y % 3) == 2) vTaskDelay(1);
    }
}

#define UI_LAYOUT_DEBUG 0
#define SCREEN_W 720
#define SCREEN_H 1280
#define PAGE_X 24
#define PAGE_W 672

lv_obj_t *tick_value_change_obj;

static void base_screen(lv_obj_t *obj)
{
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, SCREEN_W, SCREEN_H);
    ui_style_screen_bg(obj);
}

static lv_obj_t *card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                      lv_coord_t w, lv_coord_t h, lv_coord_t r)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    ui_style_glass_card(obj, r);
    return obj;
}

static lv_obj_t *panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, lv_coord_t r)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    ui_style_panel(obj, r);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, const lv_font_t *font,
                       uint32_t color, lv_text_align_t align, lv_label_long_mode_t mode)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(obj, align, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(obj, mode);
    lv_label_set_text(obj, text);
    return obj;
}

static lv_obj_t *icon(lv_obj_t *parent, const lv_img_dsc_t *src, lv_coord_t x, lv_coord_t y,
                      uint16_t zoom, uint32_t recolor, bool do_recolor)
{
    lv_obj_t *obj = lv_img_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_img_set_src(obj, src);
    lv_img_set_zoom(obj, zoom);
    if (do_recolor) {
        lv_obj_set_style_img_recolor(obj, lv_color_hex(recolor), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *dot(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t size, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, size, size);
    ui_style_dot(obj, color);
    return obj;
}

static lv_obj_t *back_button(lv_obj_t *parent, uint32_t accent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, PAGE_X, 24);
    lv_obj_set_size(btn, 90, 90);
    ui_style_glass_card(btn, 8);
    icon(btn, &img_back, -6, -6, 426, accent, true);
    return btn;
}

static void add_nav_title(lv_obj_t *parent, const char *title, uint32_t accent, lv_obj_t **back_handle)
{
    lv_obj_t *nav = card(parent, PAGE_X, 24, PAGE_W, 126, 39);
    *back_handle = back_button(nav, accent);
    lv_obj_set_pos(*back_handle, 12, 18);
    label(nav, title, 120, 27, 492, 72, &lv_font_montserrat_14,
          UI_COLOR_TEXT_PRIMARY, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);
}

static void make_switch(lv_obj_t *parent, lv_obj_t **handle, lv_coord_t x, lv_coord_t y, bool checked)
{
    lv_obj_t *sw = lv_switch_create(parent);
    *handle = sw;
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_size(sw, 162, 72);
    ui_style_toggle(sw);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
}

static void create_bottom_bar(lv_obj_t *parent)
{
    objects.hub_state_cont = card(parent, PAGE_X, 822, PAGE_W, 114, 39);
    make_switch(objects.hub_state_cont, &objects.obj1, 12, 7, true);

    objects.button = lv_btn_create(objects.hub_state_cont);
    lv_obj_set_pos(objects.button, 366, 15);
    lv_obj_set_size(objects.button, 282, 84);
    ui_style_settings_button(objects.button);
    lv_obj_set_style_bg_color(objects.button, lv_color_hex(UI_COLOR_RED),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    // unused settings_zoom
    objects.settings_icon = icon(objects.button, &img_settings, 0, 0, 450,
                                 UI_COLOR_PRIMARY_TEXT, true);
    objects.obj2 = label(objects.button, "Settings", 90, 21, 174, 42, &lv_font_montserrat_12,
                         UI_COLOR_PRIMARY_TEXT, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
}

static void create_home_content(lv_obj_t *root)
{
    objects.home_time = NULL;
    objects.home_date = NULL;
    objects.strip_date = NULL;

    objects.cont_logo_card = card(root, PAGE_X, 24, PAGE_W, 168, 39);
    objects.obj0 = lv_img_create(objects.cont_logo_card);
    lv_obj_set_pos(objects.obj0, -69, -69);
    lv_img_set_src(objects.obj0, &img_gz_logo);
    lv_img_set_zoom(objects.obj0, 405);
    lv_obj_clear_flag(objects.obj0, LV_OBJ_FLAG_SCROLLABLE);
    objects.welcome_home = label(objects.cont_logo_card, "Welcome", 168, 27, 474, 54,
                                 &lv_font_montserrat_14, UI_COLOR_TEXT_PRIMARY,
                                 LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);

    objects.loc_cont_1 = lv_obj_create(objects.cont_logo_card);
    lv_obj_set_pos(objects.loc_cont_1, 168, 93);
    lv_obj_set_size(objects.loc_cont_1, 474, 48);
    ui_style_transparent(objects.loc_cont_1);
    objects.status_dot = dot(objects.loc_cont_1, 0, 15, 15, UI_COLOR_GREEN);
    objects.hub_status = label(objects.loc_cont_1, "Online", 30, 3, 126, 39,
                               &lv_font_montserrat_10, UI_COLOR_GREEN,
                               LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    objects.hub_location_dot = dot(objects.loc_cont_1, 153, 18, 9, UI_COLOR_GREEN);
    objects.hub_location = label(objects.loc_cont_1, "HUB_loc", 177, 3, 288, 39,
                                 &lv_font_montserrat_10, UI_COLOR_GREEN,
                                 LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_DOT);

    objects.sensor_hub = card(root, PAGE_X, 210, PAGE_W, 72, 33);
    dot(objects.sensor_hub, 36, 27, 15, UI_COLOR_AMBER);
    objects.sensor_info = label(objects.sensor_hub, "0 sensor nodes active", 81, 18, 555, 36,
                                &lv_font_montserrat_10, UI_COLOR_TEXT_SECONDARY,
                                LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_DOT);

    objects.temp_cont = card(root, PAGE_X, 300, 327, 354, 39);
    objects.temp_label = lv_obj_create(objects.temp_cont);
    lv_obj_set_pos(objects.temp_label, 24, 21);
    lv_obj_set_size(objects.temp_label, 279, 54);
    ui_style_transparent(objects.temp_label);
    uint16_t temp_icon_zoom = (uint16_t)((14U * 256U) / img_temp_img.header.w);
    icon(objects.temp_label, &img_temp_img, -18, -17, temp_icon_zoom, UI_COLOR_AMBER, true);
    objects.obj3 = label(objects.temp_label, "TEMP", 54, 6, 210, 42,
                         &lv_font_montserrat_10, UI_COLOR_TEXT_PRIMARY,
                         LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);

    objects.temp_arc = lv_arc_create(objects.temp_cont);
    lv_obj_set_pos(objects.temp_arc, 60, 69);
    lv_obj_set_size(objects.temp_arc, 204, 204);
    lv_arc_set_bg_angles(objects.temp_arc, 145, 35);
    lv_arc_set_range(objects.temp_arc, 0, 50);
    lv_arc_set_value(objects.temp_arc, 0);
    lv_obj_clear_flag(objects.temp_arc, LV_OBJ_FLAG_CLICKABLE);
    ui_style_arc_track(objects.temp_arc, UI_COLOR_AMBER);
    lv_obj_set_style_arc_rounded(objects.temp_arc, false,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(objects.temp_arc, false,
                                 LV_PART_INDICATOR | LV_STATE_DEFAULT);

    objects.temp_val = label(objects.temp_cont, "0.0", 72, 135, 180, 54,
                             &lv_font_montserrat_16, UI_COLOR_TEXT_PRIMARY,
                             LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.obj6 = label(objects.temp_cont, "°C", 102, 189, 120, 42,
                         &lv_font_montserrat_10, UI_COLOR_TEXT_DIM,
                         LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.obj4 = label(objects.temp_cont, "0", 30, 243, 90, 36, &lv_font_montserrat_8,
                         UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.obj5 = label(objects.temp_cont, "150", 210, 243, 90, 36, &lv_font_montserrat_8,
                         UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);

    objects.temp_mood = lv_obj_create(objects.temp_cont);
    lv_obj_set_pos(objects.temp_mood, 30, 282);
    lv_obj_set_size(objects.temp_mood, 267, 54);
    ui_style_status_pill(objects.temp_mood);
    objects.temp_img = dot(objects.temp_mood, 21, 18, 15, UI_COLOR_AMBER);
    objects.obj7 = label(objects.temp_mood, "Comfortable", 42, 9, 210, 30,
                         &lv_font_montserrat_8, UI_COLOR_AMBER,
                         LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);

    objects.aqi_cont = card(root, 369, 300, 327, 354, 39);
    objects.aqi_label = lv_obj_create(objects.aqi_cont);
    lv_obj_set_pos(objects.aqi_label, 24, 21);
    lv_obj_set_size(objects.aqi_label, 279, 54);
    ui_style_transparent(objects.aqi_label);
    objects.aqi_icon = icon(objects.aqi_label, &img_temp_img, -18, -17, temp_icon_zoom,
                            UI_COLOR_GREEN, true);
    label(objects.aqi_label, "AQI", 54, 6, 210, 42, &lv_font_montserrat_10,
          UI_COLOR_TEXT_PRIMARY, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);

    objects.aqi_arc = lv_arc_create(objects.aqi_cont);
    lv_obj_set_pos(objects.aqi_arc, 60, 69);
    lv_obj_set_size(objects.aqi_arc, 204, 204);
    lv_arc_set_bg_angles(objects.aqi_arc, 145, 35);
    lv_arc_set_range(objects.aqi_arc, 0, 500);
    lv_arc_set_value(objects.aqi_arc, 0);
    lv_obj_clear_flag(objects.aqi_arc, LV_OBJ_FLAG_CLICKABLE);
    ui_style_arc_track(objects.aqi_arc, UI_COLOR_GREEN);
    lv_obj_set_style_arc_rounded(objects.aqi_arc, false,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(objects.aqi_arc, false,
                                 LV_PART_INDICATOR | LV_STATE_DEFAULT);

    objects.aqi_val = label(objects.aqi_cont, "0", 72, 135, 180, 54,
                            &lv_font_montserrat_16, UI_COLOR_TEXT_PRIMARY,
                            LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.aqi_min = label(objects.aqi_cont, "0", 30, 243, 90, 36, &lv_font_montserrat_8,
                            UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.aqi_max = label(objects.aqi_cont, "1500", 210, 243, 90, 36, &lv_font_montserrat_8,
                            UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.aqi_mood = lv_obj_create(objects.aqi_cont);
    lv_obj_set_pos(objects.aqi_mood, 30, 282);
    lv_obj_set_size(objects.aqi_mood, 267, 54);
    ui_style_status_pill(objects.aqi_mood);
    objects.aqi_dot = dot(objects.aqi_mood, 21, 18, 15, UI_COLOR_GREEN);
    objects.aqi_state = label(objects.aqi_mood, "Healthy", 42, 9, 210, 30,
                              &lv_font_montserrat_8, UI_COLOR_GREEN,
                              LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);

    objects.hum_cont = card(root, PAGE_X, 672, PAGE_W, 132, 39);
    objects.hum_label = lv_obj_create(objects.hum_cont);
    lv_obj_set_pos(objects.hum_label, 36, 24);
    lv_obj_set_size(objects.hum_label, 276, 54);
    ui_style_transparent(objects.hum_label);
    uint16_t hum_icon_zoom = (uint16_t)((14U * 256U) / img_hum_img.header.w);
    icon(objects.hum_label, &img_hum_img, 0, 1, hum_icon_zoom * 1.2, UI_COLOR_VIOLET, true);
    objects.obj8 = label(objects.hum_label, "HUMIDITY", 66, 6, 210, 42,
                         &lv_font_montserrat_10, UI_COLOR_TEXT_PRIMARY,
                         LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    objects.hum_val = label(objects.hum_cont, "0", 288, 18, 93, 48,
                            &lv_font_montserrat_16, UI_COLOR_TEXT_PRIMARY,
                            LV_TEXT_ALIGN_RIGHT, LV_LABEL_LONG_CLIP);
    objects.obj11 = label(objects.hum_cont, "%", 387, 36, 36, 30,
                          &lv_font_montserrat_10, UI_COLOR_TEXT_DIM,
                          LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    objects.hum_mood = lv_obj_create(objects.hum_cont);
    lv_obj_set_pos(objects.hum_mood, 432, 24);
    lv_obj_set_size(objects.hum_mood, 216, 66);
    ui_style_status_pill(objects.hum_mood);
    objects.hum_img = dot(objects.hum_mood, 24, 24, 18, UI_COLOR_VIOLET);
    objects.obj12 = label(objects.hum_mood, "Moderate", 51, 15, 150, 33,
                          &lv_font_montserrat_10, UI_COLOR_VIOLET,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);
    objects.hum_bar = lv_bar_create(objects.hum_cont);
    lv_obj_set_pos(objects.hum_bar, 108, 99);
    lv_obj_set_size(objects.hum_bar, 300, 15);
    lv_bar_set_range(objects.hum_bar, 0, 100);
    lv_bar_set_value(objects.hum_bar, 0, LV_ANIM_OFF);
    ui_style_bar_track(objects.hum_bar, UI_COLOR_VIOLET);

    create_bottom_bar(root);
}

/* ── Registration screens (welcome + QR) ──────────────────────────────────
 * Light "Glazia" design over a full-bleed photo of the hub (img_welcome_bg),
 * adapted from the landscape handoff to the real portrait 720x1280 panel.
 * These two screens deliberately use the light-theme tokens / design fonts;
 * every other screen keeps the dark Ocean Night theme. */

static lv_img_dsc_t s_welcome_bg_dsc;
static bool s_welcome_bg_loaded = false;

/* Called from app_main to copy the background image from Flash XIP to PSRAM.
 * The ESP-Hosted SPI task (prio 23) starts before app_main and continuously
 * probes the C6 WiFi chip via SPI2 DMA. Those DMA transfers trigger L2-cache
 * coherency operations that suspend the shared L2 cache (Flash XIP + PSRAM).
 * A single 1.84 MB memcpy can stall for the full suspension window; if that
 * exceeds CONFIG_ESP_INT_WDT_TIMEOUT_MS the WDT fires.
 *
 * Fix: copy in 64 KB chunks with a vTaskDelay(1) yield between each chunk.
 * Each chunk takes ~3 ms to copy; even a 1000 ms L2 suspension mid-chunk
 * stays under the 2000 ms WDT budget (see sdkconfig). The yield resets the
 * interrupt watchdog and lets the SPI task drain its retry queue. */
void ui_preload_bg_image(void)
{
    if (s_welcome_bg_loaded) return;
    s_welcome_bg_dsc = img_welcome_bg;
    void *psram_ptr = heap_caps_malloc(img_welcome_bg.data_size, MALLOC_CAP_SPIRAM);
    if (psram_ptr) {
        ESP_LOGI("UI", "Pre-loading %lu B background image to PSRAM...",
                 (unsigned long)img_welcome_bg.data_size);
        const uint8_t *src = img_welcome_bg.data;
        uint8_t *dst = (uint8_t *)psram_ptr;
        size_t remaining = img_welcome_bg.data_size;
        const size_t CHUNK = 64 * 1024;
        while (remaining > 0) {
            size_t n = remaining < CHUNK ? remaining : CHUNK;
            memcpy(dst, src, n);
            dst += n; src += n; remaining -= n;
            vTaskDelay(1);
        }
        s_welcome_bg_dsc.data = (const uint8_t *)psram_ptr;
        ESP_LOGI("UI", "Background pre-load done");
    } else {
        ESP_LOGE("UI", "PSRAM alloc failed — background stays in Flash");
    }
    s_welcome_bg_loaded = true;
}

/* Full-screen photo background — just create the LVGL image object.
 * The pixel data pointer was set by ui_preload_bg_image(); no copy here. */
static lv_obj_t *reg_background(lv_obj_t *parent)
{
    lv_obj_t *bg = lv_img_create(parent);
    lv_img_set_src(bg, &s_welcome_bg_dsc);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return bg;
}

/* Tracked (letter-spaced) mono caption, e.g. "SMARTER LIVING". */
static lv_obj_t *mono_label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                            lv_coord_t w, uint32_t color, lv_text_align_t align)
{
    lv_obj_t *l = label(parent, text, x, y, w, 30, &lv_font_plexmono_18, color,
                        align, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_letter_space(l, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    return l;
}

/* Glazia logo (house mark + wordmark, 100x100 source). */
static lv_obj_t *reg_logo(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, uint16_t zoom)
{
    lv_obj_t *logo = lv_img_create(parent);
    lv_img_set_src(logo, &img_gz_logo);
    lv_img_set_zoom(logo, zoom);
    lv_obj_set_pos(logo, x, y);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_SCROLLABLE);
    return logo;
}

void create_screen_hub_register_welcome(void)
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.hub_register_welcome = obj;
    base_screen(obj);
    reg_background(obj);

    objects.reg_welcome_logo = reg_logo(obj, PAGE_X, 44, 360);

    mono_label(obj, "SMARTER LIVING", 28, 306, 400, UI_LIGHT_MONO, LV_TEXT_ALIGN_LEFT);

    label(obj, "Welcome to", 24, 344, 620, 86,
          &lv_font_grotesk_64, UI_LIGHT_TEXT,
          LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    label(obj, "Glazia", 24, 432, 620, 86,
          &lv_font_grotesk_64, UI_LIGHT_SLATE,
          LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);

    label(obj, "A safer, smarter and more connected home starts here.",
          28, 546, 540, 96,
          &lv_font_inter_30, UI_LIGHT_BODY,
          LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_WRAP);

    objects.reg_welcome_btn = lv_btn_create(obj);
    lv_obj_set_pos(objects.reg_welcome_btn, 24, 684);
    lv_obj_set_size(objects.reg_welcome_btn, 372, 100);
    ui_style_dark_pill(objects.reg_welcome_btn);
    lv_obj_set_style_pad_all(objects.reg_welcome_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *btn_label = label(objects.reg_welcome_btn, "Get Started", 0, -12, 372, 100,
                                &lv_font_inter_30, UI_DARK_PILL_TEXT,
                                LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);

    lv_obj_align(btn_label, LV_ALIGN_CENTER, -24, 0);

    lv_obj_t *btn_icon = icon(objects.reg_welcome_btn, &img_fwd, 0, 0, 130,
                              UI_DARK_PILL_TEXT, true);
    lv_obj_align(btn_icon, LV_ALIGN_RIGHT_MID, -40, 0);

    mono_label(obj, "BUILT FOR A SAFER TOMORROW", 28, 1214, 480,
               UI_LIGHT_MONO, LV_TEXT_ALIGN_LEFT);
    mono_label(obj, "GLAZIA", 520, 1214, 172, UI_LIGHT_MONO, LV_TEXT_ALIGN_RIGHT);

    tick_screen_hub_register_welcome();
}

void tick_screen_hub_register_welcome(void) {}

void create_screen_hub_register_qr(void)
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.hub_register_qr = obj;
    base_screen(obj);
    reg_background(obj);

    reg_logo(obj, PAGE_X, 40, 300);

    mono_label(obj, "SMARTER LIVING", 28, 190, 400, UI_LIGHT_MONO, LV_TEXT_ALIGN_LEFT);
    label(obj, "Welcome to", 24, 224, 620, 48,
          &lv_font_grotesk_34, UI_LIGHT_TEXT, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    label(obj, "a Safer Home", 24, 268, 620, 48,
          &lv_font_grotesk_34, UI_LIGHT_SLATE, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    label(obj, "Scan the code to register your Glazia Hub and start your smarter living journey.",
          28, 328, 540, 96,
          &lv_font_inter_22, UI_LIGHT_BODY, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_WRAP);

    /* Frosted registration card. */
    lv_obj_t *qr_card = lv_obj_create(obj);
    lv_obj_set_pos(qr_card, 60, 470);
    lv_obj_set_size(qr_card, 600, 760);
    ui_style_glass_light(qr_card, 30);

    /* "Setup" pill (kept as reg_qr_status so display.c could update it). */
    lv_obj_t *setup_pill = lv_obj_create(qr_card);
    lv_obj_set_pos(setup_pill, (600 - 130) / 2, 28);
    lv_obj_set_size(setup_pill, 130, 48);
    lv_obj_set_style_bg_color(setup_pill, lv_color_hex(UI_SETUP_PILL), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(setup_pill, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(setup_pill, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(setup_pill, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(setup_pill, LV_OBJ_FLAG_SCROLLABLE);
    objects.reg_qr_status = label(setup_pill, "Setup", 0, 0, 130, 48,
                                  &lv_font_inter_22, UI_SETUP_PILL_TEXT,
                                  LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    lv_obj_align(objects.reg_qr_status, LV_ALIGN_CENTER, 0, 0);

    label(qr_card, "Scan to Register the Hub", 0, 96, 600, 48,
          &lv_font_grotesk_34, UI_LIGHT_TEXT, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    label(qr_card, "Open the Glazia app and scan this code to connect the hub.",
          40, 150, 520, 72,
          &lv_font_inter_22, UI_LIGHT_BODY, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);

    /* White opaque wrapper — gives the QR a clean quiet zone against the glass card. */
    lv_obj_t *qr_wrap = lv_obj_create(qr_card);
    lv_obj_set_pos(qr_wrap, (600 - 320) / 2, 244);
    lv_obj_set_size(qr_wrap, 320, 320);
    lv_obj_set_style_bg_color(qr_wrap, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(qr_wrap, 0, 0);
    lv_obj_set_style_radius(qr_wrap, 8, 0);
    lv_obj_set_style_pad_all(qr_wrap, 0, 0);
    lv_obj_clear_flag(qr_wrap, LV_OBJ_FLAG_SCROLLABLE);

    /* QR canvas is 256 px — the 32 px margin on each side is the white quiet zone. */
    objects.reg_qr_code = lv_qrcode_create(qr_wrap, 256, lv_color_black(), lv_color_white());
    lv_obj_align(objects.reg_qr_code, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(objects.reg_qr_code, LV_OBJ_FLAG_SCROLLABLE);
    hub_qr_render(objects.reg_qr_code, g_hub_mac);

    mono_label(qr_card, "QR CODE", 0, 586, 600, UI_LIGHT_MONO, LV_TEXT_ALIGN_CENTER);
    char mac_line[40];
    snprintf(mac_line, sizeof(mac_line), "MAC: %s", g_hub_mac);
    label(qr_card, mac_line, 0, 616, 600, 30,
          &lv_font_plexmono_18, UI_LIGHT_TEXT, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);

    /* Registration completes server-side (auto-advances to the dashboard), so
     * this block is a status hint rather than the mockup's "Simulate" action. */
    lv_obj_t *status_pill = lv_obj_create(qr_card);
    lv_obj_set_pos(status_pill, 60, 672);
    lv_obj_set_size(status_pill, 480, 72);
    ui_style_dark_pill(status_pill);
    lv_obj_clear_flag(status_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *status_lbl = label(status_pill, "Waiting for the app...", 0, 0, 480, 72,
                                 &lv_font_inter_22, UI_DARK_PILL_TEXT,
                                 LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    lv_obj_align(status_lbl, LV_ALIGN_CENTER, 0, 0);

    tick_screen_hub_register_qr();
}

void tick_screen_hub_register_qr(void) {}

void create_screen_hub_online()
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.hub_online = obj;
    base_screen(obj);
    create_home_content(obj);
    tick_screen_hub_online();
}

void tick_screen_hub_online(void) {}

static void create_option_row(lv_obj_t *parent, lv_obj_t **handle, lv_obj_t **title_handle,
                              lv_obj_t **sub_handle, const lv_img_dsc_t *img,
                              const char *title, const char *sub, lv_coord_t y,
                              uint32_t accent)
{
    lv_obj_t *row = lv_btn_create(parent);
    *handle = row;
    lv_obj_set_pos(row, PAGE_X, y);
    lv_obj_set_size(row, PAGE_W, 156);
    ui_style_glass_card(row, 13);
    lv_obj_t *icon_box = panel(row, 24, 21, 114, 114, 30);
    icon(icon_box, img, -30, -30, 342, accent, true);
    *title_handle = label(row, title, 162, 30, 450, 52, &lv_font_montserrat_10,
                          UI_COLOR_TEXT_PRIMARY, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_DOT);
    *sub_handle = label(row, sub, 162, 96, 450, 42, &lv_font_montserrat_8,
                        UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_DOT);
    icon(row, &img_fwd, 612, 66, 174, UI_COLOR_TEXT_DIM, true);
}

void create_screen_settings_menu()
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.settings_menu = obj;
    base_screen(obj);
    add_nav_title(obj, "Settings", UI_COLOR_AMBER, &objects.obj14);
    objects.settings_menu_cont = lv_obj_create(obj);
    lv_obj_set_pos(objects.settings_menu_cont, 0, 162);
    lv_obj_set_size(objects.settings_menu_cont, SCREEN_W, 258);
    ui_style_transparent(objects.settings_menu_cont);
    create_option_row(obj, &objects.fingerprint_option, &objects.obj16, &objects.obj15,
                      &img_finger, "Add Fingerprint", "Register a new fingerprint",
                      162, UI_COLOR_VIOLET);
    create_option_row(obj, &objects.sensor_nodes_option, &objects.obj17, &objects.obj18,
                      &img_nodes, "Sensor Nodes", "Manage and configure sensors",
                      330, UI_COLOR_VIOLET);
    create_option_row(obj, &objects.about_section, &objects.obj20, &objects.obj19,
                      &img_about, "About", "Learn more about Glazia",
                      498, UI_COLOR_VIOLET);
    objects.obj13 = label(obj, "", 0, 0, 3, 3, &lv_font_montserrat_8,
                          UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    tick_screen_settings_menu();
}

void tick_screen_settings_menu(void) {}

void create_screen_sensor_nodes_setting()
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.sensor_nodes_setting = obj;
    base_screen(obj);
    add_nav_title(obj, "Sensor Nodes", UI_COLOR_AMBER, &objects.obj44);
    objects.obj42 = label(obj, "", 0, 0, 3, 3, &lv_font_montserrat_8,
                          UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    objects.obj43 = label(obj, "Enable or disable sensor nodes as needed.", PAGE_X + 6, 156,
                          660, 48, &lv_font_montserrat_8, UI_COLOR_TEXT_DIM,
                          LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_DOT);
    objects.settings_menu_cont_1 = lv_obj_create(obj);
    lv_obj_set_pos(objects.settings_menu_cont_1, 0, 210);
    lv_obj_set_size(objects.settings_menu_cont_1, SCREEN_W, 900);
    ui_style_transparent(objects.settings_menu_cont_1);
    lv_obj_set_scroll_dir(objects.settings_menu_cont_1, LV_DIR_VER);

    objects.add_sensor_button = lv_btn_create(obj);
    lv_obj_set_pos(objects.add_sensor_button, PAGE_X, 1140);
    lv_obj_set_size(objects.add_sensor_button, PAGE_W, 96);
    ui_style_glass_card(objects.add_sensor_button, 12);
    objects.obj45 = label(objects.add_sensor_button, "+ Add Another Sensor", 0, 0, PAGE_W - 60, 96,
                          &lv_font_montserrat_12, UI_COLOR_AMBER,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);
    lv_obj_align(objects.obj45, LV_ALIGN_CENTER, -30, 0);
    objects.obj46 = icon(objects.add_sensor_button, &img_fwd, 0, 0, 174,
                         UI_COLOR_TEXT_DIM, true);
    lv_obj_align(objects.obj46, LV_ALIGN_RIGHT_MID, -24, 0);
    tick_screen_sensor_nodes_setting();
}

void tick_screen_sensor_nodes_setting(void) {}

void create_screen_about_glazia()
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.about_glazia = obj;
    base_screen(obj);
    add_nav_title(obj, "About", UI_COLOR_VIOLET, &objects.obj47);

    lv_obj_t *logo = lv_img_create(obj);
    lv_img_set_src(logo, &img_gz_logo);
    lv_img_set_zoom(logo, 600);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 135);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_SCROLLABLE);
    // label(obj, "About Glazia", 0, 108, SCREEN_W, 18, &lv_font_montserrat_14,
    //       UI_COLOR_TEXT_PRIMARY, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);

    lv_obj_t *body = card(obj, PAGE_X, 402, PAGE_W, 498, 39);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.obj48 = label(body,
        "Glazia is dedicated to creating smart, reliable, and elegant IoT solutions for modern living and workplaces.\n\n"
        "Our mission is to connect spaces, simplify control, and empower users through intelligent technology - making everyday environments smarter and more responsive.",
        24, 24, 624, 360, &lv_font_montserrat_10, UI_COLOR_TEXT_SECONDARY,
        LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);
    label(body, "\xC2\xA9 Glazia Technologies - All rights reserved.", 24, 435, 624, 48,
          &lv_font_montserrat_8, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);
    tick_screen_about_glazia();
}

void tick_screen_about_glazia(void) {}

void create_screen_fingerprint_setting()
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.fingerprint_setting = obj;
    base_screen(obj);
    lv_obj_t *nav = card(obj, PAGE_X, 24, PAGE_W, 120, 39);
    objects.obj54 = back_button(nav, UI_COLOR_AMBER);
    lv_obj_set_pos(objects.obj54, 12, 12);
    objects.obj53 = label(nav, "Add Fingerprint", 120, 30, 432, 54,
                          &lv_font_montserrat_14, UI_COLOR_TEXT_PRIMARY,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);

    objects.fingerprint_scan_ui = card(obj, PAGE_X, 174, PAGE_W, 762, 42);
    objects.obj50 = lv_spinner_create(objects.fingerprint_scan_ui, 1000, 60);
    lv_obj_set_pos(objects.obj50, 192, 60);
    lv_obj_set_size(objects.obj50, 288, 288);
    ui_style_arc_track(objects.obj50, UI_COLOR_AMBER);
    lv_obj_t *fp_badge = panel(objects.fingerprint_scan_ui, 89, 45, 46, 46, LV_RADIUS_CIRCLE);
    icon(fp_badge, &img_finger, -18, -18, 570, UI_COLOR_AMBER, true);

    objects.obj49 = label(objects.fingerprint_scan_ui, "Place your finger\non the sensor",
                          60, 378, 552, 108, &lv_font_montserrat_12,
                          UI_COLOR_TEXT_PRIMARY, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);
    objects.fingerprint_instruction = label(objects.fingerprint_scan_ui,
                                            "Keep your finger on the sensor\nfor registration.",
                                            54, 498, 564, 96, &lv_font_montserrat_8,
                                            UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER,
                                            LV_LABEL_LONG_WRAP);
    objects.obj51 = lv_bar_create(objects.fingerprint_scan_ui);
    lv_obj_set_pos(objects.obj51, 111, 615);
    lv_obj_set_size(objects.obj51, 450, 24);
    lv_bar_set_range(objects.obj51, 0, 100);
    lv_bar_set_value(objects.obj51, 0, LV_ANIM_OFF);
    dot(objects.fingerprint_scan_ui, 183, 684, 21, UI_COLOR_AMBER);
    objects.obj52 = label(objects.fingerprint_scan_ui, "Processing...", 222, 672, 360, 72,
                          &lv_font_montserrat_10, UI_COLOR_TEXT_SECONDARY,
                          LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_WRAP);
    tick_screen_fingerprint_setting();
}

void tick_screen_fingerprint_setting(void) {}

void create_screen_add_another__sensor()
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.add_another__sensor = obj;
    base_screen(obj);
    add_nav_title(obj, "Add Sensor", UI_COLOR_AMBER, &objects.obj55);

    lv_obj_t *card_obj = card(obj, PAGE_X, 174, PAGE_W, 618, 39);
    lv_obj_t *add_logo = lv_img_create(card_obj);
    lv_img_set_src(add_logo, &img_gz_logo);
    lv_img_set_zoom(add_logo, 600);
    lv_obj_align(add_logo, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_clear_flag(add_logo, LV_OBJ_FLAG_SCROLLABLE);
    label(card_obj, "Open the Glazia app\nand scan your sensor's QR code",
          36, 378, PAGE_W - 72, 108,
          &lv_font_montserrat_10, UI_COLOR_TEXT_SECONDARY,
          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);

    objects.added_sensor_data = card(obj, PAGE_X, 174, PAGE_W, 618, 39);
    lv_obj_add_flag(objects.added_sensor_data, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *check_badge = panel(objects.added_sensor_data, 84, 25, 56, 56, LV_RADIUS_CIRCLE);
    icon(check_badge, &img_tick, 39, 39, 384, UI_COLOR_GREEN, true);
    objects.obj57 = label(objects.added_sensor_data, "Sensor Added!", 0, 282, PAGE_W, 60,
                          &lv_font_montserrat_16, UI_COLOR_TEXT_PRIMARY,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.obj58 = label(objects.added_sensor_data, "sensor_loc_k has been added",
                          54, 378, 564, 96, &lv_font_montserrat_12, UI_COLOR_GREEN,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);
    label(objects.added_sensor_data,
          "The sensor is now registered and will appear in your Sensor Nodes list.",
          54, 498, 564, 84, &lv_font_montserrat_10, UI_COLOR_TEXT_DIM,
          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);

    objects.obj59 = lv_btn_create(obj);
    lv_obj_set_pos(objects.obj59, PAGE_X, 816);
    lv_obj_set_size(objects.obj59, PAGE_W, 96);
    ui_style_primary_button(objects.obj59);
    objects.obj60 = label(objects.obj59, "Add Sensor", 0, 0, PAGE_W, 96,
                          &lv_font_montserrat_14, UI_COLOR_PRIMARY_TEXT,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    lv_obj_align(objects.obj60, LV_ALIGN_CENTER, 0, 0);
    tick_screen_add_another__sensor();
}

void tick_screen_add_another__sensor(void) {}

typedef void (*tick_screen_func_t)(void);
tick_screen_func_t tick_screen_funcs[] = {
    tick_screen_hub_register_welcome,
    tick_screen_hub_register_qr,
    tick_screen_hub_online,
    tick_screen_settings_menu,
    tick_screen_sensor_nodes_setting,
    tick_screen_about_glazia,
    tick_screen_fingerprint_setting,
    tick_screen_add_another__sensor,
};

void tick_screen(int screen_index)
{
    if (screen_index >= 0 && screen_index < (int)(sizeof(tick_screen_funcs) / sizeof(tick_screen_funcs[0]))) {
        tick_screen_funcs[screen_index]();
    }
}

void tick_screen_by_id(enum ScreensEnum screenId)
{
    if (screenId >= _SCREEN_ID_FIRST && screenId <= _SCREEN_ID_LAST) {
        tick_screen_funcs[screenId - 1]();
    }
}

ext_font_desc_t fonts[] = {
#if LV_FONT_MONTSERRAT_8
    { "MONTSERRAT_8", &lv_font_montserrat_8 },
#endif
#if LV_FONT_MONTSERRAT_10
    { "MONTSERRAT_10", &lv_font_montserrat_10 },
#endif
#if LV_FONT_MONTSERRAT_12
    { "MONTSERRAT_12", &lv_font_montserrat_12 },
#endif
#if LV_FONT_MONTSERRAT_14
    { "MONTSERRAT_14", &lv_font_montserrat_14 },
#endif
#if LV_FONT_MONTSERRAT_16
    { "MONTSERRAT_16", &lv_font_montserrat_16 },
#endif
#if LV_FONT_MONTSERRAT_18
    { "MONTSERRAT_18", &lv_font_montserrat_18 },
#endif
#if LV_FONT_MONTSERRAT_26
    { "MONTSERRAT_26", &lv_font_montserrat_26 },
#endif
};

uint32_t active_theme_index = 0;

void create_screens(void)
{
    lv_disp_t *dispp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp,
        lv_palette_main(LV_PALETTE_RED), lv_palette_main(LV_PALETTE_PURPLE),
        true, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);

    create_screen_hub_register_welcome();
    create_screen_hub_register_qr();
    create_screen_hub_online();
    create_screen_settings_menu();
    create_screen_sensor_nodes_setting();
    create_screen_about_glazia();
    create_screen_fingerprint_setting();
    create_screen_add_another__sensor();
}

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../state.h"
#include "screens.h"
#include "images.h"
#include "fonts.h"
#include "actions.h"
#include "vars.h"
#include "styles.h"
#include "ui.h"
#include "widgets/lv_label.h"

objects_t objects;

#define UI_LAYOUT_DEBUG 0
#define SCREEN_W 240
#define SCREEN_H 320
#define PAGE_X 8
#define PAGE_W 224

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
    lv_obj_set_pos(btn, PAGE_X, 8);
    lv_obj_set_size(btn, 30, 30);
    ui_style_glass_card(btn, 8);
    icon(btn, &img_back, -2, -2, 142, accent, true);
    return btn;
}

static void add_nav_title(lv_obj_t *parent, const char *title, uint32_t accent, lv_obj_t **back_handle)
{
    lv_obj_t *nav = card(parent, PAGE_X, 8, PAGE_W, 40, 13);
    *back_handle = back_button(nav, accent);
    lv_obj_set_pos(*back_handle, 4, 4);
    label(nav, title, 40, 10, 144, 18, &lv_font_montserrat_14,
          UI_COLOR_TEXT_PRIMARY, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);
}

static void make_switch(lv_obj_t *parent, lv_obj_t **handle, lv_coord_t x, lv_coord_t y, bool checked)
{
    lv_obj_t *sw = lv_switch_create(parent);
    *handle = sw;
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_size(sw, 54, 24);
    ui_style_toggle(sw);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
}

static void create_bottom_bar(lv_obj_t *parent)
{
    objects.hub_state_cont = card(parent, PAGE_X, 274, PAGE_W, 38, 13);
    make_switch(objects.hub_state_cont, &objects.obj1, 12, 7, true);

    objects.button = lv_btn_create(objects.hub_state_cont);
    lv_obj_set_pos(objects.button, 122, 5);
    lv_obj_set_size(objects.button, 94, 28);
    ui_style_settings_button(objects.button);
    lv_obj_set_style_bg_color(objects.button, lv_color_hex(UI_COLOR_RED),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    uint16_t settings_zoom = (uint16_t)((12U * 256U) / img_settings.header.w);
    objects.settings_icon = icon(objects.button, &img_settings, 0, 0, 150,
                                 UI_COLOR_PRIMARY_TEXT, true);
    objects.obj2 = label(objects.button, "Settings", 30, 7, 58, 14, &lv_font_montserrat_12,
                         UI_COLOR_PRIMARY_TEXT, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
}

static void create_home_content(lv_obj_t *root)
{
    objects.home_time = NULL;
    objects.home_date = NULL;
    objects.strip_date = NULL;

    objects.cont_logo_card = card(root, PAGE_X, 8, PAGE_W, 56, 13);
    objects.obj0 = lv_img_create(objects.cont_logo_card);
    lv_obj_set_pos(objects.obj0, -23, -23);
    lv_img_set_src(objects.obj0, &img_gz_logo);
    lv_img_set_zoom(objects.obj0, 135);
    lv_obj_clear_flag(objects.obj0, LV_OBJ_FLAG_SCROLLABLE);
    objects.welcome_home = label(objects.cont_logo_card, "Welcome", 56, 9, 158, 18,
                                 &lv_font_montserrat_14, UI_COLOR_TEXT_PRIMARY,
                                 LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);

    objects.loc_cont_1 = lv_obj_create(objects.cont_logo_card);
    lv_obj_set_pos(objects.loc_cont_1, 56, 31);
    lv_obj_set_size(objects.loc_cont_1, 158, 16);
    ui_style_transparent(objects.loc_cont_1);
    objects.status_dot = dot(objects.loc_cont_1, 0, 5, 5, UI_COLOR_GREEN);
    objects.hub_status = label(objects.loc_cont_1, "Online", 10, 1, 42, 13,
                               &lv_font_montserrat_10, UI_COLOR_GREEN,
                               LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    objects.hub_location_dot = dot(objects.loc_cont_1, 51, 6, 3, UI_COLOR_GREEN);
    objects.hub_location = label(objects.loc_cont_1, "HUB_loc", 59, 1, 96, 13,
                                 &lv_font_montserrat_10, UI_COLOR_GREEN,
                                 LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_DOT);

    objects.sensor_hub = card(root, PAGE_X, 70, PAGE_W, 24, 11);
    dot(objects.sensor_hub, 12, 9, 5, UI_COLOR_AMBER);
    objects.sensor_info = label(objects.sensor_hub, "0 sensor nodes active", 27, 6, 185, 12,
                                &lv_font_montserrat_10, UI_COLOR_TEXT_SECONDARY,
                                LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_DOT);

    objects.temp_cont = card(root, PAGE_X, 100, 109, 118, 13);
    objects.temp_label = lv_obj_create(objects.temp_cont);
    lv_obj_set_pos(objects.temp_label, 8, 7);
    lv_obj_set_size(objects.temp_label, 93, 18);
    ui_style_transparent(objects.temp_label);
    uint16_t temp_icon_zoom = (uint16_t)((14U * 256U) / img_temp_img.header.w);
    icon(objects.temp_label, &img_temp_img, -18, -17, temp_icon_zoom, UI_COLOR_AMBER, true);
    objects.obj3 = label(objects.temp_label, "TEMP", 18, 2, 70, 14,
                         &lv_font_montserrat_10, UI_COLOR_TEXT_PRIMARY,
                         LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);

    objects.temp_arc = lv_arc_create(objects.temp_cont);
    lv_obj_set_pos(objects.temp_arc, 20, 23);
    lv_obj_set_size(objects.temp_arc, 68, 68);
    lv_arc_set_bg_angles(objects.temp_arc, 145, 35);
    lv_arc_set_range(objects.temp_arc, 0, 50);
    lv_arc_set_value(objects.temp_arc, 0);
    lv_obj_clear_flag(objects.temp_arc, LV_OBJ_FLAG_CLICKABLE);
    ui_style_arc_track(objects.temp_arc, UI_COLOR_AMBER);
    lv_obj_set_style_arc_rounded(objects.temp_arc, false,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(objects.temp_arc, false,
                                 LV_PART_INDICATOR | LV_STATE_DEFAULT);

    objects.temp_val = label(objects.temp_cont, "0.0", 24, 45, 60, 18,
                             &lv_font_montserrat_16, UI_COLOR_TEXT_PRIMARY,
                             LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.obj6 = label(objects.temp_cont, "°C", 34, 63, 40, 14,
                         &lv_font_montserrat_10, UI_COLOR_TEXT_DIM,
                         LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.obj4 = label(objects.temp_cont, "0", 10, 81, 30, 12, &lv_font_montserrat_8,
                         UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.obj5 = label(objects.temp_cont, "50", 70, 81, 30, 12, &lv_font_montserrat_8,
                         UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);

    objects.temp_mood = lv_obj_create(objects.temp_cont);
    lv_obj_set_pos(objects.temp_mood, 10, 94);
    lv_obj_set_size(objects.temp_mood, 89, 18);
    ui_style_status_pill(objects.temp_mood);
    objects.temp_img = dot(objects.temp_mood, 7, 6, 5, UI_COLOR_AMBER);
    objects.obj7 = label(objects.temp_mood, "Comfortable", 14, 3, 70, 10,
                         &lv_font_montserrat_8, UI_COLOR_AMBER,
                         LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);

    objects.aqi_cont = card(root, 123, 100, 109, 118, 13);
    objects.aqi_label = lv_obj_create(objects.aqi_cont);
    lv_obj_set_pos(objects.aqi_label, 8, 7);
    lv_obj_set_size(objects.aqi_label, 93, 18);
    ui_style_transparent(objects.aqi_label);
    objects.aqi_icon = icon(objects.aqi_label, &img_temp_img, -18, -17, temp_icon_zoom,
                            UI_COLOR_GREEN, true);
    label(objects.aqi_label, "AQI", 18, 2, 70, 14, &lv_font_montserrat_10,
          UI_COLOR_TEXT_PRIMARY, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);

    objects.aqi_arc = lv_arc_create(objects.aqi_cont);
    lv_obj_set_pos(objects.aqi_arc, 20, 23);
    lv_obj_set_size(objects.aqi_arc, 68, 68);
    lv_arc_set_bg_angles(objects.aqi_arc, 145, 35);
    lv_arc_set_range(objects.aqi_arc, 0, 500);
    lv_arc_set_value(objects.aqi_arc, 0);
    lv_obj_clear_flag(objects.aqi_arc, LV_OBJ_FLAG_CLICKABLE);
    ui_style_arc_track(objects.aqi_arc, UI_COLOR_GREEN);
    lv_obj_set_style_arc_rounded(objects.aqi_arc, false,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(objects.aqi_arc, false,
                                 LV_PART_INDICATOR | LV_STATE_DEFAULT);

    objects.aqi_val = label(objects.aqi_cont, "0", 24, 45, 60, 18,
                            &lv_font_montserrat_16, UI_COLOR_TEXT_PRIMARY,
                            LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.aqi_min = label(objects.aqi_cont, "0", 10, 81, 30, 12, &lv_font_montserrat_8,
                            UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.aqi_max = label(objects.aqi_cont, "500", 70, 81, 30, 12, &lv_font_montserrat_8,
                            UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.aqi_mood = lv_obj_create(objects.aqi_cont);
    lv_obj_set_pos(objects.aqi_mood, 10, 94);
    lv_obj_set_size(objects.aqi_mood, 89, 18);
    ui_style_status_pill(objects.aqi_mood);
    objects.aqi_dot = dot(objects.aqi_mood, 7, 6, 5, UI_COLOR_GREEN);
    objects.aqi_state = label(objects.aqi_mood, "Healthy", 14, 3, 70, 10,
                              &lv_font_montserrat_8, UI_COLOR_GREEN,
                              LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);

    objects.hum_cont = card(root, PAGE_X, 224, PAGE_W, 44, 13);
    objects.hum_label = lv_obj_create(objects.hum_cont);
    lv_obj_set_pos(objects.hum_label, 12, 8);
    lv_obj_set_size(objects.hum_label, 92, 18);
    ui_style_transparent(objects.hum_label);
    uint16_t hum_icon_zoom = (uint16_t)((14U * 256U) / img_hum_img.header.w);
    icon(objects.hum_label, &img_hum_img, 0, 1, hum_icon_zoom * 1.2, UI_COLOR_VIOLET, true);
    objects.obj8 = label(objects.hum_label, "HUMIDITY", 22, 2, 70, 14,
                         &lv_font_montserrat_10, UI_COLOR_TEXT_PRIMARY,
                         LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    objects.hum_val = label(objects.hum_cont, "0", 96, 6, 31, 16,
                            &lv_font_montserrat_16, UI_COLOR_TEXT_PRIMARY,
                            LV_TEXT_ALIGN_RIGHT, LV_LABEL_LONG_CLIP);
    objects.obj11 = label(objects.hum_cont, "%", 129, 12, 12, 10,
                          &lv_font_montserrat_10, UI_COLOR_TEXT_DIM,
                          LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    objects.hum_mood = lv_obj_create(objects.hum_cont);
    lv_obj_set_pos(objects.hum_mood, 144, 8);
    lv_obj_set_size(objects.hum_mood, 72, 22);
    ui_style_status_pill(objects.hum_mood);
    objects.hum_img = dot(objects.hum_mood, 8, 8, 6, UI_COLOR_VIOLET);
    objects.obj12 = label(objects.hum_mood, "Moderate", 17, 5, 50, 11,
                          &lv_font_montserrat_10, UI_COLOR_VIOLET,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);
    objects.hum_bar = lv_bar_create(objects.hum_cont);
    lv_obj_set_pos(objects.hum_bar, 36, 33);
    lv_obj_set_size(objects.hum_bar, 100, 5);
    lv_bar_set_range(objects.hum_bar, 0, 100);
    lv_bar_set_value(objects.hum_bar, 0, LV_ANIM_OFF);
    ui_style_bar_track(objects.hum_bar, UI_COLOR_VIOLET);

    create_bottom_bar(root);
}

#define WELCOME_RING_CX      120
#define WELCOME_RING_CY      108
#define WELCOME_RING_MIN     100
#define WELCOME_RING_MAX     190
#define WELCOME_RING_OPA     160
#define WELCOME_NUM_RINGS      3
#define WELCOME_RING_PERIOD 2400

static void ring_anim_cb(void *obj, int32_t v)
{
    int32_t sz = WELCOME_RING_MIN + ((WELCOME_RING_MAX - WELCOME_RING_MIN) * v) / 256;
    lv_obj_set_size((lv_obj_t *)obj, (lv_coord_t)sz, (lv_coord_t)sz);
    lv_obj_set_pos((lv_obj_t *)obj,
                   (lv_coord_t)(WELCOME_RING_CX - sz / 2),
                   (lv_coord_t)(WELCOME_RING_CY - sz / 2));
    lv_opa_t opa = (lv_opa_t)((WELCOME_RING_OPA * (256 - v)) / 256);
    lv_obj_set_style_opa((lv_obj_t *)obj, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void create_screen_hub_register_welcome(void)
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.hub_register_welcome = obj;
    base_screen(obj);

    for (int i = 0; i < WELCOME_NUM_RINGS; i++) {
        lv_obj_t *ring = lv_arc_create(obj);
        lv_arc_set_angles(ring, 0, 359);
        lv_obj_set_style_arc_color(ring, lv_color_hex(UI_COLOR_VIOLET),
                                   LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_width(ring, 2, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_opa(ring, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_opa(ring, 0, LV_PART_KNOB | LV_STATE_DEFAULT);
        lv_obj_set_size(ring, WELCOME_RING_MIN, WELCOME_RING_MIN);
        lv_obj_set_pos(ring,
                       WELCOME_RING_CX - WELCOME_RING_MIN / 2,
                       WELCOME_RING_CY - WELCOME_RING_MIN / 2);
        lv_obj_set_style_opa(ring, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ring);
        lv_anim_set_exec_cb(&a, ring_anim_cb);
        lv_anim_set_values(&a, 0, 256);
        lv_anim_set_time(&a, WELCOME_RING_PERIOD);
        lv_anim_set_delay(&a, i * (WELCOME_RING_PERIOD / WELCOME_NUM_RINGS));
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, 0);
        lv_anim_start(&a);
    }

    objects.reg_welcome_logo = lv_img_create(obj);
    lv_img_set_src(objects.reg_welcome_logo, &img_gz_logo);
    lv_obj_align(objects.reg_welcome_logo, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_clear_flag(objects.reg_welcome_logo, LV_OBJ_FLAG_SCROLLABLE);

    label(obj, "Your Hub for Smarter Living", 0, 176, SCREEN_W, 16,
          &lv_font_montserrat_12, UI_COLOR_TEXT_PRIMARY,
          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);

    label(obj, "Home Automation \xE2\x80\xA2 Surveillance \xE2\x80\xA2 Monitoring",
          PAGE_X, 196, PAGE_W, 24,
          &lv_font_montserrat_10, UI_COLOR_TEXT_SECONDARY,
          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);

    objects.reg_welcome_btn = lv_btn_create(obj);
    lv_obj_set_pos(objects.reg_welcome_btn, PAGE_X, 268);
    lv_obj_set_size(objects.reg_welcome_btn, PAGE_W, 40);
    ui_style_primary_button(objects.reg_welcome_btn);
    lv_obj_set_style_pad_all(objects.reg_welcome_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *btn_label = label(objects.reg_welcome_btn, "Register Hub", 0, 0, PAGE_W, 18,
                                &lv_font_montserrat_14, UI_COLOR_PRIMARY_TEXT,
                                LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    lv_obj_align(btn_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *btn_icon = icon(objects.reg_welcome_btn, &img_fwd, 0, 0, 150,
                              UI_COLOR_PRIMARY_TEXT, true);
    lv_obj_align(btn_icon, LV_ALIGN_RIGHT_MID, -14, 0);

    tick_screen_hub_register_welcome();
}

void tick_screen_hub_register_welcome(void) {}

void create_screen_hub_register_qr(void)
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.hub_register_qr = obj;
    base_screen(obj);

    lv_obj_t *hdr = card(obj, PAGE_X, 8, PAGE_W, 56, 13);

    lv_obj_t *logo_img = lv_img_create(hdr);
    lv_obj_set_pos(logo_img, -23, -23);
    lv_img_set_src(logo_img, &img_gz_logo);
    lv_img_set_zoom(logo_img, 135);
    lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_SCROLLABLE);

    label(hdr, "Welcome", 56, 9, 158, 18,
          &lv_font_montserrat_14, UI_COLOR_TEXT_PRIMARY,
          LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);

    lv_obj_t *status_row = lv_obj_create(hdr);
    lv_obj_set_pos(status_row, 56, 31);
    lv_obj_set_size(status_row, 100, 16);
    ui_style_transparent(status_row);

    dot(status_row, 0, 5, 5, UI_COLOR_AMBER);
    objects.reg_qr_status = label(status_row, "Setup", 10, 1, 80, 13,
                                  &lv_font_montserrat_10, UI_COLOR_AMBER,
                                  LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);

    lv_obj_t *qr_card = card(obj, PAGE_X, 70, PAGE_W, 238, 13);

    objects.reg_qr_code = lv_qrcode_create(qr_card, 160, lv_color_black(), lv_color_white());
    lv_obj_set_pos(objects.reg_qr_code, (PAGE_W - 160) / 2, 20);
    lv_obj_clear_flag(objects.reg_qr_code, LV_OBJ_FLAG_SCROLLABLE);
    lv_qrcode_update(objects.reg_qr_code, g_hub_mac, (uint32_t)strlen(g_hub_mac));

    lv_obj_t *scan_row = lv_obj_create(qr_card);
    lv_obj_set_pos(scan_row, 14, 192);
    lv_obj_set_size(scan_row, PAGE_W - 28, 32);
    ui_style_transparent(scan_row);

    label(scan_row, "Scan to Register the Hub",
          0, 8, PAGE_W - 28, 14,
          &lv_font_montserrat_10, UI_COLOR_TEXT_SECONDARY,
          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);

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
    lv_obj_set_size(row, PAGE_W, 64);
    ui_style_glass_card(row, 13);
    lv_obj_t *icon_box = panel(row, 13, 13, 38, 38, 10);
    icon(icon_box, img, -10, -10, 114, accent, true);
    *title_handle = label(row, title, 62, 13, 122, 16, &lv_font_montserrat_10,
                          UI_COLOR_TEXT_PRIMARY, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_DOT);
    *sub_handle = label(row, sub, 62, 32, 126, 12, &lv_font_montserrat_8,
                        UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_DOT);
    icon(row, &img_fwd, 198, 25, 58, UI_COLOR_TEXT_DIM, true);
}

void create_screen_settings_menu()
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.settings_menu = obj;
    base_screen(obj);
    add_nav_title(obj, "Settings", UI_COLOR_AMBER, &objects.obj14);
    objects.settings_menu_cont = lv_obj_create(obj);
    lv_obj_set_pos(objects.settings_menu_cont, 0, 54);
    lv_obj_set_size(objects.settings_menu_cont, SCREEN_W, 258);
    ui_style_transparent(objects.settings_menu_cont);
    create_option_row(obj, &objects.fingerprint_option, &objects.obj16, &objects.obj15,
                      &img_finger, "Add Fingerprint", "Register a new fingerprint",
                      58, UI_COLOR_VIOLET);
    create_option_row(obj, &objects.sensor_nodes_option, &objects.obj17, &objects.obj18,
                      &img_nodes, "Sensor Nodes", "Manage and configure sensors",
                      132, UI_COLOR_VIOLET);
    create_option_row(obj, &objects.about_section, &objects.obj20, &objects.obj19,
                      &img_about, "About", "Learn more about Glazia",
                      206, UI_COLOR_VIOLET);
    objects.obj13 = label(obj, "", 0, 0, 1, 1, &lv_font_montserrat_8,
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
    objects.obj42 = label(obj, "", 0, 0, 1, 1, &lv_font_montserrat_8,
                          UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_CLIP);
    objects.obj43 = label(obj, "Enable or disable sensor nodes as needed.", PAGE_X + 2, 54,
                          220, 13, &lv_font_montserrat_8, UI_COLOR_TEXT_DIM,
                          LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_DOT);
    objects.settings_menu_cont_1 = lv_obj_create(obj);
    lv_obj_set_pos(objects.settings_menu_cont_1, 0, 70);
    lv_obj_set_size(objects.settings_menu_cont_1, SCREEN_W, 202);
    ui_style_transparent(objects.settings_menu_cont_1);
    lv_obj_set_scroll_dir(objects.settings_menu_cont_1, LV_DIR_VER);

    objects.add_sensor_button = lv_btn_create(obj);
    lv_obj_set_pos(objects.add_sensor_button, PAGE_X, 278);
    lv_obj_set_size(objects.add_sensor_button, PAGE_W, 34);
    ui_style_glass_card(objects.add_sensor_button, 12);
    objects.obj45 = label(objects.add_sensor_button, "Add Another Sensor", 0, 10, PAGE_W, 14,
                          &lv_font_montserrat_12, UI_COLOR_AMBER,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);
    objects.obj46 = icon(objects.add_sensor_button, &img_fwd, 198, 11, 58,
                         UI_COLOR_TEXT_DIM, true);
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
    lv_img_set_zoom(logo, 200);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_SCROLLABLE);
    // label(obj, "About Glazia", 0, 108, SCREEN_W, 18, &lv_font_montserrat_14,
    //       UI_COLOR_TEXT_PRIMARY, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);

    lv_obj_t *body = card(obj, PAGE_X, 134, PAGE_W, 166, 13);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.obj48 = label(body,
        "Glazia is dedicated to creating smart, reliable, and elegant IoT solutions for modern living and workplaces.\n\n"
        "Our mission is to connect spaces, simplify control, and empower users through intelligent technology - making everyday environments smarter and more responsive.",
        14, 12, 196, 118, &lv_font_montserrat_10, UI_COLOR_TEXT_SECONDARY,
        LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);
    label(body, "© 2024 Glazia Technologies - All rights reserved.", 8, 145, 208, 10,
          &lv_font_montserrat_8, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);
    tick_screen_about_glazia();
}

void tick_screen_about_glazia(void) {}

void create_screen_fingerprint_setting()
{
    lv_obj_t *obj = lv_obj_create(0);
    objects.fingerprint_setting = obj;
    base_screen(obj);
    lv_obj_t *nav = card(obj, PAGE_X, 8, PAGE_W, 40, 13);
    objects.obj54 = back_button(nav, UI_COLOR_AMBER);
    lv_obj_set_pos(objects.obj54, 4, 4);
    objects.obj53 = label(nav, "Add Fingerprint", 40, 10, 144, 18,
                          &lv_font_montserrat_14, UI_COLOR_TEXT_PRIMARY,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_DOT);

    objects.fingerprint_scan_ui = card(obj, PAGE_X, 58, PAGE_W, 254, 14);
    objects.obj50 = lv_spinner_create(objects.fingerprint_scan_ui, 1000, 60);
    lv_obj_set_pos(objects.obj50, 64, 20);
    lv_obj_set_size(objects.obj50, 96, 96);
    ui_style_arc_track(objects.obj50, UI_COLOR_AMBER);
    lv_obj_t *fp_badge = panel(objects.fingerprint_scan_ui, 89, 45, 46, 46, LV_RADIUS_CIRCLE);
    icon(fp_badge, &img_finger, -6, -6, 190, UI_COLOR_AMBER, true);

    objects.obj49 = label(objects.fingerprint_scan_ui, "Place your finger\non the sensor",
                          20, 126, 184, 36, &lv_font_montserrat_12,
                          UI_COLOR_TEXT_PRIMARY, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);
    objects.fingerprint_instruction = label(objects.fingerprint_scan_ui,
                                            "Keep your finger on the sensor\nfor registration.",
                                            18, 166, 188, 32, &lv_font_montserrat_8,
                                            UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER,
                                            LV_LABEL_LONG_WRAP);
    objects.obj51 = lv_bar_create(objects.fingerprint_scan_ui);
    lv_obj_set_pos(objects.obj51, 37, 205);
    lv_obj_set_size(objects.obj51, 150, 8);
    lv_bar_set_range(objects.obj51, 0, 100);
    lv_bar_set_value(objects.obj51, 0, LV_ANIM_OFF);
    dot(objects.fingerprint_scan_ui, 61, 228, 7, UI_COLOR_AMBER);
    objects.obj52 = label(objects.fingerprint_scan_ui, "Processing...", 74, 224, 120, 24,
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

    lv_obj_t *card_obj = card(obj, PAGE_X, 58, PAGE_W, 206, 13);
    lv_obj_t *qr = lv_qrcode_create(card_obj, 160, lv_color_black(), lv_color_white());
    lv_obj_set_pos(qr, 32, 12);
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_SCROLLABLE);
    lv_qrcode_update(qr, g_hub_mac, (uint32_t)strlen(g_hub_mac));
    label(card_obj, "Scan QR to add sensor", 0, 176, PAGE_W, 12,
          &lv_font_montserrat_10, UI_COLOR_TEXT_SECONDARY,
          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);

    objects.added_sensor_data = card(obj, PAGE_X, 58, PAGE_W, 206, 13);
    lv_obj_add_flag(objects.added_sensor_data, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *check_badge = panel(objects.added_sensor_data, 84, 25, 56, 56, LV_RADIUS_CIRCLE);
    icon(check_badge, &img_tick, 13, 13, 128, UI_COLOR_GREEN, true);
    objects.obj57 = label(objects.added_sensor_data, "Sensor Added!", 0, 94, PAGE_W, 20,
                          &lv_font_montserrat_16, UI_COLOR_TEXT_PRIMARY,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
    objects.obj58 = label(objects.added_sensor_data, "sensor_loc_k has been added",
                          18, 126, 188, 32, &lv_font_montserrat_12, UI_COLOR_GREEN,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);
    label(objects.added_sensor_data,
          "The sensor is now registered and will appear in your Sensor Nodes list.",
          18, 166, 188, 28, &lv_font_montserrat_10, UI_COLOR_TEXT_DIM,
          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP);

    objects.obj59 = lv_btn_create(obj);
    lv_obj_set_pos(objects.obj59, PAGE_X, 272);
    lv_obj_set_size(objects.obj59, PAGE_W, 40);
    ui_style_primary_button(objects.obj59);
    objects.obj60 = label(objects.obj59, "Add Sensor", 0, 12, PAGE_W, 16,
                          &lv_font_montserrat_14, UI_COLOR_PRIMARY_TEXT,
                          LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP);
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

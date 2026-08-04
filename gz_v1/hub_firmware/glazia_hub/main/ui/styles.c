#include "styles.h"

static void clear_base(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void apply_card_base(lv_obj_t *obj, uint32_t bg, uint32_t grad,
                            uint32_t border, lv_coord_t radius)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(grad), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 230, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(obj, 190, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN | LV_STATE_DEFAULT);
    clear_base(obj);
}

void ui_style_screen_bg(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(UI_COLOR_BG_GRAD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    clear_base(obj);
}

void ui_style_glass_card(lv_obj_t *obj, lv_coord_t radius)
{
    apply_card_base(obj, UI_COLOR_CARD, UI_COLOR_CARD_GRAD, UI_COLOR_CARD_BORDER, radius);
}

void ui_style_panel(lv_obj_t *obj, lv_coord_t radius)
{
    apply_card_base(obj, UI_COLOR_PANEL, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER, radius);
}

void ui_style_alert_card(lv_obj_t *obj, lv_coord_t radius)
{
    apply_card_base(obj, UI_COLOR_ALERT_BG, UI_COLOR_ALERT_BG, UI_COLOR_ALERT_BORDER, radius);
}

void ui_style_transparent(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    clear_base(obj);
}

void ui_style_text_primary(lv_obj_t *obj, const lv_font_t *font)
{
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void ui_style_text_muted(lv_obj_t *obj, const lv_font_t *font)
{
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void ui_style_text_dim(lv_obj_t *obj, const lv_font_t *font)
{
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void ui_style_text_cyan(lv_obj_t *obj, const lv_font_t *font)
{
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_AMBER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void ui_style_dot(lv_obj_t *obj, uint32_t color)
{
    if (!obj) return;
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    clear_base(obj);
}

void ui_style_status_pill(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_COLOR_PANEL_BORDER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(obj, 120, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    clear_base(obj);
}

void ui_style_progress(lv_obj_t *obj, uint32_t color)
{
    ui_style_bar_track(obj, color);
}

void ui_style_settings_button(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_AMBER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, 9, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    clear_base(obj);
}

void ui_style_primary_button(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_PRIMARY_BUTTON), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, 13, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    clear_base(obj);
}

void ui_style_toggle(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_TOGGLE_INACTIVE), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_TOGGLE_ACTIVE), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_TOGGLE_KNOB), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_KNOB | LV_STATE_DEFAULT);
}

void ui_style_arc_track(lv_obj_t *obj, uint32_t indicator_color)
{
    if (!obj) return;
    lv_obj_set_style_arc_width(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(obj, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(obj, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(obj, 6, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(obj, true, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(obj, lv_color_hex(indicator_color), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(obj, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 0, LV_PART_KNOB | LV_STATE_DEFAULT);
}

void ui_style_bar_track(lv_obj_t *obj, uint32_t indicator_color)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(indicator_color), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(UI_COLOR_AMBER), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, 3, LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

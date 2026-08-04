#ifndef EEZ_LVGL_UI_STYLES_H
#define EEZ_LVGL_UI_STYLES_H

#include "lvgl.h"

#define UI_COLOR_BG                 0x100B19
#define UI_COLOR_BG_GRAD            0x080610

#define UI_COLOR_CARD               0x1F1628
#define UI_COLOR_CARD_GRAD          0x261B34
#define UI_COLOR_CARD_BORDER        0x4B4058

#define UI_COLOR_PANEL              0x2C2035
#define UI_COLOR_PANEL_BORDER       0x584764

#define UI_COLOR_ALERT_BG           0x33232D
#define UI_COLOR_ALERT_BORDER       0x80572A

#define UI_COLOR_TEXT_PRIMARY       0xF2EDF4
#define UI_COLOR_TEXT_SECONDARY     0xAAA1AF
#define UI_COLOR_TEXT_DIM           0x665D71

#define UI_COLOR_GREEN              0x34D399
#define UI_COLOR_RED                0xFF6B6B
#define UI_COLOR_RED_SOFT           0xFF9696
#define UI_COLOR_AMBER              0xFFB13B
#define UI_COLOR_AMBER_SOFT         0xFFE07A
#define UI_COLOR_VIOLET             0xA78BFA

#define UI_COLOR_PRIMARY_BUTTON     0xFF6B6B
#define UI_COLOR_PRIMARY_TEXT       0xFFF7F7
#define UI_COLOR_TOGGLE_ACTIVE      0xFFB13B
#define UI_COLOR_TOGGLE_INACTIVE    0x3A3044
#define UI_COLOR_TOGGLE_KNOB        0xFFFFFF

#define UI_COLOR_TRACK              0x383044
#define UI_COLOR_TRACK_DIM          0x2E2838

#ifdef __cplusplus
extern "C" {
#endif

void ui_style_screen_bg(lv_obj_t *obj);
void ui_style_glass_card(lv_obj_t *obj, lv_coord_t radius);
void ui_style_panel(lv_obj_t *obj, lv_coord_t radius);
void ui_style_alert_card(lv_obj_t *obj, lv_coord_t radius);
void ui_style_transparent(lv_obj_t *obj);
void ui_style_text_primary(lv_obj_t *obj, const lv_font_t *font);
void ui_style_text_muted(lv_obj_t *obj, const lv_font_t *font);
void ui_style_text_dim(lv_obj_t *obj, const lv_font_t *font);
void ui_style_text_cyan(lv_obj_t *obj, const lv_font_t *font);
void ui_style_dot(lv_obj_t *obj, uint32_t color);
void ui_style_status_pill(lv_obj_t *obj);
void ui_style_progress(lv_obj_t *obj, uint32_t color);
void ui_style_settings_button(lv_obj_t *obj);
void ui_style_primary_button(lv_obj_t *obj);
void ui_style_toggle(lv_obj_t *obj);
void ui_style_arc_track(lv_obj_t *obj, uint32_t indicator_color);
void ui_style_bar_track(lv_obj_t *obj, uint32_t indicator_color);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_STYLES_H*/

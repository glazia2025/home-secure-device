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

/* Light-theme tokens — used ONLY by the registration welcome + QR screens,
 * which sit over the full-bleed hub photo. Do not use elsewhere (rest of the
 * UI is the dark Ocean Night theme above). */
#define UI_LIGHT_TEXT               0x1A1A1A   /* hero "Welcome to", card title  */
#define UI_LIGHT_SLATE              0x6B7280   /* hero accent word ("Glazia")    */
#define UI_LIGHT_BODY               0x4B5563   /* subtitle / body copy           */
#define UI_LIGHT_MONO               0x8A8A82   /* tracked mono labels / footer   */
#define UI_DARK_PILL                0x1B2027   /* Get Started / CTA pill fill    */
#define UI_DARK_PILL_TEXT           0xFFFFFF
#define UI_SETUP_PILL               0xE7E2D5   /* small "Setup" pill (from spec) */
#define UI_SETUP_PILL_TEXT          0x57534E

/* ── Light "Glazia" dashboard tokens (new design — dashboard.docx §3) ────────
 * The dashboard + every data screen now render in this light theme, over the
 * same warm interior photo (img_welcome_bg) as the welcome/QR screens. The dark
 * Ocean Night tokens above are retained only for reference/back-compat. */
#define UI_D_HEADING        0x1A2024   /* card heading (TEMPERATURE / HUMIDITY)  */
#define UI_D_VALUE          0x0F1725   /* primary metric value                    */
#define UI_D_VALUE_ALT      0x101A2B   /* AQI numeric value                       */
#define UI_D_MUTED          0x8C9296   /* secondary label ("Living Room")         */
#define UI_D_MICRO          0x8A959B   /* micro / mono caption                    */
#define UI_D_SUBTLE         0x726E69   /* page subtitle over the photo            */
#define UI_D_TRACK          0xE7EBEB   /* neutral gauge / thermometer track       */
#define UI_D_CARD_BORDER    0xE5EBE8   /* hairline card border                    */
#define UI_D_STAT_SURF      0xF6F7F6   /* min/avg/max stat tile surface           */

#define UI_D_GREEN          0x17AE82   /* success / healthy                       */
#define UI_D_GREEN_SURF     0xEAF9F3
#define UI_D_AQI_SURF       0xF0FAF5
#define UI_D_AMBER          0xF1A31F   /* moderate                                */
#define UI_D_AMBER_TEXT     0xC68413
#define UI_D_AMBER_SURF     0xFFF6E7
#define UI_D_RED            0xFF6254   /* hot                                      */
#define UI_D_RED_HI         0xFF6A5C
#define UI_D_RED_SURF       0xFFF0EC
#define UI_D_BLUE           0x2C93F5   /* humidity                                */
#define UI_D_BLUE_HI        0x57A6F8
#define UI_D_BLUE_SURF      0xECF6FF

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
/* Registration (welcome/QR) light-theme helpers. */
void ui_style_dark_pill(lv_obj_t *obj);
void ui_style_glass_light(lv_obj_t *obj, lv_coord_t radius);
void ui_style_toggle(lv_obj_t *obj);
void ui_style_arc_track(lv_obj_t *obj, uint32_t indicator_color);
void ui_style_bar_track(lv_obj_t *obj, uint32_t indicator_color);
/* Light "Glazia" dashboard helpers. */
void ui_style_card_light(lv_obj_t *obj, lv_coord_t radius);
void ui_style_status_pill_light(lv_obj_t *obj, uint32_t text_color, uint32_t surface);
void ui_style_metric_arc_light(lv_obj_t *obj, uint32_t indicator_color, lv_coord_t width);
void ui_style_stat_cell(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_STYLES_H*/

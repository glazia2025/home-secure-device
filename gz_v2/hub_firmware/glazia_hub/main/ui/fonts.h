#ifndef EEZ_LVGL_UI_FONTS_H
#define EEZ_LVGL_UI_FONTS_H

#include "lvgl.h"

/* CaskaydiaMono Nerd Font (Consolas-lineage monospace nerd font) */
extern const lv_font_t lv_font_caskaydia_28;
extern const lv_font_t lv_font_caskaydia_32;
extern const lv_font_t lv_font_caskaydia_40;
extern const lv_font_t lv_font_caskaydia_48;
extern const lv_font_t lv_font_caskaydia_56;

/* Registration screens (welcome + QR) — Glazia design fonts.
 * Space Grotesk (display/headings), Inter (body/UI), IBM Plex Mono (mono labels). */
extern const lv_font_t lv_font_grotesk_64;
extern const lv_font_t lv_font_grotesk_34;
extern const lv_font_t lv_font_inter_30;
extern const lv_font_t lv_font_inter_22;
extern const lv_font_t lv_font_plexmono_18;

#define lv_font_montserrat_8  lv_font_caskaydia_28
#define lv_font_montserrat_10 lv_font_caskaydia_32
#define lv_font_montserrat_12 lv_font_caskaydia_40
#define lv_font_montserrat_14 lv_font_caskaydia_48
#define lv_font_montserrat_16 lv_font_caskaydia_56
#define lv_font_montserrat_18 lv_font_caskaydia_56

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EXT_FONT_DESC_T
#define EXT_FONT_DESC_T
typedef struct _ext_font_desc_t {
    const char *name;
    const void *font_ptr;
} ext_font_desc_t;
#endif

extern ext_font_desc_t fonts[];

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_FONTS_H*/

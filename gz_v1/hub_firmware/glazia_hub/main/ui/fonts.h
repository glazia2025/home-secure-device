#ifndef EEZ_LVGL_UI_FONTS_H
#define EEZ_LVGL_UI_FONTS_H

#include "lvgl.h"

#if !LV_FONT_MONTSERRAT_16
#define lv_font_montserrat_16 lv_font_montserrat_14
#endif
#if !LV_FONT_MONTSERRAT_18
#define lv_font_montserrat_18 lv_font_montserrat_14
#endif
#if !LV_FONT_MONTSERRAT_20
#define lv_font_montserrat_20 lv_font_montserrat_26
#endif
#if !LV_FONT_MONTSERRAT_30
#define lv_font_montserrat_30 lv_font_montserrat_26
#endif

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

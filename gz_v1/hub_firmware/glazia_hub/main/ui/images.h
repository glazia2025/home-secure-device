#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_logo_hub;
extern const lv_img_dsc_t img_hub_home;
extern const lv_img_dsc_t img_date;
extern const lv_img_dsc_t img_temp_img;
extern const lv_img_dsc_t img_hum_img;
extern const lv_img_dsc_t img_loc;
extern const lv_img_dsc_t img_cloud;
extern const lv_img_dsc_t img_green;
extern const lv_img_dsc_t img_red;
extern const lv_img_dsc_t img_settings;
extern const lv_img_dsc_t img_back;
extern const lv_img_dsc_t img_about;
extern const lv_img_dsc_t img_nodes;
extern const lv_img_dsc_t img_finger;
extern const lv_img_dsc_t img_fwd;
extern const lv_img_dsc_t img_sensor;
extern const lv_img_dsc_t img_fs_img;
extern const lv_img_dsc_t img_tick;
extern const lv_img_dsc_t img_gz_logo;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[18];

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/
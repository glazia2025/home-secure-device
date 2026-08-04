#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST                = 1,
    SCREEN_ID_HUB_REGISTER_WELCOME  = 1,
    SCREEN_ID_HUB_REGISTER_QR       = 2,
    SCREEN_ID_HUB_ONLINE            = 3,
    SCREEN_ID_SETTINGS_MENU         = 4,
    SCREEN_ID_SENSOR_NODES_SETTING  = 5,
    SCREEN_ID_ABOUT_GLAZIA          = 6,
    SCREEN_ID_FINGERPRINT_SETTING   = 7,
    SCREEN_ID_ADD_ANOTHER__SENSOR   = 8,
    _SCREEN_ID_LAST                 = 8,
};

/* Screen objects MUST be the first 8 fields in declaration order — they are
 * accessed by index (screenId - 1) via getLvglObjectFromIndex in ui.c. */
typedef struct _objects_t {
    lv_obj_t *hub_register_welcome;
    lv_obj_t *hub_register_qr;
    lv_obj_t *hub_online;
    lv_obj_t *settings_menu;
    lv_obj_t *sensor_nodes_setting;
    lv_obj_t *about_glazia;
    lv_obj_t *fingerprint_setting;
    lv_obj_t *add_another__sensor;
    /* ── hub_online widgets ─────────────────────────────────── */
    lv_obj_t *cont_logo_card;
    lv_obj_t *obj0;
    lv_obj_t *hub_state_cont;
    lv_obj_t *loc_cont_1;
    lv_obj_t *hub_status;
    lv_obj_t *hub_location;
    lv_obj_t *hub_location_dot;
    lv_obj_t *status_dot;
    lv_obj_t *welcome_home;
    lv_obj_t *obj1;
    lv_obj_t *button;
    lv_obj_t *obj2;
    lv_obj_t *settings_icon;
    lv_obj_t *sensor_hub;
    lv_obj_t *sensor_info;
    lv_obj_t *temp_cont;
    lv_obj_t *temp_label;
    lv_obj_t *obj3;
    lv_obj_t *temp_img;
    lv_obj_t *temp_arc;
    lv_obj_t *obj4;
    lv_obj_t *obj5;
    lv_obj_t *temp_val;
    lv_obj_t *obj6;
    lv_obj_t *temp_mood;
    lv_obj_t *obj7;
    lv_obj_t *aqi_cont;
    lv_obj_t *aqi_label;
    lv_obj_t *aqi_icon;
    lv_obj_t *aqi_arc;
    lv_obj_t *aqi_val;
    lv_obj_t *aqi_min;
    lv_obj_t *aqi_max;
    lv_obj_t *aqi_mood;
    lv_obj_t *aqi_dot;
    lv_obj_t *aqi_state;
    lv_obj_t *hum_cont;
    lv_obj_t *hum_label;
    lv_obj_t *obj8;
    lv_obj_t *hum_img;
    lv_obj_t *hum_bar;      /* was hum_arc — now lv_bar */
    lv_obj_t *hum_val;
    lv_obj_t *obj11;
    lv_obj_t *hum_mood;
    lv_obj_t *obj12;
    lv_obj_t *home_time;
    lv_obj_t *home_date;
    lv_obj_t *strip_date;
    /* ── settings_menu widgets ──────────────────────────────── */
    lv_obj_t *settings_menu_cont;
    lv_obj_t *obj13;
    lv_obj_t *obj14;
    lv_obj_t *fingerprint_option;
    lv_obj_t *obj15;
    lv_obj_t *obj16;
    lv_obj_t *sensor_nodes_option;
    lv_obj_t *obj17;
    lv_obj_t *obj18;
    lv_obj_t *about_section;
    lv_obj_t *obj19;
    lv_obj_t *obj20;
    /* ── sensor_nodes_setting widgets ───────────────────────── */
    lv_obj_t *settings_menu_cont_1;
    lv_obj_t *obj42;
    lv_obj_t *obj43;
    lv_obj_t *obj44;
    lv_obj_t *add_sensor_button;
    lv_obj_t *obj45;
    lv_obj_t *obj46;
    /* ── about_glazia widgets ───────────────────────────────── */
    lv_obj_t *obj47;
    lv_obj_t *obj48;
    /* ── fingerprint_setting widgets ────────────────────────── */
    lv_obj_t *fingerprint_scan_ui;
    lv_obj_t *fingerprint_instruction;
    lv_obj_t *obj49;
    lv_obj_t *obj50;
    lv_obj_t *obj51;
    lv_obj_t *obj52;
    lv_obj_t *obj53;
    lv_obj_t *obj54;
    /* ── add_another__sensor widgets ────────────────────────── */
    lv_obj_t *obj55;
    lv_obj_t *added_sensor_data;
    lv_obj_t *obj57;
    lv_obj_t *obj58;
    lv_obj_t *obj59;
    lv_obj_t *obj60;
    /* ── hub_register_welcome widgets ──────────────────────────── */
    lv_obj_t *reg_welcome_logo;
    lv_obj_t *reg_welcome_btn;
    /* ── hub_register_qr widgets ───────────────────────────────── */
    lv_obj_t *reg_qr_status;
    lv_obj_t *reg_qr_code;
} objects_t;

extern objects_t objects;

void create_screen_hub_register_welcome(void);
void tick_screen_hub_register_welcome(void);
void create_screen_hub_register_qr(void);
void tick_screen_hub_register_qr(void);

void create_screen_hub_online();
void tick_screen_hub_online();

void create_screen_settings_menu();
void tick_screen_settings_menu();

void create_screen_sensor_nodes_setting();
void tick_screen_sensor_nodes_setting();

void create_screen_about_glazia();
void tick_screen_about_glazia();

void create_screen_fingerprint_setting();
void tick_screen_fingerprint_setting();

void create_screen_add_another__sensor();
void tick_screen_add_another__sensor();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/

#include "ui.h"
#include "screens.h"
#include "images.h"
#include "actions.h"
#include "vars.h"
#include "../state.h"

#include <string.h>

static int16_t currentScreen = -1;

static lv_obj_t *getLvglObjectFromIndex(int32_t index) {
    if (index == -1) {
        return 0;
    }
    return ((lv_obj_t **)&objects)[index];
}

void ui_ensure_screen(enum ScreensEnum screenId) {
    if (screenId < _SCREEN_ID_FIRST || screenId > _SCREEN_ID_LAST) {
        return;
    }

    if (getLvglObjectFromIndex(screenId - 1)) {
        return;
    }

    switch (screenId) {
    case SCREEN_ID_HUB_REGISTER_WELCOME:
        create_screen_hub_register_welcome();
        break;
    case SCREEN_ID_HUB_REGISTER_QR:
        create_screen_hub_register_qr();
        break;
    case SCREEN_ID_HUB_ONLINE:
        create_screen_hub_online();
        break;
    case SCREEN_ID_SETTINGS_MENU:
        create_screen_settings_menu();
        break;
    case SCREEN_ID_SENSOR_NODES_SETTING:
        create_screen_sensor_nodes_setting();
        break;
    case SCREEN_ID_ABOUT_GLAZIA:
        create_screen_about_glazia();
        break;
    case SCREEN_ID_FINGERPRINT_SETTING:
        create_screen_fingerprint_setting();
        break;
    case SCREEN_ID_ADD_ANOTHER__SENSOR:
        create_screen_add_another__sensor();
        break;
    default:
        break;
    }
}

void loadScreen(enum ScreensEnum screenId) {
    ui_ensure_screen(screenId);
    currentScreen = screenId - 1;
    lv_obj_t *screen = getLvglObjectFromIndex(currentScreen);
    if (screen) {
        lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    }
}

void ui_init() {
    if (g_hub_secret[0] != '\0') {
        loadScreen(SCREEN_ID_HUB_ONLINE);
    } else {
        loadScreen(SCREEN_ID_HUB_REGISTER_WELCOME);
    }
}

void ui_tick() {
    tick_screen(currentScreen);
}

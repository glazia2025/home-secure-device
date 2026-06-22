/**
 * display.c — ILI9341 TFT driven directly from ESP32-S3 over SPI + LVGL.
 *
 * Replaces the old UART-to-C6 bridge. The public API (display_show,
 * display_hub_location, display_sensor_location, display_sensor_list)
 * is identical — callers in wifi.c, button.c, api_client.c etc. need
 * no changes.
 *
 * Layout (240 × 320, portrait):
 *   [0]  Header      — Glazia logo + "GLAZIA / Hub"       y: 0–67
 *   [1]  Status panel — status text + location             y: 78–147
 *   [2]  Critical Toggle (always ON while powered)         y: 147–182
 *   [3]  Sensor list  — scrollable paired-sensor table     y: 192–311
 */
#include "display.h"
#include "ble.h"
#include "button.h"
#include "espnow.h"
#include "fingerprint.h"
#include "wifi.h"
#include "ui/ui.h"
#include "ui/screens.h"
#include "ui/images.h"
#include "ui/styles.h"
#include "misc/lv_area.h"
#include "state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>

static const char *TAG = "DISPLAY";

/* ── Display constants ───────────────────────────────────────────────────── */
#define LCD_H_RES          240
#define LCD_V_RES          320
#define LCD_SPI_HOST       SPI2_HOST
#define LCD_PIXEL_CLK_HZ   (10 * 1000 * 1000)
#define LCD_CMD_BITS       8
#define LCD_PARAM_BITS     8
#define DRAW_BUF_LINES     4

/* ── Display-side theme aliases ─────────────────────────────────────────── */
#define C_CYAN_U32   UI_COLOR_VIOLET
#define C_AMBER_U32  UI_COLOR_AMBER
#define C_RED_U32    UI_COLOR_RED
#define C_GREEN_U32  UI_COLOR_GREEN
#define C_T2_U32     UI_COLOR_TEXT_SECONDARY
#define C_AQI_NOMINAL_U32   0x84CC16
#define C_AQI_POOR_U32      0xF97316
#define C_AQI_UNHEALTHY_U32 0xFF8247

/* ── Alert thresholds ────────────────────────────────────────────────────── */
#define TEMP_THRESH_WARM  30.0f
#define TEMP_THRESH_HOT   35.0f
#define HUM_THRESH_HIGH   65.0f

/* ── LVGL object handles (set once in display_init, read-only after) ─────── */
static lv_obj_t *s_critical_sw    = NULL;
static TaskHandle_t s_auth_task   = NULL;
static bool s_switch_internal     = false;
static bool s_ui_online           = true;
static bool s_sensor_added_view   = false;
static enum ScreensEnum s_prev_screen = SCREEN_ID_HUB_ONLINE;
static enum ScreensEnum s_current_screen = SCREEN_ID_HUB_ONLINE;
static bool s_screen_configured[_SCREEN_ID_LAST + 1];

typedef enum {
    DISPLAY_NOT_STARTED = 0,
    DISPLAY_STARTING,
    DISPLAY_READY,
    DISPLAY_FAILED,
} display_state_t;

typedef enum {
    CACHE_VIEW_NONE = 0,
    CACHE_VIEW_SETUP,
    CACHE_VIEW_ONLINE,
    CACHE_VIEW_OFFLINE,
    CACHE_VIEW_FINGERPRINT,
} cached_view_t;

typedef struct {
    cached_view_t view;
    char line1[64];
    char line2[96];
    char fp_title[48];
    char fp_phase[64];
    char fp_message[96];
    uint8_t fp_progress;
    bool has_temp_hum;
    float temp;
    float hum;
    bool has_aqi;
    float aqi;
    char aqi_state[16];
    char home_name[64];
    char user_name[64];
} display_cache_t;

static volatile display_state_t s_display_state = DISPLAY_NOT_STARTED;
static SemaphoreHandle_t s_display_cache_mutex = NULL;
static display_cache_t s_display_cache = {
    .view = CACHE_VIEW_NONE,
};

void display_fingerprint_status(const char *message);
static void create_sensor_row(lv_obj_t *parent, int index, const char *name, bool enabled);
static void display_init_task(void *arg);
static esp_err_t lcd_hw_init(void);
static void load_screen_locked(enum ScreensEnum screen);
static void configure_screen_locked(enum ScreensEnum screen);
static void refresh_sensor_nodes_locked(void);
static void set_hub_connection_status_locked(bool online);
static void update_temp_pill_locked(float temp);
static void update_hum_pill_locked(float hum);
static void set_aqi_value_locked(float aqi, const char *state);
static void update_home_datetime_locked(void);
static void cache_copy(char *dst, size_t dst_size, const char *src);
static void cache_lock(void);
static void cache_unlock(void);
static const char *nonnull_text(const char *text, const char *fallback);
static const char *hub_location_text_locked(void);
static void set_welcome_text_locked(const char *user_name);
static void set_cached_welcome_text_locked(void);
static const char *fingerprint_phase_for_title(const char *title);
static const char *fingerprint_instruction_for_phase(const char *phase);
static const char *fingerprint_message_normalize(const char *message);
static void align_dashboard_value_locked(lv_obj_t *value, lv_obj_t *arc);
static void align_fingerprint_text_locked(void);
static void show_fingerprint_screen_locked(const char *title, const char *prompt);
static bool display_is_ready(void);
static void cache_apply_locked(void);
static void make_touch_target(lv_obj_t *obj);
static void make_touch_target_tree(lv_obj_t *obj);
static void make_back_touch_target(lv_obj_t *obj);
static void xpt2046_process_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                        uint16_t *strength, uint8_t *point_num,
                                        uint8_t max_point_num);

/* ── XPT2046 touch — official esp_lcd_touch driver ──────────────────────── */
static esp_lcd_touch_handle_t s_tp = NULL;
static lv_indev_t         *s_tp_indev = NULL;
static lv_disp_t          *s_lvgl_disp = NULL;
/* ── Hardware init ───────────────────────────────────────────────────────── */

/*
 * Calibration constants measured by the standalone touch calibration app.
 * The official XPT2046 driver now owns SPI/PENIRQ/sample acquisition; this
 * callback only maps stable raw ADC coordinates into LVGL screen pixels.
 */
/*
 * Raw endpoints from the official XPT2046 driver:
 *   top-left     ~= (240, 3800)
 *   top-right    ~= (3700, 3800)
 *   bottom-left  ~= (345, 550)
 *   toggle area  ~= (260, 360..460)
 *
 * raw_x increases left-to-right. raw_y decreases top-to-bottom.
 */
#define TP_RAW_LEFT      240
#define TP_RAW_RIGHT    3700
#define TP_RAW_TOP      3800
#define TP_RAW_BOTTOM   430

#define TP_CAL_MARGIN_RAW 300
#define TP_DEBUG_TOUCH_LOGS 1

static bool raw_in_range_with_margin(uint16_t value, uint16_t a, uint16_t b)
{
    int min = a < b ? a : b;
    int max = a < b ? b : a;
    return (int)value >= min - TP_CAL_MARGIN_RAW && (int)value <= max + TP_CAL_MARGIN_RAW;
}

static uint16_t clamp_u16(int value, int min, int max)
{
    if (value < min) return (uint16_t)min;
    if (value > max) return (uint16_t)max;
    return (uint16_t)value;
}

static bool map_xpt2046_coordinate(uint16_t raw_x, uint16_t raw_y,
                                   uint16_t *screen_x, uint16_t *screen_y)
{
    const float raw_width = (float)(TP_RAW_RIGHT - TP_RAW_LEFT);
    const float raw_height = (float)(TP_RAW_TOP - TP_RAW_BOTTOM);

    if (raw_width == 0.0f || raw_height == 0.0f) {
        return false;
    }

    if (!raw_in_range_with_margin(raw_x, TP_RAW_LEFT, TP_RAW_RIGHT) ||
        !raw_in_range_with_margin(raw_y, TP_RAW_BOTTOM, TP_RAW_TOP)) {
        return false;
    }

    const float mapped_x = ((float)((int)raw_x - TP_RAW_LEFT) / raw_width) * (float)(LCD_H_RES - 1);
    const float mapped_y = ((float)(TP_RAW_TOP - (int)raw_y) / raw_height) * (float)(LCD_V_RES - 1);

    *screen_x = clamp_u16((int)(mapped_x + 0.5f), 0, LCD_H_RES - 1);
    *screen_y = clamp_u16((int)(mapped_y + 0.5f), 0, LCD_V_RES - 1);
    return true;
}

static void xpt2046_process_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                        uint16_t *strength, uint8_t *point_num,
                                        uint8_t max_point_num)
{
    LV_UNUSED(tp);
    LV_UNUSED(max_point_num);

    if (!x || !y || !point_num || *point_num == 0) {
        return;
    }

    const uint16_t raw_x = x[0];
    const uint16_t raw_y = y[0];
    uint16_t mapped_x = 0;
    uint16_t mapped_y = 0;
    bool valid = map_xpt2046_coordinate(raw_x, raw_y, &mapped_x, &mapped_y);

    if (!valid) {
        *point_num = 0;
    } else {
        x[0] = mapped_x;
        y[0] = mapped_y;
    }

#if TP_DEBUG_TOUCH_LOGS
    static int64_t s_last_log_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_log_us > 250000) {
        ESP_LOGI(TAG, "TOUCH raw=(%u,%u) strength=%u %s pixel=(%u,%u) screen=%d",
                 raw_x, raw_y, strength ? strength[0] : 0,
                 valid ? "mapped" : "rejected",
                 valid ? mapped_x : 0, valid ? mapped_y : 0,
                 (int)s_current_screen);
        s_last_log_us = now_us;
    }
#endif
}

typedef struct {
    lv_obj_t *sw;
    bool turn_on;
    int action;
} auth_task_arg_t;

enum {
    AUTH_ACTION_HUB_TOGGLE = 1,
    AUTH_ACTION_ADD_SENSOR,
    AUTH_ACTION_ADD_FINGERPRINT,
};

static void set_switch_checked(lv_obj_t *sw, bool checked)
{
    if (!sw) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;

    s_switch_internal = true;
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
    s_switch_internal = false;

    lvgl_port_unlock();
}

static void auth_toggle_task(void *arg)
{
    auth_task_arg_t *a = (auth_task_arg_t *)arg;
    lv_obj_t *sw = a->sw;
    bool turn_on = a->turn_on;
    int action = a->action;
    free(a);

    g_mode = MODE_FINGERPRINT_VERIFY;
    display_show_fingerprint_screen(action == AUTH_ACTION_ADD_FINGERPRINT ? "Authentication" : "Authentication",
                                    "Place your finger on the sensor");
    vTaskDelay(pdMS_TO_TICKS(350));
    esp_err_t result = (action == AUTH_ACTION_ADD_FINGERPRINT) ? fp_verify_admin() : fp_verify();

    if (result == ESP_OK) {
        if (action == AUTH_ACTION_ADD_SENSOR) {
            display_clear_sensor_notifications();
            s_prev_screen = SCREEN_ID_SENSOR_NODES_SETTING;
            if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                load_screen_locked(SCREEN_ID_ADD_ANOTHER__SENSOR);
                lvgl_port_unlock();
            }
            g_mode = MODE_OPERATIONAL;
        } else if (action == AUTH_ACTION_ADD_FINGERPRINT) {
            display_show_fingerprint_screen("Registration", "Place your finger on the sensor");
            vTaskDelay(pdMS_TO_TICKS(350));
            g_mode = MODE_FINGERPRINT_ENROLL;
            if (fp_enroll() == ESP_OK) {
                display_fingerprint_status("Registration completed");
            } else {
                display_fingerprint_status("Denied");
            }
            g_mode = s_ui_online ? MODE_OPERATIONAL : MODE_OFFLINE;
            vTaskDelay(pdMS_TO_TICKS(1200));
            display_show_dashboard(s_ui_online);
        } else if (turn_on) {
            display_fingerprint_status("Turning hub on");
            if (wifi_resume_from_offline_mode()) {
                set_switch_checked(sw, true);
                display_show_dashboard(true);
            } else {
                set_switch_checked(sw, false);
                g_mode = MODE_OFFLINE;
                display_show_dashboard(false);
            }
        } else {
            display_fingerprint_status("Turning hub off");
            wifi_enter_offline_mode();
            set_switch_checked(sw, false);
            display_show_dashboard(false);
        }
    } else {
        display_fingerprint_status("Denied");
        vTaskDelay(pdMS_TO_TICKS(1200));
        if (action == AUTH_ACTION_HUB_TOGGLE && turn_on) {
            set_switch_checked(sw, false);
            g_mode = MODE_OFFLINE;
            display_show_dashboard(false);
        } else if (action == AUTH_ACTION_HUB_TOGGLE) {
            set_switch_checked(sw, true);
            g_mode = MODE_OPERATIONAL;
            display_show_dashboard(true);
        } else {
            g_mode = MODE_OPERATIONAL;
            display_show_dashboard(s_ui_online);
        }
    }

    s_auth_task = NULL;
    vTaskDelete(NULL);
}

static void start_auth_action(int action, lv_obj_t *sw, bool turn_on)
{
    if (s_auth_task != NULL) {
        display_fingerprint_status("Auth in progress");
        return;
    }

    auth_task_arg_t *arg = malloc(sizeof(*arg));
    if (!arg) {
        display_fingerprint_status("Auth unavailable");
        return;
    }
    arg->sw = sw;
    arg->turn_on = turn_on;
    arg->action = action;

    if (xTaskCreatePinnedToCore(auth_toggle_task, "fp_auth", 6144, arg, 5, &s_auth_task, 1) != pdPASS) {
        free(arg);
        s_auth_task = NULL;
        display_fingerprint_status("Auth unavailable");
    }
}

static void critical_toggle_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED || s_switch_internal) return;

    lv_obj_t *sw = lv_event_get_target(e);
    bool is_on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    ESP_LOGI(TAG, "Critical Toggle requested: %s", is_on ? "ON" : "OFF");

    if (s_auth_task != NULL) {
        set_switch_checked(sw, !is_on);
        display_fingerprint_status("Auth in progress");
        return;
    }

    bool can_toggle_on = (g_mode == MODE_OFFLINE && is_on);
    bool can_toggle_off = (g_mode == MODE_OPERATIONAL && !is_on);
    if (!can_toggle_on && !can_toggle_off) {
        set_switch_checked(sw, g_mode != MODE_OFFLINE);
        return;
    }

    start_auth_action(AUTH_ACTION_HUB_TOGGLE, sw, is_on);
}

static esp_err_t lcd_hw_init(void)
{
    ESP_LOGI(TAG, "Display init: SPI bus starting, free internal heap=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    spi_bus_config_t bus = {
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = TOUCH_PIN_MISO,
        .sclk_io_num     = LCD_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Display init: SPI bus ready");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = LCD_PIN_DC,
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_LOGI(TAG, "Display init: creating LCD panel IO");
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: LCD panel IO create failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_endian     = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_LOGI(TAG, "Display init: creating ILI9341 panel");
    err = esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: ILI9341 panel create failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Display init: panel reset");
    err = esp_lcd_panel_reset(panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: panel reset failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Display init: panel init");
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: panel init failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "Display init: panel orientation/on");
    err = esp_lcd_panel_invert_color(panel, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: invert color failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_mirror(panel, true, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: panel mirror failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_set_gap(panel, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: panel gap failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: panel on failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Hand off to LVGL port */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    port_cfg.task_affinity = 1;
    ESP_LOGI(TAG, "Display init: LVGL port init");
    err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: LVGL port init failed: %s", esp_err_to_name(err));
        return err;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = false,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .flags         = { .buff_dma = true },
    };
    ESP_LOGI(TAG, "Display init: adding LVGL display, internal_free=%u internal_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s_lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_lvgl_disp) {
        ESP_LOGE(TAG, "Display init: lvgl_port_add_disp returned NULL, internal_free=%u internal_largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Display init: LVGL display registered");

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(TOUCH_PIN_CS);
    ESP_LOGI(TAG, "Display init: creating XPT2046 touch IO");
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &tp_io_cfg, &tp_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: touch IO create failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = TOUCH_PIN_IRQ,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .process_coordinates = xpt2046_process_coordinates,
    };
    ESP_LOGI(TAG, "Display init: creating XPT2046 touch driver");
    err = esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &s_tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init: touch driver create failed: %s", esp_err_to_name(err));
        return err;
    }

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_lvgl_disp,
        .handle = s_tp,
    };
    s_tp_indev = lvgl_port_add_touch(&touch_cfg);
    if (!s_tp_indev) {
        ESP_LOGE(TAG, "Display init: lvgl_port_add_touch returned NULL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Display init: XPT2046 touch registered, IRQ GPIO%d level=%d",
             TOUCH_PIN_IRQ, gpio_get_level(TOUCH_PIN_IRQ));
    return ESP_OK;
}

/* ── UI build (called once LVGL task is ready) ───────────────────────────── */

static void load_screen_locked(enum ScreensEnum screen)
{
    ui_ensure_screen(screen);
    configure_screen_locked(screen);
    s_current_screen = screen;
    loadScreen(screen);
}

static void set_switch_checked_locked(lv_obj_t *sw, bool checked)
{
    if (!sw) return;
    s_switch_internal = true;
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
    s_switch_internal = false;
}

static void make_touch_target(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_ext_click_area(obj, 6);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void clear_child_click_targets(lv_obj_t *obj)
{
    if (!obj) return;

    uint32_t child_count = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (!child) continue;
        lv_obj_clear_flag(child, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(child, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_clear_flag(child, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
        clear_child_click_targets(child);
    }
}

static void make_touch_target_tree(lv_obj_t *obj)
{
    if (!obj) return;

    make_touch_target(obj);
    clear_child_click_targets(obj);
}

static void make_back_touch_target(lv_obj_t *obj)
{
    if (!obj) return;
    make_touch_target_tree(obj);
    lv_obj_set_ext_click_area(obj, 14);
}

static void set_label_locked(lv_obj_t *label, const char *text)
{
    if (label && text) {
        lv_label_set_text(label, text);
    }
}

static void show_fingerprint_screen_locked(const char *title, const char *prompt)
{
    const char *phase = fingerprint_phase_for_title(title);
    const char *message = fingerprint_message_normalize(nonnull_text(prompt, "Place your finger on the sensor"));

    cache_lock();
    s_display_cache.view = CACHE_VIEW_FINGERPRINT;
    cache_copy(s_display_cache.fp_title, sizeof(s_display_cache.fp_title), title ? title : "Fingerprint");
    cache_copy(s_display_cache.fp_phase, sizeof(s_display_cache.fp_phase), phase);
    cache_copy(s_display_cache.fp_message, sizeof(s_display_cache.fp_message), message);
    s_display_cache.fp_progress = 0;
    cache_unlock();

    s_prev_screen = s_current_screen;
    load_screen_locked(SCREEN_ID_FINGERPRINT_SETTING);
    set_label_locked(objects.obj53, title ? title : "Fingerprint");
    set_label_locked(objects.obj49, phase);
    set_label_locked(objects.fingerprint_instruction, fingerprint_instruction_for_phase(phase));
    set_label_locked(objects.obj52, message);
    align_fingerprint_text_locked();
    if (objects.obj51) lv_bar_set_value(objects.obj51, 0, LV_ANIM_OFF);
}

static void cache_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void cache_lock(void)
{
    if (s_display_cache_mutex) {
        xSemaphoreTake(s_display_cache_mutex, portMAX_DELAY);
    }
}

static void cache_unlock(void)
{
    if (s_display_cache_mutex) {
        xSemaphoreGive(s_display_cache_mutex);
    }
}

static bool display_is_ready(void)
{
    return s_display_state == DISPLAY_READY;
}

bool display_wait_ready(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (s_display_state == DISPLAY_STARTING || s_display_state == DISPLAY_NOT_STARTED) {
        if (timeout_ms > 0 && (xTaskGetTickCount() - start) >= timeout_ticks) {
            ESP_LOGW(TAG, "Display readiness wait timed out, state=%d", (int)s_display_state);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return s_display_state == DISPLAY_READY;
}

static const char *nonnull_text(const char *text, const char *fallback)
{
    return (text && text[0]) ? text : fallback;
}

static const char *hub_location_text_locked(void)
{
    if (s_display_cache.home_name[0] != '\0') {
        return s_display_cache.home_name;
    }
    if (g_home_name[0] != '\0') {
        return g_home_name;
    }
    return "HUB_loc";
}

static void set_welcome_text_locked(const char *user_name)
{
    char greeting[80] = "Welcome";
    const unsigned char *start = (const unsigned char *)(user_name ? user_name : "");

    while (*start && isspace(*start)) {
        start++;
    }

    if (*start) {
        char first_name[64];
        size_t len = 0;
        while (start[len] && !isspace(start[len]) && len < sizeof(first_name) - 1) {
            first_name[len] = (char)start[len];
            len++;
        }
        first_name[len] = '\0';
        if (len > 0) {
            snprintf(greeting, sizeof(greeting), "Welcome, %s", first_name);
        }
    }

    set_label_locked(objects.welcome_home, greeting);
}

static void set_cached_welcome_text_locked(void)
{
    char user_name[sizeof(s_display_cache.user_name)];
    cache_lock();
    cache_copy(user_name, sizeof(user_name), s_display_cache.user_name);
    cache_unlock();
    set_welcome_text_locked(user_name);
}

static void set_status_pill_style_locked(lv_obj_t *pill, uint32_t color, uint32_t bg)
{
    if (!pill) return;
    lv_obj_set_style_bg_color(pill, lv_color_hex(bg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(pill, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(pill, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(pill, 170, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void update_home_datetime_locked(void)
{
    char time_buf[8] = "--:--";
    char date_buf[16] = "---";
    time_t now = time(NULL);
    struct tm tm_now;

    if (now > 0 && localtime_r(&now, &tm_now) != NULL && tm_now.tm_year >= 124) {
        strftime(time_buf, sizeof(time_buf), "%H:%M", &tm_now);
        strftime(date_buf, sizeof(date_buf), "%a %d %b", &tm_now);
    }

    if (objects.home_time) set_label_locked(objects.home_time, time_buf);
    if (objects.home_date) set_label_locked(objects.home_date, date_buf);
    if (objects.strip_date) set_label_locked(objects.strip_date, date_buf);
}

static void set_hub_connection_status_locked(bool online)
{
    load_screen_locked(SCREEN_ID_HUB_ONLINE);
    update_home_datetime_locked();
    set_cached_welcome_text_locked();
    set_switch_checked_locked(objects.obj1, online);

    if (online) {
        if (objects.status_dot)
            lv_obj_set_style_bg_color(objects.status_dot, lv_color_hex(C_GREEN_U32),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        if (objects.hub_location_dot)
            lv_obj_set_style_bg_color(objects.hub_location_dot, lv_color_hex(C_GREEN_U32),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        if (objects.hub_status)
            lv_obj_set_style_text_color(objects.hub_status, lv_color_hex(C_GREEN_U32),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        if (objects.hub_location)
            lv_obj_set_style_text_color(objects.hub_location, lv_color_hex(C_GREEN_U32),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        set_label_locked(objects.hub_status, "Online");
        set_label_locked(objects.hub_location, hub_location_text_locked());

        int n = espnow_get_sensor_count();
        char info_buf[64];
        snprintf(info_buf, sizeof(info_buf), "%d sensor node%s active", n, n == 1 ? "" : "s");
        set_label_locked(objects.sensor_info, info_buf);
    } else {
        if (objects.status_dot)
            lv_obj_set_style_bg_color(objects.status_dot, lv_color_hex(C_RED_U32),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        if (objects.hub_location_dot)
            lv_obj_set_style_bg_color(objects.hub_location_dot, lv_color_hex(C_RED_U32),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        if (objects.hub_status)
            lv_obj_set_style_text_color(objects.hub_status, lv_color_hex(C_RED_U32),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        if (objects.hub_location)
            lv_obj_set_style_text_color(objects.hub_location, lv_color_hex(C_RED_U32),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        set_label_locked(objects.hub_status, "Offline");
        set_label_locked(objects.hub_location, hub_location_text_locked());
        set_label_locked(objects.sensor_info, "0 nodes active - Sensors disconnected");
    }
}

static const char *aqi_display_state(const char *state)
{
    if (!state) return "Healthy";
    if (strcmp(state, "good") == 0 || strcmp(state, "very_good") == 0 ||
        strcmp(state, "ver_good") == 0) return "Healthy";
    if (strcmp(state, "nominal") == 0) return "Nominal";
    if (strcmp(state, "moderate") == 0) return "Moderate";
    if (strcmp(state, "poor") == 0) return "Poor";
    if (strcmp(state, "very_poor") == 0 || strcmp(state, "ver_poor") == 0) return "Unhealthy";
    if (strcmp(state, "severe") == 0) return "Severe";
    return "Unknown";
}

static uint32_t aqi_state_color(const char *state)
{
    if (!state || strcmp(state, "good") == 0 || strcmp(state, "very_good") == 0 ||
        strcmp(state, "ver_good") == 0) return C_GREEN_U32;
    if (strcmp(state, "nominal") == 0) return C_AQI_NOMINAL_U32;
    if (strcmp(state, "moderate") == 0) return C_AMBER_U32;
    if (strcmp(state, "poor") == 0) return C_AQI_POOR_U32;
    if (strcmp(state, "very_poor") == 0 || strcmp(state, "ver_poor") == 0)
        return C_AQI_UNHEALTHY_U32;
    if (strcmp(state, "severe") == 0) return C_RED_U32;
    return C_T2_U32;
}

static void set_aqi_value_locked(float aqi, const char *state)
{
    if (aqi < 0.0f) aqi = 0.0f;
    if (aqi > 500.0f) aqi = 500.0f;

    char value[16];
    snprintf(value, sizeof(value), "%.0f", aqi);
    set_label_locked(objects.aqi_val, value);
    align_dashboard_value_locked(objects.aqi_val, objects.aqi_arc);
    if (objects.aqi_arc) lv_arc_set_value(objects.aqi_arc, (int)aqi);
    set_label_locked(objects.aqi_state, aqi_display_state(state));

    uint32_t color = aqi_state_color(state);
    bool alert = strcmp(aqi_display_state(state), "Unhealthy") == 0 ||
                 strcmp(aqi_display_state(state), "Severe") == 0;
    if (objects.aqi_arc) {
        lv_obj_set_style_arc_color(objects.aqi_arc, lv_color_hex(color),
                                   LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }
    if (objects.aqi_dot) {
        lv_obj_set_style_bg_color(objects.aqi_dot, lv_color_hex(color),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (objects.aqi_state) {
        lv_obj_set_style_text_color(objects.aqi_state, lv_color_hex(color),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    set_status_pill_style_locked(objects.aqi_mood, color,
                                 alert ? UI_COLOR_ALERT_BG : UI_COLOR_PANEL);
    if (objects.aqi_cont) {
        lv_obj_set_style_bg_color(objects.aqi_cont,
                                  lv_color_hex(alert ? UI_COLOR_ALERT_BG : UI_COLOR_CARD),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(objects.aqi_cont,
                                      lv_color_hex(alert ? color : UI_COLOR_CARD_BORDER),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void update_temp_pill_locked(float temp)
{
    if (!objects.obj7 || !objects.temp_mood || !objects.temp_arc) return;
    const char *text;
    uint32_t color;
    if (temp > TEMP_THRESH_HOT) {
        text = "Too Hot!";
        color = C_RED_U32;
    } else if (temp > TEMP_THRESH_WARM) {
        text = "Warm";
        color = C_AMBER_U32;
    } else {
        text = "Comfortable";
        color = C_CYAN_U32;
    }
    uint32_t chip_bg = temp > TEMP_THRESH_HOT ? UI_COLOR_ALERT_BG : UI_COLOR_PANEL;
    lv_label_set_text(objects.obj7, text);
    lv_obj_set_style_text_color(objects.obj7, lv_color_hex(color),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    if (objects.temp_img)
        lv_obj_set_style_bg_color(objects.temp_img, lv_color_hex(color),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(objects.temp_arc, lv_color_hex(color),
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
    set_status_pill_style_locked(objects.temp_mood, color, chip_bg);
    if (objects.temp_cont) {
        lv_obj_set_style_bg_color(objects.temp_cont,
                                  lv_color_hex(temp > TEMP_THRESH_HOT ? UI_COLOR_ALERT_BG : UI_COLOR_CARD),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(objects.temp_cont,
                                      lv_color_hex(temp > TEMP_THRESH_HOT ? UI_COLOR_ALERT_BORDER : UI_COLOR_CARD_BORDER),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void update_hum_pill_locked(float hum)
{
    if (!objects.obj12 || !objects.hum_mood || !objects.hum_bar) return;
    const char *text;
    uint32_t color;
    if (hum >= 80.0f) {
        text = "Critical";
        color = C_RED_U32;
    } else if (hum > HUM_THRESH_HIGH) {
        text = "High";
        color = C_AMBER_U32;
    } else {
        text = "Moderate";
        color = C_CYAN_U32;
    }
    uint32_t chip_bg = hum >= 80.0f ? UI_COLOR_ALERT_BG : UI_COLOR_PANEL;
    lv_label_set_text(objects.obj12, text);
    lv_obj_set_style_text_color(objects.obj12, lv_color_hex(color),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    if (objects.hum_img)
        lv_obj_set_style_bg_color(objects.hum_img, lv_color_hex(color),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(objects.hum_bar, lv_color_hex(color),
                              LV_PART_INDICATOR | LV_STATE_DEFAULT);
    set_status_pill_style_locked(objects.hum_mood, color, chip_bg);
    if (objects.hum_cont) {
        lv_obj_set_style_bg_color(objects.hum_cont,
                                  lv_color_hex(hum >= 80.0f ? UI_COLOR_ALERT_BG : UI_COLOR_CARD),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(objects.hum_cont,
                                      lv_color_hex(hum >= 80.0f ? UI_COLOR_ALERT_BORDER : UI_COLOR_CARD_BORDER),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static const char *fingerprint_phase_for_title(const char *title)
{
    if (title && (strstr(title, "Admin") || strstr(title, "admin"))) {
        return "Admin User Only\nVerifying your fingerprint";
    }
    if (title && (strstr(title, "Verify") || strstr(title, "verify") ||
                  strstr(title, "Authentication") || strstr(title, "authentication"))) {
        return "Verifying your fingerprint";
    }
    return "Registering your fingerprint";
}

static const char *fingerprint_instruction_for_phase(const char *phase)
{
    if (phase && (strstr(phase, "Register") || strstr(phase, "register"))) {
        return "Keep your finger on the sensor\nfor registration.";
    }
    return "Place your finger on the sensor\nfor verification.";
}

static const char *fingerprint_message_normalize(const char *message)
{
    if (!message) return NULL;
    if (strcmp(message, "Place finger on sensor") == 0 ||
        strcmp(message, "Scan your fingerprint") == 0 ||
        strcmp(message, "Try fingerprint again") == 0) {
        return "Place your finger on the sensor";
    }
    if (strcmp(message, "Processing . . .") == 0 ||
        strcmp(message, "Processing") == 0) {
        return "Processing...";
    }
    if (strcmp(message, "Fingerprint registered") == 0) {
        return "Registration completed";
    }
    if (strcmp(message, "Access denied") == 0 ||
        strcmp(message, "Enrollment failed") == 0) {
        return "Denied";
    }
    return message;
}

static void align_dashboard_value_locked(lv_obj_t *value, lv_obj_t *arc)
{
    if (value && arc) {
        lv_obj_align_to(value, arc, LV_ALIGN_CENTER, 0, -3);
    }
}

static void align_fingerprint_text_locked(void)
{
    if (objects.obj53) {
        lv_obj_set_pos(objects.obj53, 40, 10);
        lv_obj_set_width(objects.obj53, 144);
        lv_obj_set_style_text_align(objects.obj53, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    if (objects.obj49) {
        lv_obj_set_pos(objects.obj49, 20, 126);
        lv_obj_set_width(objects.obj49, 184);
        lv_obj_set_style_text_font(objects.obj49, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_align(objects.obj49, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    if (objects.fingerprint_instruction) {
        lv_obj_set_pos(objects.fingerprint_instruction, 18, 166);
        lv_obj_set_width(objects.fingerprint_instruction, 188);
        lv_obj_set_style_text_align(objects.fingerprint_instruction, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    if (objects.obj52) {
        lv_obj_set_pos(objects.obj52, 74, 226);
        lv_obj_set_width(objects.obj52, 90);
        lv_obj_set_style_text_align(objects.obj52, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    }
}

static void set_dashboard_values_locked(float temp, float hum)
{
    char buf[16];
    if (temp < 0.0f) temp = 0.0f;
    if (temp > 50.0f) temp = 50.0f;
    if (hum < 0.0f) hum = 0.0f;
    if (hum > 100.0f) hum = 100.0f;

    snprintf(buf, sizeof(buf), "%.1f", temp);
    set_label_locked(objects.temp_val, buf);
    align_dashboard_value_locked(objects.temp_val, objects.temp_arc);
    if (objects.temp_arc) lv_arc_set_value(objects.temp_arc, (int)temp);

    snprintf(buf, sizeof(buf), "%.0f", hum);
    set_label_locked(objects.hum_val, buf);
    if (objects.hum_bar) lv_bar_set_value(objects.hum_bar, (int)hum, LV_ANIM_OFF);

    update_temp_pill_locked(temp);
    update_hum_pill_locked(hum);
}

static void cache_apply_locked(void)
{
    cache_lock();
    display_cache_t cache = s_display_cache;
    cache_unlock();

    switch (cache.view) {
    case CACHE_VIEW_SETUP:
        ESP_LOGI(TAG, "Display applying cached setup prompt");
        load_screen_locked(SCREEN_ID_HUB_REGISTER_WELCOME);
        break;
    case CACHE_VIEW_ONLINE:
        set_hub_connection_status_locked(true);
        break;
    case CACHE_VIEW_OFFLINE:
        set_hub_connection_status_locked(false);
        break;
    case CACHE_VIEW_FINGERPRINT:
        load_screen_locked(SCREEN_ID_FINGERPRINT_SETTING);
        set_label_locked(objects.obj53, nonnull_text(cache.fp_title, "Fingerprint"));
        set_label_locked(objects.obj49, nonnull_text(cache.fp_phase, "Registering your fingerprint"));
        set_label_locked(objects.fingerprint_instruction,
                         fingerprint_instruction_for_phase(nonnull_text(cache.fp_phase,
                                                                         "Registering your fingerprint")));
        set_label_locked(objects.obj52,
                         fingerprint_message_normalize(nonnull_text(cache.fp_message,
                                                                    "Place your finger on the sensor")));
        align_fingerprint_text_locked();
        if (objects.obj51) lv_bar_set_value(objects.obj51, cache.fp_progress, LV_ANIM_OFF);
        break;
    case CACHE_VIEW_NONE:
    default:
        break;
    }

    if (cache.has_temp_hum) {
        set_dashboard_values_locked(cache.temp, cache.hum);
    }
    if (cache.has_aqi) {
        set_aqi_value_locked(cache.aqi, cache.aqi_state);
    }
}

static void nav_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: Back");
    enum ScreensEnum target = SCREEN_ID_HUB_ONLINE;
    if (s_current_screen == SCREEN_ID_ABOUT_GLAZIA ||
        s_current_screen == SCREEN_ID_SENSOR_NODES_SETTING ||
        s_current_screen == SCREEN_ID_FINGERPRINT_SETTING) {
        target = SCREEN_ID_SETTINGS_MENU;
    } else if (s_current_screen == SCREEN_ID_ADD_ANOTHER__SENSOR) {
        display_clear_sensor_notifications();
        target = SCREEN_ID_SENSOR_NODES_SETTING;
    } else {
        target = SCREEN_ID_HUB_ONLINE;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        load_screen_locked(target);
        lvgl_port_unlock();
    }
}

static void settings_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: Settings");
    s_prev_screen = SCREEN_ID_HUB_ONLINE;
    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        load_screen_locked(SCREEN_ID_SETTINGS_MENU);
        lvgl_port_unlock();
    }
}

static void sensor_nodes_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: Sensor Nodes");
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        load_screen_locked(SCREEN_ID_SENSOR_NODES_SETTING);
        refresh_sensor_nodes_locked();
        lvgl_port_unlock();
    }
}

static void about_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: About");
    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        load_screen_locked(SCREEN_ID_ABOUT_GLAZIA);
        lvgl_port_unlock();
    }
}

static void add_fingerprint_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: Add Fingerprint");
    show_fingerprint_screen_locked("Authentication", "Place your finger on the sensor");
    start_auth_action(AUTH_ACTION_ADD_FINGERPRINT, NULL, false);
}

static void add_sensor_auth_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: Add Sensor");
    show_fingerprint_screen_locked("Authentication", "Place your finger on the sensor");
    start_auth_action(AUTH_ACTION_ADD_SENSOR, NULL, false);
}

static void add_sensor_start_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_sensor_added_view) {
        ESP_LOGI(TAG, "Touch: Sensor Added Done");
        s_sensor_added_view = false;
        display_clear_sensor_notifications();
        if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
            load_screen_locked(SCREEN_ID_SENSOR_NODES_SETTING);
            refresh_sensor_nodes_locked();
            lvgl_port_unlock();
        }
        return;
    }
    ESP_LOGI(TAG, "Touch: Start Sensor Pairing");
    sensor_pairing_open_window();
}

static void sensor_switch_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_switch_internal) return;
    lv_obj_t *sw = lv_event_get_target(e);
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Touch: Sensor %d %s", index, enabled ? "enabled" : "disabled");
    espnow_set_sensor_enabled(index, enabled);
}

static void create_sensor_row(lv_obj_t *parent, int index, const char *name, bool enabled)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_pos(row, 8, 4 + index * 50);
    lv_obj_set_size(row, 224, 44);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(row, 190, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, 230, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sensor_icon = lv_img_create(row);
    lv_obj_set_pos(sensor_icon, 10, 7);
    lv_img_set_src(sensor_icon, &img_sensor);
    lv_img_set_zoom(sensor_icon, 150);
    lv_obj_set_style_img_recolor(sensor_icon, lv_color_hex(UI_COLOR_AMBER),
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_img_recolor_opa(sensor_icon, 255,
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(sensor_icon, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(enabled ? UI_COLOR_TEXT_PRIMARY : UI_COLOR_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_pos(label, 52, 16);
    lv_obj_set_size(label, 100, 14);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_pos(sw, 166, 10);
    lv_obj_set_size(sw, 44, 24);
    ui_style_toggle(sw);
    make_touch_target(sw);
    set_switch_checked_locked(sw, enabled);
    lv_obj_add_event_cb(sw, sensor_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
}

static void reg_welcome_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Show QR screen immediately for visual feedback */
    if (lvgl_port_lock(pdMS_TO_TICKS(100))) {
        load_screen_locked(SCREEN_ID_HUB_REGISTER_QR);
        lvgl_port_unlock();
    }
    /* Start BLE provisioning — identical to physical button press in MODE_IDLE */
    g_mode = MODE_HUB_PAIRING;
    ble_start();
}

static void configure_screen_locked(enum ScreensEnum screen)
{
    if (screen < _SCREEN_ID_FIRST || screen > _SCREEN_ID_LAST || s_screen_configured[screen]) {
        return;
    }

    switch (screen) {
    case SCREEN_ID_HUB_REGISTER_WELCOME:
        make_touch_target_tree(objects.reg_welcome_btn);
        if (objects.reg_welcome_btn)
            lv_obj_add_event_cb(objects.reg_welcome_btn,
                                reg_welcome_btn_event_cb, LV_EVENT_CLICKED, NULL);
        break;

    case SCREEN_ID_HUB_REGISTER_QR:
        break;

    case SCREEN_ID_HUB_ONLINE:
        s_critical_sw = objects.obj1;
        make_touch_target(objects.obj1);
        make_touch_target_tree(objects.button);
        if (objects.obj1) lv_obj_add_event_cb(objects.obj1, critical_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
        if (objects.button) lv_obj_add_event_cb(objects.button, settings_cb, LV_EVENT_CLICKED, NULL);
        cache_lock();
        bool has_temp_hum = s_display_cache.has_temp_hum;
        float temp = s_display_cache.temp;
        float hum = s_display_cache.hum;
        cache_unlock();
        set_dashboard_values_locked(has_temp_hum ? temp : 0.0f, has_temp_hum ? hum : 0.0f);
        cache_lock();
        bool has_aqi = s_display_cache.has_aqi;
        float aqi = s_display_cache.aqi;
        char aqi_state[sizeof(s_display_cache.aqi_state)];
        cache_copy(aqi_state, sizeof(aqi_state), s_display_cache.aqi_state);
        cache_unlock();
        set_aqi_value_locked(has_aqi ? aqi : 0.0f, has_aqi ? aqi_state : "good");
        set_label_locked(objects.hub_location, hub_location_text_locked());
        set_cached_welcome_text_locked();
        break;

    case SCREEN_ID_SETTINGS_MENU:
        make_back_touch_target(objects.obj14);
        make_touch_target_tree(objects.fingerprint_option);
        make_touch_target_tree(objects.sensor_nodes_option);
        make_touch_target_tree(objects.about_section);
        if (objects.obj14) lv_obj_add_event_cb(objects.obj14, nav_back_cb, LV_EVENT_CLICKED, NULL);
        if (objects.fingerprint_option) lv_obj_add_event_cb(objects.fingerprint_option, add_fingerprint_cb, LV_EVENT_CLICKED, NULL);
        if (objects.sensor_nodes_option) lv_obj_add_event_cb(objects.sensor_nodes_option, sensor_nodes_cb, LV_EVENT_CLICKED, NULL);
        if (objects.about_section) lv_obj_add_event_cb(objects.about_section, about_cb, LV_EVENT_CLICKED, NULL);
        break;

    case SCREEN_ID_SENSOR_NODES_SETTING:
        make_back_touch_target(objects.obj44);
        make_touch_target_tree(objects.add_sensor_button);
        if (objects.settings_menu_cont_1) {
            lv_obj_set_scroll_dir(objects.settings_menu_cont_1, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(objects.settings_menu_cont_1, LV_SCROLLBAR_MODE_ACTIVE);
        }
        if (objects.obj44) lv_obj_add_event_cb(objects.obj44, nav_back_cb, LV_EVENT_CLICKED, NULL);
        if (objects.add_sensor_button) lv_obj_add_event_cb(objects.add_sensor_button, add_sensor_auth_cb, LV_EVENT_CLICKED, NULL);
        break;

    case SCREEN_ID_ABOUT_GLAZIA: {
        lv_obj_t *about_parent = objects.obj48 ? lv_obj_get_parent(objects.obj48) : NULL;
        if (about_parent) {
            lv_obj_set_scroll_dir(about_parent, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(about_parent, LV_SCROLLBAR_MODE_ACTIVE);
        }
        make_back_touch_target(objects.obj47);
        if (objects.obj47) lv_obj_add_event_cb(objects.obj47, nav_back_cb, LV_EVENT_CLICKED, NULL);
        break;
    }

    case SCREEN_ID_FINGERPRINT_SETTING:
        make_back_touch_target(objects.obj54);
        if (objects.obj54) lv_obj_add_event_cb(objects.obj54, nav_back_cb, LV_EVENT_CLICKED, NULL);
        if (objects.obj51) lv_bar_set_value(objects.obj51, 0, LV_ANIM_OFF);
        break;

    case SCREEN_ID_ADD_ANOTHER__SENSOR:
        make_back_touch_target(objects.obj55);
        make_touch_target_tree(objects.obj59);
        if (objects.obj55) lv_obj_add_event_cb(objects.obj55, nav_back_cb, LV_EVENT_CLICKED, NULL);
        if (objects.obj59) lv_obj_add_event_cb(objects.obj59, add_sensor_start_cb, LV_EVENT_CLICKED, NULL);
        break;

    default:
        break;
    }

    s_screen_configured[screen] = true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

static void display_init_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Display init task started");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = lcd_hw_init();
    if (err != ESP_OK) {
        s_display_state = DISPLAY_FAILED;
        ESP_LOGE(TAG, "Display init failed during hardware/LVGL setup: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* Wait up to 2 s for the LVGL task to be ready before building the UI. */
    ESP_LOGI(TAG, "Display init: waiting for LVGL lock");
    if (!lvgl_port_lock(pdMS_TO_TICKS(2000))) {
        s_display_state = DISPLAY_FAILED;
        ESP_LOGE(TAG, "LVGL lock timed out — display not ready");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Display init: ui_init starting");
    ui_init();
    ESP_LOGI(TAG, "Display init: ui_init done");
    enum ScreensEnum boot_screen = (g_hub_secret[0] != '\0')
        ? SCREEN_ID_HUB_ONLINE : SCREEN_ID_HUB_REGISTER_WELCOME;
    configure_screen_locked(boot_screen);

    s_display_state = DISPLAY_READY;
    cache_apply_locked();

    lvgl_port_unlock();

    fp_set_display_cb(display_fingerprint_status);

    ESP_LOGI(TAG, "Display ready — SPI direct, MOSI=GPIO%d, touch enabled", LCD_PIN_MOSI);
    vTaskDelete(NULL);
}

void display_init(void)
{
    ESP_LOGI(TAG, "Display init requested");
    if (!s_display_cache_mutex) {
        s_display_cache_mutex = xSemaphoreCreateMutex();
        if (!s_display_cache_mutex) {
            ESP_LOGE(TAG, "Display init: failed to create cache mutex");
            s_display_state = DISPLAY_FAILED;
            return;
        }
    }

    fp_set_display_cb(display_fingerprint_status);

    if (s_display_state == DISPLAY_READY || s_display_state == DISPLAY_STARTING) {
        ESP_LOGI(TAG, "Display init skipped, state=%d", (int)s_display_state);
        return;
    }

    s_display_state = DISPLAY_STARTING;
    if (xTaskCreatePinnedToCore(display_init_task, "display_init", 8192, NULL, tskIDLE_PRIORITY + 1, NULL, 1) != pdPASS) {
        s_display_state = DISPLAY_FAILED;
        ESP_LOGE(TAG, "Display init: failed to create display init task");
        return;
    }
    ESP_LOGI(TAG, "Display init queued");
}

static void refresh_sensor_nodes_locked(void)
{
    ui_ensure_screen(SCREEN_ID_SENSOR_NODES_SETTING);
    configure_screen_locked(SCREEN_ID_SENSOR_NODES_SETTING);
    if (!objects.settings_menu_cont_1) return;

    lv_obj_clean(objects.settings_menu_cont_1);
    int count = espnow_get_sensor_count();
    if (count == 0) {
        lv_obj_t *label = lv_label_create(objects.settings_menu_cont_1);
        lv_label_set_text(label, "No sensors paired yet");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(C_T2_U32), LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        char name[24];
        bool enabled = true;
        bool paired = false;
        if (espnow_get_sensor_info(i, name, sizeof(name), &enabled, &paired)) {
            create_sensor_row(objects.settings_menu_cont_1, i, name, enabled);
        }
    }
}

void display_show(const char *line1, const char *line2)
{
    cache_lock();
    cache_copy(s_display_cache.line1, sizeof(s_display_cache.line1), line1);
    cache_copy(s_display_cache.line2, sizeof(s_display_cache.line2), line2);
    cache_unlock();

    ESP_LOGI(TAG, "Display: [%s] [%s]", line1 ? line1 : "", line2 ? line2 : "");
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached message only (state=%d)", (int)s_display_state);
        return;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        if (s_current_screen == SCREEN_ID_HUB_ONLINE) {
            set_label_locked(objects.sensor_info, line2 ? line2 : "");
        }
        lvgl_port_unlock();
    }
}

void display_show_setup_prompt(void)
{
    s_ui_online = false;
    cache_lock();
    s_display_cache.view = CACHE_VIEW_SETUP;
    s_display_cache.line1[0] = '\0';
    s_display_cache.line2[0] = '\0';
    s_display_cache.home_name[0] = '\0';
    s_display_cache.user_name[0] = '\0';
    cache_unlock();

    ESP_LOGI(TAG, "Display setup prompt");
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached setup prompt only (state=%d)", (int)s_display_state);
        return;
    }

    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    ESP_LOGI(TAG, "Display loading setup registration welcome");
    load_screen_locked(SCREEN_ID_HUB_REGISTER_WELCOME);
    lvgl_port_unlock();
}

void display_fingerprint_status(const char *message)
{
    if (!message) return;
    cache_lock();
    cache_copy(s_display_cache.fp_message, sizeof(s_display_cache.fp_message),
               fingerprint_message_normalize(message));
    s_display_cache.view = CACHE_VIEW_FINGERPRINT;
    cache_unlock();

    ESP_LOGI(TAG, "Fingerprint panel: %s", message);
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached fingerprint status only (state=%d)", (int)s_display_state);
        return;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        set_label_locked(objects.obj52, fingerprint_message_normalize(message));
        align_fingerprint_text_locked();
        lvgl_port_unlock();
    }
}

void display_hub_location(const char *home_name)
{
    if (!home_name) return;
    cache_lock();
    cache_copy(s_display_cache.home_name, sizeof(s_display_cache.home_name), home_name);
    cache_unlock();

    ESP_LOGI(TAG, "Display hub location: %s", home_name);
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached hub location only (state=%d)", (int)s_display_state);
        return;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        if (s_current_screen == SCREEN_ID_HUB_ONLINE) {
            set_label_locked(objects.hub_status, s_ui_online ? "Online" : "Offline");
            set_label_locked(objects.hub_location, hub_location_text_locked());
            if (objects.hub_status)
                lv_obj_set_style_text_color(objects.hub_status,
                                            lv_color_hex(s_ui_online ? C_GREEN_U32 : C_RED_U32),
                                            LV_PART_MAIN | LV_STATE_DEFAULT);
            if (objects.hub_location)
                lv_obj_set_style_text_color(objects.hub_location,
                                            lv_color_hex(s_ui_online ? C_GREEN_U32 : C_RED_U32),
                                            LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        lvgl_port_unlock();
    }
}

void display_user_name(const char *user_name)
{
    cache_lock();
    cache_copy(s_display_cache.user_name, sizeof(s_display_cache.user_name), user_name);
    cache_unlock();

    if (!display_is_ready()) {
        return;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        if (s_current_screen == SCREEN_ID_HUB_ONLINE) {
            set_welcome_text_locked(user_name);
        }
        lvgl_port_unlock();
    }
}

void display_update_aqi(float aqi, const char *state)
{
    cache_lock();
    s_display_cache.has_aqi = true;
    s_display_cache.aqi = aqi;
    cache_copy(s_display_cache.aqi_state, sizeof(s_display_cache.aqi_state), state);
    cache_unlock();

    if (!display_is_ready()) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    set_aqi_value_locked(aqi, state);
    lvgl_port_unlock();
}

void display_sensor_location(const char *mac_str)
{
    (void)mac_str;
}

void display_sensor_list(void)
{
    display_refresh_sensor_nodes();
}

void display_update_sensor_count(void)
{
    if (!display_is_ready() || !s_ui_online) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    int n = espnow_get_sensor_count();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d sensor node%s active", n, n == 1 ? "" : "s");
    set_label_locked(objects.sensor_info, buf);
    lvgl_port_unlock();
}

void display_show_dashboard(bool online)
{
    s_ui_online = online;
    g_mode = online ? MODE_OPERATIONAL : MODE_OFFLINE;
    cache_lock();
    s_display_cache.view = online ? CACHE_VIEW_ONLINE : CACHE_VIEW_OFFLINE;
    s_display_cache.line1[0] = '\0';
    s_display_cache.line2[0] = '\0';
    cache_unlock();

    ESP_LOGI(TAG, "Display dashboard: %s", online ? "online" : "offline");
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached dashboard only (state=%d)", (int)s_display_state);
        return;
    }

    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    set_hub_connection_status_locked(online);
    lvgl_port_unlock();
}

void display_show_fingerprint_screen(const char *title, const char *prompt)
{
    ESP_LOGI(TAG, "Fingerprint screen: title='%s' prompt='%s'",
             title ? title : "Fingerprint",
             prompt ? prompt : "Place your finger on the sensor");
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached fingerprint screen only (state=%d)", (int)s_display_state);
        cache_lock();
        s_display_cache.view = CACHE_VIEW_FINGERPRINT;
        cache_copy(s_display_cache.fp_title, sizeof(s_display_cache.fp_title), title ? title : "Fingerprint");
        cache_copy(s_display_cache.fp_phase, sizeof(s_display_cache.fp_phase), fingerprint_phase_for_title(title));
        cache_copy(s_display_cache.fp_message, sizeof(s_display_cache.fp_message),
                   fingerprint_message_normalize(nonnull_text(prompt, "Place your finger on the sensor")));
        s_display_cache.fp_progress = 0;
        cache_unlock();
        return;
    }

    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    show_fingerprint_screen_locked(title, prompt);
    lvgl_port_unlock();
}

void display_fingerprint_phase(const char *phase, const char *message)
{
    const char *normalized = fingerprint_message_normalize(message);

    cache_lock();
    s_display_cache.view = CACHE_VIEW_FINGERPRINT;
    cache_copy(s_display_cache.fp_phase, sizeof(s_display_cache.fp_phase), phase);
    cache_copy(s_display_cache.fp_message, sizeof(s_display_cache.fp_message), normalized);
    cache_unlock();

    ESP_LOGI(TAG, "Fingerprint panel: phase='%s' message='%s'",
             phase ? phase : "", message ? message : "");
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, cached fingerprint phase only (state=%d)", (int)s_display_state);
        return;
    }

    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        set_label_locked(objects.obj49, phase);
        set_label_locked(objects.fingerprint_instruction, fingerprint_instruction_for_phase(phase));
        set_label_locked(objects.obj52, normalized);
        align_fingerprint_text_locked();
        lvgl_port_unlock();
    }
}

void display_fingerprint_progress(uint8_t percent)
{
    if (percent > 100) percent = 100;
    cache_lock();
    s_display_cache.fp_progress = percent;
    cache_unlock();

    if (!display_is_ready()) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    if (objects.obj51) lv_bar_set_value(objects.obj51, percent, LV_ANIM_OFF);
    lvgl_port_unlock();
}

void display_update_temp_hum(float temp, float hum)
{
    cache_lock();
    s_display_cache.has_temp_hum = true;
    s_display_cache.temp = temp;
    s_display_cache.hum = hum;
    cache_unlock();

    if (!display_is_ready()) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    set_dashboard_values_locked(temp, hum);
    lvgl_port_unlock();
}

void display_refresh_sensor_nodes(void)
{
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, skipped sensor-node refresh (state=%d)", (int)s_display_state);
        return;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    refresh_sensor_nodes_locked();
    lvgl_port_unlock();
}

void display_sensor_added_notification(const char *name)
{
    if (!name) return;
    ESP_LOGI(TAG, "Display sensor added notification: %s", name);
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, skipped sensor-added notification (state=%d)", (int)s_display_state);
        return;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(300))) return;
    ui_ensure_screen(SCREEN_ID_ADD_ANOTHER__SENSOR);
    configure_screen_locked(SCREEN_ID_ADD_ANOTHER__SENSOR);
    if (!objects.added_sensor_data || !objects.obj58 || !objects.obj59 || !objects.obj60) {
        lvgl_port_unlock();
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "%s has been added", name);
    s_sensor_added_view = true;
    lv_label_set_text(objects.obj58, buf);
    lv_obj_clear_flag(objects.added_sensor_data, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(objects.obj60, "Done");
    lv_obj_set_style_bg_color(objects.obj59, lv_color_hex(UI_COLOR_GREEN), LV_PART_MAIN);
    lv_obj_set_style_text_color(objects.obj60, lv_color_hex(UI_COLOR_BG_GRAD), LV_PART_MAIN);
    lvgl_port_unlock();
}

void display_clear_sensor_notifications(void)
{
    if (!display_is_ready()) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(300))) return;
    s_sensor_added_view = false;
    if (objects.added_sensor_data) lv_obj_add_flag(objects.added_sensor_data, LV_OBJ_FLAG_HIDDEN);
    if (objects.obj60) {
        lv_label_set_text(objects.obj60, "+ Add Sensor");
        lv_obj_set_style_text_color(objects.obj60, lv_color_hex(UI_COLOR_PRIMARY_TEXT), LV_PART_MAIN);
    }
    if (objects.obj59) {
        lv_obj_set_style_bg_color(objects.obj59, lv_color_hex(UI_COLOR_PRIMARY_BUTTON), LV_PART_MAIN);
    }
    lvgl_port_unlock();
}

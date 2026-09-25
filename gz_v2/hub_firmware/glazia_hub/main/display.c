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
/* Real P4 hub modules — the S3 build used hub_shim.h to fake these; here they
 * are the genuine symbols so touch callbacks drive the hardware directly. */
#include "state.h"           /* hub_mode_t, g_mode, g_hub_mac/secret, g_home_name */
#include "fingerprint.h"     /* fp_verify / fp_verify_admin / fp_enroll / fp_set_display_cb */
#include "wifi.h"            /* wifi_enter_offline_mode / wifi_resume_from_offline_mode */
#include "ble.h"             /* ble_start */
#include "sensor_pairing.h"  /* sensor_pairing_open_window */
#include "nrf_thread.h"      /* nrf_thread_set_sensor_enabled / _is_sensor_offline */
#include "nvs_storage.h"     /* nvs_load_thread_sensors */
#include "metrics_history.h" /* 24h temp/hum history for stat cells + wave chart */
#include "display_power.h"   /* shared MIPI D-PHY LDO + I2C bus (with the camera) */
#include "ui/ui.h"
#include "ui/screens.h"
#include "ui/images.h"
#include "ui/styles.h"
#include "ui/fonts.h"
#include "misc/lv_area.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_touch.h"
#include "esp_check.h"
#include "esp_lcd_touch_gt911.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>
#include "esp_task_wdt.h"

static const char *TAG = "DISPLAY";

/* ── MIPI-DSI panel constants (Waveshare 7-DSI-TOUCH-A: ILI9881C, portrait) ── */
#define LCD_H_RES          720
#define LCD_V_RES          1280
#define LCD_BIT_PER_PIXEL  16                   /* RGB565, native to the DPI panel */
#define MIPI_DSI_LANE_NUM  2

#define LCD_RST_GPIO     (-1)

/* Backlight controller on the shared I2C bus (Waveshare panel): write brightness
 * 0x00..0xFF to register 0x96 of device 0x45. */
#define BL_I2C_ADDR      0x45
#define BL_I2C_REG       0x96
#define GT911_RST_GPIO   (-1)   /* not routed on FireBeetle 2 P4 — VERIFY */
#define GT911_INT_GPIO   (-1)   /* not routed → GT911 runs in polling mode */

/* ── Display-side theme aliases ─────────────────────────────────────────── */
/* Light "Glazia" theme aliases (dashboard + data screens). */
#define C_CYAN_U32   UI_D_BLUE
#define C_AMBER_U32  UI_D_AMBER
#define C_RED_U32    UI_D_RED
#define C_GREEN_U32  UI_D_GREEN
#define C_T2_U32     UI_D_MUTED
#define C_AQI_NOMINAL_U32   0x84CC16
#define C_AQI_POOR_U32      0xF97316
#define C_AQI_UNHEALTHY_U32 0xFF8247

/* ── Alert thresholds ────────────────────────────────────────────────────── */
#define TEMP_THRESH_WARM  30.0f
#define TEMP_THRESH_HOT   35.0f
#define HUM_THRESH_HIGH   65.0f

/* ── LVGL object handles (set once in display_init, read-only after) ─────── */
static lv_obj_t *s_critical_sw    = NULL;
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
    uint16_t pm25;
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
static void display_init_task(void *arg);
static esp_err_t dsi_hw_init(void);
static int active_sensor_count(void);
static void load_screen_locked(enum ScreensEnum screen);
static void configure_screen_locked(enum ScreensEnum screen);
static void refresh_sensor_nodes_locked(void);
static void set_hub_connection_status_locked(bool online);
static void update_temp_pill_locked(float temp);
static void update_hum_pill_locked(float hum);
static void set_aqi_value_locked(float aqi, const char *state, uint16_t pm25);
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
static void align_fingerprint_text_locked(void);
static void show_fingerprint_screen_locked(const char *title, const char *prompt);
static bool display_is_ready(void);
static void cache_apply_locked(void);
static void make_touch_target(lv_obj_t *obj);
static void make_touch_target_tree(lv_obj_t *obj);
static void make_back_touch_target(lv_obj_t *obj);

/* ── GT911 capacitive touch (absolute coords — no calibration needed) ─────── */
static esp_lcd_touch_handle_t s_tp = NULL;
static lv_indev_t         *s_tp_indev = NULL;
static lv_disp_t          *s_lvgl_disp = NULL;
static i2c_master_dev_handle_t s_backlight_dev = NULL;

/* ── LVGL rendering pipeline (owned here — no esp_lvgl_port) ────────────── */
static SemaphoreHandle_t      s_lvgl_mux       = NULL;
static SemaphoreHandle_t      s_flush_done_sem  = NULL;
static SemaphoreHandle_t      s_vsync_sem       = NULL;
static volatile bool          s_lvgl_flushing   = false;
static lv_disp_draw_buf_t     s_draw_buf;
static lv_disp_drv_t          s_disp_drv;
static lv_indev_drv_t         s_indev_drv;
static esp_lcd_panel_handle_t s_panel           = NULL;

/* ── Hardware init ───────────────────────────────────────────────────────── */

enum {
    AUTH_ACTION_HUB_TOGGLE = 1,
    AUTH_ACTION_ADD_SENSOR,
    AUTH_ACTION_ADD_FINGERPRINT,
};

static void set_switch_checked(lv_obj_t *sw, bool checked)
{
    if (!sw) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(200)) != pdTRUE) return;

    s_switch_internal = true;
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
    s_switch_internal = false;

    xSemaphoreGive(s_lvgl_mux);
}

/* The fingerprint sensor (R307), Wi-Fi and Thread mesh all live on this P4, so a
 * touch runs the auth flow LOCALLY (this used to be display_uart.c::auth_task on
 * the S3-bridge build): show the fingerprint prompt, gate on the R307, then apply
 * the requested action. Runs on its own task because fp_verify()/fp_enroll()
 * block on the sensor; the UI thread must not stall. */
static volatile bool s_auth_busy;

typedef struct { int action; bool want_on; } auth_arg_t;

static void load_screen_async(enum ScreensEnum screen)
{
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(300)) != pdTRUE) return;
    load_screen_locked(screen);
    xSemaphoreGive(s_lvgl_mux);
}

static void auth_task(void *arg)
{
    auth_arg_t a = *(auth_arg_t *)arg;
    free(arg);

    display_show_fingerprint_screen("Authentication", "Place your finger on the sensor");
    vTaskDelay(pdMS_TO_TICKS(350));

    esp_err_t result = (a.action == AUTH_ACTION_ADD_FINGERPRINT) ? fp_verify_admin() : fp_verify();

    if (result == ESP_OK) {
        if (a.action == AUTH_ACTION_ADD_SENSOR) {
            g_mode = MODE_OPERATIONAL;
            display_clear_sensor_notifications();
            load_screen_async(SCREEN_ID_ADD_ANOTHER__SENSOR);
        } else if (a.action == AUTH_ACTION_ADD_FINGERPRINT) {
            display_show_fingerprint_screen("Registration", "Place your finger on the sensor");
            vTaskDelay(pdMS_TO_TICKS(350));
            g_mode = MODE_FINGERPRINT_ENROLL;
            display_fingerprint_status(fp_enroll() == ESP_OK ? "Registration completed" : "Denied");
            g_mode = s_ui_online ? MODE_OPERATIONAL : MODE_OFFLINE;
            vTaskDelay(pdMS_TO_TICKS(1200));
            display_show_dashboard(s_ui_online);
        } else if (a.want_on) {          /* hub toggle ON */
            display_fingerprint_status("Turning hub on");
            bool ok = wifi_resume_from_offline_mode();
            display_show_dashboard(ok);
        } else {                          /* hub toggle OFF */
            display_fingerprint_status("Turning hub off");
            wifi_enter_offline_mode();
            display_show_dashboard(false);
        }
    } else {
        display_fingerprint_status("Denied");
        vTaskDelay(pdMS_TO_TICKS(1200));
        display_show_dashboard(s_ui_online);
    }

    s_auth_busy = false;
    vTaskDelete(NULL);
}

static void start_auth_action(int action, lv_obj_t *sw, bool turn_on, const uint8_t *eui64)
{
    (void)sw; (void)eui64;
    if (s_auth_busy) { display_fingerprint_status("Auth in progress"); return; }
    auth_arg_t *arg = malloc(sizeof(*arg));
    if (!arg) { display_fingerprint_status("Auth unavailable"); return; }
    arg->action = action; arg->want_on = turn_on;
    s_auth_busy = true;
    if (xTaskCreatePinnedToCore(auth_task, "disp_auth", 6144, arg, 5, NULL, tskNO_AFFINITY) != pdPASS) {
        free(arg);
        s_auth_busy = false;
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

    if (s_auth_busy) {
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

    start_auth_action(AUTH_ACTION_HUB_TOGGLE, sw, is_on, NULL);
}

/* Set panel brightness (0x00..0xFF) via the Waveshare backlight controller on
 * the shared I2C bus. Public so the caller could dim it later if wanted. */
void display_backlight_set(uint8_t level)
{
    if (!s_backlight_dev) return;
    uint8_t cmd[2] = { BL_I2C_REG, level };
    i2c_master_transmit(s_backlight_dev, cmd, sizeof(cmd), pdMS_TO_TICKS(100));
}

static esp_err_t backlight_init(void)
{
    i2c_master_bus_handle_t bus = display_i2c_bus();
    if (!bus) return ESP_FAIL;
    i2c_device_config_t bl_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BL_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    return i2c_master_bus_add_device(bus, &bl_cfg, &s_backlight_dev);
}

/* ── Rendering pipeline — ISR, flush, touch, tick, LVGL task ────────────── */

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_handle_t panel,
        esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    if (s_lvgl_flushing) {
        s_lvgl_flushing = false;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_flush_done_sem, &woken);
        portYIELD_FROM_ISR(woken);
    }
    return false;
}

static bool IRAM_ATTR on_dpi_vsync(esp_lcd_panel_handle_t panel,
        esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync_sem, &woken);
    return woken == pdTRUE;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    s_lvgl_flushing = true;

    /* Drain any old vsync signals */
    xSemaphoreTake(s_vsync_sem, 0);
    /* Block until the exact moment VBLANK starts */
    xSemaphoreTake(s_vsync_sem, portMAX_DELAY);

    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);

    if (xSemaphoreTake(s_flush_done_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "lvgl_flush_cb: flush timeout — ISR missed");
    }
    lv_disp_flush_ready(drv);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    if (!s_tp) { data->state = LV_INDEV_STATE_RELEASED; return; }
    uint16_t tx[1], ty[1], ts[1];
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(s_tp);
    bool pressed = esp_lcd_touch_get_coordinates(s_tp, tx, ty, ts, &cnt, 1);
    if (pressed && cnt > 0) {
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = tx[0];
        data->point.y = ty[0];
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lv_tick_cb(void *arg) { lv_tick_inc(5); }

static void lvgl_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mux);
        }
    }
}

static esp_err_t dsi_hw_init(void)
{
    esp_err_t err;

    /* MIPI D-PHY power (LDO ch3 @ 2.5V) — shared with the camera, owned here. */
    err = display_power_acquire_mipi_phy();
    if (err != ESP_OK) { ESP_LOGE(TAG, "mipi phy ldo: %s", esp_err_to_name(err)); return err; }

    /* ── DSI bus (2 lanes @ 1000 Mbps — waveshare validated params) ────────── */
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = ILI9881C_PANEL_BUS_DSI_2CH_CONFIG();
    err = esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus);
    if (err != ESP_OK) { ESP_LOGE(TAG, "dsi bus: %s", esp_err_to_name(err)); return err; }

    /* ── Panel IO (DBI, low-speed command channel) ───────────────────────── */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = ILI9881C_PANEL_IO_DBI_CONFIG();
    err = esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "dbi io: %s", esp_err_to_name(err)); return err; }

    /* ── DPI (80 MHz, validated porch values) + ILI9881C panel ──────────── */
    esp_lcd_dpi_panel_config_t dpi_cfg = ILI9881C_720_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_cfg.dpi_clock_freq_mhz = 45;
    ili9881c_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = MIPI_DSI_LANE_NUM,
        },
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config  = &vendor_cfg,
    };
    err = esp_lcd_new_panel_ili9881c(io, &panel_cfg, &panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ili9881c: %s", esp_err_to_name(err)); return err; }
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);

    /* Register ISR-safe transfer-done callback (gives flush_done_sem from ISR) */
    esp_lcd_dpi_panel_event_callbacks_t panel_cbs = {
        .on_color_trans_done = on_color_trans_done,
        .on_refresh_done = on_dpi_vsync,
    };
    esp_lcd_dpi_panel_register_event_callbacks(panel, &panel_cbs, NULL);

    esp_lcd_panel_disp_on_off(panel, true);
    s_panel = panel;

    /* ── LVGL 8 — init, draw buffer, display driver ──────────────────────── */
    lv_init();
    s_lvgl_mux       = xSemaphoreCreateMutex();
    s_flush_done_sem = xSemaphoreCreateBinary();
    s_vsync_sem      = xSemaphoreCreateBinary();

    /* 22-row draw buffer in internal SRAM; LVGL is told only 20 rows.  The extra
     * 2 rows (2880 bytes) are a guard zone that absorbs any small lv_memcpy overrun
     * from LVGL's blending pipeline without touching adjacent heap objects.
     * Internal SRAM keeps the draw buffer off the MSPI bus — PSRAM draw buffers
     * cause MSPI arbitration stalls with the DW-GDMA framebuffer stream, producing
     * display flicker and Load access faults under load.  Heap allocation (not BSS)
     * ensures SDIO mempools (allocated at do_global_ctors) are not affected. */
    lv_color_t *buf1 = heap_caps_malloc(LCD_H_RES * 52 * sizeof(lv_color_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(LCD_H_RES * 52 * sizeof(lv_color_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf1 || !buf2) { ESP_LOGE(TAG, "draw buffer alloc failed"); return ESP_ERR_NO_MEM; }
    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, LCD_H_RES * 50);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = LCD_H_RES;
    s_disp_drv.ver_res  = LCD_V_RES;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_lvgl_disp = lv_disp_drv_register(&s_disp_drv);
    ESP_LOGI(TAG, "LVGL display driver registered");

    /* ── Backlight on (I2C 0x45/0x96) ────────────────────────────────────── */
    if (backlight_init() == ESP_OK) {
        display_backlight_set(0xFF);
    } else {
        ESP_LOGW(TAG, "backlight controller not found on I2C — panel may stay dark");
    }

    /* ── GT911 capacitive touch (polling; INT/RST not routed on this board) ─ */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    err = esp_lcd_new_panel_io_i2c(display_i2c_bus(), &tp_io_cfg, &tp_io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "gt911 io: %s", esp_err_to_name(err)); return err; }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GT911_RST_GPIO,
        .int_gpio_num = GT911_INT_GPIO,
        .flags = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    };
    err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_tp);
    if (err != ESP_OK) { ESP_LOGE(TAG, "gt911: %s", esp_err_to_name(err)); return err; }

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_read_cb;
    s_tp_indev = lv_indev_drv_register(&s_indev_drv);

    /* ── LVGL tick timer (5 ms) ───────────────────────────────────────────── */
    esp_timer_handle_t tick_timer;
    esp_timer_create_args_t tick_args = { .callback = lv_tick_cb, .name = "lv_tick" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG, "tick timer create");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, 5000), TAG, "tick timer start");

    ESP_LOGI(TAG, "MIPI-DSI %dx%d + GT911 touch ready", LCD_H_RES, LCD_V_RES);
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

/* Count paired Thread sensors that are enabled AND currently reachable.
 * Replaces the S3 shim's espnow_get_active_sensor_count(); the real hub reads
 * the NVS sensor table + the liveness watchdog directly. */
static int active_sensor_count(void)
{
    uint8_t eui64s[10][8];
    char    names[10][32];
    char    zones[10][32];
    bool    enabled[10];
    int count = nvs_load_thread_sensors(eui64s, names, zones, enabled, 10);
    if (count < 0) count = 0;
    int active = 0;
    for (int i = 0; i < count; i++) {
        if (enabled[i] && !nrf_thread_is_sensor_offline(eui64s[i])) active++;
    }
    return active;
}

static void set_hub_connection_status_locked(bool online)
{
    if (s_current_screen != SCREEN_ID_HUB_ONLINE) {
        load_screen_locked(SCREEN_ID_HUB_ONLINE);
    }
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

        int n = active_sensor_count();
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

/* Soft tinted surface behind the AQI status pill for a given accent color. */
static uint32_t aqi_state_surface(uint32_t color)
{
    if (color == C_GREEN_U32 || color == UI_D_GREEN) return UI_D_GREEN_SURF;
    if (color == C_RED_U32   || color == UI_D_RED)   return UI_D_RED_SURF;
    return UI_D_AMBER_SURF;
}

static void set_aqi_value_locked(float aqi, const char *state, uint16_t pm25)
{
    if (aqi < 0.0f) aqi = 0.0f;
    if (aqi > 500.0f) aqi = 500.0f;

    char value[16];
    snprintf(value, sizeof(value), "%.0f", aqi);
    set_label_locked(objects.aqi_val, value);
    if (objects.aqi_arc) lv_arc_set_value(objects.aqi_arc, (int)aqi);   /* arc range 0-300 clamps */
    set_label_locked(objects.aqi_state, aqi_display_state(state));

    uint32_t color = aqi_state_color(state);
    uint32_t surface = aqi_state_surface(color);
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
    if (objects.aqi_mood) {
        lv_obj_set_style_bg_color(objects.aqi_mood, lv_color_hex(surface),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (objects.aqi_pm25) {
        char pm[24];
        snprintf(pm, sizeof(pm), "%u \xC2\xB5g/m\xC2\xB3", (unsigned)pm25);
        set_label_locked(objects.aqi_pm25, pm);
    }
}

/* Recolor a light status pill (surface fill + text + leading dot). */
static void set_light_pill_locked(lv_obj_t *pill, lv_obj_t *label, lv_obj_t *dot,
                                  const char *text, uint32_t color, uint32_t surface)
{
    if (label) {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (dot)
        lv_obj_set_style_bg_color(dot, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    if (pill)
        lv_obj_set_style_bg_color(pill, lv_color_hex(surface), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void update_temp_pill_locked(float temp)
{
    if (!objects.obj7 || !objects.temp_mood) return;
    const char *text;
    uint32_t color, surface;
    if (temp > TEMP_THRESH_HOT) {
        text = "Hot"; color = UI_D_RED; surface = UI_D_RED_SURF;
    } else if (temp > TEMP_THRESH_WARM) {
        text = "Warm"; color = UI_D_AMBER_TEXT; surface = UI_D_AMBER_SURF;
    } else {
        text = "Comfortable"; color = UI_D_GREEN; surface = UI_D_GREEN_SURF;
    }
    set_light_pill_locked(objects.temp_mood, objects.obj7, objects.temp_img,
                          text, color, surface);
    /* Marker pill (objects.temp_arc) tracks the same accent. */
    if (objects.temp_arc) {
        lv_obj_set_style_bg_color(objects.temp_arc, lv_color_hex(surface),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (objects.obj4)
        lv_obj_set_style_text_color(objects.obj4, lv_color_hex(color),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void update_hum_pill_locked(float hum)
{
    if (!objects.obj12 || !objects.hum_mood) return;
    const char *text;
    uint32_t color, surface;
    if (hum >= 80.0f) {
        text = "Critical"; color = UI_D_RED; surface = UI_D_RED_SURF;
    } else if (hum > HUM_THRESH_HIGH) {
        text = "Humid"; color = UI_D_BLUE; surface = UI_D_BLUE_SURF;
    } else if (hum < 30.0f) {
        text = "Dry"; color = UI_D_AMBER_TEXT; surface = UI_D_AMBER_SURF;
    } else {
        text = "Comfortable"; color = UI_D_GREEN; surface = UI_D_GREEN_SURF;
    }
    set_light_pill_locked(objects.hum_mood, objects.obj12, objects.hum_img,
                          text, color, surface);
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

/* Position the thermometer marker pill (objects.temp_arc) from a temperature. */
static void position_temp_marker_locked(float temp)
{
    if (!objects.temp_arc) return;
    float frac = (temp - (float)DASH_TEMP_MIN) /
                 (float)(DASH_TEMP_MAX - DASH_TEMP_MIN);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    int y = DASH_THERMO_YBOT - (int)(frac * (DASH_THERMO_YBOT - DASH_THERMO_YTOP));
    lv_obj_set_y(objects.temp_arc, y);
    if (objects.obj4) {
        char b[12];
        snprintf(b, sizeof(b), "%.0f\xC2\xB0", temp);
        lv_label_set_text(objects.obj4, b);
    }
}

/* Refresh the humidity wave chart + its floating marker bubble. */
static void update_hum_chart_locked(float hum)
{
    if (!objects.hum_bar) return;
    lv_chart_series_t *ser = lv_chart_get_series_next(objects.hum_bar, NULL);
    if (!ser) return;

    float series[DASH_HUM_POINTS];
    size_t k = metrics_history_series(METRIC_HUM, series, DASH_HUM_POINTS);
    for (int i = 0; i < DASH_HUM_POINTS; i++) {
        int v = (k > 0) ? (int)series[i] : (int)hum;
        lv_chart_set_value_by_id(objects.hum_bar, ser, i, v);
    }
    lv_chart_refresh(objects.hum_bar);

    /* Marker bubble at the right edge, height mapped from the current value. */
    if (objects.hum_marker) {
        const int chart_x = 24, chart_y = 252, chart_w = 282, chart_h = 96;
        float f = hum / 100.0f;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        int my = chart_y + (int)((1.0f - f) * chart_h) - 15;
        if (my < chart_y - 6) my = chart_y - 6;
        if (my > chart_y + chart_h - 24) my = chart_y + chart_h - 24;
        lv_obj_set_pos(objects.hum_marker, chart_x + chart_w - 56, my);
        lv_obj_t *lbl = lv_obj_get_child(objects.hum_marker, 0);
        if (lbl) {
            char b[8];
            snprintf(b, sizeof(b), "%.0f%%", hum);
            lv_label_set_text(lbl, b);
        }
        lv_obj_clear_flag(objects.hum_marker, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Update the MIN/AVG/MAX-24H stat cells for one metric. */
static void set_stat_cells_locked(metric_kind_t kind, lv_obj_t *mn, lv_obj_t *av,
                                  lv_obj_t *mx, const char *unit)
{
    float lo, avg, hi;
    if (!metrics_history_stats(kind, &lo, &avg, &hi)) return;
    char b[16];
    if (mn) { snprintf(b, sizeof(b), "%.0f", lo); set_label_locked(mn, b); }
    if (av) { snprintf(b, sizeof(b), "%.0f", avg); set_label_locked(av, b); }
    if (mx) { snprintf(b, sizeof(b), "%.0f", hi); set_label_locked(mx, b); }
}

/* Keep the fingerprint text centered/wrapped for the light scan layout. The
 * label positions themselves are set at create time (create_screen_fingerprint_
 * setting); here we only re-assert alignment/width in case a long status string
 * was pushed while the screen was cached. */
static void align_fingerprint_text_locked(void)
{
    if (objects.obj49) {
        lv_obj_set_width(objects.obj49, 720 - 48 - 72);
        lv_obj_set_style_text_align(objects.obj49, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    if (objects.fingerprint_instruction) {
        lv_obj_set_width(objects.fingerprint_instruction, 720 - 48 - 72);
        lv_obj_set_style_text_align(objects.fingerprint_instruction, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    if (objects.obj52) {
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

    snprintf(buf, sizeof(buf), "%.0f", temp);
    set_label_locked(objects.temp_val, buf);
    position_temp_marker_locked(temp);

    snprintf(buf, sizeof(buf), "%.0f", hum);
    set_label_locked(objects.hum_val, buf);
    update_hum_chart_locked(hum);

    update_temp_pill_locked(temp);
    update_hum_pill_locked(hum);

    set_stat_cells_locked(METRIC_TEMP, objects.temp_stat_min, objects.temp_stat_avg,
                          objects.temp_stat_max, "°C");
    set_stat_cells_locked(METRIC_HUM, objects.hum_stat_min, objects.hum_stat_avg,
                          objects.hum_stat_max, "%");
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
        set_aqi_value_locked(cache.aqi, cache.aqi_state, cache.pm25);
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

    load_screen_locked(target);
}

static void settings_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: Settings");
    s_prev_screen = SCREEN_ID_HUB_ONLINE;
    load_screen_locked(SCREEN_ID_SETTINGS_MENU);
}

static void sensor_nodes_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: Sensor Nodes");
    load_screen_locked(SCREEN_ID_SENSOR_NODES_SETTING);
    refresh_sensor_nodes_locked();
}

static void about_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: About");
    load_screen_locked(SCREEN_ID_ABOUT_GLAZIA);
}

static void add_fingerprint_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: Add Fingerprint");
    show_fingerprint_screen_locked("Authentication", "Place your finger on the sensor");
    start_auth_action(AUTH_ACTION_ADD_FINGERPRINT, NULL, false, NULL);
}

static void add_sensor_auth_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Touch: Add Sensor");
    show_fingerprint_screen_locked("Authentication", "Place your finger on the sensor");
    start_auth_action(AUTH_ACTION_ADD_SENSOR, NULL, false, NULL);
}

static void add_sensor_start_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_sensor_added_view) {
        ESP_LOGI(TAG, "Touch: Sensor Added Done");
        s_sensor_added_view = false;
        display_clear_sensor_notifications();
        load_screen_locked(SCREEN_ID_SENSOR_NODES_SETTING);
        refresh_sensor_nodes_locked();
        return;
    }
    ESP_LOGI(TAG, "Touch: Start Sensor Pairing");
    sensor_pairing_open_window();
}

#if CONFIG_ESPNOW_ENABLE
static void sensor_switch_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_switch_internal) return;
    lv_obj_t *sw = lv_event_get_target(e);
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Touch: Sensor %d %s", index, enabled ? "enabled" : "disabled");
    espnow_set_sensor_enabled(index, enabled);
}
#endif

#if !CONFIG_ESPNOW_ENABLE
/* Thread sensor rows are keyed by EUI64, but LVGL callbacks carry only an int index. This mirror of
 * the on-screen list (rebuilt on every refresh) lets the toggle/delete callbacks resolve an index
 * back to its EUI64. */
static uint8_t s_thread_row_eui64[10][8];
static int     s_thread_row_count;

static void thread_switch_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_switch_internal) return;
    lv_obj_t *sw = lv_event_get_target(e);
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= s_thread_row_count) return;
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Touch: Thread sensor %d %s", index, enabled ? "enabled" : "disabled");
    nrf_thread_set_sensor_enabled(s_thread_row_eui64[index], enabled);
}

static void create_thread_sensor_row(lv_obj_t *parent, int index, const char *name, bool enabled)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_pos(row, 24, 12 + index * 150);
    lv_obj_set_size(row, 672, 132);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_D_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(row, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, 235, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sensor_icon = lv_img_create(row);
    lv_obj_set_pos(sensor_icon, 24, 21);
    lv_img_set_src(sensor_icon, &img_sensor);
    lv_img_set_zoom(sensor_icon, 450);
    lv_obj_set_style_img_recolor(sensor_icon, lv_color_hex(UI_D_GREEN),
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_img_recolor_opa(sensor_icon, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(sensor_icon, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, &lv_font_inter_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(label,
                                lv_color_hex(enabled ? UI_D_HEADING : UI_D_MUTED),
                                LV_PART_MAIN);
    lv_obj_set_pos(label, 138, 48);
    lv_obj_set_size(label, 234, 42);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_pos(sw, 384, 30);
    lv_obj_set_size(sw, 132, 72);
    ui_style_toggle(sw);
    make_touch_target(sw);
    set_switch_checked_locked(sw, enabled);
    lv_obj_add_event_cb(sw, thread_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
    /* No delete button on the TFT — Thread sensors are removed via the app only. */

    /* Offline badge: the liveness watchdog declared this sensor dead (still paired, just
     * unreachable). Recolor red and add an "offline" tag; auto-clears on reconnect. */
    if (nrf_thread_is_sensor_offline(s_thread_row_eui64[index])) {
        lv_obj_set_style_img_recolor(sensor_icon, lv_color_hex(C_RED_U32),
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(label, lv_color_hex(C_RED_U32), LV_PART_MAIN);
        lv_obj_set_pos(label, 138, 24);   /* nudge name up to make room for the tag */
        lv_obj_t *off = lv_label_create(row);
        lv_label_set_text(off, "offline");
        lv_obj_set_style_text_font(off, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_set_style_text_color(off, lv_color_hex(C_RED_U32), LV_PART_MAIN);
        lv_obj_set_pos(off, 138, 78);
    }
}
#endif  /* !CONFIG_ESPNOW_ENABLE */

#if CONFIG_ESPNOW_ENABLE
static void create_sensor_row(lv_obj_t *parent, int index, const char *name, bool enabled, bool paired)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_pos(row, 24, 12 + index * 150);
    lv_obj_set_size(row, 672, 132);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(row, 190, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, 230, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sensor_icon = lv_img_create(row);
    lv_obj_set_pos(sensor_icon, 30, 21);
    lv_img_set_src(sensor_icon, &img_sensor);
    lv_img_set_zoom(sensor_icon, 450);
    lv_obj_set_style_img_recolor(sensor_icon, lv_color_hex(UI_COLOR_AMBER),
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_img_recolor_opa(sensor_icon, 255,
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(sensor_icon, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, LV_PART_MAIN);
    uint32_t label_color = !enabled ? UI_COLOR_TEXT_DIM
                         : paired   ? UI_COLOR_TEXT_PRIMARY
                                    : C_AMBER_U32;
    lv_obj_set_style_text_color(label, lv_color_hex(label_color), LV_PART_MAIN);
    lv_obj_set_pos(label, 156, 48);
    lv_obj_set_size(label, 300, 42);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_pos(sw, 498, 30);
    lv_obj_set_size(sw, 132, 72);
    ui_style_toggle(sw);
    make_touch_target(sw);
    set_switch_checked_locked(sw, enabled);
    lv_obj_add_event_cb(sw, sensor_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
}
#endif  /* CONFIG_ESPNOW_ENABLE */

/* BLE bring-up can block for seconds on the ESP-Hosted RPC path; run it in its
 * own task so the LVGL event thread returns immediately and the QR screen paints
 * without waiting on the radio (a slow/failed C6 link must never freeze the UI). */
static void ble_start_task(void *arg)
{
    ble_start();
    vTaskDelete(NULL);
}

static void reg_welcome_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI("UI", "Get Started button clicked! Loading QR screen...");
    load_screen_locked(SCREEN_ID_HUB_REGISTER_QR);
    /* Start BLE provisioning — identical to physical button press in MODE_IDLE.
     * Spawn off the LVGL thread so the QR screen renders without blocking. */
    g_mode = MODE_HUB_PAIRING;
    if (xTaskCreate(ble_start_task, "ble_start", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE("UI", "Failed to create ble_start task; starting BLE inline");
        ble_start();
    }
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
        uint16_t pm25 = s_display_cache.pm25;
        char aqi_state[sizeof(s_display_cache.aqi_state)];
        cache_copy(aqi_state, sizeof(aqi_state), s_display_cache.aqi_state);
        cache_unlock();
        set_aqi_value_locked(has_aqi ? aqi : 0.0f, has_aqi ? aqi_state : "good", has_aqi ? pm25 : 0);
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
        if (objects.add_sensor_button)
            lv_obj_set_ext_click_area(objects.add_sensor_button, 0);
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

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = dsi_hw_init();
    if (err != ESP_OK) {
        s_display_state = DISPLAY_FAILED;
        ESP_LOGE(TAG, "dsi_hw_init: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* LVGL task is NOT running yet — build UI single-threaded (safe, no lock). */
    ESP_LOGI(TAG, "ui_init starting");
    ui_init();
    ESP_LOGI(TAG, "ui_init done");
    enum ScreensEnum boot_screen = (g_hub_secret[0] != '\0')
        ? SCREEN_ID_HUB_ONLINE : SCREEN_ID_HUB_REGISTER_WELCOME;
    configure_screen_locked(boot_screen);
    cache_apply_locked();
    s_display_state = DISPLAY_READY;

    /* Start LVGL task AFTER UI is fully built — eliminates init/render race. */
    if (xTaskCreatePinnedToCore(lvgl_task, "lvgl", 32768, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "lvgl task create failed");
        s_display_state = DISPLAY_FAILED;
        vTaskDelete(NULL);
        return;
    }

    fp_set_display_cb(display_fingerprint_status);
    ESP_LOGI(TAG, "Display ready — native MIPI-DSI %dx%d, GT911 touch", LCD_H_RES, LCD_V_RES);
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

#if CONFIG_ESPNOW_ENABLE
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
            create_sensor_row(objects.settings_menu_cont_1, i, name, enabled, paired);
        }
    }
#else
    /* Thread sensors: load the table, cache EUI64s so the row callbacks can resolve their index. */
    uint8_t eui64s[10][8];
    char    names[10][32];
    char    zones[10][32];
    bool    enabled[10];
    int count = nvs_load_thread_sensors(eui64s, names, zones, enabled, 10);
    s_thread_row_count = count;

    if (count == 0) {
        lv_obj_t *label = lv_label_create(objects.settings_menu_cont_1);
        lv_label_set_text(label, "No sensors paired yet");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(C_T2_U32), LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        memcpy(s_thread_row_eui64[i], eui64s[i], 8);
        create_thread_sensor_row(objects.settings_menu_cont_1, i, names[i], enabled[i]);
    }
#endif
}

/* Called by the liveness watchdog when a sensor is declared offline or comes back. The offline
 * state itself lives in the watchdog (nrf_thread_is_sensor_offline); here we just re-render the
 * sensor list if it happens to be on screen so the badge appears/clears live. */
void display_set_thread_sensor_offline(const uint8_t eui64[8], bool offline)
{
    (void)eui64;
    (void)offline;
    if (!display_is_ready()) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(300)) != pdTRUE) return;
    if (s_current_screen == SCREEN_ID_SENSOR_NODES_SETTING) {
        refresh_sensor_nodes_locked();
    }
    xSemaphoreGive(s_lvgl_mux);
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

    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_current_screen == SCREEN_ID_HUB_ONLINE) {
            set_label_locked(objects.sensor_info, line2 ? line2 : "");
        }
        xSemaphoreGive(s_lvgl_mux);
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

    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(500)) != pdTRUE) return;
    ESP_LOGI(TAG, "Display loading setup registration welcome");
    load_screen_locked(SCREEN_ID_HUB_REGISTER_WELCOME);
    xSemaphoreGive(s_lvgl_mux);
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

    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        set_label_locked(objects.obj52, fingerprint_message_normalize(message));
        align_fingerprint_text_locked();
        xSemaphoreGive(s_lvgl_mux);
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

    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
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
        xSemaphoreGive(s_lvgl_mux);
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

    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_current_screen == SCREEN_ID_HUB_ONLINE) {
            set_welcome_text_locked(user_name);
        }
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_update_aqi(float aqi, const char *state, uint16_t pm25)
{
    cache_lock();
    s_display_cache.has_aqi = true;
    s_display_cache.aqi = aqi;
    s_display_cache.pm25 = pm25;
    cache_copy(s_display_cache.aqi_state, sizeof(s_display_cache.aqi_state), state);
    cache_unlock();

    if (!display_is_ready()) return;
    if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY) != pdTRUE) return;
    set_aqi_value_locked(aqi, state, pm25);
    xSemaphoreGive(s_lvgl_mux);
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
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(500)) != pdTRUE) return;
    int n = active_sensor_count();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d sensor node%s active", n, n == 1 ? "" : "s");
    set_label_locked(objects.sensor_info, buf);
    xSemaphoreGive(s_lvgl_mux);
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

    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(500)) != pdTRUE) return;
    set_hub_connection_status_locked(online);
    xSemaphoreGive(s_lvgl_mux);
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

    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(500)) != pdTRUE) return;
    show_fingerprint_screen_locked(title, prompt);
    xSemaphoreGive(s_lvgl_mux);
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

    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        set_label_locked(objects.obj49, phase);
        set_label_locked(objects.fingerprint_instruction, fingerprint_instruction_for_phase(phase));
        set_label_locked(objects.obj52, normalized);
        align_fingerprint_text_locked();
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_fingerprint_progress(uint8_t percent)
{
    if (percent > 100) percent = 100;
    cache_lock();
    s_display_cache.fp_progress = percent;
    cache_unlock();

    if (!display_is_ready()) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (objects.obj51) lv_bar_set_value(objects.obj51, percent, LV_ANIM_OFF);
    xSemaphoreGive(s_lvgl_mux);
}

void display_update_temp_hum(float temp, float hum)
{
    cache_lock();
    s_display_cache.has_temp_hum = true;
    s_display_cache.temp = temp;
    s_display_cache.hum = hum;
    cache_unlock();

    metrics_history_push(temp, hum);

    if (!display_is_ready()) return;
    if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY) != pdTRUE) return;
    set_dashboard_values_locked(temp, hum);
    xSemaphoreGive(s_lvgl_mux);
}

void display_refresh_sensor_nodes(void)
{
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, skipped sensor-node refresh (state=%d)", (int)s_display_state);
        return;
    }
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(500)) != pdTRUE) return;
    refresh_sensor_nodes_locked();
    xSemaphoreGive(s_lvgl_mux);
}

void display_sensor_added_notification(const char *name)
{
    if (!name) return;
    ESP_LOGI(TAG, "Display sensor added notification: %s", name);
    if (!display_is_ready()) {
        ESP_LOGI(TAG, "Display not ready, skipped sensor-added notification (state=%d)", (int)s_display_state);
        return;
    }
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(300)) != pdTRUE) return;
    ui_ensure_screen(SCREEN_ID_ADD_ANOTHER__SENSOR);
    configure_screen_locked(SCREEN_ID_ADD_ANOTHER__SENSOR);
    if (!objects.added_sensor_data || !objects.obj58 || !objects.obj59 || !objects.obj60) {
        xSemaphoreGive(s_lvgl_mux);
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
    xSemaphoreGive(s_lvgl_mux);
}

void display_clear_sensor_notifications(void)
{
    if (!display_is_ready()) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(300)) != pdTRUE) return;
    s_sensor_added_view = false;
    if (objects.added_sensor_data) lv_obj_add_flag(objects.added_sensor_data, LV_OBJ_FLAG_HIDDEN);
    if (objects.obj60) {
        lv_label_set_text(objects.obj60, "+ Add Sensor");
        lv_obj_set_style_text_color(objects.obj60, lv_color_hex(UI_COLOR_PRIMARY_TEXT), LV_PART_MAIN);
    }
    if (objects.obj59) {
        lv_obj_set_style_bg_color(objects.obj59, lv_color_hex(UI_COLOR_PRIMARY_BUTTON), LV_PART_MAIN);
    }
    xSemaphoreGive(s_lvgl_mux);
}

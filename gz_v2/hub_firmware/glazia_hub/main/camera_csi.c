#include "camera_csi.h"
#include "state.h"
#include "display_power.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "esp_ldo_regulator.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"

static const char *TAG = "CAM_CSI";

/* Pipeline: OV5647 RAW8 → CSI → ISP → YUV420(O_UYY_E_VYY) in PSRAM.
 * The P4 rev-1 hardware H.264 encoder accepts ONLY the O_UYY_E_VYY layout
 * (odd lines U Y Y…, even lines V Y Y…) — this is exactly the P4 ISP's YUV420
 * output mode (isp_out_type=3), the co-designed camera→encoder feed. No CPU
 * colour conversion is needed — see webrtc_stream.c.
 * (UYVY / VUY are only accepted by rev-3 (>=300) silicon; not this chip.) */

/* LDO channel + I2C bus are owned by display_power.c (shared with the DSI panel).
 * The camera borrows them and never acquires/releases/deletes them. */
static i2c_master_bus_handle_t  s_i2c_bus;   /* borrowed from display_i2c_bus() */
static esp_sccb_io_handle_t     s_sccb;
static esp_cam_sensor_device_t *s_cam;
static esp_cam_ctlr_handle_t    s_ctlr;
static isp_proc_handle_t        s_isp;

static uint8_t *s_frame;                 /* single YUV422 frame buffer (PSRAM, cache-aligned) */
static size_t   s_frame_len;             /* w * h * 2 */
static esp_cam_ctlr_trans_t s_trans;     /* buffer handed to the CSI driver each receive */
static SemaphoreHandle_t s_frame_ready;  /* given by the CSI ISR when a frame lands in s_frame */
static int s_width, s_height;

int camera_csi_width(void)  { return s_width; }
int camera_csi_height(void) { return s_height; }

/* CSI driver asks for a buffer to fill — always hand back our single frame buffer
 * (drop-latest: if the encoder is still busy we simply weren't in receive()). */
static bool IRAM_ATTR on_get_new_trans(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *t, void *user)
{
    esp_cam_ctlr_trans_t *cfg = (esp_cam_ctlr_trans_t *)user;
    t->buffer = cfg->buffer;
    t->buflen = cfg->buflen;
    return false;
}

/* CSI ISR: a frame has landed in s_frame (the driver already M2C-invalidated it). Wake the
 * video task waiting in camera_csi_get_frame(). Binary semaphore = drop-latest, no backlog. */
static bool IRAM_ATTR on_trans_finished(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *t, void *user)
{
    BaseType_t woken = pdFALSE;
    if (s_frame_ready) xSemaphoreGiveFromISR(s_frame_ready, &woken);
    return woken == pdTRUE;
}

/* Pick the OV5647 CSI mode with the smallest frame area (nearest the VGA target;
 * OV5647's smallest native CSI mode is 800x640, there is no true 640x480). */
static const esp_cam_sensor_format_t *select_smallest_format(esp_cam_sensor_format_array_t *arr)
{
    const esp_cam_sensor_format_t *best = NULL;
    uint32_t best_area = UINT32_MAX;
    for (int i = 0; i < arr->count; i++) {
        const esp_cam_sensor_format_t *f = &arr->format_array[i];
        ESP_LOGI(TAG, "sensor fmt[%d]: %s (%dx%d)", i, f->name, f->width, f->height);
        uint32_t area = (uint32_t)f->width * (uint32_t)f->height;
        if (area && area < best_area) { best_area = area; best = f; }
    }
    return best;
}

static esp_err_t sensor_bringup(void)
{
    /* Shared I2C_NUM_0 bus, created + owned by the always-on display module. */
    s_i2c_bus = display_i2c_bus();
    if (!s_i2c_bus) { ESP_LOGE(TAG, "shared I2C bus unavailable"); return ESP_FAIL; }

    esp_cam_sensor_config_t cam_cfg = {
        .sccb_handle = NULL,
        .reset_pin   = CAM_RESET_GPIO,
        .pwdn_pin    = CAM_PWDN_GPIO,
        .xclk_pin    = CAM_XCLK_GPIO,
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };

    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        if (p->port != ESP_CAM_SENSOR_MIPI_CSI) continue;
        sccb_i2c_config_t sccb_cfg = {
            .scl_speed_hz    = CAM_SCCB_FREQ_HZ,
            .device_address  = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        if (sccb_new_i2c_io(s_i2c_bus, &sccb_cfg, &cam_cfg.sccb_handle) != ESP_OK) continue;
        s_cam = (*(p->detect))(&cam_cfg);
        if (s_cam) { s_sccb = cam_cfg.sccb_handle; break; }
        esp_sccb_del_i2c_io(cam_cfg.sccb_handle);
    }
    if (!s_cam) { ESP_LOGE(TAG, "no MIPI-CSI camera sensor detected"); return ESP_ERR_NOT_FOUND; }

    esp_cam_sensor_format_array_t fmts = {0};
    ESP_LOGI(TAG, "bringup: query_format...");
    esp_cam_sensor_query_format(s_cam, &fmts);
    ESP_LOGI(TAG, "bringup: query_format done (%d fmts)", (int)fmts.count);
    const esp_cam_sensor_format_t *fmt = select_smallest_format(&fmts);
    if (!fmt) { ESP_LOGE(TAG, "sensor exposes no usable format"); return ESP_ERR_NOT_SUPPORTED; }

    ESP_LOGI(TAG, "bringup: set_format '%s'...", fmt->name);
    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(s_cam, fmt), TAG, "set_format");
    s_width  = fmt->width;
    s_height = fmt->height;
    ESP_LOGI(TAG, "using sensor format '%s' %dx%d", fmt->name, s_width, s_height);

    int on = 1;
    ESP_LOGI(TAG, "bringup: S_STREAM on...");
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(s_cam, ESP_CAM_SENSOR_IOC_S_STREAM, &on), TAG, "S_STREAM");
    ESP_LOGI(TAG, "bringup: S_STREAM on done");
    return ESP_OK;
}

esp_err_t camera_csi_start(void)
{
    esp_err_t ret;

    /* MIPI D-PHY power — shared with the DSI panel, owned by display_power.c.
     * Idempotent; the display already acquired it at boot, this is defensive. */
    ESP_RETURN_ON_ERROR(display_power_acquire_mipi_phy(), TAG, "mipi phy ldo");

    ret = sensor_bringup();
    if (ret != ESP_OK) goto fail;

    /* CSI controller: RAW8 in from the sensor, YUV422 out to memory. */
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id                = 0,
        .h_res                  = s_width,
        .v_res                  = s_height,
        .lane_bit_rate_mbps     = CAM_CSI_LANE_BITRATE_MBPS,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_YUV420,
        .data_lane_num          = CAM_CSI_LANE_NUM,
        .byte_swap_en           = false,
        .queue_items            = 1,
    };
    ESP_LOGI(TAG, "start: new_csi_ctlr...");
    ret = esp_cam_new_csi_ctlr(&csi_cfg, &s_ctlr);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "csi ctlr: %s", esp_err_to_name(ret)); goto fail; }
    ESP_LOGI(TAG, "start: new_csi_ctlr done");

    int w = (s_width + 15) & ~15;
    int h = (s_height + 15) & ~15;
    s_frame_len = (size_t)w * (size_t)h * 3 / 2;         /* YUV420 = 1.5 B/px (padded for H264) */
    size_t aligned = (s_frame_len + 127) & ~(size_t)127;             /* HW H.264 DMA alignment */
    s_frame = heap_caps_aligned_calloc(128, 1, aligned, MALLOC_CAP_SPIRAM);
    if (!s_frame) { ESP_LOGE(TAG, "frame buffer alloc failed (%u B)", (unsigned)aligned); ret = ESP_ERR_NO_MEM; goto fail; }
    s_trans.buffer = s_frame;
    s_trans.buflen = s_frame_len;

    s_frame_ready = xSemaphoreCreateBinary();
    if (!s_frame_ready) { ESP_LOGE(TAG, "frame-ready sem alloc failed"); ret = ESP_ERR_NO_MEM; goto fail; }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans  = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    ret = esp_cam_ctlr_register_event_callbacks(s_ctlr, &cbs, &s_trans);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "cbs register: %s", esp_err_to_name(ret)); goto fail; }
    ret = esp_cam_ctlr_enable(s_ctlr);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "ctlr enable: %s", esp_err_to_name(ret)); goto fail; }

    /* ISP: demosaic RAW8 → YUV422. */
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz                 = 80 * 1000 * 1000,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_YUV420,
        .has_line_start_packet  = false,
        .has_line_end_packet    = false,
        .h_res                  = s_width,
        .v_res                  = s_height,
    };
    ESP_LOGI(TAG, "start: isp new_processor...");
    ret = esp_isp_new_processor(&isp_cfg, &s_isp);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "isp new: %s", esp_err_to_name(ret)); goto fail; }
    ESP_LOGI(TAG, "start: isp enable...");
    ret = esp_isp_enable(s_isp);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "isp enable: %s", esp_err_to_name(ret)); goto fail; }

    ESP_LOGI(TAG, "start: ctlr_start...");
    ret = esp_cam_ctlr_start(s_ctlr);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "ctlr start: %s", esp_err_to_name(ret)); goto fail; }
    ESP_LOGI(TAG, "start: ctlr_start done");

    ESP_LOGI(TAG, "camera streaming %dx%d YUV420/O_UYY_E_VYY (%u B/frame)",
             s_width, s_height, (unsigned)s_frame_len);
    return ESP_OK;

fail:
    camera_csi_stop();
    return ret;
}

esp_err_t camera_csi_get_frame(uint8_t **out_buf, size_t *out_len)
{
    if (!s_ctlr || !s_frame || !s_frame_ready) return ESP_ERR_INVALID_STATE;

    /* Wait for the CSI ISR (on_trans_finished) to report a DMA-completed frame in s_frame.
     * The driver runs free-running via on_get_new_trans — esp_cam_ctlr_receive() is NOT used
     * (registering on_get_new_trans makes the ISR ignore the receive queue, so calling it just
     * jams a size-1 queue → "transaction queue is full"). Bounded (200ms > the ~66ms 15fps
     * interval): a stalled camera or teardown lets the video task re-check its run flag and
     * exit cleanly, freeing the H.264 encoder rather than leaking its interrupt. */
    if (xSemaphoreTake(s_frame_ready, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_ERR_TIMEOUT;

    /* No manual esp_cache_msync here: the CSI driver already M2C-invalidates the completed
     * buffer (esp_cam_ctlr_csi.c) right before firing on_trans_finished. */
    *out_buf = s_frame;
    *out_len = s_frame_len;
    return ESP_OK;
}

void camera_csi_stop(void)
{
    if (s_ctlr) {
        esp_cam_ctlr_stop(s_ctlr);
        esp_cam_ctlr_disable(s_ctlr);
    }
    if (s_isp) { esp_isp_disable(s_isp); esp_isp_del_processor(s_isp); s_isp = NULL; }
    if (s_ctlr) { esp_cam_ctlr_del(s_ctlr); s_ctlr = NULL; }
    if (s_cam)  { esp_cam_sensor_del_dev(s_cam); s_cam = NULL; }
    if (s_sccb) { esp_sccb_del_i2c_io(s_sccb); s_sccb = NULL; }
    /* s_i2c_bus is borrowed from display_power.c — do NOT delete it here.
     * The MIPI D-PHY LDO is likewise owned by the display and stays acquired. */
    s_i2c_bus = NULL;
    if (s_frame) { heap_caps_free(s_frame); s_frame = NULL; }
    /* Safe to delete now: the controller is stopped (ISR can no longer give the sem) and the
     * video task has already drained in webrtc_stream_stop() before reaching here. */
    if (s_frame_ready) { vSemaphoreDelete(s_frame_ready); s_frame_ready = NULL; }
    s_frame_len = 0;
    s_width = s_height = 0;
    s_trans.buffer = NULL;
    s_trans.buflen = 0;
}

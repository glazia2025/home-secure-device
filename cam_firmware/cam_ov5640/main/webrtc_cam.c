#include "webrtc_cam.h"
#include "esp_log_level.h"
#include "uart_bridge.h"
#include "camera_core.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_h264_enc_single_sw.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "WEBRTC_CAM";

/* ── Constants ─────────────────────────────────────────────────────────────── */
#define VIDEO_WIDTH    320
#define VIDEO_HEIGHT   240
#define VIDEO_FPS      10
#define VIDEO_BITRATE  512000

/* H264 output buffer: worst-case raw frame size (2 bytes/px for YUV422) */
#define H264_OUT_BUF_SIZE  (VIDEO_WIDTH * VIDEO_HEIGHT * 2)

#define WIFI_CONNECT_BIT   BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   5

/* ── Module state ────────────────────────────────────────────────────────────
 * All task stacks in PSRAM to avoid internal SRAM pressure during TLS/WebRTC. */
static StaticTask_t     s_loop_tcb;
static StackType_t     *s_loop_stack;
static StaticTask_t     s_video_tcb;
static StackType_t     *s_video_stack;
static TaskHandle_t     s_loop_task  = NULL;
static TaskHandle_t     s_video_task = NULL;
static SemaphoreHandle_t s_cert_ready = NULL;

static esp_peer_handle_t  s_peer    = NULL;
static volatile bool      s_running = false;
static volatile bool      s_connected = false;
static volatile bool      s_stopping = false;
static volatile bool      s_stop_scheduled = false;

/* ICE configuration must outlive cam_webrtc_task. */
static char s_turn_user[32];
static char s_turn_psw[32];
static esp_peer_ice_server_cfg_t s_ice_servers[3];

static EventGroupHandle_t s_wifi_event_group = NULL;
static int                s_wifi_retry_count = 0;
static bool               s_wifi_started     = false;

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "%s: internal_free=%u largest=%u psram_free=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void wifi_stop(void)
{
    if (!s_wifi_started) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
}

/* ── WiFi event handler ──────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_wifi_retry_count++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECT_BIT);
    }
}

static bool wifi_connect(const char *ssid, const char *pass)
{
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }

    esp_event_handler_instance_t h_wifi, h_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, &h_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, &h_ip);

    if (!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_netif_create_default_wifi_sta();   /* creates STA netif so DHCP works */
        s_wifi_started = true;
    }

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,     ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    s_wifi_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECT_BIT | WIFI_FAIL_BIT);
    esp_err_t wifi_err = esp_wifi_start();
    if (wifi_err == ESP_ERR_INVALID_STATE) {
        wifi_err = esp_wifi_connect();
    }
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start/connect failed: %s", esp_err_to_name(wifi_err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECT_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, h_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, h_ip);

    if (bits & WIFI_CONNECT_BIT) {
        ESP_LOGI(TAG, "WiFi connected to '%s'", ssid);
        return true;
    }
    ESP_LOGE(TAG, "WiFi connection to '%s' failed", ssid);
    wifi_stop();
    return false;
}

/* ── esp_peer callbacks ──────────────────────────────────────────────────── */

static int on_msg_cb(esp_peer_msg_t *msg, void *ctx)
{
    if (!msg || !msg->data || msg->size <= 0) return 0;
    const char *data = (const char *)msg->data;

    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        /* esp_peer generated our local SDP offer — forward to hub.
         * Must use cJSON (not snprintf) so raw SDP CRLF bytes are escaped
         * to \r\n in the JSON string; literal control chars are invalid JSON. */
        char *sdp = malloc(msg->size + 1);
        if (!sdp) { ESP_LOGE(TAG, "OOM building offer JSON"); return 0; }
        memcpy(sdp, data, msg->size);
        sdp[msg->size] = '\0';

        /* Patch 1: replace non-standard msid-semantic "esp-webrtc" with "WMS".
         * RFC 8830: browsers ignore a=msid: lines with unrecognised semantics,
         * causing Unified Plan to reject the video m-section. */
        {
            char *sem = strstr(sdp, "a=msid-semantic: esp-webrtc");
            if (sem) {
                char *tok = sem + strlen("a=msid-semantic: ");
                size_t old_len = strlen("esp-webrtc");
                size_t new_len = strlen("WMS");
                size_t tail    = strlen(tok + old_len) + 1;
                memmove(tok + new_len, tok + old_len, tail);
                memcpy(tok, "WMS", new_len);
                ESP_LOGI(TAG, "SDP patched: esp-webrtc -> WMS");
            }
        }

        /* Patch 2: strip a=ssrc:... msid:... lines (Plan-B artifact).
         * flutter_webrtc in Unified Plan mode detects SSRC-level msid as
         * Plan-B signalling and rejects the m-section on plan mismatch.
         * Keep a=ssrc:... cname:... lines — RTCP needs them. */
        {
            char *p = sdp;
            while ((p = strstr(p, "\na=ssrc:")) != NULL) {
                char *line_end = strchr(p + 1, '\n');
                if (!line_end) break;
                char *msid_pos = strstr(p + 1, " msid:");
                if (msid_pos && msid_pos < line_end) {
                    memmove(p, line_end, strlen(line_end) + 1);
                    /* do not advance p; next char is now what was after line_end */
                } else {
                    p = line_end;
                }
            }
        }

        /* Patch 3: move misplaced c= line to immediately after m=video line.
         * RFC 4566 §5 grammar: c= must precede a= lines in an m-section.
         * esp_peer places c= after ~14 a= lines; libwebrtc may fail to
         * associate ice-ufrag/ice-pwd/fingerprint with the video track,
         * causing the browser to reject the m-section (m=video 0 SAVPF 0). */
        {
            char *mv = strstr(sdp, "m=video");
            if (mv) {
                char *mv_end = strchr(mv, '\n');          /* \n ending m=video line */
                if (mv_end) {
                    char *cv     = strstr(mv_end + 1, "\nc="); /* \n before c= line */
                    char *cv_end = cv ? strchr(cv + 1, '\n') : NULL; /* \n ending c= line */
                    if (cv && cv_end) {
                        /* c= content (without the leading \n): cv+1 .. cv_end inclusive */
                        size_t c_len     = (size_t)(cv_end - cv);   /* includes trailing \n */
                        /* intermediate block (codec a= lines): mv_end+1 .. cv inclusive */
                        size_t inter_len = (size_t)(cv - mv_end);   /* includes \n of last line */
                        if (c_len > 0 && c_len <= 64 && inter_len > 0) {
                            char c_buf[64];
                            memcpy(c_buf, cv + 1, c_len);
                            /* Shift intermediate right — overwrites c= in place, already saved */
                            memmove(mv_end + 1 + c_len, mv_end + 1, inter_len);
                            /* Place c= immediately after m=video line */
                            memcpy(mv_end + 1, c_buf, c_len);
                            ESP_LOGI(TAG, "SDP patched: c= moved to after m=video");
                        }
                    }
                }
            }
        }

        /* Patch 4: change a=sendonly to a=sendrecv.
         * flutter_webrtc (Unified Plan) does not auto-create a recvonly transceiver
         * for an incoming sendonly offer, so Chrome answers m=video 0 (rejected).
         * sendrecv prompts Chrome to create a bidirectional transceiver; it answers
         * with a valid port and sends ICE candidates. esp_peer (SEND_ONLY) ignores
         * any incoming video Chrome may send. Same 8-byte length — in-place memcpy. */
        {
            char *so = strstr(sdp, "\na=sendonly");
            if (so) {
                memcpy(so + 3, "sendrecv", 8); /* \na=sendonly → \na=sendrecv (same length) */
                ESP_LOGI(TAG, "SDP patched: sendonly -> sendrecv");
            }
        }

        /* Log full patched SDP in 400-char chunks for verification */
        {
            const char *ptr = sdp;
            int remaining = (int)strlen(sdp);
            int chunk = 0;
            while (remaining > 0) {
                int take = remaining > 400 ? 400 : remaining;
                ESP_LOGI(TAG, "SDP offer [%d]: %.*s", chunk++, take, ptr);
                ptr += take;
                remaining -= take;
            }
        }

        cJSON *root    = cJSON_CreateObject();
        cJSON *sdp_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(root,    "type", "offer");
        cJSON_AddStringToObject(sdp_obj, "type", "offer");
        cJSON_AddStringToObject(sdp_obj, "sdp",  sdp);
        free(sdp);
        cJSON_AddItemToObject(root, "sdp", sdp_obj);
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json) {
            uart_bridge_send_msg(CAM_MSG_OFFER, json, (uint16_t)strlen(json));
            ESP_LOGI(TAG, "SDP offer sent (%d raw bytes, %u JSON bytes)",
                     msg->size, (unsigned)strlen(json));
            free(json);
        }

    } else if (msg->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        /* Local ICE candidate — same cJSON approach to handle any special chars. */
        char *cand = malloc(msg->size + 1);
        if (!cand) { ESP_LOGE(TAG, "OOM building ICE JSON"); return 0; }
        memcpy(cand, data, msg->size);
        cand[msg->size] = '\0';

        cJSON *root     = cJSON_CreateObject();
        cJSON *cand_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(root,     "type", "ice-candidate");
        cJSON_AddStringToObject(cand_obj, "candidate", cand);
        free(cand);
        cJSON_AddStringToObject(cand_obj, "sdpMid", "0");
        cJSON_AddNumberToObject(cand_obj, "sdpMLineIndex", 0);
        cJSON_AddItemToObject(root, "candidate", cand_obj);
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json) {
            uart_bridge_send_msg(CAM_MSG_ICE_FROM_CAM, json, (uint16_t)strlen(json));
            ESP_LOGI(TAG, "Local ICE candidate → hub (%u bytes): %.80s",
                     (unsigned)strlen(json), json);
            free(json);
        }
    }
    return 0;
}

static void video_task_fn(void *arg);
static void deferred_stop_task(void *arg);

static int on_state_cb(esp_peer_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "Peer state: %d", (int)state);
    switch (state) {
    case ESP_PEER_STATE_CONNECTED:
        ESP_LOGI(TAG, "WebRTC connected — starting video");
        s_connected = true;
        if (s_running && s_video_task == NULL && s_video_stack) {
            s_video_task = xTaskCreateStaticPinnedToCore(
                video_task_fn, "cam_video",
                8192, NULL, 4,
                s_video_stack, &s_video_tcb, 1);
            if (!s_video_task) {
                ESP_LOGE(TAG, "Failed to create video task");
                s_running = false;
                s_connected = false;
                if (!s_stop_scheduled) {
                    s_stop_scheduled = true;
                    if (xTaskCreate(deferred_stop_task, "cam_stop", 4096, NULL, 6, NULL) != pdPASS) {
                        s_stop_scheduled = false;
                    }
                }
            }
        }
        break;
    case ESP_PEER_STATE_CONNECT_FAILED:
    case ESP_PEER_STATE_DISCONNECTED:
        ESP_LOGW(TAG, "WebRTC disconnected/failed — scheduling stop");
        s_running = false;
        s_connected = false;
        if (!s_stopping && !s_stop_scheduled) {
            s_stop_scheduled = true;
            if (xTaskCreate(deferred_stop_task, "cam_stop", 4096, NULL, 6, NULL) != pdPASS) {
                s_stop_scheduled = false;
                ESP_LOGE(TAG, "Failed to create deferred stop task");
            }
        }
        break;
    default:
        break;
    }
    return 0;
}

/* ── Video capture + encode + send task ─────────────────────────────────── */
static void video_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Video task start");

    camera_core_deinit();
    esp_err_t err = camera_core_init_webrtc();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera_core_init_webrtc failed: %s", esp_err_to_name(err));
        s_video_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* H264 encoder — software, YUYV input */
    esp_h264_enc_cfg_t enc_cfg = {
        .pic_type = ESP_H264_RAW_FMT_YUYV,
        .gop      = VIDEO_FPS,
        .fps      = VIDEO_FPS,
        .res      = {.width = VIDEO_WIDTH, .height = VIDEO_HEIGHT},
        .rc       = {.bitrate = VIDEO_BITRATE, .qp_min = 20, .qp_max = 40},
    };
    esp_h264_enc_handle_t enc = NULL;
    if (esp_h264_enc_sw_new(&enc_cfg, &enc) != ESP_H264_ERR_OK || !enc) {
        ESP_LOGE(TAG, "H264 encoder create failed");
        camera_core_deinit();
        s_video_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if (esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "H264 encoder open failed");
        enc->del(enc);
        camera_core_deinit();
        s_video_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    log_heap("H264 encoder opened");

    uint8_t *h264_buf = heap_caps_malloc(H264_OUT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!h264_buf) {
        ESP_LOGE(TAG, "H264 output buffer alloc failed (%d B PSRAM)", H264_OUT_BUF_SIZE);
        esp_h264_enc_close(enc);
        enc->del(enc);
        camera_core_deinit();
        s_video_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Streaming H264 %dx%d @ %dfps", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);
    uint32_t pts = 0;

    while (s_running && s_connected) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        esp_h264_enc_in_frame_t in  = {
            .raw_data = {.buffer = fb->buf, .len = fb->len},
            .pts      = pts,
        };
        esp_h264_enc_out_frame_t out = {
            .raw_data = {.buffer = h264_buf, .len = H264_OUT_BUF_SIZE},
        };
        pts += (1000 / VIDEO_FPS);

        esp_h264_err_t h_err = esp_h264_enc_process(enc, &in, &out);
        esp_camera_fb_return(fb);

        if (h_err == ESP_H264_ERR_OK && out.length > 0) {
            esp_peer_video_frame_t vf = {
                .pts  = out.pts,
                .data = h264_buf,
                .size = (int)out.length,
            };
            esp_peer_send_video(s_peer, &vf);
        }

        vTaskDelay(pdMS_TO_TICKS(1000 / VIDEO_FPS));
    }

    heap_caps_free(h264_buf);
    esp_h264_enc_close(enc);
    enc->del(enc);
    camera_core_deinit();
    ESP_LOGI(TAG, "Video task exit");
    s_video_task = NULL;
    vTaskDelete(NULL);
}

/* ── esp_peer loop task ──────────────────────────────────────────────────── */
static void loop_task_fn(void *arg)
{
    while (s_running) {
        esp_peer_main_loop(s_peer);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "Loop task exit");
    s_loop_task = NULL;
    vTaskDelete(NULL);
}

/* ── DTLS cert pre-gen task (runs once at boot) ──────────────────────────── */
static void cert_pregen_task(void *arg)
{
    ESP_LOGI(TAG, "Pre-generating DTLS certificate...");
    int ret = esp_peer_pre_generate_cert();
    if (ret == ESP_PEER_ERR_NONE) {
        ESP_LOGI(TAG, "DTLS cert ready");
    } else {
        ESP_LOGW(TAG, "DTLS cert pre-gen failed (%d) — will re-try at connect", ret);
    }
    xSemaphoreGive(s_cert_ready);
    vTaskDelete(NULL);
}

/* ── cam_webrtc_task: runs the full WiFi→peer flow ─────────────────────── */
typedef struct {
    char ssid[64];
    char pass[64];
    char turn_user[32];
    char turn_psw[32];
} cam_start_args_t;

static void cam_webrtc_task(void *arg)
{
    cam_start_args_t *a = (cam_start_args_t *)arg;

    /* Wait for DTLS cert (fast if pre-gen succeeded) */
    xSemaphoreTake(s_cert_ready, portMAX_DELAY);
    xSemaphoreGive(s_cert_ready);   /* put it back so future sessions don't block */

    if (!wifi_connect(a->ssid, a->pass)) {
        ESP_LOGE(TAG, "cam_webrtc_task: WiFi failed — aborting");
        free(a);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    strlcpy(s_turn_user, a->turn_user, sizeof(s_turn_user));
    strlcpy(s_turn_psw, a->turn_psw, sizeof(s_turn_psw));
    s_ice_servers[0] = (esp_peer_ice_server_cfg_t){
        .stun_url = "stun:stun.l.google.com:19302", .user = NULL, .psw = NULL};
    s_ice_servers[1] = (esp_peer_ice_server_cfg_t){
        .stun_url = "turn:13.51.196.176:3478", .user = s_turn_user, .psw = s_turn_psw};
    s_ice_servers[2] = (esp_peer_ice_server_cfg_t){
        .stun_url = "turns:home-secure.glazia.in:5349", .user = s_turn_user, .psw = s_turn_psw};

    esp_peer_cfg_t cfg = {
        .server_lists    = s_ice_servers,
        .server_num      = 3,
        .role            = ESP_PEER_ROLE_CONTROLLING,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .video_info      = {
            .codec  = ESP_PEER_VIDEO_CODEC_H264,
            .width  = VIDEO_WIDTH,
            .height = VIDEO_HEIGHT,
            .fps    = VIDEO_FPS,
        },
        .audio_dir       = ESP_PEER_MEDIA_DIR_NONE,
        .video_dir       = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .no_auto_reconnect = true,
        .on_state        = on_state_cb,
        .on_msg          = on_msg_cb,
    };

    int peer_err = esp_peer_open(&cfg, esp_peer_get_default_impl(), &s_peer);
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", peer_err);
        free(a);
        s_running = false;
        wifi_stop();
        vTaskDelete(NULL);
        return;
    }
    log_heap("Peer opened");

    /* Start the main-loop task */
    s_loop_task = xTaskCreateStaticPinnedToCore(
        loop_task_fn, "cam_peer_loop",
        6144, NULL, 5,
        s_loop_stack, &s_loop_tcb, 0);
    if (!s_loop_task) {
        ESP_LOGE(TAG, "Failed to create peer loop task");
        esp_peer_close(s_peer);
        s_peer = NULL;
        free(a);
        s_running = false;
        wifi_stop();
        vTaskDelete(NULL);
        return;
    }

    /* Trigger SDP offer generation */
    peer_err = esp_peer_new_connection(s_peer);
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_new_connection failed: %d", peer_err);
        free(a);
        webrtc_cam_stop();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "WebRTC session started — waiting for offer/answer");

    free(a);
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void webrtc_cam_init(void)
{
    /* esp_peer INFO logs include TURN credentials. */
    esp_log_level_set("AGENT", ESP_LOG_INFO);
    s_cert_ready = xSemaphoreCreateBinary();
    if (!s_cert_ready) {
        ESP_LOGE(TAG, "Failed to allocate certificate semaphore");
        return;
    }

    /* Pre-allocate PSRAM stacks so they're available when a session starts */
    s_loop_stack  = heap_caps_malloc(6144, MALLOC_CAP_SPIRAM);
    s_video_stack = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!s_loop_stack || !s_video_stack) {
        ESP_LOGE(TAG, "PSRAM stack alloc failed — WebRTC unavailable");
        return;
    }

    /* Kick off cert pre-generation in background */
    if (xTaskCreate(cert_pregen_task, "cert_pregen", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create certificate task");
        xSemaphoreGive(s_cert_ready);
    }
    ESP_LOGI(TAG, "WebRTC cam initialised (PSRAM stacks ready)");
    log_heap("WebRTC init");
}

void webrtc_cam_start_from_json(const char *json)
{
    if (s_running) {
        ESP_LOGW(TAG, "webrtc_cam_start_from_json: already running — ignoring");
        return;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse WEBRTC_START JSON");
        return;
    }

    cam_start_args_t *a = calloc(1, sizeof(cam_start_args_t));
    if (!a) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "OOM for cam_start_args");
        return;
    }

    const char *s_ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
    const char *s_pass = cJSON_GetStringValue(cJSON_GetObjectItem(root, "pass"));
    const char *s_user = cJSON_GetStringValue(cJSON_GetObjectItem(root, "turn_user"));
    const char *s_psw  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "turn_psw"));

    if (!s_ssid || !s_pass) {
        ESP_LOGE(TAG, "WEBRTC_START JSON missing ssid/pass");
        free(a);
        cJSON_Delete(root);
        return;
    }

    strlcpy(a->ssid,      s_ssid, sizeof(a->ssid));
    strlcpy(a->pass,      s_pass, sizeof(a->pass));
    strlcpy(a->turn_user, s_user ? s_user : "", sizeof(a->turn_user));
    strlcpy(a->turn_psw,  s_psw  ? s_psw  : "", sizeof(a->turn_psw));
    cJSON_Delete(root);

    s_running   = true;
    s_connected = false;
    s_stopping  = false;
    s_stop_scheduled = false;
    s_loop_task  = NULL;
    s_video_task = NULL;

    /* Spawn async task so spi_listener_task is never blocked */
    if (xTaskCreate(cam_webrtc_task, "cam_webrtc", 8192, a, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WebRTC startup task");
        free(a);
        s_running = false;
    }
}

void webrtc_cam_on_answer(const char *sdp_str, int len)
{
    if (!s_peer) { ESP_LOGW(TAG, "on_answer: no peer"); return; }
    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)sdp_str,
        .size = len,
    };
    int ret = esp_peer_send_msg(s_peer, &msg);
    if (ret == ESP_PEER_ERR_NONE) {
        ESP_LOGI(TAG, "SDP answer forwarded to esp_peer (%d bytes)", len);
    } else {
        ESP_LOGE(TAG, "Failed to forward SDP answer: %d", ret);
    }
}

void webrtc_cam_on_ice(const char *cand_str, int len)
{
    if (!s_peer) { ESP_LOGW(TAG, "on_ice: no peer"); return; }
    ESP_LOGI(TAG, "Remote ICE candidate from hub (%d bytes): %.80s", len, cand_str);
    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_CANDIDATE,
        .data = (uint8_t *)cand_str,
        .size = len,
    };
    int ret = esp_peer_send_msg(s_peer, &msg);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGW(TAG, "Failed to forward ICE candidate: %d", ret);
    }
}

void webrtc_cam_stop(void)
{
    if (s_stopping) return;
    if (!s_running && !s_peer) return;
    s_stopping = true;
    bool was_connected = s_connected;
    s_running   = false;
    s_connected = false;

    /* Let tasks leave before destroying the peer/camera objects they use. */
    for (int i = 0; i < 50 && (s_loop_task || s_video_task); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    esp_peer_handle_t peer = s_peer;
    s_peer = NULL;
    if (peer) {
        if (was_connected) esp_peer_disconnect(peer);
        esp_peer_close(peer);
    }

    /* Disconnect WiFi so it can reconnect on next session with fresh creds */
    wifi_stop();

    s_stopping = false;
    ESP_LOGI(TAG, "WebRTC stopped");
    log_heap("After WebRTC stop");
}

static void deferred_stop_task(void *arg)
{
    (void)arg;
    s_stop_scheduled = false;
    webrtc_cam_stop();
    vTaskDelete(NULL);
}

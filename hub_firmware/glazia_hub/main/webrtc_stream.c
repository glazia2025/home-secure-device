#include "webrtc_stream.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "camera_stream.h"
#include "hub_control_ws.h"
#include "state.h"

#include "esp_h264_enc_single_sw.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"


static const char *TAG = "WEBRTC";

/* ── ICE servers ──────────────────────────────────────────────────────────── */

/* STUN/TURN URLs and credentials — read from NVS globals at peer-open time.
 * Defined in state.h / instantiated in main.c; defaults in state.h. */
static esp_peer_ice_server_cfg_t s_ice_servers[3];

/* ── State ─────────────────────────────────────────────────────────────────── */

static esp_peer_handle_t s_peer;
static bool              s_peer_open;
static bool              s_cert_pregen_started;
static bool              s_connecting;
static TaskHandle_t      s_loop_task;
static TaskHandle_t      s_video_task;
static QueueHandle_t     s_webrtc_queue = NULL;
static TimerHandle_t     s_answer_timer = NULL;

/* Queue message types */
#define WEBRTC_CMD_START ((uint8_t)0)
#define WEBRTC_CMD_STOP  ((uint8_t)1)

/* PSRAM-backed stacks for loop and video tasks — pre-allocated once at init,
 * reused across sessions so they never consume internal heap. */
static StackType_t  *s_loop_stack  = NULL;
static StaticTask_t *s_loop_tcb    = NULL;
static StackType_t  *s_video_stack = NULL;
static StaticTask_t *s_video_tcb   = NULL;

/* ── Forward declarations ──────────────────────────────────────────────────── */

static int  on_msg_cb(esp_peer_msg_t *msg, void *ctx);
static int  on_state_cb(esp_peer_state_t state, void *ctx);
static void loop_task_fn(void *arg);
static void video_task_fn(void *arg);


/* Called from FreeRTOS Timer Service task — must not block or call ESP_LOGI.
 * Sends a stop command to rtc_ctrl_task which does the actual teardown. */
static void answer_timeout_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    uint8_t cmd = WEBRTC_CMD_STOP;
    xQueueSend(s_webrtc_queue, &cmd, 0);
}

static void rtc_ctrl_task(void *arg) {
    (void)arg;
    uint8_t cmd;

    // 1. Run Init ONCE (Pre-generate certs)
    ESP_LOGI(TAG, "Controller: Performing one-time initialization...");
    webrtc_stream_init();

    while(1) {
        // 2. Wait for viewer triggers or timeout stop commands
        if (xQueueReceive(s_webrtc_queue, &cmd, portMAX_DELAY)) {
            if (cmd == WEBRTC_CMD_START) {
                ESP_LOGI(TAG, "Controller: Trigger received, starting viewer connection...");
                webrtc_stream_on_viewer_ready();
            } else if (cmd == WEBRTC_CMD_STOP) {
                ESP_LOGI(TAG, "Controller: Stop requested");
                webrtc_stream_stop();
            }
        }
    }
}

void webrtc_stream_controller_init(void) {
    if (s_webrtc_queue) return;
    s_webrtc_queue = xQueueCreate(2, sizeof(uint8_t));
    s_answer_timer = xTimerCreate("ans_tmr", pdMS_TO_TICKS(30000),
                                  pdFALSE, NULL, answer_timeout_cb);

    // Allocate 16KB stack in PSRAM
    size_t stack_size = 16384;
    StackType_t *stack = heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);

    if (!stack || !tcb) {
        ESP_LOGE(TAG, "OOM allocating DTLS controller task");
        return;
    }

    xTaskCreateStatic(rtc_ctrl_task, "rtc_ctrl", stack_size / 4, NULL, 5, stack, tcb);

    /* Pre-allocate loop and video task stacks in PSRAM so they never draw from
     * the ≈15 KB of internal heap that DTLS and ICE also need. */
    s_loop_stack  = heap_caps_malloc(6144, MALLOC_CAP_SPIRAM);
    s_loop_tcb    = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    s_video_stack = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    s_video_tcb   = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (!s_loop_stack || !s_loop_tcb || !s_video_stack || !s_video_tcb) {
        ESP_LOGE(TAG, "OOM pre-allocating WebRTC task stacks");
    }
}

void webrtc_trigger_start(void) {
    if (!s_webrtc_queue) return;
    uint8_t cmd = WEBRTC_CMD_START;
    xQueueSend(s_webrtc_queue, &cmd, 0);
}

void webrtc_trigger_stop(void) {
    if (!s_webrtc_queue) return;
    uint8_t cmd = WEBRTC_CMD_STOP;
    xQueueSend(s_webrtc_queue, &cmd, 0);
}

/* ── Signaling helpers ─────────────────────────────────────────────────────── */

static void send_offer(const char *sdp_raw, int sdp_len)
{
    /* Use cJSON so the SDP string is properly JSON-escaped: raw SDP contains
     * literal \r\n between lines, which are invalid inside a JSON string value. */
    char *sdp_copy = malloc(sdp_len + 1);
    if (!sdp_copy) { ESP_LOGE(TAG, "OOM building offer JSON"); return; }
    memcpy(sdp_copy, sdp_raw, sdp_len);
    sdp_copy[sdp_len] = '\0';

    cJSON *root    = cJSON_CreateObject();
    cJSON *sdp_obj = cJSON_CreateObject();
    if (!root || !sdp_obj) {
        free(sdp_copy); cJSON_Delete(root); cJSON_Delete(sdp_obj); return;
    }
    cJSON_AddStringToObject(root, "type", "offer");
    cJSON_AddStringToObject(sdp_obj, "type", "offer");
    cJSON_AddStringToObject(sdp_obj, "sdp", sdp_copy);
    free(sdp_copy);
    cJSON_AddItemToObject(root, "sdp", sdp_obj);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str) {
        hub_control_ws_send_json(json_str);
        free(json_str);
        /* Start 30s answer timeout. If app never sends an answer (e.g. crashes
         * before transmitting), this resets s_connecting so reconnect works. */
        if (s_answer_timer) xTimerReset(s_answer_timer, 0);
    }
}

static void send_ice_candidate(const char *cand_raw, int cand_len)
{
    char *cand_copy = malloc(cand_len + 1);
    if (!cand_copy) { ESP_LOGE(TAG, "OOM building ice-candidate JSON"); return; }
    memcpy(cand_copy, cand_raw, cand_len);
    cand_copy[cand_len] = '\0';

    cJSON *root     = cJSON_CreateObject();
    cJSON *cand_obj = cJSON_CreateObject();
    if (!root || !cand_obj) {
        free(cand_copy); cJSON_Delete(root); cJSON_Delete(cand_obj); return;
    }
    cJSON_AddStringToObject(root, "type", "ice-candidate");
    cJSON_AddStringToObject(cand_obj, "candidate", cand_copy);
    free(cand_copy);
    cJSON_AddStringToObject(cand_obj, "sdpMid", "0");
    cJSON_AddNumberToObject(cand_obj, "sdpMLineIndex", 0);
    cJSON_AddItemToObject(root, "candidate", cand_obj);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str) { hub_control_ws_send_json(json_str); free(json_str); }
}

/* ── esp_peer callbacks ────────────────────────────────────────────────────── */

static const char *peer_state_name(esp_peer_state_t s)
{
    switch (s) {
        case ESP_PEER_STATE_CLOSED:               return "CLOSED";
        case ESP_PEER_STATE_DISCONNECTED:         return "DISCONNECTED";
        case ESP_PEER_STATE_NEW_CONNECTION:       return "NEW_CONNECTION";
        case ESP_PEER_STATE_CANDIDATE_GATHERING:  return "CANDIDATE_GATHERING";
        case ESP_PEER_STATE_PAIRING:              return "PAIRING";
        case ESP_PEER_STATE_PAIRED:               return "PAIRED";
        case ESP_PEER_STATE_CONNECTING:           return "CONNECTING";
        case ESP_PEER_STATE_CONNECTED:            return "CONNECTED";
        case ESP_PEER_STATE_CONNECT_FAILED:       return "CONNECT_FAILED";
        default:                                  return "OTHER";
    }
}

static int on_msg_cb(esp_peer_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (!msg) return -1;

    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        ESP_LOGI(TAG, "Sending SDP offer (%d bytes)", msg->size);
        ESP_LOGI(TAG, "SDP offer[0..300]: %.*s", 300, (const char *)msg->data);
        send_offer((const char *)msg->data, msg->size);
    } else if (msg->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        ESP_LOGI(TAG, "Sending ICE candidate (%d bytes)", msg->size);
        send_ice_candidate((const char *)msg->data, msg->size);
    }
    return 0;
}

static int on_state_cb(esp_peer_state_t state, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "PeerState: %d (%s)  heap_int=%u",
             (int)state, peer_state_name(state),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (state == ESP_PEER_STATE_CONNECTED) {
        ESP_LOGI(TAG, "WebRTC connected — starting video");
        if (!s_video_task) {
            s_video_task = xTaskCreateStaticPinnedToCore(
                video_task_fn, "webrtc_video",
                8192 / sizeof(StackType_t),  /* 2048 words = 8192 bytes */
                NULL, 3, s_video_stack, s_video_tcb, 1);
        }
    } else if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CONNECT_FAILED) {
        ESP_LOGW(TAG, "WebRTC disconnected/failed — stopping");
        webrtc_stream_stop();
    }
    return 0;
}

/* ── Tasks ─────────────────────────────────────────────────────────────────── */

static void loop_task_fn(void *arg)
{
    (void)arg;
    while (s_peer_open) {
        if (s_peer) {
            esp_peer_main_loop(s_peer);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_loop_task = NULL;
    vTaskDelete(NULL);
}

static void video_task_fn(void *arg)
{
    (void)arg;

    esp_err_t err = camera_ensure_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed in video_task: %s", esp_err_to_name(err));
        s_video_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_h264_enc_cfg_sw_t h264_cfg = {
        .pic_type = ESP_H264_RAW_FMT_YUYV,
        .gop      = 10,
        .fps      = 10,
        .res      = {.width = 320, .height = 240},
        .rc       = {.bitrate = 512000, .qp_min = 10, .qp_max = 51},
    };
    esp_h264_enc_handle_t h264 = NULL;
    esp_h264_err_t h264_err = esp_h264_enc_sw_new(&h264_cfg, &h264);
    if (h264_err != ESP_H264_ERR_OK || !h264) {
        ESP_LOGE(TAG, "H.264 encoder create failed: %d", (int)h264_err);
        camera_deinit();
        s_video_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    h264_err = esp_h264_enc_open(h264);
    if (h264_err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "H.264 encoder open failed: %d", (int)h264_err);
        esp_h264_enc_del(h264);
        camera_deinit();
        s_video_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Output buffer in PSRAM — sized to uncompressed QVGA YUV422 (safe upper bound). */
    const size_t h264_out_sz = 320 * 240 * 2;
    uint8_t *h264_out_buf = heap_caps_malloc(h264_out_sz, MALLOC_CAP_SPIRAM);
    if (!h264_out_buf) {
        ESP_LOGE(TAG, "H.264 output buffer alloc failed");
        esp_h264_enc_close(h264);
        esp_h264_enc_del(h264);
        camera_deinit();
        s_video_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint32_t pts = 0;
    while (s_peer_open) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        esp_h264_enc_in_frame_t in_frame = {
            .raw_data = {.buffer = fb->buf, .len = fb->len},
            .pts      = pts,
        };
        esp_h264_enc_out_frame_t out_frame = {
            .raw_data = {.buffer = h264_out_buf, .len = h264_out_sz},
        };

        h264_err = esp_h264_enc_process(h264, &in_frame, &out_frame);
        esp_camera_fb_return(fb);

        if (h264_err == ESP_H264_ERR_OK && out_frame.length > 0) {
            esp_peer_video_frame_t vframe = {
                .pts  = pts,
                .data = h264_out_buf,
                .size = (int)out_frame.length,
            };
            if (s_peer) {
                esp_peer_send_video(s_peer, &vframe);
            }
            pts += 90000 / 10;  /* 90kHz RTP clock, 10fps */
        } else if (h264_err != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG, "H.264 encode failed: %d", (int)h264_err);
        }

        vTaskDelay(pdMS_TO_TICKS(100));  /* ~10fps */
    }

    esp_h264_enc_close(h264);
    esp_h264_enc_del(h264);
    heap_caps_free(h264_out_buf);
    camera_deinit();
    ESP_LOGI(TAG, "Video task stopped");
    s_video_task = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ────────────────────────────────────────────────────────────── */

static void cert_pregen_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Pre-generating DTLS certificate…");
    esp_peer_pre_generate_cert();
    ESP_LOGI(TAG, "DTLS certificate ready");
    vTaskDelete(NULL);
}

void webrtc_stream_init(void)
{
    // This is now purely the logic to start the peer,
    // running safely on the rtc_ctrl_task stack.
    if (s_cert_pregen_started) return;
    s_cert_pregen_started = true;

    ESP_LOGI(TAG, "Pre-generating DTLS certificate…");
    esp_peer_pre_generate_cert();
    ESP_LOGI(TAG, "DTLS certificate ready");
}

void webrtc_stream_on_viewer_ready(void)
{
    if (s_connecting) {
        ESP_LOGW(TAG, "viewer-ready: already connecting, ignoring trigger");
        return;
    }

    if (s_peer_open) {
        ESP_LOGW(TAG, "viewer-ready: peer already open — closing previous");
        webrtc_stream_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_connecting = true;

    ESP_LOGI(TAG, "viewer-ready: opening WebRTC peer connection");

    /* Build ICE server list from NVS globals (fallback to compile-time defaults). */
    s_ice_servers[0].stun_url = "stun:stun.l.google.com:19302";
    s_ice_servers[0].user     = NULL;
    s_ice_servers[0].psw      = NULL;
    s_ice_servers[1].stun_url = "turn:13.51.196.176:3478";
    s_ice_servers[1].user     = g_turn_user;
    s_ice_servers[1].psw      = g_turn_psw;
    s_ice_servers[2].stun_url = "turns:home-secure.glazia.in:5349";
    s_ice_servers[2].user     = g_turn_user;
    s_ice_servers[2].psw      = g_turn_psw;

    esp_peer_default_cfg_t extra = {
        /* coturn in dev uses a domain cert; set false if TURNS fails cert verify */
        .insecure_skip_turn_cert_verify = false,
    };

    esp_peer_cfg_t cfg = {
        .server_lists      = s_ice_servers,
        .server_num        = 3,
        .role              = ESP_PEER_ROLE_CONTROLLING,
        .ice_trans_policy  = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .video_dir         = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .audio_dir         = ESP_PEER_MEDIA_DIR_NONE,
        .no_auto_reconnect = true,
        .video_info        = {
            .codec  = ESP_PEER_VIDEO_CODEC_H264,
            .width  = 320,
            .height = 240,
            .fps    = 10,
        },
        .on_msg        = on_msg_cb,
        .on_state      = on_state_cb,
        .ctx           = NULL,
        .extra_cfg     = &extra,
        .extra_size    = sizeof(extra),
    };

    int ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &s_peer);
    if (ret != 0 || !s_peer) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", ret);
        s_connecting = false;
        return;
    }

    s_peer_open = true;

    s_loop_task = xTaskCreateStaticPinnedToCore(
        loop_task_fn, "webrtc_loop",
        6144 / sizeof(StackType_t),  /* 1536 words = 6144 bytes */
        NULL, 4, s_loop_stack, s_loop_tcb, 0);

    ret = esp_peer_new_connection(s_peer);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_peer_new_connection failed: %d", ret);
        webrtc_stream_stop();
        return;
    }

    ESP_LOGI(TAG, "WebRTC peer connection initiated — waiting for ICE");
}



void webrtc_stream_on_answer(const char *sdp_str, int sdp_len)
{
    if (!s_peer || !s_peer_open) {
        ESP_LOGW(TAG, "answer received but no active peer");
        return;
    }
    if (s_answer_timer) xTimerStop(s_answer_timer, 0);
    ESP_LOGI(TAG, "Feeding SDP answer to peer (%d bytes)", sdp_len);
    ESP_LOGI(TAG, "SDP answer[0..200]: %.200s", sdp_str);

    /* Guard: if the remote peer rejected the video section (port=0), esp_peer
     * internally derives vfmt=ESP_PEER_VIDEO_CODEC_NONE and crashes with a
     * null-deref in its codec setup path.  Detect this before feeding and stop
     * cleanly instead.  The app-side RTCPeerConnection must call
     * addTransceiver('video',{direction:'recvonly'}) before setRemoteDescription. */
    if (strstr(sdp_str, "\nm=video 0 ") || strstr(sdp_str, "\rm=video 0 ") ||
        strncmp(sdp_str, "m=video 0 ", 10) == 0) {
        ESP_LOGE(TAG, "SDP answer rejects video (m=video port=0) — app WebRTC peer must accept video before answering");
        webrtc_stream_stop();
        return;
    }

    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)sdp_str,
        .size = sdp_len,
    };
    esp_peer_send_msg(s_peer, &msg);
}

void webrtc_stream_on_ice_candidate(const char *cand_str, int cand_len)
{
    if (!s_peer || !s_peer_open) return;
    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_CANDIDATE,
        .data = (uint8_t *)cand_str,
        .size = cand_len,
    };
    esp_peer_send_msg(s_peer, &msg);
}

void webrtc_stream_stop(void)
{
    /* Guard: already stopped, or mid-stop (s_peer is nulled before close below
     * so a re-entrant call from on_state_cb sees NULL and returns here). */
    if (!s_peer_open && !s_peer) {
        s_connecting = false;
        return;
    }

    ESP_LOGI(TAG, "Stopping WebRTC stream");
    if (s_answer_timer) xTimerStop(s_answer_timer, 0);
    s_peer_open  = false;
    s_connecting = false;

    /* Close peer BEFORE deleting tasks.
     * s_loop_task is blocked in lwIP select() on the peer's sockets. If we call
     * vTaskDelete() first, the task TCB (including its select semaphore) is freed.
     * esp_peer_close() then calls lwip_netconn_do_delconn → select_check_waiters →
     * xQueueGenericSend on the freed semaphore (0xabcd poison) → LoadProhibited crash.
     * Closing the peer first lets lwIP wake the tasks cleanly; only then is it safe
     * to vTaskDelete() them.
     * s_peer is nulled before esp_peer_disconnect so the re-entrant on_state_cb
     * call hits the guard at the top of this function and returns without double-close. */
    esp_peer_handle_t peer = s_peer;
    s_peer = NULL;

    if (peer) {
        ESP_LOGI(TAG, "Closing peer...");
        esp_peer_disconnect(peer);
        esp_peer_close(peer);
    }

    if (s_loop_task)  { vTaskDelete(s_loop_task);  s_loop_task  = NULL; }
    if (s_video_task) { vTaskDelete(s_video_task); s_video_task = NULL; }

    camera_deinit();
    ESP_LOGI(TAG, "WebRTC stream stopped");
}

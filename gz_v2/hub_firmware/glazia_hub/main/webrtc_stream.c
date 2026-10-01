#include "webrtc_stream.h"
#include "camera_csi.h"
#include "hub_control_ws.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_h264_enc_single_hw.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "WEBRTC";

/* ── Stream parameters ───────────────────────────────────────────────────────
 * Geometry is learned from the sensor at start (camera_csi_width/height) — the
 * OV5647's smallest CSI mode (800x640, both 16-aligned so the H.264 encoder is
 * happy) approximates the requested VGA target. */
/* 10 fps / 500 kbps: with the SDIO DMA mempool back in internal RAM (sdkconfig:
 * ESP_HOSTED_MEMPOOL_PREFER_SPIRAM unset), lowering the encode rate cuts encoder PSRAM
 * read traffic and WiFi packet cadence, giving the single MSPI/PSRAM bus and the SDIO
 * cache-coherency path more slack under a live call. The link is TURN-relay limited
 * anyway, so 10 fps costs little in practice. */
#define STREAM_FPS      12
#define STREAM_BITRATE  500000

/* HW encoder input format. On this P4 rev-1 silicon the hardware H.264 encoder
 * accepts ONLY O_UYY_E_VYY (YUV420, odd lines U Y Y…, even lines V Y Y…), which
 * is exactly what camera_csi.c produces from the ISP. Centralised here so the
 * documented software fallback (ESP_H264_RAW_FMT_YUYV + ISP YUV422, gz_v1 path)
 * is a one-line change if the ISP-YUV420↔O_UYY_E_VYY layout ever mismatches. */
#define STREAM_PIC_TYPE ESP_H264_RAW_FMT_O_UYY_E_VYY

/* Video task stack (words == bytes for StackType_t on RISC-V, both args below use
 * the same value). It runs the HW H.264 encode AND esp_peer_send_video → RTP +
 * SRTP/AES + lwIP socket send, a deep stack-hungry chain. 8K overflowed mid-send
 * and corrupted lwIP heap metadata (crash in tcpip_thread); the proven gz_v1
 * cam firmware uses 32K. The static-task create size MUST equal the alloc size. */
#define WEBRTC_VIDEO_STACK 6144
// #define WEBRTC_VIDEO_STACK 49152


static StaticTask_t      s_loop_tcb;
static StackType_t      *s_loop_stack;
static StaticTask_t      s_video_tcb;
static StackType_t      *s_video_stack;
static TaskHandle_t      s_loop_task  = NULL;
static TaskHandle_t      s_video_task = NULL;
static SemaphoreHandle_t s_cert_ready = NULL;

static esp_peer_handle_t s_peer      = NULL;
static volatile bool     s_running   = false;
static volatile bool     s_connected = false;
static volatile bool     s_stopping  = false;
static volatile bool     s_stop_scheduled = false;
static volatile bool     s_start_in_progress = false;

/* Serialises webrtc_stream_start() vs webrtc_stream_stop() so a rapid
 * viewer-ready/viewer-gone sequence can't tear the camera/peer down while the
 * async start_task is still bringing them up (which leaked the ISP/CSI). */
static SemaphoreHandle_t s_lifecycle_lock = NULL;

static int s_stream_w, s_stream_h;

/* ICE config must outlive the startup task. */
static char s_turn_user[64];
static char s_turn_psw[64];
static esp_peer_ice_server_cfg_t s_ice_servers[3];

/* Cached last offer JSON so we can resend if the control WS reconnects. */
static char *s_pending_offer;

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "%s: internal_free=%u largest=%u psram_free=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ── esp_peer callbacks ──────────────────────────────────────────────────── */

static int on_msg_cb(esp_peer_msg_t *msg, void *ctx)
{
    if (!msg || !msg->data || msg->size <= 0) return 0;
    const char *data = (const char *)msg->data;

    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        char *sdp = malloc(msg->size + 1);
        if (!sdp) { ESP_LOGE(TAG, "OOM building offer JSON"); return 0; }
        memcpy(sdp, data, msg->size);
        sdp[msg->size] = '\0';

        /* Patch 1: non-standard msid-semantic "esp-webrtc" → "WMS" (RFC 8830). */
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

        /* Patch 2: strip a=ssrc:... msid:... (Plan-B artifact) — keep cname lines. */
        {
            char *p = sdp;
            while ((p = strstr(p, "\na=ssrc:")) != NULL) {
                char *line_end = strchr(p + 1, '\n');
                if (!line_end) break;
                char *msid_pos = strstr(p + 1, " msid:");
                if (msid_pos && msid_pos < line_end) {
                    memmove(p, line_end, strlen(line_end) + 1);
                } else {
                    p = line_end;
                }
            }
        }

        /* Patch 3: move c= to immediately after m=video (RFC 4566 §5 ordering). */
        {
            char *mv = strstr(sdp, "m=video");
            if (mv) {
                char *mv_end = strchr(mv, '\n');
                if (mv_end) {
                    char *cv     = strstr(mv_end + 1, "\nc=");
                    char *cv_end = cv ? strchr(cv + 1, '\n') : NULL;
                    if (cv && cv_end) {
                        size_t c_len     = (size_t)(cv_end - cv);
                        size_t inter_len = (size_t)(cv - mv_end);
                        if (c_len > 0 && c_len <= 64 && inter_len > 0) {
                            char c_buf[64];
                            memcpy(c_buf, cv + 1, c_len);
                            memmove(mv_end + 1 + c_len, mv_end + 1, inter_len);
                            memcpy(mv_end + 1, c_buf, c_len);
                            ESP_LOGI(TAG, "SDP patched: c= moved after m=video");
                        }
                    }
                }
            }
        }

        /* Patch 4: advertise constrained Baseline (42e01f) not Main (4d001f), so
         * mobile libwebrtc has a matching receive codec. */
        {
            const char *main_profile = "profile-level-id=4d001f";
            const char *baseline_profile = "profile-level-id=42e01f";
            char *profile = sdp;
            int replacements = 0;
            while ((profile = strstr(profile, main_profile)) != NULL) {
                memcpy(profile, baseline_profile, strlen(baseline_profile));
                profile += strlen(baseline_profile);
                replacements++;
            }
            ESP_LOGI(TAG, "SDP patched: H264 Main -> Baseline (%d fmtp)", replacements);
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
            /* Cache for resend, then emit on the control WS. */
            free(s_pending_offer);
            s_pending_offer = strdup(json);
            hub_control_ws_send_json(json);
            ESP_LOGI(TAG, "SDP offer sent (%d raw, %u JSON)", msg->size, (unsigned)strlen(json));
            free(json);
        }

    } else if (msg->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
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
            hub_control_ws_send_json(json);
            ESP_LOGI(TAG, "Local ICE → server (%u B): %.80s", (unsigned)strlen(json), json);
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
        /* See the AGENT log's 'Add remote type:N' (1=host 2=srflx 4=relay), 'Select pair'
         * and 'Connection OK <addr>' just above to tell which path won this session
         * (direct host/srflx vs coturn relay). */
        ESP_LOGI(TAG, "WebRTC connected — see AGENT 'Connection OK'/'Select pair' for the chosen path (1=host 2=srflx 4=relay)");
        s_connected = true;
        if (s_running && s_video_task == NULL && s_video_stack) {
            s_video_task = xTaskCreateStaticPinnedToCore(
                video_task_fn, "wrtc_video", WEBRTC_VIDEO_STACK, NULL, 4,
                s_video_stack, &s_video_tcb, 0);
            if (!s_video_task) {
                ESP_LOGE(TAG, "video task create failed");
                s_running = false; s_connected = false;
                webrtc_stream_request_stop();
            }
        }
        break;
    case ESP_PEER_STATE_CONNECT_FAILED:
    case ESP_PEER_STATE_DISCONNECTED:
        ESP_LOGW(TAG, "WebRTC disconnected/failed — scheduling stop");
        s_running = false; s_connected = false;
        if (!s_stopping) webrtc_stream_request_stop();
        break;
    default:
        break;
    }
    return 0;
}

/* ── Capture → hardware H.264 → send ─────────────────────────────────────── */
static void video_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Video task start");

    /* Round dimensions up to the 16-px multiple the H.264 encoder requires. */
    int w = (s_stream_w + 15) & ~15;
    int h = (s_stream_h + 15) & ~15;

    esp_h264_enc_cfg_hw_t enc_cfg = {
        .pic_type = STREAM_PIC_TYPE,                 /* rev-1 P4 hw encoder: O_UYY_E_VYY only */
        .gop      = STREAM_FPS / 2,
        .fps      = STREAM_FPS,
        .res      = { .width = w, .height = h },
        .rc       = { .bitrate = STREAM_BITRATE, .qp_min = 20, .qp_max = 40 },
    };
    esp_h264_enc_handle_t enc = NULL;
    if (esp_h264_enc_hw_new(&enc_cfg, &enc) != ESP_H264_ERR_OK || !enc) {
        ESP_LOGE(TAG, "hw H264 encoder create failed (%dx%d)", w, h);
        s_video_task = NULL; vTaskDelete(NULL); return;
    }
    if (esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "H264 encoder open failed");
        esp_h264_enc_del(enc);
        s_video_task = NULL; vTaskDelete(NULL); return;
    }
    log_heap("H264 encoder opened");

    /* The HW encoder invalidates this output buffer with an M2C esp_cache_msync
     * that does NOT set the UNALIGNED flag (esp_h264_cache.c), so each buffer's
     * start address must be cache-line aligned (align to 128).
     *
     * We allocate a ring of H264_OUT_BUFS buffers and rotate through them so that
     * esp_peer_send_video() always has at least (H264_OUT_BUFS - 1) frames of slack
     * before the encoder can reuse a buffer. This prevents the race where the RTP/
     * SRTP sender is still reading a buffer that the encoder is already overwriting,
     * which was the leading PSRAM corruption hypothesis. Cost: 3 × ~1 MB PSRAM. */
#define H264_OUT_BUFS 2
    size_t out_cap = (256 * 1024 + 127) & ~(size_t)127;
    uint8_t *h264_bufs[H264_OUT_BUFS] = { NULL };
    for (int i = 0; i < H264_OUT_BUFS; i++) {
        h264_bufs[i] = heap_caps_aligned_calloc(128, 1, out_cap, MALLOC_CAP_SPIRAM);
        if (!h264_bufs[i]) {
            ESP_LOGE(TAG, "H264 out buffer[%d] alloc failed (%u B)", i, (unsigned)out_cap);
            for (int j = 0; j < i; j++) heap_caps_free(h264_bufs[j]);
            esp_h264_enc_close(enc); esp_h264_enc_del(enc);
            s_video_task = NULL; vTaskDelete(NULL); return;
        }
    }
    int h264_idx = 0;

    ESP_LOGI(TAG, "Streaming H264 %dx%d @ %dfps", w, h, STREAM_FPS);
    uint32_t pts = 0;
    uint32_t frame_count = 0;   /* DIAG: crash-cause triage (remove once root-caused) */

    while (s_running && s_connected) {
        uint8_t *frame = NULL;
        size_t   frame_len = 0;
        if (camera_csi_get_frame(&frame, &frame_len) != ESP_OK || !frame) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        static TickType_t last_frame_tick = 0;
        TickType_t now = xTaskGetTickCount();
        if ((now - last_frame_tick) < pdMS_TO_TICKS(1000 / STREAM_FPS)) {
            continue;
        }
        last_frame_tick = now;

        /* DIAG (~every 2 s): headroom right up to the crash. hwm→0 ⇒ stack overflow (H1);
         * int_free collapsing ⇒ OOM/heap corruption (H3); both stable ⇒ MSPI/PSRAM-stack
         * cache corruption (H2). Cross-reference with the flash coredump backtrace. */
        if ((frame_count++ % 30) == 0) {
            ESP_LOGW(TAG, "diag: vid_hwm=%u words int_largest=%u int_free=%u psram=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }

        uint8_t *obuf = h264_bufs[h264_idx];
        esp_h264_enc_in_frame_t in = {
            .raw_data = { .buffer = frame, .len = frame_len },
            .pts      = pts,
        };
        esp_h264_enc_out_frame_t out = {
            .raw_data = { .buffer = obuf, .len = out_cap },
        };
        pts += (1000 / STREAM_FPS);

        esp_h264_err_t herr = esp_h264_enc_process(enc, &in, &out);
        if (herr == ESP_H264_ERR_OK && out.length > 0) {
            esp_peer_video_frame_t vf = {
                .pts  = out.pts,
                .data = obuf,
                .size = (int)out.length,
            };
            esp_peer_send_video(s_peer, &vf);
            /* Advance the ring — encoder won't touch this slot again for
             * (H264_OUT_BUFS - 1) frames, giving the sender time to finish. */
            h264_idx = (h264_idx + 1) % H264_OUT_BUFS;
        } else if (herr != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG, "encode err %d", herr);
        }
        vTaskDelay(1);
    }

    for (int i = 0; i < H264_OUT_BUFS; i++) heap_caps_free(h264_bufs[i]);
    esp_h264_enc_close(enc);
    esp_h264_enc_del(enc);
    ESP_LOGI(TAG, "Video task exit");
    s_video_task = NULL;
    vTaskDelete(NULL);
}

/* ── esp_peer main loop ──────────────────────────────────────────────────── */
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

/* ── DTLS cert pre-gen (once at boot) ────────────────────────────────────── */
static void cert_pregen_task(void *arg)
{
    ESP_LOGI(TAG, "Pre-generating DTLS certificate...");
    int ret = esp_peer_pre_generate_cert();
    ESP_LOGI(TAG, "DTLS cert pre-gen: %s", ret == ESP_PEER_ERR_NONE ? "ready" : "failed (retry at connect)");
    xSemaphoreGive(s_cert_ready);
    vTaskDelete(NULL);
}

/* ── Session startup worker ──────────────────────────────────────────────── */
/* Cleanly abort a partially-brought-up session (stop was requested while we were
 * still starting). Releases only what we managed to acquire, then clears the flag
 * so webrtc_stream_stop() can finish. */
static volatile uint32_t s_start_gen = 0;

static void start_abort(void)
{
    ESP_LOGW(TAG, "start aborted mid-bringup — cleaning up");
    s_running = false;
    s_connected = false;

    /* Let the loop task (if it was spawned) exit before destroying the peer. */
    for (int i = 0; i < 50 && (s_loop_task || s_video_task); ++i)
        vTaskDelay(pdMS_TO_TICKS(20));

    esp_peer_handle_t peer = s_peer;
    s_peer = NULL;
    if (peer) esp_peer_close(peer);
    camera_csi_stop();
    s_start_in_progress = false;
    vTaskDelete(NULL);
}

static void start_task(void *arg)
{
    uint32_t my_gen = *(uint32_t *)arg;
    free(arg);

    /* Wait for the DTLS cert (fast if pre-gen already finished). */
    xSemaphoreTake(s_cert_ready, portMAX_DELAY);
    xSemaphoreGive(s_cert_ready);

    if (!s_running || my_gen != s_start_gen) { start_abort(); return; }   /* stop arrived before we even started */

    if (camera_csi_start() != ESP_OK) {
        ESP_LOGE(TAG, "camera start failed — aborting session");
        s_running = false;
        s_start_in_progress = false;
        vTaskDelete(NULL);
        return;
    }
    s_stream_w = camera_csi_width();
    s_stream_h = camera_csi_height();

    if (!s_running || my_gen != s_start_gen) { start_abort(); return; }

    s_ice_servers[0] = (esp_peer_ice_server_cfg_t){
        .stun_url = "stun:stun.l.google.com:19302", .user = NULL, .psw = NULL };
    s_ice_servers[1] = (esp_peer_ice_server_cfg_t){
        .stun_url = "turn:13.51.196.176:3478", .user = s_turn_user, .psw = s_turn_psw };
    s_ice_servers[2] = (esp_peer_ice_server_cfg_t){
        .stun_url = "turns:home-secure.glazia.in:5349", .user = s_turn_user, .psw = s_turn_psw };

    esp_peer_cfg_t cfg = {
        .server_lists     = s_ice_servers,
        .server_num       = 3,
        .role             = ESP_PEER_ROLE_CONTROLLING,
        /* POLICY_ALL: keep the fast direct host/srflx path (works for cone-NAT carriers like
         * Vi/Airtel) AND still gather a relay candidate as a fallback. Relay-only was tried and
         * regressed on hardware: it deleted the direct path (broke Vi) and made every session
         * 100% dependent on a lossy TURN allocate to the Stockholm coturn, whose relay candidate
         * also arrives after esp_peer's minimal ICE has already timed out. ALL is a superset of
         * relay-only so it can never be worse. Reaching symmetric-CGNAT (Jio) viewers reliably
         * needs the coturn moved closer (ap-south-1 / Mumbai) — infra, not firmware. */
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .video_info = {
            .codec  = ESP_PEER_VIDEO_CODEC_H264,
            .width  = s_stream_w,
            .height = s_stream_h,
            .fps    = STREAM_FPS,
        },
        .audio_dir         = ESP_PEER_MEDIA_DIR_NONE,
        .video_dir         = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .no_auto_reconnect = true,
        .on_state          = on_state_cb,
        .on_msg            = on_msg_cb,
    };

    if (!s_running || my_gen != s_start_gen) { start_abort(); return; }

    if (esp_peer_open(&cfg, esp_peer_get_default_impl(), &s_peer) != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed");
        camera_csi_stop();
        s_running = false;
        s_start_in_progress = false;
        vTaskDelete(NULL);
        return;
    }
    log_heap("Peer opened");

    if (!s_running || my_gen != s_start_gen) { start_abort(); return; }

    s_loop_task = xTaskCreateStaticPinnedToCore(
        loop_task_fn, "wrtc_loop", 16384, NULL, 5,
        s_loop_stack, &s_loop_tcb, 0);
    if (!s_loop_task) {
        ESP_LOGE(TAG, "loop task create failed");
        start_abort();
        return;
    }

    if (esp_peer_new_connection(s_peer) != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_new_connection failed");
        start_abort();
        return;
    }
    ESP_LOGI(TAG, "WebRTC session started — waiting for answer/ICE");
    s_start_in_progress = false;
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void webrtc_stream_init(void)
{
    esp_log_level_set("AGENT", ESP_LOG_INFO);      /* esp_peer ICE/TURN: candidate types + selected pair (bounded to setup) */
    /* WARN, not INFO: under packet loss the prebuilt esp_peer logs every RTP retransmit as
     * "PEER_DEF: r <seq>" — hundreds of synchronous UART lines in milliseconds, which starves the
     * CPU and triggers HP_SYS_HP_WDT_RESET mid-stream. WARN keeps errors (e.g. "Fail to new
     * connection") without the per-packet flood. Bump back to INFO only for short local debugging. */
    esp_log_level_set("PEER_DEF", ESP_LOG_WARN);
    s_lifecycle_lock = xSemaphoreCreateMutex();
    if (!s_lifecycle_lock) { ESP_LOGE(TAG, "lifecycle mutex alloc failed"); return; }
    s_cert_ready = xSemaphoreCreateBinary();
    if (!s_cert_ready) { ESP_LOGE(TAG, "cert semaphore alloc failed"); return; }

    // s_loop_stack  = heap_caps_aligned_alloc(16, 16384, MALLOC_CAP_SPIRAM);
    // s_video_stack = heap_caps_aligned_alloc(16, WEBRTC_VIDEO_STACK, MALLOC_CAP_SPIRAM);

    s_loop_stack  = heap_caps_aligned_alloc(16, 16384, MALLOC_CAP_INTERNAL);
    s_video_stack = heap_caps_aligned_alloc(16, WEBRTC_VIDEO_STACK, MALLOC_CAP_INTERNAL);

    if (!s_loop_stack || !s_video_stack) {
        ESP_LOGE(TAG, "internal stack alloc failed — streaming unavailable");
        return;
    }

    if (xTaskCreate(cert_pregen_task, "cert_pregen", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cert task create failed");
        xSemaphoreGive(s_cert_ready);
    }
    ESP_LOGI(TAG, "webrtc_stream initialised (PSRAM stacks ready)");
    log_heap("webrtc init");
}

void webrtc_stream_start(const char *turn_user, const char *turn_psw)
{
    if (!s_lifecycle_lock) { ESP_LOGE(TAG, "start: not initialised"); return; }
    xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);

    if (s_running || s_start_in_progress) {
        ESP_LOGW(TAG, "start: already running — ignoring");
        xSemaphoreGive(s_lifecycle_lock);
        return;
    }
    if (!s_loop_stack || !s_video_stack) {
        ESP_LOGE(TAG, "start: not initialised");
        xSemaphoreGive(s_lifecycle_lock);
        return;
    }

    s_start_gen++;
    uint32_t *gen_arg = malloc(sizeof(uint32_t));
    *gen_arg = s_start_gen;

    strlcpy(s_turn_user, turn_user ? turn_user : "", sizeof(s_turn_user));
    strlcpy(s_turn_psw,  turn_psw  ? turn_psw  : "", sizeof(s_turn_psw));

    s_running           = true;
    s_connected         = false;
    s_stopping          = false;
    s_stop_scheduled    = false;
    s_loop_task         = NULL;
    s_video_task        = NULL;
    s_start_in_progress = true;

    if (xTaskCreate(start_task, "wrtc_start", 8192, gen_arg, 4, NULL) != pdPASS) {
        free(gen_arg);
        ESP_LOGE(TAG, "start task create failed");
        s_running = false;
        s_start_in_progress = false;
    }
    xSemaphoreGive(s_lifecycle_lock);
}

void webrtc_stream_on_answer(const char *sdp_str)
{
    if (!s_peer) { ESP_LOGW(TAG, "on_answer: no peer"); return; }
    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)sdp_str,
        .size = (int)strlen(sdp_str),
    };
    if (esp_peer_send_msg(s_peer, &msg) == ESP_PEER_ERR_NONE)
        ESP_LOGI(TAG, "SDP answer forwarded to esp_peer (%u B)", (unsigned)strlen(sdp_str));
    else
        ESP_LOGE(TAG, "forward SDP answer failed");
}

void webrtc_stream_on_ice(const char *cand_str)
{
    if (!s_peer) { ESP_LOGW(TAG, "on_ice: no peer"); return; }
    ESP_LOGI(TAG, "Remote ICE from server (%u B): %.80s", (unsigned)strlen(cand_str), cand_str);
    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_CANDIDATE,
        .data = (uint8_t *)cand_str,
        .size = (int)strlen(cand_str),
    };
    if (esp_peer_send_msg(s_peer, &msg) != ESP_PEER_ERR_NONE)
        ESP_LOGW(TAG, "forward ICE candidate failed");
}

void webrtc_stream_resend_pending_offer(void)
{
    if (s_pending_offer && s_running) {
        ESP_LOGI(TAG, "Resending pending offer on WS reconnect");
        hub_control_ws_send_json(s_pending_offer);
    }
}

void webrtc_stream_stop(void)
{
    if (!s_lifecycle_lock) return;
    xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);

    /* Signal both a running session and any in-flight start_task to wind down. */
    s_stopping = true;
    bool was_connected = s_connected;
    s_running = false;
    s_connected = false;

    /* If start_task is still bringing the session up, let it reach an abort check
     * and release the camera/peer itself before we touch anything. */
    for (int i = 0; i < 100 && s_start_in_progress; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));

    if (!s_peer && !s_pending_offer) {
        /* Nothing (left) to tear down — start_abort already handled it. */
        s_stopping = false;
        xSemaphoreGive(s_lifecycle_lock);
        return;
    }

    /* Let the video + loop tasks exit before destroying what they touch. */
    for (int i = 0; i < 50 && (s_loop_task || s_video_task); ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_peer_handle_t peer = s_peer;
    s_peer = NULL;
    if (peer) {
        if (was_connected) esp_peer_disconnect(peer);
        esp_peer_close(peer);
    }

    camera_csi_stop();

    free(s_pending_offer);
    s_pending_offer = NULL;

    s_stopping = false;
    ESP_LOGI(TAG, "WebRTC stopped");
    log_heap("after stop");
    xSemaphoreGive(s_lifecycle_lock);
}

static void deferred_stop_task(void *arg)
{
    (void)arg;
    s_stop_scheduled = false;
    webrtc_stream_stop();
    vTaskDelete(NULL);
}

/* Schedule teardown on a dedicated worker with a stack large enough for the
 * esp_peer_close()/DTLS/mbedTLS unwind. webrtc_stream_stop() must NEVER run on
 * the websocket client task: its 12 KB stack is already carrying WSS TLS + the
 * RX buffer + cJSON, and stacking the deep close on top overruns it — the
 * end-of-stack watchpoint (CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK) then trips
 * an MCAUSE=3 Breakpoint. s_stop_scheduled de-dupes overlapping requests. */
void webrtc_stream_request_stop(void)
{
    if (s_stop_scheduled) return;
    s_stop_scheduled = true;
    if (xTaskCreate(deferred_stop_task, "wrtc_stop", 12288, NULL, 6, NULL) != pdPASS)
        s_stop_scheduled = false;
}

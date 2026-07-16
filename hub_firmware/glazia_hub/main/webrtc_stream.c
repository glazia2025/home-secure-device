#include "webrtc_stream.h"
#include "esp_log.h"

/* WebRTC is handled by the cam_esp coprocessor, not the hub.
 * These stubs keep the build clean; no esp_peer or esp_h264 code runs on hub. */

static const char *TAG = "WEBRTC";

void webrtc_stream_controller_init(void)
{
    ESP_LOGW(TAG, "webrtc_stream_controller_init: no-op — cam_esp handles WebRTC");
}

void webrtc_trigger_start(void)
{
    ESP_LOGW(TAG, "webrtc_trigger_start: no-op — cam_esp handles WebRTC");
}

void webrtc_trigger_stop(void)
{
    ESP_LOGW(TAG, "webrtc_trigger_stop: no-op — cam_esp handles WebRTC");
}

void webrtc_stream_init(void)
{
    ESP_LOGW(TAG, "webrtc_stream_init: no-op — cam_esp handles WebRTC");
}

void webrtc_stream_on_viewer_ready(void)
{
    ESP_LOGW(TAG, "webrtc_stream_on_viewer_ready: no-op — cam_esp handles WebRTC");
}

void webrtc_stream_on_answer(const char *sdp_str, int sdp_len)
{
    (void)sdp_str;
    (void)sdp_len;
    ESP_LOGW(TAG, "webrtc_stream_on_answer: no-op — cam_esp handles WebRTC");
}

void webrtc_stream_on_ice_candidate(const char *cand_str, int cand_len)
{
    (void)cand_str;
    (void)cand_len;
    ESP_LOGW(TAG, "webrtc_stream_on_ice_candidate: no-op — cam_esp handles WebRTC");
}

void webrtc_stream_stop(void)
{
    ESP_LOGW(TAG, "webrtc_stream_stop: no-op — cam_esp handles WebRTC");
}

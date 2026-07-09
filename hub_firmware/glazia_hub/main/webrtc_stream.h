#pragma once

void webrtc_stream_controller_init(void);
void webrtc_trigger_start(void);
// Add this to webrtc_stream.h

/* Lazily pre-generates the DTLS certificate before the first WebRTC session. */
void webrtc_stream_init(void);

/* Called from hub_control_ws when { "type": "viewer-ready" } arrives. */
void webrtc_stream_on_viewer_ready(void);

/* Feed the SDP answer string (raw SDP, not JSON-wrapped) from the mobile viewer. */
void webrtc_stream_on_answer(const char *sdp_str, int sdp_len);

/* Feed an ICE candidate string from the mobile viewer. */
void webrtc_stream_on_ice_candidate(const char *cand_str, int cand_len);

/* Stop streaming and close the peer connection. Safe to call when not streaming. */
void webrtc_stream_stop(void);

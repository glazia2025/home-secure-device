#pragma once

/* On-P4 WebRTC video streamer.
 *
 * Replaces the old camera co-processor (cam_uart.c): the ESP32-P4 now runs the
 * whole WebRTC peer itself — OV5647 (camera_csi.c) → hardware H.264 → esp_peer.
 * Signaling is unchanged and still server-relayed: this module emits the SDP
 * offer / local ICE candidates through hub_control_ws_send_json(), and the
 * hub_control_ws.c handler feeds the answer / remote ICE back in via the
 * on_answer / on_ice entry points below.
 *
 * The public surface mirrors what hub_control_ws.c previously called on
 * cam_uart, so wiring the two together is a near drop-in swap.
 */

/* Call once at boot: pre-generate the DTLS cert and reserve PSRAM task stacks so
 * a session can start quickly and without internal-RAM pressure. */
void webrtc_stream_init(void);

/* A viewer connected — bring up the camera + peer and start offering. TURN
 * credentials come from the hub's NVS-loaded g_turn_user/g_turn_psw. No WiFi args
 * are needed: the P4 is already associated. Non-blocking (spawns a worker). */
void webrtc_stream_start(const char *turn_user, const char *turn_psw);

/* The viewer left, the control WS dropped, or we go offline — tear the session
 * down (peer + camera) and return heap to the idle baseline. Idempotent. */
void webrtc_stream_stop(void);

/* Remote SDP answer received on the control WS (raw SDP string). */
void webrtc_stream_on_answer(const char *sdp_str);

/* Remote ICE candidate received on the control WS (raw candidate string). */
void webrtc_stream_on_ice(const char *cand_str);

/* Control WS (re)connected — resend the last offer if one is pending, so a viewer
 * that was waiting while the socket was down still gets it. */
void webrtc_stream_resend_pending_offer(void);

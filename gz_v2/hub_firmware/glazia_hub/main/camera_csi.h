#pragma once

/* OV5647 → MIPI-CSI → ISP → YUV422 capture on the ESP32-P4.
 *
 * Replaces the old external camera co-processor: the P4 now captures natively
 * from an OV5647 on its MIPI-CSI connector. The ISP converts the sensor's RAW8
 * to packed YUV422 (UYVY byte order), which the P4 hardware H.264 encoder
 * ingests directly (see webrtc_stream.c). No CPU pixel conversion.
 *
 * Lifecycle is on-demand: camera_csi_start() is called when a viewer connects
 * and camera_csi_stop() when they leave, so the CSI/ISP/LDO and the large PSRAM
 * frame buffer only exist while streaming.
 */

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Bring up LDO + SCCB + OV5647 + CSI controller + ISP and start streaming.
 * On success the negotiated frame geometry is available via camera_csi_width()/
 * camera_csi_height(). Safe to call again after camera_csi_stop(). */
esp_err_t camera_csi_start(void);

/* Tear everything down and free the frame buffer. Idempotent. */
void camera_csi_stop(void);

/* Block until the next captured frame is ready, then hand back a pointer to the
 * YUV422 (UYVY) frame buffer and its length. The buffer is owned by the driver
 * and valid until the next camera_csi_get_frame() call. Returns ESP_OK on a
 * fresh frame. */
esp_err_t camera_csi_get_frame(uint8_t **out_buf, size_t *out_len);

/* Negotiated capture geometry (valid only between start and stop). These are the
 * OV5647 CSI-mode dimensions the ISP emits and the H.264 encoder must match. */
int camera_csi_width(void);
int camera_csi_height(void);

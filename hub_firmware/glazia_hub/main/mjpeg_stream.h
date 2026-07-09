#pragma once

/* Start the MJPEG frame capture-and-send loop.
 * Safe to call from any task; no-op if already running. */
void mjpeg_stream_start(void);

/* Stop the MJPEG frame loop.
 * Returns immediately; the task self-deletes after completing the current frame. */
void mjpeg_stream_stop(void);

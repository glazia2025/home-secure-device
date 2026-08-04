#pragma once

// Opens WebSocket to server and waits for SENSOR_PAIRED message
// When sensor MAC received, triggers ESP-NOW pairing automatically
void websocket_start(void);

// Close the WebSocket
void websocket_stop(void);

/* esp_peer (esp-webrtc-solution) prebuilt libpeer_default.a for esp32p4 calls an
 * external `esp_log(level, tag, format, ...)` for its internal logging. On the
 * ESP-ADF/esp-media-lib builds this is provided by media_lib_sal; a plain
 * ESP-IDF app doesn't have it, so the link fails with `undefined reference to
 * esp_log`.
 *
 * Disassembly of the prebuilt shows the call convention is identical to IDF's
 * esp_log_write — (esp_log_level_t level, const char *tag, const char *format,
 * <timestamp>, ...) with the same level numbering (1=ERROR, 3=INFO). So this is
 * a one-line forward to esp_log_writev(). */

#include <stdarg.h>
#include "esp_log.h"

void esp_log(esp_log_level_t level, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    esp_log_writev(level, tag, format, args);
    va_end(args);
}

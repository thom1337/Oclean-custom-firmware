#pragma once
#include <stddef.h>
#include "esp_log.h"

// Capture ESP-IDF log output into a RAM ring buffer so it can be read back over
// HTTP (the web UI "Logs" tab) on a device with no serial access. The original
// log sink (UART) is kept, so nothing is lost there either.
void weblog_init(void);

// Copy log bytes written since *cursor into out (NUL-terminated, at most max-1
// bytes), then advance *cursor to the newest position. A cursor of 0 starts from
// the oldest retained byte. Returns the number of bytes copied.
size_t weblog_read(char *out, size_t max, size_t *cursor);

// Set the runtime log level for a tag ("*" = all), e.g. to raise to VERBOSE.
void weblog_set_level(const char *tag, esp_log_level_t level);

#include "weblog.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LOGBUF 16384        // last ~16 KB of log text kept in RAM

static char s_buf[LOGBUF];
static size_t s_total;      // total bytes ever written (monotonic; wraps into s_buf)
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev;   // original sink (UART), kept so console output continues

// Installed via esp_log_set_vprintf; called (task context only) for every ESP_LOGx.
// Must never recurse into ESP_LOGx and must be cheap and crash-proof.
static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int r = s_prev ? s_prev(fmt, ap2) : 0;   // keep the UART output
    va_end(ap2);

    // Capture a formatted copy. Skip from ISR context or before init (can't lock).
    if (s_lock && !xPortInIsrContext()) {
        char line[256];
        int n = vsnprintf(line, sizeof(line), fmt, ap);
        if (n > 0) {
            if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
            if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
                for (int i = 0; i < n; i++) s_buf[s_total++ % LOGBUF] = line[i];
                xSemaphoreGive(s_lock);
            }
        }
    }
    return r;
}

void weblog_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    s_prev = esp_log_set_vprintf(log_hook);
}

size_t weblog_read(char *out, size_t max, size_t *cursor)
{
    if (max == 0) return 0;
    out[0] = 0;
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t total = s_total;
    size_t oldest = total > LOGBUF ? total - LOGBUF : 0;
    size_t from = *cursor;
    if (from < oldest) from = oldest;   // requested data already overwritten
    if (from > total)  from = oldest;   // cursor from a previous boot -> restart
    size_t avail = total - from;
    if (avail > max - 1) { from = total - (max - 1); avail = max - 1; }
    for (size_t i = 0; i < avail; i++) out[i] = s_buf[(from + i) % LOGBUF];
    out[avail] = 0;
    *cursor = total;
    xSemaphoreGive(s_lock);
    return avail;
}

void weblog_set_level(const char *tag, esp_log_level_t level)
{
    esp_log_level_set(tag, level);
}

// Host stand-in for ESP-IDF's esp_timer.h (input-module simulators): one-shot timers
// on a virtual clock that the simulator advances.
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef void (*esp_timer_cb_t)(void *arg);
typedef struct { esp_timer_cb_t callback; void *arg; const char *name; } esp_timer_create_args_t;
typedef struct sim_timer *esp_timer_handle_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
int64_t   esp_timer_get_time(void);

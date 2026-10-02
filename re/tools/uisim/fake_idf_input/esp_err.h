// Host stand-in for ESP-IDF's esp_err.h (input-module simulators).
#pragma once
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK                0
#define ESP_FAIL              (-1)
#define ESP_ERR_INVALID_STATE 0x103
static inline const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "error"; }
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) abort(); } while (0)

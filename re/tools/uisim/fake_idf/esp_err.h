// Host stand-in for ESP-IDF's esp_err.h (re/tools/uisim): just enough for main/hw_led.c.
#pragma once
typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL (-1)

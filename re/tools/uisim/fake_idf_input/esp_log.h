// Host stand-in for ESP-IDF's esp_log.h (input-module simulators): errors and
// warnings go to stderr when SIM_VERBOSE is set, the rest is dropped.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#define SIM_LOG_(lvl, tag, fmt, ...) do { if (getenv("SIM_VERBOSE")) fprintf(stderr, lvl " %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) SIM_LOG_("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) SIM_LOG_("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) SIM_LOG_("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)(tag))

#include "metrics.h"
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "driver/temperature_sensor.h"

static brush_state_t s_brush = {
    .battery_pct = -1, .mode = -1, .last_session_secs = -1,
    .brush_score = -1, .brush_head_days = -1, .pressure = -1,
    .imu_temp_c = NAN,
};
static SemaphoreHandle_t s_lock;
static temperature_sensor_handle_t s_temp;

static void ensure_lock(void) { if (!s_lock) s_lock = xSemaphoreCreateMutex(); }

void metrics_set_brush_state(const brush_state_t *s)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_brush = *s;
    xSemaphoreGive(s_lock);
}

void metrics_get_brush_state(brush_state_t *out)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_brush;
    xSemaphoreGive(s_lock);
}

// Static definition of every metric we publish. "All possible metrics" =
// full device/system health + Wi-Fi link + the brush-domain values.
static const metric_def_t DEFS[] = {
    // --- brush domain ---
    {"battery",          "Battery",             "%",   "battery",     "measurement", "mdi:battery",        M_INT},
    {"charging",         "Charging",            NULL,  "battery_charging", NULL,     "mdi:power-plug",     M_BOOL},
    {"brushing",         "Brushing",            NULL,  "running",     NULL,          "mdi:toothbrush",     M_BOOL},
    {"mode",             "Cleaning Mode",       NULL,  NULL,          NULL,          "mdi:tune",           M_INT},
    {"last_session_secs","Last Session",        "s",   "duration",    "measurement", "mdi:timer",          M_INT},
    {"last_session_time","Last Session Time",   NULL,  "timestamp",   NULL,          "mdi:clock",          M_STR},
    {"brush_score",      "Brush Score",         NULL,  NULL,          "measurement", "mdi:star",           M_INT},
    {"total_sessions",   "Total Sessions",      NULL,  NULL,          "total_increasing","mdi:counter",    M_INT},
    {"brush_head_days",  "Brush Head Age",      "d",   "duration",    "measurement", "mdi:toothbrush-paste",M_INT},
    {"pressure",         "Brush Pressure",      NULL,  NULL,          "measurement", "mdi:gauge",          M_INT},
    {"imu_temp",         "Brush Temperature",   "°C",  "temperature", "measurement", "mdi:thermometer",    M_FLOAT},
    {"fw_version",       "Firmware Version",    NULL,  NULL,          NULL,          "mdi:chip",           M_STR},
    // --- system health ---
    {"uptime",           "Uptime",              "s",   "duration",    "total_increasing","mdi:timer-outline",M_INT},
    {"free_heap",        "Free Heap",           "B",   "data_size",   "measurement", "mdi:memory",         M_INT},
    {"min_free_heap",    "Min Free Heap",       "B",   "data_size",   "measurement", "mdi:memory",         M_INT},
    {"largest_block",    "Largest Free Block",  "B",   "data_size",   "measurement", "mdi:memory",         M_INT},
    {"task_count",       "Task Count",          NULL,  NULL,          "measurement", "mdi:format-list-numbered", M_INT},
    {"cpu_temp",         "CPU Temperature",     "°C",  "temperature", "measurement", "mdi:thermometer",    M_FLOAT},
    {"reset_reason",     "Last Reset Reason",   NULL,  NULL,          NULL,          "mdi:restart",        M_STR},
    {"idf_version",      "ESP-IDF Version",     NULL,  NULL,          NULL,          "mdi:information",     M_STR},
    {"app_version",      "App Version",         NULL,  NULL,          NULL,          "mdi:information",     M_STR},
    {"flash_size",       "Flash Size",          "B",   "data_size",   NULL,          "mdi:chip",           M_INT},
    // --- wifi link ---
    {"wifi_rssi",        "Wi-Fi RSSI",          "dBm", "signal_strength","measurement","mdi:wifi",         M_INT},
    {"wifi_ssid",        "Wi-Fi SSID",          NULL,  NULL,          NULL,          "mdi:wifi",           M_STR},
    {"wifi_ip",          "IP Address",          NULL,  NULL,          NULL,          "mdi:ip-network",     M_STR},
    {"wifi_channel",     "Wi-Fi Channel",       NULL,  NULL,          "measurement", "mdi:wifi",           M_INT},
    {"mac",              "MAC Address",         NULL,  NULL,          NULL,          "mdi:network",        M_STR},
};

const metric_def_t *metrics_defs(size_t *count)
{
    *count = sizeof(DEFS) / sizeof(DEFS[0]);
    return DEFS;
}

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power_on";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT:      return "other_wdt";
        case ESP_RST_DEEPSLEEP:return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO:     return "sdio";
        default:               return "unknown";
    }
}

static float read_cpu_temp(void)
{
    if (!s_temp) {
        temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&tcfg, &s_temp) != ESP_OK) return 0;
        temperature_sensor_enable(s_temp);
    }
    float t = 0;
    temperature_sensor_get_celsius(s_temp, &t);
    return t;
}

cJSON *metrics_build_state_json(void)
{
    brush_state_t b; metrics_get_brush_state(&b);
    cJSON *o = cJSON_CreateObject();

    // brush domain (use null when unknown so HA shows "unavailable")
    if (b.battery_pct >= 0) cJSON_AddNumberToObject(o, "battery", b.battery_pct);
    else cJSON_AddNullToObject(o, "battery");
    cJSON_AddStringToObject(o, "charging", b.charging ? "ON" : "OFF");
    cJSON_AddStringToObject(o, "brushing", b.brushing ? "ON" : "OFF");
    if (b.mode >= 0) cJSON_AddNumberToObject(o, "mode", b.mode); else cJSON_AddNullToObject(o, "mode");
    if (b.last_session_secs >= 0) cJSON_AddNumberToObject(o, "last_session_secs", b.last_session_secs);
    else cJSON_AddNullToObject(o, "last_session_secs");
    if (b.last_session_epoch) {
        char ts[32]; time_t t = b.last_session_epoch; struct tm tmv; gmtime_r(&t, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S+00:00", &tmv);
        cJSON_AddStringToObject(o, "last_session_time", ts);
    } else cJSON_AddNullToObject(o, "last_session_time");
    if (b.brush_score >= 0) cJSON_AddNumberToObject(o, "brush_score", b.brush_score);
    else cJSON_AddNullToObject(o, "brush_score");
    cJSON_AddNumberToObject(o, "total_sessions", b.total_sessions);
    if (b.brush_head_days >= 0) cJSON_AddNumberToObject(o, "brush_head_days", b.brush_head_days);
    else cJSON_AddNullToObject(o, "brush_head_days");
    if (b.pressure >= 0) cJSON_AddNumberToObject(o, "pressure", b.pressure); else cJSON_AddNullToObject(o, "pressure");
    if (!isnan(b.imu_temp_c)) cJSON_AddNumberToObject(o, "imu_temp", b.imu_temp_c); else cJSON_AddNullToObject(o, "imu_temp");
    cJSON_AddStringToObject(o, "fw_version", b.fw_version[0] ? b.fw_version : "unknown");

    // system
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(o, "largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(o, "task_count", uxTaskGetNumberOfTasks());
    cJSON_AddNumberToObject(o, "cpu_temp", read_cpu_temp());
    cJSON_AddStringToObject(o, "reset_reason", reset_reason_str());
    cJSON_AddStringToObject(o, "idf_version", esp_get_idf_version());
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(o, "app_version", app ? app->version : "unknown");
    uint32_t fsz = 0; esp_flash_get_size(NULL, &fsz);
    cJSON_AddNumberToObject(o, "flash_size", fsz);

    // wifi
    wifi_ap_record_t ap; char ip[16] = "0.0.0.0";
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(o, "wifi_rssi", ap.rssi);
        cJSON_AddStringToObject(o, "wifi_ssid", (char *)ap.ssid);
        cJSON_AddNumberToObject(o, "wifi_channel", ap.primary);
    } else {
        cJSON_AddNullToObject(o, "wifi_rssi");
        cJSON_AddStringToObject(o, "wifi_ssid", "");
        cJSON_AddNullToObject(o, "wifi_channel");
    }
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;
    if (nif && esp_netif_get_ip_info(nif, &ipi) == ESP_OK) {
        esp_ip4addr_ntoa(&ipi.ip, ip, sizeof(ip));
    }
    cJSON_AddStringToObject(o, "wifi_ip", ip);
    uint8_t mac[6]; char macs[18];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(o, "mac", macs);
    return o;
}

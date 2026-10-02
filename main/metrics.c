#include "metrics.h"
#include "oem_hal.h"
#include "oem_api.h"
#include "oem_glue.h"
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

static char s_fw_version[16];
static temperature_sensor_handle_t s_temp;

void metrics_set_fw_version(const char *v) { strlcpy(s_fw_version, v, sizeof s_fw_version); }
const char *metrics_fw_version(void) { return s_fw_version; }

// Snapshot of the brush state kept by the oem core (g_oem), taken under the core
// lock so the web UI, MQTT and BLE see consistent values.
void metrics_get_brush_state(brush_state_t *b)
{
    memset(b, 0, sizeof *b);
    hal_lock();
    b->battery_pct  = g_oem.batt_fault ? -1 : g_oem.batt_pct;
    b->battery_mv   = g_oem.batt_mv;
    b->power_state  = g_oem.power_state;
    b->charging     = g_oem.power_state == OEM_PWR_CHARGING;
    b->brushing     = g_oem.session_active && g_oem.running;
    b->paused       = g_oem.session_active && !g_oem.running;
    b->mode         = g_oem.profile[6];
    b->strength     = hal_rtc()->strength;
    b->session_secs = g_oem.done_s;
    b->session_total= g_oem.total_s;
    b->brush_score  = g_oem.score == 0xFF ? -1 : g_oem.score;
    b->sessions_today = hal_rtc()->hist_count;
    b->seconds_today  = hal_rtc()->hist_seconds;
    b->screen       = g_oem.now_ui;
    b->asleep       = g_oem.asleep;
    b->locked       = g_oem.locked;
    b->pressure     = g_oem.pressure;
    b->touch_state  = oem_touch_state();
    b->lang         = g_oem.lang;
    // The main task's last reading (NAN: none, or safe mode). Not oem_imu_temp(): the
    // IMU's SPI device is the main task's alone, and a read busy-waits 3 ms, with the
    // core lock held, in whichever task asks (MQTT discovery asked 37 times in a row).
    b->imu_temp_c   = oem_glue_imu_temp();
    hal_unlock();
    strlcpy(b->fw_version, s_fw_version, sizeof b->fw_version);
}

// Static definition of every metric we publish. "All possible metrics" =
// full device/system health + Wi-Fi link + the brush-domain values.
static const metric_def_t DEFS[] = {
    // --- brush domain ---
    {"battery",          "Battery",             "%",   "battery",     "measurement", "mdi:battery",        M_INT},
    {"battery_mv",       "Battery Voltage",     "mV",  "voltage",     "measurement", "mdi:battery",        M_INT},
    {"charging",         "Charging",            NULL,  "battery_charging", NULL,     "mdi:power-plug",     M_BOOL},
    {"power_state",      "Power State",         NULL,  NULL,          NULL,          "mdi:power-plug",     M_STR},
    {"brushing",         "Brushing",            NULL,  "running",     NULL,          "mdi:toothbrush",     M_BOOL},
    {"paused",           "Paused",              NULL,  NULL,          NULL,          "mdi:pause",          M_BOOL},
    {"mode",             "Cleaning Mode",       NULL,  NULL,          NULL,          "mdi:tune",           M_INT},
    {"strength",         "Intensity",           NULL,  NULL,          NULL,          "mdi:speedometer",    M_INT},
    {"session_secs",     "Session Elapsed",     "s",   "duration",    "measurement", "mdi:timer",          M_INT},
    {"session_total",    "Session Length",      "s",   "duration",    NULL,          "mdi:timer-sand",     M_INT},
    {"brush_score",      "Brush Score",         NULL,  NULL,          "measurement", "mdi:star",           M_INT},
    {"sessions_today",   "Sessions Today",      NULL,  NULL,          "measurement", "mdi:counter",        M_INT},
    {"seconds_today",    "Brushed Today",       "s",   "duration",    "measurement", "mdi:clock",          M_INT},
    {"screen",           "Screen",              NULL,  NULL,          NULL,          "mdi:cellphone",      M_INT},
    {"asleep",           "Screen Off",          NULL,  NULL,          NULL,          "mdi:sleep",          M_BOOL},
    {"locked",           "Touch Lock",          NULL,  "lock",        NULL,          "mdi:lock",           M_BOOL},
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
    cJSON_AddNumberToObject(o, "battery_mv", b.battery_mv);
    cJSON_AddStringToObject(o, "charging", b.charging ? "ON" : "OFF");
    cJSON_AddStringToObject(o, "power_state", b.power_state == OEM_PWR_CHARGING ? "charging"
                            : b.power_state == OEM_PWR_FULL ? "full" : "battery");
    cJSON_AddStringToObject(o, "brushing", b.brushing ? "ON" : "OFF");
    cJSON_AddStringToObject(o, "paused", b.paused ? "ON" : "OFF");
    cJSON_AddNumberToObject(o, "mode", b.mode);
    cJSON_AddNumberToObject(o, "strength", b.strength);
    cJSON_AddNumberToObject(o, "session_secs", b.session_secs);
    cJSON_AddNumberToObject(o, "session_total", b.session_total);
    if (b.brush_score >= 0) cJSON_AddNumberToObject(o, "brush_score", b.brush_score);
    else cJSON_AddNullToObject(o, "brush_score");
    cJSON_AddNumberToObject(o, "sessions_today", b.sessions_today);
    cJSON_AddNumberToObject(o, "seconds_today", b.seconds_today);
    cJSON_AddNumberToObject(o, "screen", b.screen);
    cJSON_AddStringToObject(o, "asleep", b.asleep ? "ON" : "OFF");
    cJSON_AddStringToObject(o, "locked", b.locked ? "ON" : "OFF");
    cJSON_AddNumberToObject(o, "pressure", b.pressure);
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

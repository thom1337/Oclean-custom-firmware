#include "config_store.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "config";
#define NS "oclean"

void config_defaults(app_config_t *o)
{
    memset(o, 0, sizeof(*o));
    o->mqtt_enabled = false;
    o->mqtt_port = 1883;
    o->mqtt_tls = false;
    strncpy(o->mqtt_base_topic, "oclean", sizeof(o->mqtt_base_topic) - 1);
    strncpy(o->mqtt_discovery_prefix, "homeassistant", sizeof(o->mqtt_discovery_prefix) - 1);
    strncpy(o->device_name, "Oclean X Ultra", sizeof(o->device_name) - 1);
    o->publish_interval_s = 30;
}

static void get_str(nvs_handle_t h, const char *k, char *buf, size_t len)
{
    size_t l = len;
    if (nvs_get_str(h, k, buf, &l) != ESP_OK) {
        // leave whatever default was already in buf
    }
}

void config_load(app_config_t *o)
{
    config_defaults(o);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no saved config, using defaults");
        return;
    }
    get_str(h, "wifi_ssid", o->wifi_ssid, sizeof(o->wifi_ssid));
    get_str(h, "wifi_pass", o->wifi_pass, sizeof(o->wifi_pass));
    get_str(h, "mqtt_host", o->mqtt_host, sizeof(o->mqtt_host));
    get_str(h, "mqtt_user", o->mqtt_user, sizeof(o->mqtt_user));
    get_str(h, "mqtt_pass", o->mqtt_pass, sizeof(o->mqtt_pass));
    get_str(h, "mqtt_base", o->mqtt_base_topic, sizeof(o->mqtt_base_topic));
    get_str(h, "mqtt_disc", o->mqtt_discovery_prefix, sizeof(o->mqtt_discovery_prefix));
    get_str(h, "dev_name", o->device_name, sizeof(o->device_name));

    uint8_t en = o->mqtt_enabled, tls = o->mqtt_tls;
    nvs_get_u8(h, "mqtt_en", &en);  o->mqtt_enabled = en;
    nvs_get_u8(h, "mqtt_tls", &tls); o->mqtt_tls = tls;
    nvs_get_u16(h, "mqtt_port", &o->mqtt_port);
    nvs_get_u16(h, "pub_int", &o->publish_interval_s);
    nvs_close(h);
    ESP_LOGI(TAG, "config loaded (mqtt=%s host=%s:%u)",
             o->mqtt_enabled ? "on" : "off", o->mqtt_host, o->mqtt_port);
}

bool config_save(const app_config_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = true;
    ok &= nvs_set_str(h, "wifi_ssid", c->wifi_ssid) == ESP_OK;
    ok &= nvs_set_str(h, "wifi_pass", c->wifi_pass) == ESP_OK;
    ok &= nvs_set_str(h, "mqtt_host", c->mqtt_host) == ESP_OK;
    ok &= nvs_set_str(h, "mqtt_user", c->mqtt_user) == ESP_OK;
    ok &= nvs_set_str(h, "mqtt_pass", c->mqtt_pass) == ESP_OK;
    ok &= nvs_set_str(h, "mqtt_base", c->mqtt_base_topic) == ESP_OK;
    ok &= nvs_set_str(h, "mqtt_disc", c->mqtt_discovery_prefix) == ESP_OK;
    ok &= nvs_set_str(h, "dev_name", c->device_name) == ESP_OK;
    ok &= nvs_set_u8(h, "mqtt_en", c->mqtt_enabled) == ESP_OK;
    ok &= nvs_set_u8(h, "mqtt_tls", c->mqtt_tls) == ESP_OK;
    ok &= nvs_set_u16(h, "mqtt_port", c->mqtt_port) == ESP_OK;
    ok &= nvs_set_u16(h, "pub_int", c->publish_interval_s) == ESP_OK;
    if (ok) ok &= nvs_commit(h) == ESP_OK;
    nvs_close(h);
    ESP_LOGI(TAG, "config save %s", ok ? "ok" : "FAILED");
    return ok;
}

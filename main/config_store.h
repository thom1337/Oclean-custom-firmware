#pragma once
#include <stdbool.h>
#include <stdint.h>

// Persistent configuration, stored in NVS namespace "oclean".
typedef struct {
    // Wi-Fi station credentials (so the device can reach the MQTT broker / HA).
    char     wifi_ssid[33];
    char     wifi_pass[65];
    // MQTT broker settings.
    bool     mqtt_enabled;
    char     mqtt_host[64];
    uint16_t mqtt_port;          // e.g. 1883 (plain) or 8883 (TLS)
    bool     mqtt_tls;
    char     mqtt_user[64];
    char     mqtt_pass[64];
    char     mqtt_base_topic[48];      // e.g. "oclean"
    char     mqtt_discovery_prefix[32]; // Home Assistant discovery prefix, e.g. "homeassistant"
    char     device_name[32];          // friendly name shown in HA, e.g. "Oclean X Ultra"
    uint16_t publish_interval_s;       // state publish cadence
    // Time zone of the brush's clock as a POSIX TZ string, e.g.
    // "CET-1CEST,M3.5.0,M10.5.0/3" (there is no zone database on the device, so not a
    // name like "Europe/Berlin"). The day totals, the morning / evening mode and the
    // greeting dates follow it; SNTP only delivers UTC.
    char     tz[48];
} app_config_t;

#define APP_CONFIG_TZ_DEFAULT "UTC0"

// Load config from NVS into *out, filling defaults for any missing key.
void config_load(app_config_t *out);

// Persist *cfg to NVS. Returns true on success.
bool config_save(const app_config_t *cfg);

// Fill *out with factory defaults (does not touch NVS).
void config_defaults(app_config_t *out);

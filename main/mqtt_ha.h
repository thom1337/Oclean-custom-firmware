#pragma once
#include "config_store.h"
// Start (or restart) the MQTT client with Home Assistant auto-discovery.
// Safe to call again after settings change; it tears down the old client first.
void mqtt_ha_start(const app_config_t *cfg);
void mqtt_ha_restart(const app_config_t *cfg);
bool mqtt_ha_is_connected(void);
// Force an immediate state publish (also called periodically on a timer).
void mqtt_ha_publish_state(void);

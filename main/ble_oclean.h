#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "config_store.h"

// Start the NimBLE central that connects to the Oclean brush and bridges its
// GATT protocol (service 8082caa8…) into metrics_set_brush_state().
void ble_oclean_start(const app_config_t *cfg);
bool ble_oclean_is_connected(void);

// Send a raw command opcode to the brush command characteristic (bb85).
esp_err_t ble_oclean_send_cmd(const uint8_t *data, size_t len);

// Convenience actions used by the MQTT command layer (all non-destructive).
esp_err_t ble_oclean_sync_time(void);        // 0201 + datetime
esp_err_t ble_oclean_reset_head(void);       // 020F
esp_err_t ble_oclean_set_brightness(uint8_t pct);  // 020C + value
esp_err_t ble_oclean_wake_gesture(bool on);  // 0209 + 0x01/0xEC
esp_err_t ble_oclean_request_status(void);   // 0303
esp_err_t ble_oclean_request_sessions(void); // 0307

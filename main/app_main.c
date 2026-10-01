#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#include "esp_sntp.h"

#include "config_store.h"
#include "fs_storage.h"
#include "wifi_mgr.h"
#include "mqtt_ha.h"
#include "web_server.h"
#include "metrics.h"
#include "hardware.h"
#include "ble_server.h"

static const char *TAG = "app";

// Seed the FW version so Home Assistant and the web UI show it; the hardware
// task fills in battery / temperature / brushing state as it samples them.
static void seed_brush_state(void)
{
    brush_state_t b; metrics_get_brush_state(&b);
    const esp_app_desc_t *d = esp_app_get_description();
    if (d) strlcpy(b.fw_version, d->version, sizeof(b.fw_version));
    metrics_set_brush_state(&b);
}

void app_main(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    app_config_t cfg; config_load(&cfg);

    if (!fs_storage_mount()) ESP_LOGW(TAG, "storage mount failed; file browser will be empty");
    seed_brush_state();

    // On-device hardware: sensors, buttons, LEDs, display, and the motor and
    // charge rails. All driven from boot, though some motor/charge pins are only
    // "likely" — see re/HARDWARE_MAP.md.
    hardware_init();
    hardware_start();

    wifi_mgr_start(&cfg);
    web_server_start();      // reachable on STA IP or the setup AP (192.168.4.1)

    // SNTP so time-sync to the brush and session timestamps are correct.
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    mqtt_ha_start(&cfg);
    ble_server_start();      // serve the Oclean GATT service so the phone app works

    // Mark this OTA image valid so rollback won't revert us after a healthy boot.
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA image marked valid");
    }

    ESP_LOGI(TAG, "oclean custom firmware up: wifi+web+mqtt started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

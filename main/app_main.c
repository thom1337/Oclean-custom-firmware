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

    // If a previous OTA selected a slot the bootloader could not boot, it fell back
    // to this (working) image; realign the boot selection to the running slot and
    // mark it valid so rollback has a correct target for the next update.
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot && boot != run) {
        ESP_LOGW(TAG, "otadata named a slot that did not boot; re-selecting %s", run->label);
        if (esp_ota_set_boot_partition(run) == ESP_OK) esp_ota_mark_app_valid_cancel_rollback();
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
    bool web_ok = web_server_start();   // reachable on STA IP or the setup AP (192.168.4.1)

    // SNTP so session timestamps are correct.
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    mqtt_ha_start(&cfg);
    ble_server_start();      // serve the Oclean GATT service so the phone app works

    // Confirm a freshly-OTA'd image only once it has proven it can run, so a build
    // that boots then crashes within the first minute rolls back instead of looping.
    esp_ota_img_states_t st;
    bool pending = (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY);
    if (pending && !web_ok) {
        ESP_LOGE(TAG, "web server failed to start on a trial image; rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();   // reboots; returns only with no rollback target
    }

    ESP_LOGI(TAG, "oclean custom firmware up: wifi+web+mqtt started");
    int uptime_s = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uptime_s += 10;
        if (pending && web_ok && uptime_s >= 60) {
            esp_ota_mark_app_valid_cancel_rollback();
            pending = false;
            ESP_LOGI(TAG, "OTA image confirmed valid after %ds", uptime_s);
        }
    }
}

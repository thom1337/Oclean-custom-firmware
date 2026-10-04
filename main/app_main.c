#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#include "esp_sntp.h"

#include "config_store.h"
#include "wifi_mgr.h"
#include "mqtt_ha.h"
#include "web_server.h"
#include "metrics.h"
#include "ble_server.h"
#include "weblog.h"
#include "boot_guard.h"
#include "brush_app.h"
#include "hardware.h"
#include "oem_glue.h"

static const char *TAG = "app";

// Waits 10 s. In safe mode no button driver runs (brush_app_start() never did), so the
// 8 s hold that clears the web password is polled here meanwhile: GPIO3, active low,
// every 100 ms, once per hold.
static void idle_10s(bool safe)
{
    static int held;
    if (!safe) { vTaskDelay(pdMS_TO_TICKS(10000)); return; }
    for (int i = 0; i < 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        held = gpio_get_level(HW_PIN_BUTTON) ? 0 : held + 1;
        if (held == 80) web_auth_forget();
    }
}

// Seed the FW version so Home Assistant and the web UI show it; the hardware
// task fills in battery / temperature / brushing state as it samples them.
static void seed_brush_state(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    if (d) metrics_set_fw_version(d->version);
}

void app_main(void)
{
    weblog_init();   // capture logs into RAM for the web UI (no serial on this device)
    // The core lock must exist even if brush_app_start() is never called: in safe
    // mode the web server and Wi-Fi handlers still go through hal_lock().
    oem_glue_early_init();
    boot_mode_t mode = boot_guard_check();

    // The stock firmware keeps the panel id, UI language and brush settings in this
    // NVS partition, so it is never erased wholesale: on an init error we carry on
    // without persistence rather than wipe it.
    esp_err_t nv = nvs_flash_init();
    if (nv != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init: %s (settings will not persist)", esp_err_to_name(nv));

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
    seed_brush_state();

    bool radios = true;
    if (mode == BOOT_SAFE) {
        ESP_LOGE(TAG, "safe mode: hardware drivers and brush logic are NOT started; use the web UI to update");
    } else {
        // On-device hardware and the brush behaviour (screen, touch, button, motor,
        // LEDs, charging, sleep). This may put the brush straight back to sleep
        // (e.g. a spurious motion wake, or an empty battery) and not return.
        radios = brush_app_start(&cfg);
    }

    bool web_ok = false;
    if (radios) {
        wifi_mgr_start(&cfg);
        web_ok = web_server_start();   // reachable on STA IP or the setup AP (192.168.4.1)

        // SNTP so session timestamps and the clock page are correct.
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();

        mqtt_ha_start(&cfg);
        if (mode != BOOT_SAFE) ble_server_start();   // Oclean GATT service for the phone app; nothing unless Bluetooth is built
    }

    // Confirm a freshly-OTA'd image only once it has proven it can run, so a build
    // that boots then crashes within the first minute rolls back instead of looping.
    esp_ota_img_states_t st;
    bool pending = (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY);
    if (pending && radios && !web_ok) {
        ESP_LOGE(TAG, "web server failed to start on a trial image; rolling back");
        boot_guard_clean_exit();
        esp_ota_mark_app_invalid_rollback_and_reboot();   // reboots; returns only with no rollback target
    }

    ESP_LOGI(TAG, "oclean custom firmware up (%s)", mode == BOOT_SAFE ? "SAFE MODE" : radios ? "wifi+web+mqtt started" : "radios off");
    if (mode == BOOT_SAFE) {
        gpio_hold_dis(HW_PIN_BUTTON);        // a pad hold survives the panic reset that led here
        const gpio_config_t io = { .pin_bit_mask = 1ULL << HW_PIN_BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
        gpio_config(&io);
    }
    int uptime_s = 0;
    while (1) {
        idle_10s(mode == BOOT_SAFE);
        uptime_s += 10;
        if (uptime_s == 60) {
            boot_guard_healthy();
            if (pending && (web_ok || !radios)) {
                esp_ota_mark_app_valid_cancel_rollback();
                pending = false;
                ESP_LOGI(TAG, "OTA image confirmed valid after %ds", uptime_s);
            }
        }
    }
}

#include "boot_guard.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"

// This brush has no serial port and its (stock) bootloader does not roll back, so an
// image that crashes on every boot could only be replaced by the image itself. The
// guard counts boots that did not reach a healthy state; the counter lives in RTC
// memory, so it survives panics, watchdog and software resets (a power-on reset
// clears it, and so does a deliberate restart or deep sleep).
//   >= SAFE_AFTER   boots: safe mode — Wi-Fi + web UI (firmware update) only
//   >= REVERT_AFTER boots: boot the other OTA slot (the previous firmware), if valid
#define MAGIC        0x0c1ea9b0u
#define SAFE_AFTER   4
#define REVERT_AFTER 8

static const char *TAG = "boot_guard";
static RTC_NOINIT_ATTR struct { uint32_t magic; uint32_t count; } s_rtc;
static boot_mode_t s_mode = BOOT_NORMAL;

boot_mode_t boot_guard_check(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    if (s_rtc.magic != MAGIC || why == ESP_RST_POWERON || why == ESP_RST_DEEPSLEEP) {
        s_rtc.magic = MAGIC;
        s_rtc.count = 0;
    }
    s_rtc.count++;
    if (s_rtc.count >= REVERT_AFTER) {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        ESP_LOGE(TAG, "%lu boots without reaching a healthy state; reverting to %s",
                 (unsigned long)s_rtc.count, other ? other->label : "(none)");
        s_rtc.count = 0;
        // set_boot_partition verifies the image first, so a blank slot is refused.
        if (other && esp_ota_set_boot_partition(other) == ESP_OK) esp_restart();
        ESP_LOGE(TAG, "no bootable image in the other slot; staying in safe mode");
        s_mode = BOOT_SAFE;
    } else if (s_rtc.count >= SAFE_AFTER) {
        ESP_LOGE(TAG, "%lu boots without reaching a healthy state: SAFE MODE (web UI only)",
                 (unsigned long)s_rtc.count);
        s_mode = BOOT_SAFE;
    }
    return s_mode;
}

boot_mode_t boot_guard_mode(void) { return s_mode; }

void boot_guard_healthy(void)
{
    // In safe mode the count is kept: the next boot tries the full firmware once
    // more only after a deliberate restart (which clears it).
    if (s_mode == BOOT_NORMAL) s_rtc.count = 0;
}

void boot_guard_clean_exit(void) { s_rtc.magic = MAGIC; s_rtc.count = 0; }

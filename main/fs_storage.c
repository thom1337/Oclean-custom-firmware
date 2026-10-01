#include "fs_storage.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "wear_levelling.h"

static const char *TAG = "fs";
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_mounted;
static const char *s_reason = "not mounted";

bool fs_storage_mount(void)
{
    // Only mount if a FAT "storage" partition actually exists. On a unit flashed by
    // app-only OTA the running image sits on the STOCK partition table, which has no
    // such partition — so probe first and degrade cleanly instead of erroring.
    // find_first(DATA/FAT/"storage") can only match our own partition; it can never
    // match the stock nvs or brushdata, so this cannot touch stock data.
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!p) {
        s_reason = "no 'storage' partition in this flash layout (stock OTA table)";
        ESP_LOGW(TAG, "%s; file browser disabled", s_reason);
        return false;
    }
    // format_if_mount_failed stays FALSE: a read-only browser must never auto-format.
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FS_MOUNT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        s_reason = esp_err_to_name(err);
        ESP_LOGE(TAG, "FAT mount failed: %s", s_reason);
        return false;
    }
    s_mounted = true;
    s_reason = "mounted";
    ESP_LOGI(TAG, "storage mounted at %s", FS_MOUNT);
    return true;
}

bool fs_storage_available(void) { return s_mounted; }
const char *fs_storage_reason(void) { return s_reason; }
const char *fs_browse_root(void) { return FS_MOUNT; }

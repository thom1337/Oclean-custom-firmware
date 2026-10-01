#include "fs_storage.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "wear_levelling.h"

static const char *TAG = "fs";
static wl_handle_t s_wl = WL_INVALID_HANDLE;

bool fs_storage_mount(void)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 6,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FS_MOUNT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAT mount failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "storage mounted at %s", FS_MOUNT);
    return true;
}

const char *fs_browse_root(void) { return FS_MOUNT; }

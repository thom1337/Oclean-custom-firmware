#include "ui_res.h"
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "stock_ui_tables.h"

// The OEM pictures are not part of any firmware image: they sit in a raw data
// partition (type 0x40, subtype 0x00) that the factory wrote and that the stock
// partition table — still on flash after an app-only OTA — describes. We only ever
// read it.
#define RES_UI_BYTES 0x800000u     // pictures, custom-picture banks and zone frames end below this

static const char *TAG = "ui_res";
static const esp_partition_t *s_part;
static const uint8_t *s_map;       // memory-mapped view (NULL: fall back to partition reads)
static size_t s_map_len;
static bool s_ok;

bool ui_res_init(void)
{
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        ESP_LOGI(TAG, "partition %-10s type 0x%02x sub 0x%02x @0x%06lx size 0x%06lx",
                 p->label, p->type, p->subtype, (unsigned long)p->address, (unsigned long)p->size);
    }
    s_part = esp_partition_find_first((esp_partition_type_t)0x40, (esp_partition_subtype_t)0x00, NULL);
    if (!s_part) {
        ESP_LOGW(TAG, "no OEM picture partition (type 0x40): falling back to the built-in screen");
        return false;
    }
    // A screen is composed from up to a few hundred row reads; through the flash
    // driver that takes tens of milliseconds, mapped it is a memcpy.
    s_map_len = s_part->size < RES_UI_BYTES ? s_part->size : RES_UI_BYTES;
    esp_partition_mmap_handle_t h;
    const void *ptr = NULL;
    if (esp_partition_mmap(s_part, 0, s_map_len, ESP_PARTITION_MMAP_DATA, &ptr, &h) == ESP_OK) s_map = ptr;
    else ESP_LOGW(TAG, "could not map the picture partition; using flash reads");

    // A wiped partition reads as 0xFF. Digit '0' (#173, 14x24) is a cheap probe.
    uint8_t probe[64]; bool blank = true;
    if (ui_res_read(STOCK_PIC_OFF[173], probe, sizeof probe))
        for (unsigned i = 0; i < sizeof probe; i++) if (probe[i] != 0xFF) { blank = false; break; }
    s_ok = !blank && s_part->size >= 0x7BC000;
    ESP_LOGI(TAG, "OEM picture partition '%s' @0x%06lx size 0x%06lx (%s): %s", s_part->label,
             (unsigned long)s_part->address, (unsigned long)s_part->size, s_map ? "mapped" : "unmapped",
             s_ok ? "pictures present" : "blank or too small -> built-in screen");
    return s_ok;
}

bool ui_res_available(void) { return s_ok; }

bool ui_res_read(uint32_t off, void *dst, size_t len)
{
    if (!s_part || off + len > s_part->size) return false;
    if (s_map && off + len <= s_map_len) { memcpy(dst, s_map + off, len); return true; }
    return esp_partition_read(s_part, off, dst, len) == ESP_OK;
}

size_t ui_res_size(void) { return s_part ? s_part->size : 0; }

#pragma once
#include <stdbool.h>

// Mount the FAT "storage" partition read/write at FS_MOUNT (used for browsable
// files). The web browser serves it read-only regardless.
#define FS_MOUNT "/data"

bool fs_storage_mount(void);
// Root the web file-browser is confined to.
const char *fs_browse_root(void);

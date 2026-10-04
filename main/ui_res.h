#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Read-only access to the OEM picture partition (type 0x40 / subtype 0x00).
bool   ui_res_init(void);        // locate + sanity-check; false if missing or blank
bool   ui_res_available(void);
bool   ui_res_read(uint32_t off, void *dst, size_t len);

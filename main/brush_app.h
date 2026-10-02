#pragma once
#include <stdbool.h>
#include "config_store.h"

// Entry point of the OEM-behaviour port on the device (implemented by the platform
// glue, main/oem_glue.c — see re/spec/ARCH.md and the "glue" sections of
// re/spec/NOTES_*.md for what it has to do). Brings up the drivers in the documented
// order, starts the main and UI tasks, and returns whether the radios (Wi-Fi, web,
// MQTT, BLE) may be started; it does not return at all when the boot-time checks
// send the brush straight back to sleep.
bool brush_app_start(const app_config_t *cfg);

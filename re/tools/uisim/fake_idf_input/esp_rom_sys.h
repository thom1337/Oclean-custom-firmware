// Host stand-in for ESP-IDF's esp_rom_sys.h (input-module simulators).
#pragma once
#include <stdint.h>
void esp_rom_delay_us(uint32_t us);

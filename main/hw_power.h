#pragma once
#include <stdbool.h>
#include "oem_state.h"
// Power-management driver (hw_power.c): entry points that are not part of the oem
// core API (oem_api.h, block "power + IMU").

// Called first when a deep sleep is really entered (after the checks that can still
// refuse it): stop Wi-Fi / BLE here. NULL = none.
void hw_power_set_pre_sleep_hook(void (*fn)(void));

// The RTC-retained variables (storage behind hal_rtc()): kept through deep sleep,
// back to the stock initial values after any other reset.
oem_rtc_t *hw_power_rtc(void);

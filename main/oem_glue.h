#pragma once
#include <stdbool.h>

// Platform glue of the OEM-behaviour port (main/oem_glue.c): the HAL of oem_hal.h on
// FreeRTOS / ESP-IDF, the two tasks of the stock layout ("brush_app" and "UI_TASK"),
// brush_app_start() (brush_app.h) and hw_emulated() (hardware.h). Details:
// re/spec/NOTES_glue.md. What the rest of the firmware needs from it besides those:

// Call at the start of app_main (after weblog_init, so that its log line is kept).
// Creates the core lock, which makes hal_lock() usable even when brush_app_start()
// is never called (safe mode), and decides hw_emulated().
void oem_glue_early_init(void);

// True once brush_app_start() has brought up the drivers and the two tasks. False in
// safe mode: g_oem then only holds its initial values, nobody runs the main loop,
// and no oem_* function that touches hardware, NVS or the UI may be called. Code in
// other tasks (web server, MQTT, Wi-Fi events, BLE) checks this before such calls.
bool brush_app_running(void);

// Die temperature of the IMU in degrees Celsius, as the main task last read it (it
// does so every few seconds); NAN when there is no reading, and always in safe mode.
// The IMU is the main task's: other tasks take this copy instead of calling
// oem_imu_temp(), which talks to the chip and busy-waits 3 ms.
float oem_glue_imu_temp(void);

// Sets the time zone hal_time() reports in: a POSIX TZ string such as
// "CET-1CEST,M3.5.0,M10.5.0/3" (app_config_t.tz). brush_app_start() applies the
// stored one; the web server calls this when the setting changes. Any task. False,
// and nothing changed, when the string is not of that form.
bool oem_glue_set_tz(const char *tz);

#pragma once

// Crash-loop protection for a device with no serial port (see boot_guard.c).
typedef enum { BOOT_NORMAL, BOOT_SAFE } boot_mode_t;

boot_mode_t boot_guard_check(void);    // call once, first thing in app_main
boot_mode_t boot_guard_mode(void);
void boot_guard_healthy(void);         // the firmware has been up and serving for a while
void boot_guard_clean_exit(void);      // before a deliberate restart or deep sleep

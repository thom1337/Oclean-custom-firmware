#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "oem_state.h"

// Services the platform provides to the oem core (oem_*.c). On the brush they are
// implemented in oem_glue.c on top of FreeRTOS / ESP-IDF; the host simulator
// (re/tools/uisim) provides fakes with virtual time. Core files include only this
// header, oem_state.h, oem_api.h and the C library — never ESP-IDF headers — so the
// same source builds on the host.

// ---- locking, time ----------------------------------------------------------------
// One recursive lock protects g_oem and all core module state. The glue takes it
// around every call into the core from the main task and the UI task; core code does
// not take it itself. Code running in other tasks (web server, MQTT, BLE) must take
// it around any core call.
void     hal_lock(void);
void     hal_unlock(void);
uint32_t hal_ms(void);                 // milliseconds since boot
uint32_t hal_uptime_s(void);           // seconds since boot (stock 0x4201b5e8)
void     hal_delay(uint32_t ms);       // stock vTaskDelay(ms); the lock stays held

typedef struct { uint8_t year, month, day, hour, min, sec, wday; } oem_time_t;
// Wall clock (stock 0x4201d89c). year = years since 2000; a value above 200 means the
// clock has not been set.
void     hal_time(oem_time_t *t);

void     hal_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- events and timers ------------------------------------------------------------
// Stock has two FreeRTOS event groups. The main task waits on the first and runs its
// loop body for the bits that were set; the UI task does the same on the second.
// Both posts are safe from interrupt context.
void hal_event_post(uint32_t bits);     // main group, stock setter 0x40377f10
void hal_ui_event_post(uint32_t bits);  // UI group (stock handle 0x3fca50e4)

// Main event bits (stock values)
#define OEM_EV_MOTION        0x000002u  // GPIO8 any-motion interrupt
#define OEM_EV_BLE_TIMEOUT   0x000008u  // deep-sleep window expired
#define OEM_EV_CHARGER       0x000020u  // GPIO9 went low (charger attached)
#define OEM_EV_TICK          0x000040u  // 10 ms tick
#define OEM_EV_BUTTON        0x000080u  // a button code is waiting (oem_button_pop)
#define OEM_EV_TOUCH_RDY     0x000100u  // touch IC RDY falling edge
#define OEM_EV_FAST          0x000200u  // 30 ms session timer
#define OEM_EV_PRESSURE      0x000400u  // 20 ms pressure-sample timer
#define OEM_EV_BLE_WAKE      0x004000u  // BLE connect / Wi-Fi connected: wake the screen if asleep
#define OEM_EV_SESSION_START 0x040000u
#define OEM_EV_SESSION_END   0x080000u
#define OEM_EV_SLEEP_STAGE2  0x100000u  // 200 ms after the screen-off sequence
// Custom additions (not stock bits)
#define OEM_EV_REMOTE        0x200000u  // a remote command is waiting (web / MQTT / BLE), see oem_api.h
// UI event bits (stock values)
#define OEM_UIEV_MSG         0x001u     // a message is queued
#define OEM_UIEV_BLINK       0x002u     // 50 ms blink / animation tick
#define OEM_UIEV_GESTURE     0x010u     // 70 ms after the last touch sample
#define OEM_UIEV_LCD_REINIT  0x020u
#define OEM_UIEV_FACTORY     0x200u     // factory reset requested

// One-shot / periodic timers. Expiry posts the listed event bit. To add a timer,
// append a line (keep the existing order).
//      name            group  bit
#define HAL_TIMER_LIST(X) \
    X(TICK10,           MAIN,  OEM_EV_TICK)          /* periodic 10 ms, started by the glue   */ \
    X(BLINK50,          UI,    OEM_UIEV_BLINK)       /* periodic 50 ms, started by the glue   */ \
    X(FAST30,           MAIN,  OEM_EV_FAST)          /* periodic 30 ms during a session       */ \
    X(PRESSURE20,       MAIN,  OEM_EV_PRESSURE)      /* periodic 20 ms pressure sampling      */ \
    X(BLE_TIMEOUT,      MAIN,  OEM_EV_BLE_TIMEOUT)   /* one-shot 30 s / 120 s                 */ \
    X(SLEEP200,         MAIN,  OEM_EV_SLEEP_STAGE2)  /* one-shot 200 ms                       */ \
    X(GESTURE70,        UI,    OEM_UIEV_GESTURE)     /* one-shot 70 ms                        */

#define HAL_TIMER_ENUM(name, group, bit) HAL_TMR_##name,
typedef enum { HAL_TIMER_LIST(HAL_TIMER_ENUM) HAL_TMR_COUNT } hal_timer_t;
void hal_timer_start(hal_timer_t t, uint32_t ms, bool periodic);   // restarts if already running
void hal_timer_stop(hal_timer_t t);

// ---- persistent storage -----------------------------------------------------------
// Blobs in the stock NVS namespace "storage" (same keys and layouts as stock, so
// settings made with the stock firmware or the phone app carry over).
// get: copies min(len, stored) bytes, returns the stored length (0 = key absent).
size_t     hal_nvs_get(const char *key, void *buf, size_t len);
bool       hal_nvs_set(const char *key, const void *buf, size_t len);
oem_rtc_t *hal_rtc(void);              // RTC-retained variables (see oem_state.h)

// ---- display ----------------------------------------------------------------------
void hal_lcd_init(void);               // stock 0x42029120: reset, init table, black fill
void hal_lcd_sleep(void);              // stock 0x42028918: panel command 0x10 (SLPIN)
void hal_lcd_blit(const uint8_t *fb);  // push an 80x160 frame (25600 bytes, panel byte order)
bool hal_res_read(uint32_t off, void *dst, size_t len);   // OEM picture partition

// ---- system / radio ---------------------------------------------------------------
void hal_restart(void);                // esp_restart()
bool hal_wifi_has_ssid(void);          // a Wi-Fi network is configured
bool hal_ble_connected(void);
void hal_net_sleep(void);              // screen-off stage: stock stops Wi-Fi 27 s later
void hal_net_wake(void);               // wake from the screen-off stage
// Not stock: the web UI's own. The passcode of the setup AP ("oclean-setup") while it
// is up and has one, else NULL; oem_ui.c draws it on the screen. And the 8 s
// factory-reset hold also clears the web password.
const char *hal_setup_ap_code(void);
void hal_forget_web_password(void);

// ---- additions by modules (keep each block to its owner) --------------------------
// UI (agent U):

// app / brush (agent A):

// LED / gauge (agent L):
// LED driver (hw_led.c), the stock "ocleanhal" LED calls. HAL LED ids are the stock
// ones: 1..4 = indicator LEDs on GPIO17..20 (LEDC channel 0..3), 5 = LCD backlight on
// GPIO21 (channel 4). A level is a brightness (0 = dark); the driver owns the
// polarity of each channel and the limits. Only oem_led.c calls these. The simulator
// builds hw_led.c itself, against a fake LEDC (re/tools/uisim/fake_idf).
extern const uint32_t hal_led_max[6];              // led_configs[]: {0, 4000, 4000, 4000, 8191, 6000}
void     hal_led_hw_init(void);                    // led_init 0x4200d2a8: timer + 5 channels, everything dark
void     hal_led_set(int hal_id, uint32_t level);      // set_led_light_level 0x4200d2f0 (clamps to the limit)
void     hal_led_process(int hal_id, uint32_t level);  // set_led_light_level_process 0x4200d49c (drops level > limit)
uint32_t hal_led_get(int hal_id);                  // get level 0x4200d604
void     hal_led_pins_release(bool boot);          // un-hold GPIO17..21, pins at their "off" level (before led_init)
void     hal_led_pin_release_charge(void);         // gpio_hold_dis(19) only
void     hal_led_pins_park(void);                  // GPIO17..21 as held plain outputs at "off" (LEDC detached)

// input (agent I):

// power (agent P):
// Stock allow_anymotion_check_flag (0x3fca2885): true for 5 s after a GPIO8 motion
// edge or a motion wake from deep sleep. Implemented in hw_power.c.
bool hal_anymotion_allowed(void);

// motor (agent M):

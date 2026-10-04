// Platform glue of the OEM-behaviour port: what the oem core (oem_*.c) and its
// drivers (hw_*.c) need from FreeRTOS / ESP-IDF (the HAL of oem_hal.h), the two
// tasks of the stock layout and the boot sequence (brush_app_start). The threading
// model is in re/spec/ARCH.md, the decisions taken here in re/spec/NOTES_glue.md.
//
//   "brush_app"    main task: waits on the main event group, runs oem_app_handle()
//   "UI_TASK"      waits on the UI event group, runs oem_ui_handle(), pushes the frame
//   "brush_music"  motor stream (hw_motor.c, not created here; its priority is set here)
//
// One recursive mutex, the core lock, protects g_oem and all core state. The two
// tasks hold it around every call into the core; web server, MQTT, Wi-Fi and BLE
// handlers take it through hal_lock() themselves.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "ble_server.h"
#include "boot_guard.h"
#include "brush_app.h"
#include "hardware.h"
#include "hw_power.h"
#include "oem_api.h"
#include "oem_glue.h"
#include "oem_hal.h"
#include "ui_render.h"
#include "ui_res.h"
#include "wifi_mgr.h"

static const char *TAG = "glue";

// A FreeRTOS event group carries 24 bits; the stock bit values fit (highest: 0x200000).
#define MAIN_EV_MASK  0x00FFFFFFu
#define UI_EV_MASK    0x000003FFu       // what the stock UI task waits for

// Stock creates both tasks with 3072 bytes, priority 3 and no core affinity, on a
// 1 ms tick; its motor task has priority 3 on core 0 (hw_motor.c creates ours so).
//  * Stacks are generous: the main task runs the whole brush logic including NVS
//    writes and logging (the web log hook alone formats into 256 bytes of stack).
//  * The main task keeps priority 3. The UI task gets 2: composing and pushing a
//    frame keeps a core busy for about 10 ms, up to 20 times a second. On stock's
//    1 ms tick equal priorities share a core in 1 ms slices; on the 10 ms tick here
//    the main task would wait out the whole frame. One step lower, the UI task can
//    never delay the 10 ms tick, a touch report or the motor stream.
//  * Both run on the second core. The first one has the radios, the esp_timer task
//    and the motor stream (37.5 ms of samples buffered), which this keeps clear of
//    the brush logic whatever it does. Fixed rather than "no affinity" because a
//    task is pinned to wherever it happens to run at its first float operation.
//  * The motor stream task is raised to 10 (brush_app_start). At stock's 3 it sits
//    below what this firmware adds on its core: the web server and the MQTT client
//    (both 5, no affinity) can compute for longer than the 37.5 ms the DMA holds
//    (Home Assistant discovery, a TLS handshake), and every buffer the stream misses
//    goes out as silence. 10 is above those two and below lwIP (18), the default
//    event loop (20), the NimBLE host (21), the esp_timer task (22) and Wi-Fi (23).
//    The task sleeps in the I2S write nearly all the time (about 1 % of a core).
#define MAIN_STACK    8192
#define UI_STACK      6144
#define MAIN_PRIO     3
#define UI_PRIO       2
#define MOTOR_PRIO    10
#define MOTOR_TASK    "brush_music"     // the name hw_motor.c gives its task
#define BRUSH_CORE    (portNUM_PROCESSORS - 1)

#define NVS_NS        "storage"         // the stock namespace
#define BOOT_WAIT_MS  3000              // brush_app_start waits this long for oem_app_boot()
#define STALL_TICKS   3000              // 30 s of 10 ms ticks without a main-loop pass: restart
#define FRAME_LOG     50                // frames until the first progress line of a screen (see ui_task)
#define IMU_TEMP_MS   5000              // how often the main task reads the IMU temperature (see imu_temp_poll)

enum { GRP_MAIN, GRP_UI };

// One esp_timer per line of HAL_TIMER_LIST: expiry posts the listed bit.
#define TMR_ROW(name, group, bit) { #name, GRP_##group, (bit) },
static const struct { const char *name; uint8_t group; uint32_t bit; } TMR[HAL_TMR_COUNT] = {
    HAL_TIMER_LIST(TMR_ROW)
};

static SemaphoreHandle_t  s_lock;                   // the core lock
static EventGroupHandle_t s_ev[2];                  // stock 0x3fca4eac (main) and 0x3fca50e4 (UI)
static esp_timer_handle_t s_tmr[HAL_TMR_COUNT];
static TaskHandle_t       s_main_task, s_ui_task;
static SemaphoreHandle_t  s_booted;                 // given once oem_app_boot() has run
static uint8_t           *s_fb;                     // the 80x160 frame the UI composes
static int                s_wake_cause;
static bool               s_has_ssid;               // the configuration loaded at boot names a Wi-Fi network
static float              s_imu_temp = NAN;         // IMU die temperature as the main task last read it
static uint32_t           s_imu_temp_at;            // hal_ms() of that reading (0: none yet)
static bool               s_radios;                 // brush_app_start() allowed Wi-Fi / BLE
static volatile bool      s_running;
static volatile uint32_t  s_main_stall;             // 10 ms ticks since the main task finished a pass
static volatile uint32_t  s_isr_lost[2];            // bits an interrupt could not hand over (see post())
static uint32_t           s_frames;
static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;

// ---- emulation ----------------------------------------------------------------------
// QEMU's ESP32-S3 has blank eFuses, so the factory MAC reads 00:00:00:00:00:00. No
// real chip has that (this brush's starts with the Oclean OUI e8:06:90:…), and a read error counts as
// "real hardware": a brush wrongly taken for emulated would keep its drivers and
// radios off.
static int8_t  s_emulated = -1;
static uint8_t s_mac[6];

bool hw_emulated(void)
{
    if (s_emulated < 0) {
        bool blank = esp_efuse_mac_get_default(s_mac) == ESP_OK;
        for (int i = 0; blank && i < 6; i++) blank = s_mac[i] == 0;
        s_emulated = blank;
    }
    return s_emulated;
}

// ---- locking, time ------------------------------------------------------------------

// oem_glue_early_init() creates the lock before any other task exists. Should
// hal_lock() ever be reached without it, the lock is created here; the swap makes
// that safe against two tasks doing it at once.
static SemaphoreHandle_t lock_get(void)
{
    if (s_lock) return s_lock;
    SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    portENTER_CRITICAL(&s_mux);
    if (!s_lock) { s_lock = m; m = NULL; }
    portEXIT_CRITICAL(&s_mux);
    if (m) vSemaphoreDelete(m);
    return s_lock;
}

void hal_lock(void)   { xSemaphoreTakeRecursive(lock_get(), portMAX_DELAY); }
void hal_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

uint32_t hal_ms(void)       { return (uint32_t)(esp_timer_get_time() / 1000); }
uint32_t hal_uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

// Stock vTaskDelay(ms) on a 1 ms tick. The caller keeps the core lock (it is the
// owner, the mutex is recursive), so the other task stays out meanwhile, as the core
// expects. A delay of n ticks ends at the n-th tick interrupt, i.e. after n-1..n
// periods of 10 ms: one tick is added to the rounded-up time so that the wait is
// never shorter than asked (these are settling times).
void hal_delay(uint32_t ms)
{
    if (ms == 0) { taskYIELD(); return; }
    vTaskDelay((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS + 1);
}

// Stock 0x4201d89c: time() + localtime_r(), year - 100. With the clock never set
// (1970, or 1969 west of Greenwich) the year byte comes out as 226 or 225, the "not
// set" the core tests for (> 200). SNTP (app_main) sets the clock in UTC; the zone
// localtime_r() applies is the configured one (oem_glue_set_tz), where stock is
// given local time by the phone app.
void hal_time(oem_time_t *t)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    t->year  = (uint8_t)(tm.tm_year - 100);
    t->month = (uint8_t)(tm.tm_mon + 1);
    t->day   = (uint8_t)tm.tm_mday;
    t->hour  = (uint8_t)tm.tm_hour;
    t->min   = (uint8_t)tm.tm_min;
    t->sec   = (uint8_t)tm.tm_sec;
    t->wday  = (uint8_t)tm.tm_wday;
}

// The start of a POSIX TZ string as newlib's tzset() wants it: a zone name of 3 to 10
// letters ("CET"), or of letters, digits and signs in <> ("<+0530>"), then the
// offset. The C library has no zone database; given a name like "Europe/Berlin" it
// gives up without a word and keeps the zone it had.
static bool tz_valid(const char *s)
{
    if (!s) return false;
    bool quoted = *s == '<';
    size_t n = 0;
    if (quoted) s++;
    for (;; n++) {
        char c = s[n];
        bool letter = (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
        if (!letter && !(quoted && ((c >= '0' && c <= '9') || c == '+' || c == '-'))) break;
    }
    if (n < 3 || n > 10 || (quoted && s[n] != '>')) return false;
    s += n + quoted;
    if (*s == '+' || *s == '-') s++;
    return *s >= '0' && *s <= '9';
}

// The zone hal_time() reports in. localtime_r() reads TZ under the C library's own
// lock, so the web server may change it while the brush logic runs.
bool oem_glue_set_tz(const char *tz)
{
    if (!tz_valid(tz) || setenv("TZ", tz, 1) != 0) {
        ESP_LOGW(TAG, "time zone '%s' not set: not a POSIX TZ string", tz ? tz : "");
        return false;
    }
    tzset();
    // What the C library made of it goes to the log: the offset on the clock's date
    // (1 January 1970 while the clock is not set).
    time_t now = time(NULL);
    struct tm l, u;
    localtime_r(&now, &l);
    gmtime_r(&now, &u);
    int days = l.tm_yday - u.tm_yday;        // -1, 0 or 1; anything else is the turn of the year
    if (days > 1) days = -1;
    else if (days < -1) days = 1;
    int min = (days * 24 + l.tm_hour - u.tm_hour) * 60 + l.tm_min - u.tm_min;
    ESP_LOGI(TAG, "time zone %s: local time is UTC%c%d:%02d", tz, min < 0 ? '-' : '+', abs(min) / 60, abs(min) % 60);
    return true;
}

void hal_log(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ESP_LOGI("oem", "%s", line);
}

// ---- events and timers --------------------------------------------------------------

// From a task (esp_timer callbacks run in the esp_timer task in this configuration)
// the bits are set directly. From an interrupt FreeRTOS cannot do that: the request
// goes to its timer service task (xEventGroupSetBitsFromISR), which brush_app_start()
// raises to the priority of the esp_timer task for this reason. Its queue holds 10
// requests; one that does not fit is kept and posted by the next 10 ms tick.
static void post(int grp, uint32_t bits)
{
    EventGroupHandle_t g = s_ev[grp];
    if (!g || !bits) return;                 // safe mode, or before brush_app_start(): nobody listens
    if (!xPortInIsrContext()) {
        xEventGroupSetBits(g, bits);
        return;
    }
    BaseType_t woken = pdFALSE;
    if (xEventGroupSetBitsFromISR(g, bits, &woken) != pdPASS) {
        portENTER_CRITICAL_ISR(&s_mux);
        s_isr_lost[grp] |= bits;
        portEXIT_CRITICAL_ISR(&s_mux);
    }
    if (woken) portYIELD_FROM_ISR();
}

void hal_event_post(uint32_t bits)    { post(GRP_MAIN, bits & MAIN_EV_MASK); }
void hal_ui_event_post(uint32_t bits) { post(GRP_UI, bits & UI_EV_MASK); }

// Runs in the esp_timer task on every 10 ms tick.
static void tick_housekeeping(void)
{
    if (s_isr_lost[GRP_MAIN] | s_isr_lost[GRP_UI]) {
        portENTER_CRITICAL(&s_mux);
        uint32_t m = s_isr_lost[GRP_MAIN], u = s_isr_lost[GRP_UI];
        s_isr_lost[GRP_MAIN] = s_isr_lost[GRP_UI] = 0;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "events from an interrupt posted late: main %06x, ui %03x", (unsigned)m, (unsigned)u);
        post(GRP_MAIN, m);
        post(GRP_UI, u);
    }
    // The brush has no serial port and no reset button, and the firmware update goes
    // through handlers that take the core lock. A main task that hangs (with the lock,
    // most likely) would leave a brush that can only run its battery down; a restart
    // at least gets the web UI back, and through the boot guard safe mode if the hang
    // comes back at every boot. No handler blocks for more than about half a second.
    if (s_main_task && ++s_main_stall >= STALL_TICKS)
        esp_system_abort("glue: the brush_app task made no pass for 30 s");
}

static void timer_cb(void *arg)
{
    unsigned t = (unsigned)(uintptr_t)arg;
    if (t == HAL_TMR_TICK10) tick_housekeeping();
    post(TMR[t].group, TMR[t].bit);
}

static bool timers_create(void)
{
    for (unsigned t = 0; t < HAL_TMR_COUNT; t++) {
        const esp_timer_create_args_t a = {
            .callback = timer_cb,
            .arg = (void *)(uintptr_t)t,
            .dispatch_method = ESP_TIMER_TASK,
            .name = TMR[t].name,
            // A periodic timer that fell behind (flash write, long handler) fires once,
            // not once per missed period: the event bit could not count them anyway.
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&a, &s_tmr[t]) != ESP_OK) return false;
    }
    return true;
}

void hal_timer_start(hal_timer_t t, uint32_t ms, bool periodic)
{
    if ((unsigned)t >= HAL_TMR_COUNT || !s_tmr[t]) return;
    esp_timer_stop(s_tmr[t]);                // restart if it runs ("not running" is the usual answer)
    uint64_t us = (uint64_t)ms * 1000;
    esp_err_t e = periodic ? esp_timer_start_periodic(s_tmr[t], us) : esp_timer_start_once(s_tmr[t], us);
    if (e != ESP_OK) ESP_LOGE(TAG, "timer %s (%u ms): %s", TMR[t].name, (unsigned)ms, esp_err_to_name(e));
}

void hal_timer_stop(hal_timer_t t)
{
    if ((unsigned)t < HAL_TMR_COUNT && s_tmr[t]) esp_timer_stop(s_tmr[t]);
}

// ---- persistent storage -------------------------------------------------------------

size_t hal_nvs_get(const char *key, void *buf, size_t len)
{
    nvs_handle_t h;
    size_t stored = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;      // also: namespace not there yet
    if (nvs_get_blob(h, key, NULL, &stored) != ESP_OK) stored = 0;
    if (stored && buf && len) {
        if (stored <= len) {
            size_t n = len;
            if (nvs_get_blob(h, key, buf, &n) != ESP_OK) stored = 0;
        } else {
            // NVS does not read part of a blob, and stock stores some longer than
            // what is read here (30 bytes for a calibration of 16): read it whole.
            uint8_t *tmp = malloc(stored);
            size_t n = stored;
            if (tmp && nvs_get_blob(h, key, tmp, &n) == ESP_OK) memcpy(buf, tmp, len);
            else stored = 0;
            free(tmp);
        }
    }
    nvs_close(h);
    return stored;
}

bool hal_nvs_set(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, key, buf, len);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs %s/%s (%u bytes) not written: %s", NVS_NS, key, (unsigned)len, esp_err_to_name(e));
    return e == ESP_OK;
}

// hw_power.c owns the RTC-retained struct and gives it the stock initial values
// (strength 3, hist_day 0xff, counts 0) whenever its magic is wrong.
oem_rtc_t *hal_rtc(void) { return hw_power_rtc(); }

// ---- display ------------------------------------------------------------------------

void hal_lcd_init(void)               { hw_display_wake(); }
void hal_lcd_sleep(void)              { hw_display_sleep(); }
void hal_lcd_blit(const uint8_t *fb)  { hw_display_blit(fb); }
bool hal_res_read(uint32_t off, void *dst, size_t len) { return ui_res_read(off, dst, len); }

// ---- system / radio -----------------------------------------------------------------

void hal_restart(void)
{
    boot_guard_clean_exit();                 // a deliberate restart is not a failed boot
    esp_restart();
}

// "A network is configured", as it is now: the configuration loaded at boot, or
// credentials the web UI applied since (wifi_mgr.c takes them without a restart).
// Not while the setup AP is up: the brush then has no network, or cannot join the one
// it has, and whoever comes to correct that through the AP needs the long deep-sleep
// window of an unconfigured brush (120 s, not 30 s). Called by the main task; the two
// wifi_mgr functions only read a flag. Under emulation wifi_mgr.c is never started
// and the boot configuration alone decides.
bool hal_wifi_has_ssid(void)
{
    return (s_has_ssid || wifi_mgr_has_creds()) && !wifi_mgr_setup_ap_up();
}

bool hal_ble_connected(void) { return ble_server_connected(); }

float oem_glue_imu_temp(void) { return s_imu_temp; }

// Stock stops Wi-Fi 27 s into the screen-off stage and starts it again on a wake.
// Here Wi-Fi carries the web UI and MQTT, which are what the brush is kept up for in
// that stage, and in the usual case (network configured, no phone connected) deep
// sleep follows 3 s later anyway. So Wi-Fi and MQTT stay as they are until the
// pre-sleep hook; the station keeps the modem sleep it has from esp_wifi_init() on
// (WIFI_PS_MIN_MODEM, see wifi_mgr_start). A wake therefore finds the link up and has
// nothing to restore.
void hal_net_sleep(void)
{
    ESP_LOGI(TAG, "screen off for 27 s: radios stay up until deep sleep");
}

void hal_net_wake(void)
{
}

// hw_power.c calls this when a deep sleep is certain: in the main task, with the core
// lock held. A web / MQTT / BLE handler may be blocked on that lock right now and
// will stay so, hence only calls that do not wait for one of those tasks (no
// esp_mqtt_client_stop, no nimble_port_stop: both join a task).
static void stop_radios(void)
{
    if (!s_radios) return;                   // never started: boot-time re-sleep, emulation
    ble_server_stop_adv();                   // stock: esp_ble_gap_stop_advertising() at this point (nothing without Bluetooth)
    esp_err_t e = esp_wifi_stop();           // stock did it at the 27th second
    ESP_LOGI(TAG, "deep sleep: Wi-Fi stop: %s", esp_err_to_name(e));
}

// ---- the two tasks ------------------------------------------------------------------

// The IMU die temperature for the web UI / MQTT / BLE (metrics.c). Reading it is
// three register reads with 1 ms of busy-wait each, on an SPI device that only one
// task may use. Web, MQTT and BLE handlers used to do that themselves under the
// core lock, as often as they were asked; now the main task, which owns the IMU,
// reads it every 5 s between two handler runs and the others copy the value.
// Called with the core lock held.
static void imu_temp_poll(void)
{
    uint32_t now = hal_ms();
    if (s_imu_temp_at && now - s_imu_temp_at < IMU_TEMP_MS) return;
    s_imu_temp_at = now ? now : 1;
    float t;
    s_imu_temp = oem_imu_temp(&t) ? t : NAN;
}

// Stock 0x4201cc40.
static void main_task(void *arg)
{
    (void)arg;
    hal_lock();
    oem_app_boot(s_wake_cause);
    hal_unlock();
    xSemaphoreGive(s_booted);
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_ev[GRP_MAIN], MAIN_EV_MASK, pdTRUE, pdFALSE, portMAX_DELAY);
        hal_lock();
        oem_app_handle(bits & MAIN_EV_MASK);
        if (bits & OEM_EV_TICK) imu_temp_poll();
        hal_unlock();
        s_main_stall = 0;
    }
}

// Stock 0x42022564.
static void ui_task(void *arg)
{
    (void)arg;
    uint8_t shown = 0xff;
    uint32_t log_step = FRAME_LOG, log_at = 0;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_ev[GRP_UI], UI_EV_MASK, pdTRUE, pdFALSE, portMAX_DELAY);
        hal_lock();
        bool blit = oem_ui_handle(bits & UI_EV_MASK);
        uint8_t now = oem_ui_now();
        hal_unlock();
        if (!blit) continue;
        // Outside the core lock: the transfer takes about 7 ms and the main task must
        // not wait for it. Only this task composes into the buffer; a page change made
        // by the main task meanwhile (short press on a side page) can tear this one
        // frame, the redraw it requests follows at once.
        hal_lcd_blit(s_fb);
        s_frames++;
        // The log (the only view into a brush without a serial port) gets a line per
        // screen change, and while a screen stays, after 50, 150, 350, 750 ... more
        // frames: the charging screen draws 12 frames a second while its backlight is
        // on (nothing is drawn behind a dark one, see oem_ui_handle) and must not fill
        // the log.
        bool changed = now != shown;
        if (changed) log_step = FRAME_LOG;
        else if (s_frames < log_at) continue;
        else if (log_step < (1u << 24)) log_step *= 2;
        shown = now;
        log_at = s_frames + log_step;
        ESP_LOGI(TAG, "ui: screen %u, frame %u (stack left: main %u, ui %u)", now, (unsigned)s_frames,
                 (unsigned)uxTaskGetStackHighWaterMark(s_main_task), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

// ---- boot ---------------------------------------------------------------------------

void oem_glue_early_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    lock_get();
    bool emu = hw_emulated();
    ESP_LOGI(TAG, "emulated: %s (eFuse MAC %02x:%02x:%02x:%02x:%02x:%02x)", emu ? "yes" : "no",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    // The GPIO driver logs every gpio_config() at INFO: about 30 lines for one
    // screen-off and wake, in a web log that holds 16 KB. Warnings and errors stay;
    // the log-level switch of the web UI (it sets "*") brings the lines back.
    esp_log_level_set("gpio", ESP_LOG_WARN);
}

bool brush_app_running(void) { return s_running; }

// Info page 0 shows g_oem.fw_version as "V a.b.c.d", one digit each. The IDF app
// version only has that form if the project sets one (PROJECT_VER / version.txt); a
// git-describe string would give four arbitrary digits, so it shows as 0.0.0.0.
static void fw_version_set(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    const char *v = d ? d->version : "";
    bool dotted = strlen(v) >= 7;
    for (int i = 0; dotted && i < 7; i++) dotted = (i & 1) ? v[i] == '.' : (v[i] >= '0' && v[i] <= '9');
    snprintf(g_oem.fw_version, sizeof g_oem.fw_version, "%.7s", dotted ? v : "0.0.0.0");
}

bool brush_app_start(const app_config_t *cfg)
{
    static bool called;
    bool radios = !hw_emulated();            // QEMU has no Wi-Fi / BLE: app_main must not start them
    if (called) return radios;
    called = true;
    oem_glue_early_init();                   // app_main has done it; harmless if not
    s_has_ssid = cfg && cfg->wifi_ssid[0] != '\0';
    // The time zone before the main task exists: its first hal_time() (day totals,
    // greeting) is in oem_app_boot(), and after a wake from deep sleep the clock is
    // already right then (the RTC keeps it).
    if (!cfg || !oem_glue_set_tz(cfg->tz)) oem_glue_set_tz(APP_CONFIG_TZ_DEFAULT);
    fw_version_set();

    // Order: NOTES_power 2.1, NOTES_input 4, NOTES_led 2, NOTES_motor 6, NOTES_ui 5.
    // The hook first: oem_power_boot() itself may go back to sleep (empty battery,
    // refused motion wake) and then does not return.
    hw_power_set_pre_sleep_hook(stop_radios);
    s_wake_cause = oem_power_boot();         // wake cause, pad holds released, IMU, GPIO37 low
    oem_touch_init();                        // right after it: the RDY line is held low from here
    oem_charge_pins_init();
    oem_batt_adc_init();
    oem_led_init();                          // LEDC, everything dark
    hw_display_init();                       // bus only; the panel is initialised by hal_lcd_init()
    if (!ui_res_init()) ESP_LOGW(TAG, "no OEM pictures: the screens will be black");
    oem_button_init();
    oem_motor_init();
    // The stream task above the web server and the MQTT client (see MOTOR_PRIO). Done
    // here and not in hw_motor.c, which creates it as stock does. Without the task
    // (hw_motor.c has logged why) there is nothing to raise; NULL would mean "this task".
    TaskHandle_t motor = xTaskGetHandle(MOTOR_TASK);
    if (motor) vTaskPrioritySet(motor, MOTOR_PRIO);
    // oem_pressure_init() is left to the first oem_pressure_start(), which the second
    // half of the boot calls after the IMU is up, where stock runs it.

    s_ev[GRP_MAIN] = xEventGroupCreate();
    s_ev[GRP_UI] = xEventGroupCreate();
    s_booted = xSemaphoreCreateBinary();
    s_fb = heap_caps_malloc(UI_FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);   // sent to the panel as it is
    if (!s_ev[GRP_MAIN] || !s_ev[GRP_UI] || !s_booted || !s_fb || !timers_create()) {
        // Like safe mode: no brush logic, but the web UI can still update the firmware.
        ESP_LOGE(TAG, "out of memory: the brush logic is NOT started");
        s_ev[GRP_MAIN] = s_ev[GRP_UI] = NULL;
        return radios;
    }
    memset(s_fb, 0, UI_FB_BYTES);
    oem_ui_init(s_fb);                       // before anything posts a UI message

    // Interrupt handlers (button, touch RDY, motion, charger) post through the FreeRTOS
    // timer service task. The project configures it at priority 1, below everything
    // here: its turn would come only once the first core has nothing else to do.
    // Stock relays its GPIO interrupts through a priority-30 task ("key_int"). At the
    // priority of the esp_timer task an interrupt posts as promptly as a timer does.
    // Nothing else in this firmware uses FreeRTOS software timers.
    vTaskPrioritySet(xTimerGetTimerDaemonTaskHandle(), ESP_TASK_TIMER_PRIO);

    hal_timer_start(HAL_TMR_TICK10, 10, true);
    hal_timer_start(HAL_TMR_BLINK50, 50, true);
    if (xTaskCreatePinnedToCore(ui_task, "UI_TASK", UI_STACK, NULL, UI_PRIO, &s_ui_task, BRUSH_CORE) != pdPASS
            || xTaskCreatePinnedToCore(main_task, "brush_app", MAIN_STACK, NULL, MAIN_PRIO, &s_main_task, BRUSH_CORE) != pdPASS) {
        // Without the main task nothing posts to the UI: a UI task that exists just waits.
        ESP_LOGE(TAG, "task creation failed: the brush logic is NOT started");
        hal_timer_stop(HAL_TMR_TICK10);
        hal_timer_stop(HAL_TMR_BLINK50);
        return radios;
    }
    // The first thing the main task does is oem_app_boot() (settings, gauge, panel
    // init, first screen). Waiting for it means that what app_main starts next (Wi-Fi,
    // web, MQTT, BLE) finds g_oem loaded; wifi_mgr_start() writes into it.
    if (xSemaphoreTake(s_booted, pdMS_TO_TICKS(BOOT_WAIT_MS)) != pdTRUE)
        ESP_LOGE(TAG, "oem_app_boot() has not finished after %d ms", BOOT_WAIT_MS);
    s_radios = radios;
    s_running = true;
    ESP_LOGI(TAG, "brush app running (wake cause %d, tasks on core %d, prio %d / %d, motor stream prio %d); radios %s",
             s_wake_cause, BRUSH_CORE, MAIN_PRIO, UI_PRIO, motor ? (int)uxTaskPriorityGet(motor) : -1,
             radios ? "allowed" : "off (emulated)");
    return radios;
}

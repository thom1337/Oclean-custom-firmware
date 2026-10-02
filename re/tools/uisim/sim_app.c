// Host test of the app + brush engine (main/oem_app.c, main/oem_brush.c).
//   cc -std=gnu11 -Wall -Wextra -I main re/tools/uisim/sim_app.c main/oem_app.c main/oem_brush.c -o sim_app
//   ./sim_app            all scenarios, trace + checks
//   ./sim_app -v         also the hal_log() lines
//   ./sim_app boot_idle  one scenario (prefix match)
//
// The HAL is faked with virtual time: a loop advances 1 ms at a time, fires the timers
// and calls oem_app_handle() with the bits that were posted. The functions of the
// other modules (UI, LEDs, gauge, input, motor, power) are fakes that record the
// calls; the few of them that have behaviour the app logic depends on imitate it
// (touch IC reaching state 5 after some RDY edges, LED requests dropped while a script
// runs, the gauge declaring "full" after six ticks at 100 %).
//
// Every scenario runs in its own process (fork), i.e. from a fresh boot.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_state.h"

// ---- trace ------------------------------------------------------------------------------

static uint32_t g_ms;
static int  g_fail, g_checks;
static bool g_verbose;

#define LOG_MAX 60000
static char     g_log[LOG_MAX][72];
static uint32_t g_log_ms[LOG_MAX];
static int      g_log_n;

static void trv(bool print, const char *fmt, va_list ap)
{
    char line[160];
    vsnprintf(line, sizeof line, fmt, ap);
    if (g_log_n < LOG_MAX) {
        snprintf(g_log[g_log_n], sizeof g_log[0], "%.71s", line);
        g_log_ms[g_log_n++] = g_ms;
    }
    if (print) printf("  [%8.3f] %s\n", g_ms / 1000.0, line);
}
static void tr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tr(const char *fmt, ...) { va_list ap; va_start(ap, fmt); trv(true, fmt, ap); va_end(ap); }
static void trq(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // logged, not printed
static void trq(const char *fmt, ...) { va_list ap; va_start(ap, fmt); trv(g_verbose, fmt, ap); va_end(ap); }

static int mark(void) { return g_log_n; }
static int find(const char *sub, int from)
{
    if (from < 0) return -1;
    for (int i = from; i < g_log_n; i++) if (strstr(g_log[i], sub)) return i;
    return -1;
}
static int count(const char *sub, int from)
{
    int n = 0;
    for (int i = from < 0 ? 0 : from; i < g_log_n; i++) if (strstr(g_log[i], sub)) n++;
    return n;
}

#define CHECK(cond, ...) do { \
    g_checks++; \
    bool ok_ = (cond); \
    if (!ok_) g_fail++; \
    printf("  %s ", ok_ ? "  ok  " : "**FAIL**"); printf(__VA_ARGS__); printf("\n"); } while (0)

// "sub" appears at or after log index `from`, at time t_ms +- tol_ms. Returns its index.
static int expect_at(const char *sub, int from, uint32_t t_ms, uint32_t tol_ms)
{
    int i = find(sub, from);
    if (i < 0) { CHECK(false, "\"%s\" expected at %.3f s: not seen", sub, t_ms / 1000.0); return from; }
    uint32_t t = g_log_ms[i];
    uint32_t d = t > t_ms ? t - t_ms : t_ms - t;
    CHECK(d <= tol_ms, "\"%s\" at %.3f s (expected %.3f +- %.3f)", sub, t / 1000.0, t_ms / 1000.0, tol_ms / 1000.0);
    return i;
}
static int expect(const char *sub, int from)
{
    int i = find(sub, from);
    CHECK(i >= 0, "\"%s\"%s", sub, i >= 0 ? "" : " not seen");
    return i < 0 ? from : i;
}
static void expect_none(const char *sub, int from)
{
    int i = find(sub, from);
    CHECK(i < 0, "no \"%s\"%s", sub, i < 0 ? "" : " (but it was seen)");
}

// ---- fake HAL -----------------------------------------------------------------------------

static uint32_t g_pending;
static int      g_lock_depth;
static bool     g_ssid = true, g_ble;
static int      g_deep_sleep;          // 0 no, 1 = EXT1 only ("deep sleep1"), 2 = with motion wake
static bool     g_restart;
static oem_time_t g_time = { .year = 26, .month = 10, .day = 2, .hour = 8, .min = 0, .sec = 0, .wday = 5 };

#define T_BIT(n, g, b) b,
#define T_GRP(n, g, b) GRP_##g,
#define T_NAME(n, g, b) #n,
enum { GRP_MAIN, GRP_UI };
static const uint32_t    TMR_BIT[] = { HAL_TIMER_LIST(T_BIT) };
static const uint8_t     TMR_GRP[] = { HAL_TIMER_LIST(T_GRP) };
static const char *const TMR_NAME[] = { HAL_TIMER_LIST(T_NAME) };
static struct { bool on, periodic; uint32_t period, due; } g_tmr[HAL_TMR_COUNT];

void hal_lock(void) { g_lock_depth++; }
void hal_unlock(void) { g_lock_depth--; }
uint32_t hal_ms(void) { return g_ms; }
uint32_t hal_uptime_s(void) { return g_ms / 1000; }
void hal_time(oem_time_t *t) { *t = g_time; }
void hal_log(const char *fmt, ...)
{
    char line[160];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    trq("log: %s", line);
}
void hal_event_post(uint32_t bits)
{
    if (bits & OEM_EV_SESSION_START) tr("event SESSION_START");
    if (bits & OEM_EV_SESSION_END) tr("event SESSION_END");
    g_pending |= bits;
}
void hal_ui_event_post(uint32_t bits) { (void)bits; }
void hal_timer_start(hal_timer_t t, uint32_t ms, bool periodic)
{
    if (t != HAL_TMR_TICK10) tr("timer %s start %u ms%s", TMR_NAME[t], ms, periodic ? " periodic" : "");
    g_tmr[t].on = true; g_tmr[t].periodic = periodic; g_tmr[t].period = ms; g_tmr[t].due = g_ms + ms;
}
void hal_timer_stop(hal_timer_t t)
{
    if (g_tmr[t].on) tr("timer %s stop", TMR_NAME[t]);
    g_tmr[t].on = false;
}
static void clock_advance(uint32_t ms)
{
    while (ms--) {
        g_ms++;
        for (int t = 0; t < HAL_TMR_COUNT; t++) {
            if (!g_tmr[t].on || (int32_t)(g_ms - g_tmr[t].due) < 0) continue;
            if (TMR_GRP[t] == GRP_MAIN) g_pending |= TMR_BIT[t];
            if (g_tmr[t].periodic) g_tmr[t].due += g_tmr[t].period;
            else g_tmr[t].on = false;
        }
    }
}
void hal_delay(uint32_t ms) { clock_advance(ms); }   // the handler blocks; timer bits pile up

static struct { char key[16]; uint8_t data[256]; size_t len; int writes; } g_nvs[8];
static int nvs_slot(const char *key, bool create)
{
    for (int i = 0; i < 8; i++) if (!strcmp(g_nvs[i].key, key)) return i;
    if (!create) return -1;
    for (int i = 0; i < 8; i++) if (!g_nvs[i].key[0]) { snprintf(g_nvs[i].key, sizeof g_nvs[i].key, "%s", key); return i; }
    abort();
}
size_t hal_nvs_get(const char *key, void *buf, size_t len)
{
    int i = nvs_slot(key, false);
    if (i < 0) return 0;
    memcpy(buf, g_nvs[i].data, len < g_nvs[i].len ? len : g_nvs[i].len);
    return g_nvs[i].len;
}
bool hal_nvs_set(const char *key, const void *buf, size_t len)
{
    int i = nvs_slot(key, true);
    memcpy(g_nvs[i].data, buf, len);
    g_nvs[i].len = len;
    g_nvs[i].writes++;
    tr("nvs write %s (%zu bytes)", key, len);
    return true;
}
static const uint8_t *nvs_blob(const char *key) { int i = nvs_slot(key, false); return i < 0 ? NULL : g_nvs[i].data; }

static oem_rtc_t g_rtc = { .magic = OEM_RTC_MAGIC, .ota_oneshot = 1, .strength = 3, .hist_day = 0xff };
oem_rtc_t *hal_rtc(void) { return &g_rtc; }

void hal_lcd_init(void) { tr("lcd init"); }
void hal_lcd_sleep(void) { tr("lcd sleep (SLPIN)"); }
void hal_lcd_blit(const uint8_t *fb) { (void)fb; }
bool hal_res_read(uint32_t off, void *dst, size_t len) { (void)off; (void)dst; (void)len; return false; }
void hal_restart(void) { tr("RESTART"); g_restart = true; }
bool hal_wifi_has_ssid(void) { return g_ssid; }
bool hal_ble_connected(void) { return g_ble; }
void hal_net_sleep(void) { tr("net sleep (Wi-Fi off)"); }
void hal_net_wake(void) { tr("net wake"); }
bool hal_anymotion_allowed(void) { return true; }

// ---- fake UI ------------------------------------------------------------------------------

static uint8_t g_ui_now = 0xff;
static bool    g_ui_enabled = true;
void oem_ui_post(uint16_t id, const void *payload, int len)
{
    char p[48] = "";
    const uint8_t *b = payload;
    for (int i = 0, o = 0; i < len && o < 40; i++) o += snprintf(p + o, sizeof p - o, "%s%u", i ? "," : "", b[i]);
    bool refresh = id == 82 && g_ui_now == 82 && b[1] % 30 != 0;   // per-second countdown refresh
    if (refresh) trq("ui post %u {%s}", id, p);
    else if (len) tr("ui post %u {%s}", id, p);
    else tr("ui post %u", id);
    if (id >= 70) g_ui_now = (uint8_t)id;
    if (id == 0) g_ui_enabled = true;
}
uint8_t oem_ui_now(void) { return g_ui_now; }
bool oem_ui_enabled(void) { return g_ui_enabled; }
void oem_ui_set_enabled(bool on) { g_ui_enabled = on; }
void oem_ui_lock_button(int arg) { tr("ui lock_button(%d)", arg); }
void oem_ui_page_back(void) { tr("ui page_back"); g_oem.subpage = 0; }
bool oem_ui_swipe_allowed(void) { return true; }
void oem_ui_page_reset(void) { tr("ui page_reset"); }

// ---- fake LEDs ------------------------------------------------------------------------------
// State per LED; a request with a script (anim 0..3) blocks other requests until the
// script is over, like the real engine. Only changes are traced.

static uint8_t  g_led[5] = { 0xff, 0xff, 0xff, 0xff, 0xff };
static bool     g_script;
static uint32_t g_script_end;
static int      g_pend_led, g_pend_state, g_led_dropped;
static const uint16_t SCRIPT_MS[4] = { 2300, 2300, 1510, 510 };

void oem_led_init(void) {}
void oem_led_park(void) {}
static void led_apply(int led, int state)
{
    if (g_led[led] == state) return;
    g_led[led] = (uint8_t)state;
    tr("led %d -> %s", led, state == 0 ? "ON" : state == 1 ? "off" : state == 2 ? "BLINK" : "BREATHE");
}
void oem_led_set(int led, int state, int anim)
{
    if (g_script) {
        if (g_led[led] != state) { g_led_dropped++; trq("led_set(%d,%d,%d) dropped: script running", led, state, anim); }
        return;
    }
    if (anim == 4) { led_apply(led, state); return; }
    tr("led script %d, then led %d = %d", anim, led, state);
    g_script = true; g_script_end = g_ms + SCRIPT_MS[anim]; g_pend_led = led; g_pend_state = state;
}
void oem_led_all(int state) { for (int i = 0; i < 4; i++) oem_led_set(i, state, 4); }
void oem_led_abort_script(void) { if (g_script) tr("led script aborted"); g_script = false; }
void oem_led_tick(void)
{
    if (g_script && (int32_t)(g_ms - g_script_end) >= 0) { g_script = false; led_apply(g_pend_led, g_pend_state); }
}
void oem_led_level(int hal_id, int level) { tr("led_level(hal %d, %d)", hal_id, level); }
void oem_led_reinit_charge_light(void)
{
    tr("led reinit charge light");
    g_script = false;
    led_apply(2, g_oem.power_state == OEM_PWR_FULL ? 0 : 3);
}

// ---- fake gauge / charger -------------------------------------------------------------------

static bool g_charger;
static int  g_gauge_ticks;
void oem_gauge_boot(void)
{
    g_oem.power_state = OEM_PWR_BATTERY; g_oem.plug_cnt = 100; g_oem.gauge_period = 64; g_oem.gauge_inited = 0;
}
void oem_gauge_tick(void)
{
    g_gauge_ticks++;
    if (g_oem.plug_cnt <= 9) g_oem.plug_cnt++;
    if (g_oem.plug_cnt == 3) { oem_led_set(0, 1, 4); oem_led_set(1, 1, 4); oem_led_set(2, 3, 2); }
    if (!g_oem.gauge_inited && g_gauge_ticks >= 3) { g_oem.gauge_inited = 1; tr("gauge: first measurement done"); }
    if (g_oem.batt_pct > 99 && g_oem.power_state == OEM_PWR_CHARGING) {
        if (g_oem.full_cnt < 10) g_oem.full_cnt++;
        if (g_oem.full_cnt > 5) { g_oem.power_state = OEM_PWR_FULL; tr("gauge: FULL"); oem_led_set(2, 0, 4); }
    }
}
void oem_gauge_save(void) { tr("gauge save"); }
int  oem_batt_mv_now(void) { return 3900; }
void oem_charge_thermal_check(void) {}
void oem_gauge_settle_reset(void) { trq("gauge settle_reset"); }
void oem_gauge_rearm(uint8_t n) { trq("gauge rearm(%u)", n); }
void oem_gauge_session_done(uint16_t secs) { tr("gauge session_done(%u)", secs); }
void oem_gauge_sleep_enter(void) { trq("gauge sleep_enter"); }
void oem_gauge_sleep_exit(void) { trq("gauge sleep_exit"); }
bool oem_charger_present(void) { return g_charger; }
void oem_charge_allow(bool allow) { trq("charge_allow(%d)", allow); }
void oem_wlc_off(void) { trq("wlc_off"); }
bool oem_charger_alive_take(void) { return false; }

// ---- fake input -------------------------------------------------------------------------------

static uint8_t g_touch = 0;
static bool    g_touch_irq, g_have_pressure, g_pressure_on;
static int     g_force, g_force_wobble;       // value fed to g_oem.pressure, +- wobble per sample
static int     g_touch_ticks;
void oem_touch_init(void) {}
void oem_touch_set_state(uint8_t st) { if (st != g_touch) tr("touch state 0x%02x", st); g_touch = st; }
uint8_t oem_touch_state(void) { return g_touch; }
void oem_touch_irq(bool enable) { if (enable != g_touch_irq) tr("touch irq %s", enable ? "on" : "off"); g_touch_irq = enable; }
void oem_touch_step(void)
{
    uint8_t n = g_touch;
    switch (g_touch) {
    case 0: n = 0x20; break;
    case 0x20: n = 0x21; break;
    case 0x21: n = 1; break;
    case 1: case 2: case 3: case 6: case 10: case 11: n = 5; break;
    }
    if (n != g_touch) { g_touch = n; if (n == 5) tr("touch state 0x05 (running)"); }
}
void oem_touch_tick_10ms(void) { g_touch_ticks++; }
void oem_gesture_end(void) {}
void oem_button_init(void) {}
void oem_pressure_init(void) {}
void oem_pressure_start(void)
{
    if (g_pressure_on) return;
    tr("pressure sampling on");
    g_pressure_on = true;
    g_tmr[HAL_TMR_PRESSURE20].on = true; g_tmr[HAL_TMR_PRESSURE20].periodic = true;
    g_tmr[HAL_TMR_PRESSURE20].period = 20; g_tmr[HAL_TMR_PRESSURE20].due = g_ms + 20;
}
void oem_pressure_stop(void)
{
    if (g_pressure_on) tr("pressure sampling off");
    g_pressure_on = false;
    g_tmr[HAL_TMR_PRESSURE20].on = false;
}
void oem_pressure_sample(void)
{
    static int flip;
    flip ^= 1;
    g_oem.pressure = (int16_t)(g_force + (flip ? g_force_wobble : -g_force_wobble));
}
bool oem_pressure_available(void) { return g_have_pressure; }

// ---- fake motor -------------------------------------------------------------------------------

static bool g_motor_on;
static int  g_motor_gear, g_amp = 1;
void oem_motor_init(void) {}
void oem_motor_gear(uint8_t gear_id, bool use_app_table)
{
    tr("motor gear %u%s%s", gear_id, use_app_table ? " (app table)" : "", g_oem.motor_state == 2 ? " [duty halved]" : "");
    g_motor_on = true; g_motor_gear = gear_id; g_amp = 1;
}
void oem_motor_off(void) { if (g_motor_on) tr("motor off"); g_motor_on = false; g_motor_gear = 0; }
bool oem_motor_playing(void) { return g_motor_on; }
void oem_motor_amp(bool on) { tr("amp %d", on); g_amp = on; }
void oem_motor_app_gear_set(int index, const uint8_t entry[7]) { (void)index; (void)entry; }

// ---- fake power / IMU -------------------------------------------------------------------------

static bool g_motion_irq = true;
int  oem_power_boot(void) { return OEM_WAKE_COLD; }
void oem_power_prep_screen_off(void) { tr("power prep_screen_off"); }
void oem_power_restore_after_screen_off(void) { tr("power restore_after_screen_off"); }
void oem_power_deep_sleep(bool motion_wake)
{
    tr("DEEP SLEEP (%s)", motion_wake ? "deep sleep2: button, charger, motion" : "deep sleep1: button, charger");
    g_deep_sleep = motion_wake ? 2 : 1;
}
void oem_power_stay_alive(void) { tr("power stay_alive"); }
void oem_power_resume_sleep(void) { tr("power resume_sleep"); }
void oem_power_cpu_lock(bool take) { tr("cpu lock %s", take ? "taken" : "released"); }
void oem_motion_irq(bool enable) { if (enable != g_motion_irq) tr("motion irq %s", enable ? "on" : "off"); g_motion_irq = enable; }
void oem_imu_normal(void) { trq("imu normal"); }
void oem_imu_amd(void) { tr("imu any-motion mode"); }
void oem_imu_power_down(void) { tr("imu power down"); }
bool oem_imu_temp(float *celsius) { *celsius = 25; return true; }

// ---- running the device -------------------------------------------------------------------------

static uint32_t g_blk[4000];           // times of the 1 Hz blocks (every 100th handled tick)
static int      g_blk_n;
static uint32_t g_tick_passes;

// Advance virtual time. The brush "hardware": the touch IC pulls RDY every 12 ms while
// its interrupt is enabled and it has something to do; nothing runs after deep sleep.
static void run_ms(uint32_t ms)
{
    while (ms-- && !g_deep_sleep && !g_restart) {
        clock_advance(1);
        if (g_touch_irq && g_touch != 5 && g_touch != 7 && g_ms % 12 == 0) g_pending |= OEM_EV_TOUCH_RDY;
        if (g_pending) {
            uint32_t bits = g_pending;
            g_pending = 0;
            if ((bits & OEM_EV_TICK) && ++g_tick_passes % 100 == 0 && g_blk_n < 4000) g_blk[g_blk_n++] = g_ms;
            hal_lock();
            oem_app_handle(bits);
            hal_unlock();
        }
    }
}
static void run_until(uint32_t t_ms) { if (t_ms > g_ms) run_ms(t_ms - g_ms); }

// The 1 Hz block is counted in 10 ms ticks, and ticks that fall into a handler that
// blocks (wake 100 ms, session start 30 ms, pause 100 ms ...) merge into one, exactly
// as the event bit does in stock. So "n seconds later" is "n blocks later", and the
// checks below are written in blocks.
static uint32_t blk_after(uint32_t t_ms, int n)          // time of the n-th block after t_ms (0: not run yet)
{
    for (int i = 0; i < g_blk_n; i++) if (g_blk[i] > t_ms && --n == 0) return g_blk[i];
    return 0;
}
static int blk_count(uint32_t t_a, uint32_t t_b)         // blocks in (t_a, t_b]
{
    int n = 0;
    for (int i = 0; i < g_blk_n; i++) if (g_blk[i] > t_a && g_blk[i] <= t_b) n++;
    return n;
}
static uint32_t blk_uptime(uint32_t sec)                 // first block at which hal_uptime_s() >= sec
{
    for (int i = 0; i < g_blk_n; i++) if (g_blk[i] / 1000 >= sec) return g_blk[i];
    return 0;
}
static uint32_t t_of(const char *sub, int from)          // time of a trace line (0: none)
{
    int i = find(sub, from);
    return i < 0 ? 0 : g_log_ms[i];
}

// Returns the time of the loop pass that handles the press.
static uint32_t press(uint8_t code)
{
    static const char *const NAME[] = { "short", "2 s", "3 s", "5 s", "8 s" };
    tr(">>> button %s", NAME[code]);
    oem_button_push(code);
    hal_event_post(OEM_EV_BUTTON);
    uint32_t t = g_ms + 1;
    run_ms(1);
    return t;
}

// Boot the brush: NVS as given, battery percent, then the main-task init. The glue
// starts the 10 ms tick when the task enters its loop; the second half of the boot
// sequence runs 50 ticks later.
static void boot(int stage, uint8_t batt, int cause)
{
    if (stage >= 0) {
        uint8_t sys[100] = { (uint8_t)stage };
        int i = nvs_slot("sys_config", true);
        memcpy(g_nvs[i].data, sys, sizeof sys);
        g_nvs[i].len = sizeof sys;
    }
    g_oem.batt_pct = batt;
    tr(">>> boot (boot stage %d, battery %u %%, wake cause %d)", stage, batt, cause);
    hal_lock();
    oem_app_boot(cause);
    hal_unlock();
    hal_timer_start(HAL_TMR_TICK10, 10, true);
}

static void note(const char *s) { printf("  -- %s\n", s); }

// What the UI task does on a swipe to another mode page.
static void ui_select_mode(uint8_t mode)
{
    static const uint8_t SCR[6] = { 81, 79, 77, 76, 78, 80 };
    hal_lock(); oem_set_mode(mode); oem_show(SCR[mode], NULL, 0); hal_unlock();
}
// What the UI task does on a swipe up / down on the pause screen (page_commit).
static void ui_swipe_quit(uint8_t new_screen)
{
    note("swipe on the pause screen (UI: user_quit = 1, oem_motor_stop(), now_ui = target)");
    hal_lock(); g_oem.user_quit = 1; oem_motor_stop(); g_oem.now_ui = new_screen; hal_unlock();
}

// ---- scenarios ----------------------------------------------------------------------------------

// Cold boot -> wake screen -> mode page after 6 s -> idle 30 s -> screen-off sequence ->
// second stage -> BLE window 30 s -> deep sleep with motion wake.
static void sc_boot_idle_sleep(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    CHECK(g_ms == 0 && g_oem.init_ok == 0, "oem_app_boot() does not block; inputs not live yet");
    int m = 0;
    m = expect("lcd init", m);
    m = expect("ui post 84 {0,0,0,0,0,0,0,0}", m);          // history, the stock wake screen
    m = expect("led 4 -> ON", m);
    run_until(600);
    expect_at("touch irq on", m, 500, 0);                    // second half 500 ms later (stock vTaskDelay(500))
    expect_at("led 1 -> ON", m, 500, 0);
    expect_at("pressure sampling on", m, 500, 0);
    CHECK(g_oem.init_ok == 5 && g_oem.power_state == OEM_PWR_BATTERY, "init_ok 5, on battery");
    CHECK(g_oem.profile[6] == 5 && g_oem.profile[7] == 5, "factory default mode 5");
    CHECK(g_oem.lang == 2, "language 2 without the 0xAA marker");
    CHECK(nvs_blob("sys_config")[0] == 2, "boot stage stays 2");
    CHECK(nvs_blob("user_config") && nvs_blob("user_config")[19] == 5 && nvs_blob("user_config")[45] == 180,
          "default user_config written (mode 5, profile[3] 180)");

    run_until(7000);
    CHECK(g_blk_n == 7 && g_blk[0] == 1000 && g_blk[6] == 7000, "1 Hz block every 100 ticks");
    m = expect_at("ui post 80", m, blk_after(0, 6), 0);     // dwell 6 -> mode page of mode 5
    CHECK(g_oem.now_ui == 80, "now_ui 80");
    CHECK(g_touch == 5, "touch IC reached state 5");
    CHECK(g_oem.gauge_inited == 1, "gauge first measurement done");

    run_until(40000);
    uint32_t ti = blk_uptime(6 + 30);                       // show_main at uptime 6 s, idle 30 s
    m = expect_at("led script 3, then led 1 = 1", m, ti, 0);
    expect_at("touch state 0x07", m, ti, 0);
    uint32_t tseq = ti + 550;
    int seq = expect_at("lcd sleep", m, tseq, 0);
    CHECK(count("nvs write user_config", 0) == 1, "unchanged settings are not written again");
    expect("imu any-motion mode", m);
    expect_none("imu power down", m);
    expect("pressure sampling off", seq);
    expect("touch irq off", seq);
    expect("ui post 1", seq);
    expect("timer BLE_TIMEOUT start 30000 ms", seq);        // SSID stored, no BLE client
    CHECK(g_oem.wake_gate == 5 && g_oem.asleep == 1, "wake_gate 5, asleep");
    CHECK(!g_ui_enabled, "UI drawing disabled");
    CHECK(g_led[0] == 1 && g_led[1] == 1 && g_led[2] == 1 && g_led[3] == 1 && g_led[4] == 1, "all LEDs and the backlight off");
    int st2 = expect_at("power prep_screen_off", seq, tseq + 200, 0);
    expect("power resume_sleep", st2);
    expect("motion irq on", st2);
    CHECK(g_oem.motion_gate == 0, "motion gate open");
    expect_none("DEEP SLEEP", 0);

    run_until(70000);
    expect_at("net sleep", st2, blk_after(tseq, 27), 0);    // Wi-Fi off 27 s after the screen
    expect("gauge save", st2);
    expect_at("DEEP SLEEP (deep sleep2", st2, tseq + 30000 + 150, 0);
    CHECK(g_deep_sleep == 2, "deep sleep with motion wake");
}

// Client activity keeps the brush in the screen-off stage.
static void sc_net_activity(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(60000);
    int m = expect("timer BLE_TIMEOUT start 30000 ms", 0);
    note("web client active at 60 s and 80 s");
    oem_net_activity(); run_until(80000);
    oem_net_activity(); run_until(125000);
    int d = expect_at("DEEP SLEEP", m, 80001 + 30000 + 150, 0);
    int n = 0;
    for (int i = m + 1; i < d; i++) if (strstr(g_log[i], "timer BLE_TIMEOUT start 30000 ms")) n++;
    CHECK(n == 2, "window restarted twice (%d)", n);
    expect_at("net sleep", m, blk_after(80001, 27), 0);     // the Wi-Fi-off counter restarts too
    CHECK(g_lock_depth == 0, "lock balanced");
}

static void sc_no_ssid_window(void)
{
    g_ssid = false;
    boot(2, 80, OEM_WAKE_COLD);
    run_until(170000);
    int m = expect("timer BLE_TIMEOUT start 120000 ms", 0);
    expect_at("DEEP SLEEP", m, g_log_ms[m] + 120000 + 150, 0);
}

// Button in the screen-off stage: first press wakes only, second press starts a mode-5
// session. With the force sensor: idle hum until contact, intensity swipe, over-pressure,
// zone cues, step at 150 s, auto stop at 302 s, result screens, sleep.
static void sc_wake_session_mode5(void)
{
    g_have_pressure = true;
    boot(2, 80, OEM_WAKE_COLD);
    run_until(40000);
    CHECK(g_oem.asleep == 1 && g_oem.wake_gate == 5, "in the screen-off stage");
    g_rtc.gyro_wakeup_count = 2;

    int m = mark();
    uint32_t tp = press(0);
    expect("power stay_alive", m);
    expect("power restore_after_screen_off", m);
    expect("lcd init", m);
    expect("led script 0, then led 1 = 0", m);
    expect("timer BLE_TIMEOUT stop", m);
    expect("ui post 84", m);
    expect("ui post 0", m);
    expect("motion irq off", m);
    expect("net wake", m);
    expect_none("motor gear", m);
    CHECK(g_oem.asleep == 0 && g_oem.state == OEM_ST_WOKEN && !g_oem.session_active, "awake, state 1, no session (first press wakes only)");
    CHECK(g_rtc.gyro_wakeup_count == 0, "gyro wake count cleared by the button");
    CHECK(g_ms == tp + 100, "wake takes 100 ms");

    run_until(42000);
    CHECK(g_touch == 5, "touch IC re-initialised");
    m = mark();
    uint32_t ts = press(0);                                  // start
    expect("ui post 87 {2}", m);                             // intensity screen, level 3
    expect("motor gear 49", m);                              // anti-splash: idle hum first
    expect("cpu lock taken", m);
    expect("event SESSION_START", m);
    expect("timer FAST30 start 30 ms periodic", m);
    expect_none("touch irq off", m);                         // mode 5: touch stays on for the intensity swipe
    CHECK(g_oem.session_active && g_oem.running && g_oem.motor_state == 0, "session active, motor_state 0 (waiting for contact)");
    CHECK(g_oem.total_s == 300 && g_oem.step_count == 2 && g_oem.gear == 1, "mode 5: 2 steps, 300 s, strength 3 = gear 1");
    CHECK(g_oem.saved_mode == 5 && g_oem.state == OEM_ST_SESSION, "saved_mode 5, state 2");
    CHECK(g_ms == ts + 30, "start handler takes 30 ms");

    run_until(43000);
    CHECK(count("motor gear", m) == 1 && g_oem.elapsed_s == 1, "still humming after 1 s without contact");
    note("bristles touch the teeth at 43.0 s (force 80 +- 20)");
    g_force = 80; g_force_wobble = 20;
    run_until(44000);
    int c = expect_at("motor gear 1", m, 43300, 300);        // a few 30 ms samples until the filter fires
    CHECK(g_oem.motor_state == 1, "motor_state 1");
    expect("led 2 -> ON", c);                                // "pressure fine" light

    note("swipe up at 44.0 s on the intensity screen");
    m = mark();
    hal_lock(); oem_strength_step(true); hal_unlock();
    expect("ui post 87 {3}", m);
    expect("motor gear 24", m);
    CHECK(g_rtc.strength == 4 && g_oem.gear == 24, "strength 4 stored, gear 24");
    run_until(50000);
    expect_at("touch irq off", m, blk_after(44000, 5), 0);   // 5 s after the last swipe: 87 -> 82
    expect_at("ui post 82 {1,", m, blk_after(44000, 5), 0);
    CHECK(g_oem.now_ui == 82, "now_ui 82");
    note("swipe down at 50 s, screen 87 gone: stored, not applied");
    m = mark();
    hal_lock(); oem_strength_step(false); hal_unlock();
    CHECK(g_rtc.strength == 3 && g_oem.gear == 24, "RTC strength 3, gear still 24");
    expect_none("motor gear", m);

    run_until(60000);
    note("pressing too hard at 60 s (force 500), harder at 62 s (700), released at 64 s");
    m = mark();
    g_force = 500; g_force_wobble = 0; run_until(62000);
    c = expect_at("motor gear 24 [duty halved]", m, 60030, 30);
    expect("led 3 -> ON", m);
    CHECK(g_led[2] == 1 && g_oem.motor_state == 2, "led 2 off, motor_state 2");
    g_force = 700; run_until(64000);
    expect("led 3 -> BLINK", c);
    g_force = 80; g_force_wobble = 20; run_until(66000);
    expect_at("motor gear 24", c + 1, 64000 + 34 * 30, 60);  // 34 samples below 390
    CHECK(g_oem.motor_state == 1 && g_led[2] == 0 && g_led[3] == 1, "back to normal: state 1, led 2 on, led 3 off");
    CHECK(g_oem.contact_s > 15, "contact seconds counted (%u)", g_oem.contact_s);

    run_until(ts + 306000);
    int cues = 0, last = 0;
    for (int n = 30; n <= 300; n += 30) {
        int off = find("amp 0", last);
        int on = find("amp 1", off);
        if (off < 0 || on < 0) break;
        uint32_t want = blk_after(ts, n);
        if (g_log_ms[off] >= want && g_log_ms[off] <= want + 30 && g_log_ms[on] - g_log_ms[off] == 60) cues++;
        else printf("     cue at %d s: off %.3f on %.3f, block %.3f\n", n, g_log_ms[off] / 1000.0, g_log_ms[on] / 1000.0, want / 1000.0);
        last = on + 1;
    }
    CHECK(cues == 10 && count("amp 0", 0) == 10, "zone cue: amp off for 60 ms at 30, 60 .. 300 s (%d good, %d in all)", cues, count("amp 0", 0));
    uint32_t t150 = blk_after(ts, 150), t302 = blk_after(ts, 302);
    int st = -1;
    for (int i = c + 2; i < g_log_n; i++) if (g_log_ms[i] == t150 && strstr(g_log[i], "motor gear 24")) st = i;
    CHECK(st >= 0, "second step at 150 s re-issues the strength gear 24 (stock: placeholder gear 10 from here on)");
    expect_none("motor gear 10", 0);
    int end = expect_at("motor off", st, t302, 0);           // two steps: 2 s past the total
    expect_at("event SESSION_END", st, t302, 0);
    expect("timer FAST30 stop", end);
    expect("gauge session_done(300)", end);
    uint32_t te = t_of("ui post 103", end);
    CHECK(te == t302 + 1, "complete screen 103 right away");
    expect("cpu lock released", end);
    CHECK(g_oem.done_s == 300 && g_oem.score == 100, "done 300 s, score 100 (substitute: no zone data)");
    CHECK(g_rtc.hist_count == 1 && g_rtc.hist_seconds == 300 && g_rtc.hist_score_sum == 100, "history totals 1 session / 300 s / 100");
    const uint8_t *h = nvs_blob("shuanhuan");
    CHECK(h && h[0] == 1 && h[1] == 44 && h[3] == 100 && h[5] == 1 && h[6] == 2, "NVS shuanhuan: 300 s, 100, 1, day 2");
    size_t rl; const uint8_t *r = oem_brush_record(&rl);
    CHECK(r && rl == 182 && r[1] == 182 && r[2] == 26 && r[3] == 10 && r[4] == 2 && r[8] == 5 && r[9] == 1 && r[10] == 44
          && r[11] == 1 && r[12] == 44 && r[13] == 10 && r[14] == 20 && r[15] == 70 && r[28] == 100 && r[29] == 3,
          "record: len 182, date, profile id 5, total 300, done 300, 10/20/70, score, scheme 3");
    CHECK(r && (r[51 + 20] == 15 || r[51 + 20] == 25), "record pressure log: (80 +- 20) / 4 (got %u)", r ? r[51 + 20] : 0);

    run_until(ts + 345000);
    int s1 = expect_at("ui post 100 {100}", end, blk_after(te, 1), 0);           // complete -> score
    int s2 = expect_at("ui post 84 {44,1,100,0,1,0,1,0}", s1, blk_after(te, 11), 0);   // score 10 s -> history
    int s3 = expect_at("ui post 80", s2, blk_after(te, 21), 0);                  // history 10 s -> mode page
    expect_at("led script 3", s3, blk_uptime(g_log_ms[s3] / 1000 + 10), 0);      // idle 10 s -> sleep
    CHECK(g_oem.asleep == 1, "asleep again");
    CHECK(nvs_blob("user_config")[62] == 100, "last score saved in user_config");
}

// Modes 1..4 without a force sensor: gear right away, single step, auto stop at the
// total, cues every 30 s but not in the last 10 s.
static void fixed_mode(uint8_t mode, uint8_t gear, uint16_t total, int n_cues)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(7500);
    ui_select_mode(mode);
    run_until(8000);
    int m = mark();
    uint32_t ts = press(0);
    char buf[64];
    snprintf(buf, sizeof buf, "motor gear %u", gear);
    expect_at(buf, m, ts, 0);
    expect_none("motor gear 49", m);
    snprintf(buf, sizeof buf, "ui post 82 {1,%u}", total & 0xff);
    expect(buf, m);
    expect("touch irq off", m);
    CHECK(g_oem.total_s == total && g_oem.step_count == 1 && g_oem.motor_state == 1, "total %u s, 1 step, motor_state 1", total);
    int tt = g_touch_ticks;
    run_until(ts + total * 1000u + 15000);
    CHECK(count("motor gear", m) == 1, "one gear command for the whole session");
    CHECK(count("amp 0", m) == n_cues, "%d zone cues (got %d)", n_cues, count("amp 0", m));
    int a = m;
    bool ok = true;
    for (int n = 1; n <= n_cues; n++) {
        a = find("amp 0", a + 1);
        uint32_t want = blk_after(ts, 30 * n);
        if (a < 0 || g_log_ms[a] < want || g_log_ms[a] > want + 30) { ok = false; break; }
    }
    CHECK(ok, "cues at 30 s steps");
    uint32_t tend = blk_after(ts, total);
    int end = expect_at("motor off", m, tend, 0);            // one step: stops exactly at the total
    expect_at("ui post 103", end, tend + 1, 0);
    expect_at("ui post 100 {100}", end, blk_after(tend + 1, 1), 0);
    CHECK(g_oem.done_s == total && g_oem.score == 100 && g_rtc.hist_count == 1, "done %u, score 100, counted", total);
    CHECK(g_led[2] == 1 && g_led[3] == 1, "pressure lights off after the session");
    CHECK(g_touch_ticks - tt < 1700, "touch reseed watchdog paused while the motor ran");
}
static void sc_mode1(void) { fixed_mode(1, 54, 120, 3); }
static void sc_mode2(void) { fixed_mode(2, 47, 180, 5); }
static void sc_mode3(void) { fixed_mode(3, 50, 120, 3); }
static void sc_mode4(void) { fixed_mode(4, 48, 150, 4); }

// Mode 0: scheme stored by the phone app (three steps), then the BLE single-step profile.
static void sc_app_scheme(void)
{
    uint8_t md[255] = { 9, 3, 0, 18, 20, 0, 20, 10, 0, 22, 15 };   // id 9; gears 18 / 20 / 22 for 20 / 10 / 15 s
    hal_nvs_set("motor_data", md, sizeof md);
    boot(2, 80, OEM_WAKE_COLD);
    g_oem.profile[0] = 1;                                    // app scheme present (BLE cmd 0x06)
    run_until(7500);
    CHECK(oem_app_profile(), "app profile");
    ui_select_mode(0);
    run_until(8000);
    int m = mark();
    uint32_t ts = press(0);
    CHECK(g_oem.total_s == 45 && g_oem.step_count == 3, "45 s in 3 steps");
    run_until(ts + 60000);
    expect_at("motor gear 18", m, ts, 0);
    expect_at("motor gear 20", m, blk_after(ts, 20), 0);
    expect_at("motor gear 22", m, blk_after(ts, 30), 0);
    expect_at("motor off", m, blk_after(ts, 47), 0);         // several steps: 2 s past the total
    CHECK(g_oem.done_s == 45 && g_oem.score == 100, "done 45");
    size_t rl; const uint8_t *r = oem_brush_record(&rl);
    CHECK(r && r[8] == 9 && rl == 45 / 2 + 51, "record: profile id 9, len %zu", rl);

    note("profile[2] = 1: one step, gear profile[5], profile[3] seconds");
    g_oem.profile[2] = 1; g_oem.profile[5] = 16; g_oem.profile[3] = 25;
    ui_select_mode(0);
    m = mark();
    ts = press(0);
    expect("motor gear 16", m);
    CHECK(g_oem.total_s == 25 && g_oem.step_count == 1, "25 s, 1 step");
    run_until(ts + 30000);
    expect_at("motor off", m, blk_after(ts, 25), 0);
}

// Pause, resume, pause time-out (30 s), then a second session ended by a swipe.
static void sc_pause_resume(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(7500);
    ui_select_mode(1);
    run_until(8000);
    uint32_t ts = press(0);
    run_until(20000);
    int e1 = blk_count(ts, g_ms);                            // seconds = 1 Hz blocks since the start
    char buf[48];
    CHECK(g_oem.elapsed_s == e1 && e1 >= 11, "%d s brushed at 20.0 s (elapsed_s %u)", e1, g_oem.elapsed_s);
    int m = mark();
    uint32_t tp = press(0);                                  // pause
    expect("motor off", m);
    expect("touch state 0x21", m);
    snprintf(buf, sizeof buf, "ui post 101 {0,%d}", 120 - e1);
    expect(buf, m);
    expect("touch irq on", m);
    CHECK(g_oem.session_active && !g_oem.running && g_oem.stop_delay == 0 && g_oem.muted == 1, "paused: active, not running, stop_delay 0");
    CHECK(g_ms == tp + 100, "pause handler takes 100 ms");
    run_until(25000);
    CHECK(g_oem.elapsed_s == e1 && g_oem.stop_delay == blk_count(tp, g_ms) && g_oem.stop_delay >= 4, "clock frozen, stop_delay counts (%u)", g_oem.stop_delay);
    CHECK(g_led[2] == 1 && g_led[3] == 1, "pressure lights off while paused");
    m = mark();
    uint32_t tr_ = press(0);                                 // resume
    expect("touch irq off", m);
    expect("motor gear 54", m);
    snprintf(buf, sizeof buf, "ui post 82 {1,%d}", 120 - e1);
    expect(buf, m);
    CHECK(g_oem.running && g_oem.stop_delay == 200 && g_oem.muted == 0, "resumed");
    CHECK(g_ms == tr_ + 50, "resume handler takes 50 ms");
    run_until(40000);
    int e2 = e1 + blk_count(tr_, g_ms);
    CHECK(g_oem.elapsed_s == e2, "%d s brushed at 40.0 s (got %u)", e2, g_oem.elapsed_s);
    m = mark();
    tp = press(0);                                           // pause again and leave it
    run_until(75000);
    uint32_t tout = blk_after(tp, 30);                       // 30 s paused
    int e = expect_at("event SESSION_END", m, tout, 0);
    expect_none("ui post 103", e);                           // ended from the pause screen below 120 s
    expect_at("ui post 79", e, tout + 1, 0);                 // mode page, 10 s idle
    CHECK(g_oem.done_s == e2 && g_oem.score == e2 * 100 / 120 && g_rtc.hist_count == 1,
          "%d s, score %u (share of 120 s), counted (>= 15 s)", e2, g_oem.score);
    CHECK(g_oem.state == OEM_ST_ENDED, "state 3");
    CHECK(g_touch == 5 && g_touch_irq, "touch back on");

    note("second session (mode 2, 180 s); pause at 130 s brushed, then swipe-quit");
    ui_select_mode(2);
    m = mark();
    ts = press(0);
    expect("motor gear 47", m);
    run_until(ts + 2000);
    run_until(blk_after(ts, 1) + 129000 + 100);
    CHECK(g_oem.elapsed_s == 130, "130 s brushed (got %u)", g_oem.elapsed_s);
    press(0);
    expect("ui post 101 {0,50}", m);
    m = mark();
    ui_swipe_quit(100);
    run_ms(200);
    expect("ui post 103", m);                                // quit after >= 120 s: complete -> score
    CHECK(g_oem.done_s == 130 && g_oem.score == 72, "130 of 180 s, score 72");
    run_ms(1500);
    expect("ui post 100 {72}", m);
    CHECK(g_rtc.hist_count == 2, "two sessions today");
}

static void sc_swipe_quit_short(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(8000);
    uint32_t ts = press(0);                                  // mode 5, no sensor: gear right away
    int m = expect("motor gear 1", 0);
    expect_none("motor gear 49", 0);
    run_until(20000);
    int e1 = blk_count(ts, g_ms);
    press(0);
    expect("ui post 101 {0,", m);
    m = mark();
    ui_swipe_quit(80);
    run_ms(200);
    expect_none("ui post 103", m);
    int p = expect("ui post 80", m);                         // short session: straight to the mode page
    CHECK(g_oem.done_s == e1 && e1 < 15 && g_rtc.hist_count == 0, "%d s: not counted (below 15 s)", e1);
    size_t rl; CHECK(oem_brush_record(&rl) == NULL, "no record");
    run_until(35000);
    expect_at("led script 3", m, blk_uptime(g_log_ms[p] / 1000 + 10), 0);   // idle 10 s after the session
}

// Charger: attach while idle, remove inside 30 s, attach again, backlight time-out,
// button, full, removal after the time-out.
static void sc_charger_idle(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(10000);
    note("on the dock at 10 s");
    int m = mark();
    g_charger = true; run_ms(20);
    expect("led 0 -> off", m);
    expect("led 1 -> off", m);
    expect("led 4 -> off", m);
    expect("led script 2, then led 2 = 3", m);
    expect("ui post 93 {80}", m);
    CHECK(g_oem.power_state == OEM_PWR_CHARGING && g_oem.now_ui == 93, "charging, screen 93");
    run_until(14100);
    expect_at("led_level(hal 2, 0)", m, 10010 + 4000, 0);    // LED2 forced dark after 4 s
    expect("led 2 -> BREATHE", m);
    run_until(20000);
    note("picked up at 20 s");
    m = mark();
    g_charger = false; run_until(30000);
    expect_at("ui post 80", m, 20000 + 1510, 0);             // 151 ticks without the charger
    expect("touch state 0x06", m);
    expect("led 2 -> off", m);
    expect("led script 0, then led 1 = 0", m);
    CHECK(g_oem.power_state == OEM_PWR_BATTERY, "on battery");

    note("back on the dock at 30 s, left there");
    m = mark();
    g_charger = true; run_until(75000);
    int a = expect_at("ui post 93 {80}", m, 30010, 0);
    expect_at("led 4 -> off", a, blk_after(30010, 30), 0);   // backlight off after 30 s on the charger
    expect_none("DEEP SLEEP", 0);
    expect_none("lcd sleep", m);                             // no screen-off sequence on the charger
    note("button at 75 s: backlight for another 30 s, nothing else; battery at 100 % from now");
    m = mark();
    uint32_t tb = press(0);
    expect("led 4 -> ON", m);
    expect_none("ui post", m);
    expect_none("motor", m);
    press(1); press(3);
    expect_none("ui ", m);                                   // holds do nothing on the charger
    g_oem.batt_pct = 100; run_until(90000);
    expect_at("gauge: FULL", m, blk_after(tb, 6), 0);        // six gauge ticks at 100 %
    expect("led 2 -> ON", m);
    CHECK(g_oem.power_state == OEM_PWR_FULL, "state full");
    run_until(110000);
    expect_at("led 4 -> off", m + 1, blk_after(tb, 30), 0);
    note("picked up at 110 s (backlight had timed out)");
    m = mark();
    g_charger = false; run_until(125000);
    int rem = expect_at("led script 0, then led 1 = 0", m, 110000 + 1510, 0);
    CHECK(g_oem.power_state == OEM_PWR_BATTERY, "on battery");
    int ws = expect_at("ui post 84", rem, blk_after(111510, 4), 0);   // wake screen 4 s later
    // then the mode page as after a wake; the wake screen is posted at the top of a
    // 1 Hz block whose sequencer already counts it, so 5 more blocks
    expect_at("ui post 80", ws, blk_after(g_log_ms[ws], 5), 0);
}

static void sc_charger_while_brushing(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(8000);
    uint32_t ts = press(0);
    run_until(30000);
    int e1 = blk_count(ts, g_ms);
    note("put on the dock while brushing, at 30 s");
    int m = mark();
    g_charger = true; run_ms(300);
    expect("motor off", m);
    expect("event SESSION_END", m);
    expect("ui post 93 {80}", m);
    expect_none("ui post 103", m);
    expect_none("ui post 80", m);
    CHECK(!g_oem.session_active && g_oem.power_state == OEM_PWR_CHARGING && g_oem.now_ui == 93, "session over, charging, screen 93");
    CHECK(g_oem.done_s == e1 && e1 >= 21 && g_rtc.hist_count == 1, "%d s counted", e1);
    expect("cpu lock released", m);
    note("button: no brushing on the charger");
    m = mark(); press(0); expect_none("motor gear", m);
}

static void sc_charger_while_asleep(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(45000);
    CHECK(g_oem.asleep == 1, "screen-off stage");
    note("put on the dock at 45 s");
    int m = mark();
    g_charger = true; run_ms(200);
    expect("power stay_alive", m);
    expect("lcd init", m);
    expect("timer BLE_TIMEOUT stop", m);
    int w = expect("ui post 84", m);
    expect("ui post 93 {80}", w);
    expect("led script 2, then led 2 = 3", m);
    CHECK(g_oem.asleep == 0 && g_oem.power_state == OEM_PWR_CHARGING && g_oem.plug_cnt == 0, "awake, charging, plug_cnt 0");
    run_until(50000);
    CHECK(count("led script 2", m) == 2, "charger script again on the third gauge tick");
    run_until(120000);
    expect_none("DEEP SLEEP", 0);
}

// The one way to be "asleep on the charger": docked during the 550 ms between the idle
// expiry and the screen-off sequence (the wake is refused by the wake counter).
static void sc_asleep_on_charger(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(36100);
    CHECK(g_oem.asleep == 1 && g_oem.wake_gate == 0, "idle expired, sequence pending");
    int m = mark();
    g_charger = true; run_until(45000);
    CHECK(g_oem.asleep == 1 && g_oem.power_state == OEM_PWR_CHARGING && g_oem.wake_gate == 5, "asleep on the charger");
    expect("lcd sleep", m);
    expect_none("imu any-motion", m);
    expect_none("timer BLE_TIMEOUT start", m);
    expect("power prep_screen_off", m);
    expect("led reinit charge light", m);
    CHECK(g_led[2] == 3, "charge light breathing");
    note("button at 45 s");
    m = mark();
    press(0);
    expect("ui post 120", m);
    expect("led 4 -> ON", m);
    CHECK(g_oem.asleep == 0, "awake");
    run_until(200000);
    expect_none("DEEP SLEEP", 0);
}

static void sc_removed_while_asleep_on_charger(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(36100);
    g_charger = true; run_until(40000);
    CHECK(g_oem.asleep == 1 && g_oem.power_state == OEM_PWR_CHARGING, "asleep on the charger");
    note("picked up at 40 s");
    int m = mark();
    g_charger = false; run_until(42000);
    int w = expect_at("ui post 120", m, 40000 + 1510 + 100, 0);
    expect("led script 0, then led 1 = 0", m);
    CHECK(g_oem.asleep == 0 && g_oem.power_state == OEM_PWR_BATTERY, "awake, on battery");
    run_until(120000);
    expect_at("lcd sleep", m, blk_uptime(g_log_ms[w] / 1000 + 30) + 550, 0);   // idle 30 s
    expect("DEEP SLEEP", m);
}

// Battery at 8 %: after a session the idle expiry shows the warning, buzzes three
// times, and sleeps 3 s later.
static void sc_low_battery_10(void)
{
    boot(2, 8, OEM_WAKE_COLD);
    run_until(8000);
    press(0);
    run_until(28000);
    press(0);                                                // pause
    ui_swipe_quit(80);
    run_ms(200);
    int m = expect("ui post 80", find("ui post 101", 0));
    uint32_t t0s = g_log_ms[m] / 1000;
    run_until(50000);
    int w = expect_at("ui post 94", m, blk_uptime(t0s + 10), 0);   // idle 10 s after the session
    CHECK(count("motor gear 33", w) == 3, "three warning pulses");
    int p1 = find("motor gear 33", w), o1 = find("motor off", p1), p2 = find("motor gear 33", o1);
    CHECK(g_log_ms[o1] - g_log_ms[p1] == 400 && g_log_ms[p2] - g_log_ms[o1] == 400, "400 ms on, 400 ms off");
    int f = expect_at("led script 3", w, blk_uptime(t0s + 13), 0);   // warning stays 3 s
    expect("lcd sleep", f);
    run_until(90000);
    expect("DEEP SLEEP (deep sleep2", f);
}

static void sc_low_battery_idle_no_buzz(void)
{
    boot(2, 10, OEM_WAKE_COLD);
    run_until(45000);
    int w = expect_at("ui post 94", 0, blk_uptime(36), 0);
    expect_none("motor gear", 0);                            // no session since the wake: no buzz
    expect_at("led script 3", w, blk_uptime(39), 0);
}

// Battery at 0 %: low-battery screen, sleep, deep sleep at once without the BLE window
// and without motion wake.
static void sc_low_battery_0(void)
{
    boot(2, 0, OEM_WAKE_COLD);
    expect("ui post 94", 0);
    expect_none("ui post 84", 0);
    run_until(1000);
    expect_none("touch irq on", 0);
    expect_none("led 1 -> ON", 0);
    note("button at 1.0 s");
    int m = mark();
    press(0);
    expect("ui post 94", m);
    expect_none("motor gear", m);
    run_until(10000);
    uint32_t tf = blk_uptime(1 + 1 + 3);                     // idle 1 s from uptime 1, + 3 s warning
    int f = expect_at("led script 3", m, tf, 0);
    expect("imu power down", f);
    expect_at("DEEP SLEEP (deep sleep1", f, tf + 550 + 200 + 150, 0);
    CHECK(g_ms < tf + 2000, "no BLE window");
}

// Remote commands while awake.
static void sc_remote_awake(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(10000);
    int m = mark();
    note("remote: mode 2, brushing on");
    oem_remote_mode(2);
    oem_remote_brushing(true);
    run_ms(5);
    int p = expect("ui post 77", m);                         // mode page follows
    expect("motor gear 47", p);
    expect("ui post 82 {1,180}", p);
    CHECK(g_oem.session_active && g_oem.profile[6] == 2 && g_oem.saved_mode == 2, "session in mode 2");
    note("remote: mode 3 during the session is refused; strength 5 is only stored");
    m = mark();
    oem_remote_mode(3); oem_remote_strength(5); run_ms(5);
    CHECK(g_oem.profile[6] == 2 && g_rtc.strength == 5, "mode still 2, RTC strength 5");
    expect_none("motor gear", m);
    run_until(30000);
    note("remote: brushing off at 30 s");
    m = mark();
    oem_remote_brushing(false); run_ms(200);
    expect("motor off", m);
    expect("ui post 103", m);                                // as a stop from the running screen
    CHECK(!g_oem.session_active && g_oem.done_s >= 19 && g_oem.done_s <= 20, "stopped after %u s", g_oem.done_s);
    note("remote: mode 0 without an app profile becomes mode 5; mode 9 is ignored");
    oem_remote_mode(0); oem_remote_mode(9); run_ms(5);
    CHECK(g_oem.profile[6] == 5, "mode 5");
    CHECK(g_lock_depth == 0, "lock balanced");
}

// Remote start in the screen-off stage: wake, wait for the touch IC, start; strength
// changes during the mode-5 session apply at once.
static void sc_remote_asleep(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(45000);
    CHECK(g_oem.asleep == 1, "screen-off stage");
    int m = mark();
    note("remote: strength 4, brushing on at 45 s");
    oem_remote_strength(4);
    oem_remote_brushing(true);
    run_until(47000);
    int w = expect("power stay_alive", m);
    expect("ui post 84", w);
    int st = expect("ui post 87 {3}", w);
    expect("motor gear 24", w);
    CHECK(g_log_ms[st] - 45000 < 400, "started %u ms after the command", g_log_ms[st] - 45000);
    CHECK(g_oem.session_active && g_oem.asleep == 0 && g_oem.gear == 24, "session running at strength 4");
    m = mark();
    note("remote: strength 2 at 47 s (intensity screen still up), strength 5 at 60 s");
    oem_remote_strength(2); run_ms(5);
    expect("ui post 87 {1}", m);
    expect("motor gear 52", m);
    run_until(60000);
    m = mark();
    oem_remote_strength(5); run_ms(5);
    expect("motor gear 32", m);
    expect_none("ui post 87", m);
    oem_remote_brushing(false); run_ms(200);
    expect("motor off", m);
    run_until(200000);
    expect("DEEP SLEEP", m);                                 // and the brush goes back to sleep by itself
}

// Remote start while the screen-off sequence is under way (550 ms window).
static void sc_remote_during_sleep_seq(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(36200);
    CHECK(g_oem.asleep == 1 && g_oem.wake_gate == 0, "idle expired, sequence pending");
    int m = mark();
    oem_remote_brushing(true);
    run_until(40000);
    int s1 = expect("lcd sleep", m);                         // the sequence completes first
    int w = expect("power stay_alive", s1);
    expect("motor gear 1", w);
    CHECK(g_oem.session_active && g_oem.asleep == 0, "session running");
    uint32_t t = t_of("motor gear 1", w);
    CHECK(t < 38000, "started at %.3f s", t / 1000.0);
}

static void sc_remote_on_charger(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(10000);
    g_charger = true; run_until(12000);
    int m = mark();
    oem_remote_brushing(true); run_ms(500);
    expect_none("motor gear", m);
    CHECK(!g_oem.session_active, "no brushing on the charger");
}

// Motion wake in the screen-off stage, the limit of 5 in a row, BLE wake.
static void sc_motion_wake(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(45000);
    int m = mark();
    note("picked up at 45 s");
    hal_event_post(OEM_EV_MOTION); run_ms(200);
    expect("power stay_alive", m);
    expect("ui post 84", m);
    CHECK(g_oem.asleep == 0 && g_rtc.gyro_wakeup_count == 1, "awake, gyro wake count 1");
    CHECK(nvs_blob("wakeupcount") && nvs_blob("wakeupcount")[0] == 1, "count mirrored in NVS");
    note("count forced to 5: the next sleep switches motion wake off");
    g_rtc.gyro_wakeup_count = 5;
    run_until(90000);
    int s2 = find("lcd sleep", m);
    CHECK(s2 >= 0 && find("imu power down", m) >= 0 && find("imu power down", m) < s2, "IMU powered down in the screen-off sequence");
    m = mark();
    hal_event_post(OEM_EV_MOTION); run_ms(200);
    expect_none("power stay_alive", m);
    CHECK(g_oem.asleep == 1, "motion ignored over the limit");
    note("phone connects (OEM_EV_BLE_WAKE)");
    hal_event_post(OEM_EV_BLE_WAKE); run_ms(200);
    expect("power stay_alive", m);
    CHECK(g_oem.asleep == 0, "awake");
    run_until(200000);
    expect("DEEP SLEEP (deep sleep1", m);                    // still over the limit: button / charger only
}

// First boot (empty NVS): boot animation, defaults; then the 8 s factory reset.
static void sc_first_boot(void)
{
    boot(-1, 80, OEM_WAKE_COLD);
    expect("ui post 70", 0);
    expect_none("ui post 84", 0);
    run_until(600);
    CHECK(nvs_blob("sys_config")[0] == 1, "boot stage 0 -> 1");
    CHECK(g_oem.profile[6] == 5 && g_oem.sys[8] == 1 && g_oem.sys[0x37] == 1 && g_oem.sys[0x76] == 1, "defaults: mode 5, raise-to-wake, anti-splash, greetings");
    CHECK(g_oem.profile[1] == 2 && g_oem.profile[3] == 180 && g_oem.profile[5] == 16, "profile defaults 2 / 180 / 16");
    CHECK(g_oem.birthday_month == 0xff && g_oem.score == 0, "birthday unset, score 0");
    note("UI: boot animation done -> oem_show_main(), 60 s");
    hal_lock(); oem_show_main(); oem_idle_timeout(60); hal_unlock();
    run_until(5000);
    note("button short while the touch IC runs: session; 2 s and 5 s holds on a mode page");
    press(0);
    CHECK(g_oem.session_active, "session started");
    hal_lock(); oem_motor_stop(); hal_unlock(); run_ms(300);
    hal_lock(); oem_show_main(); hal_unlock();
    int m = mark();
    press(1);
    expect("ui lock_button(1)", m);
    press(3);
    expect("ui post 92 {0}", m);
    CHECK(g_oem.now_ui == 92, "info screen");
    press(0);
    expect("ui post 80", m);                                 // short press on the info screen: back
    CHECK(!g_oem.session_active, "no session from the info screen");
    note("8 s hold: UI calls oem_factory_reset()");
    m = mark();
    hal_lock(); oem_factory_reset(); hal_unlock();
    expect("gauge save", m);
    expect("RESTART", m);
    CHECK(nvs_blob("sys_config")[0] == 11 && nvs_blob("sys_config")[2] == 0x16, "sys_config: boot stage 11");
}

// Boot after a factory reset: 8 s, button dead, then the shipping sleep.
static void sc_after_factory_reset(void)
{
    uint8_t junk[100]; memset(junk, 7, sizeof junk);
    hal_nvs_set("user_config", junk, sizeof junk);
    boot(11, 80, OEM_WAKE_COLD);
    expect("ui post 70", 0);
    run_until(600);
    CHECK(g_oem.reset_flag == 1, "reset flag");
    CHECK(nvs_blob("sys_config")[0] == 0, "boot stage 11 -> 0");
    CHECK(nvs_blob("user_config")[19] == 5 && nvs_blob("user_config")[0] == 1, "user settings back to defaults");
    CHECK(g_rtc.hist_day == 0xff && g_rtc.hist_count == 0, "history reset");
    run_until(3000);
    int m = mark();
    press(0);
    expect_none("motor gear", m);
    expect_none("ui post", m);
    run_until(15000);
    int f = expect_at("led script 3", m, blk_uptime(8), 0);
    expect_at("DEEP SLEEP (deep sleep1", f, blk_uptime(8) + 550 + 200 + 150, 0);
}

// Settings written by the stock firmware carry over; unknown bytes survive a save.
static void sc_stock_config(void)
{
    uint8_t uc[100] = { 0 };
    uc[0] = 1; uc[2] = 0; uc[5] = 1; uc[6] = 0x0c; uc[12] = 0; uc[17] = 10; uc[18] = 2; uc[19] = 3;
    uc[39] = 0; uc[41] = 0; uc[43] = 2; uc[45] = 180; uc[46] = 1; uc[47] = 16; uc[48] = 1; uc[49] = 0; uc[50] = 1;
    uc[52] = 1; uc[54] = 5; uc[58] = 7; uc[59] = 9; uc[62] = 88; uc[63] = 1; uc[70] = 0x5a; uc[9] = 0x77;
    hal_nvs_set("user_config", uc, sizeof uc);
    uint8_t sc[100] = { 2, 0, 0, 0x33, 2, 7, 0, 0, 0, 0xAA, 1 };
    hal_nvs_set("sys_config", sc, sizeof sc);
    boot(-1, 80, OEM_WAKE_BUTTON);
    run_until(600);
    CHECK(g_oem.profile[6] == 3 && g_oem.lang == 7 && g_oem.sys[0x0c] == 2, "mode 3, language 7, bound");
    CHECK(g_oem.sys[8] == 0 && g_oem.locked == 1 && g_oem.score == 88, "raise-to-wake off, locked, last score 88");
    CHECK(g_oem.zone_s[0] == 5 && g_oem.zone_s[5] == 7 && g_oem.zone_s[4] == 9, "zone bytes (4 and 5 swapped)");
    CHECK(g_oem.birthday_month == 10 && g_oem.birthday_day == 2, "birthday 2 Oct");
    expect("ui post 84", 0);                                 // at boot the settings are not loaded yet: no greeting
    run_until(7000);
    expect("ui post 76", 0);                                 // mode 3 page
    g_time.hour = 20;
    hal_lock(); g_oem.sys[6] = 1; oem_show_main(); hal_unlock();
    CHECK(g_oem.profile[6] == 2, "auto mode by time: evening = mode 2");
    g_time.hour = 3; g_time.min = 1;
    hal_lock(); oem_show_main(); g_oem.sys[6] = 0; hal_unlock();
    CHECK(g_oem.profile[6] == 1, "03:01 = morning = mode 1");
    run_until(60000);
    const uint8_t *b = nvs_blob("user_config");
    CHECK(b[19] == 1 && b[49] == 0 && b[70] == 0x5a && b[9] == 0x77 && b[52] == 1 && b[62] == 88, "saved at sleep; foreign bytes kept");
    CHECK(nvs_blob("sys_config")[3] == 0x33 && nvs_blob("sys_config")[9] == 0xAA && nvs_blob("sys_config")[5] == 7, "sys_config kept");
    note("raise-to-wake off: motion does nothing, deep sleep without motion wake");
    int m = mark();
    hal_event_post(OEM_EV_MOTION); run_ms(100);
    expect_none("power stay_alive", m);
    run_until(200000);
    expect("DEEP SLEEP (deep sleep1", m);
}

// Short press on a side page, with the lock popup, OTA lock, touch gate, greeting.
static void sc_button_misc(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(8000);
    int m = mark();
    g_oem.subpage = 1;
    press(0);
    expect("ui page_back", m);
    expect_none("motor gear", m);
    g_oem.locked = 1; g_oem.lock_popup = 1;
    press(0);
    expect("ui lock_button(0)", m);
    g_oem.locked = 0; g_oem.lock_popup = 0;
    note("web OTA running: button dead");
    g_oem.ota = 1; m = mark(); press(0); expect_none("motor gear", m); g_oem.ota = 0;
    note("touch IC not ready: no start");
    uint8_t keep = g_touch; g_touch = 0x21; g_touch_irq = false; m = mark(); press(0); expect_none("motor gear", m);
    g_touch = keep; g_touch_irq = true;
    note("greeting day (5 Oct) after a wake from the screen-off stage");
    g_time.day = 5;
    run_until(60000);
    CHECK(g_oem.asleep == 1, "asleep");
    m = mark();
    press(0);
    int g = expect("ui post 85 {10}", m);
    run_until(g_ms + 7000);
    expect_at("ui post 80", g, blk_after(g_log_ms[g], 6), 0);
    CHECK(g_rtc.hist_day == 5, "history day rolled over");
}

// Wake by the charger from deep sleep: normal boot, charging screen once the gauge is up.
static void sc_boot_on_charger(void)
{
    g_charger = true;
    boot(2, 80, OEM_WAKE_CHARGER);
    expect("ui post 84", 0);
    run_until(10000);
    expect_at("ui post 93 {80}", 0, blk_after(0, 3) + 10, 0);   // first tick after the gauge's first measurement
    CHECK(g_oem.power_state == OEM_PWR_CHARGING, "charging");
    expect_none("ui post 80", 0);                            // no screen sequencing on the charger
    run_until(200000);
    expect_none("lcd sleep", 0);
    expect_none("DEEP SLEEP", 0);
}

// Firmware upload over Wi-Fi (g_oem.ota set by the web handler).
static void sc_web_ota(void)
{
    boot(2, 80, OEM_WAKE_COLD);
    run_until(8000);
    press(0);
    run_until(12000);
    note("upload starts while brushing");
    int m = mark();
    g_oem.ota = 1; run_ms(20);
    expect("motor off", m);
    expect("event SESSION_END", m);
    run_until(100000);
    expect_none("lcd sleep", m);                             // no idle sleep during the upload
    press(0);
    expect_none("motor gear", m);                            // button locked
    note("upload over at 100 s: the idle timer ran out long ago, screen off at once");
    g_oem.ota = 0;
    run_until(110000);
    int s1 = expect_at("lcd sleep", m, blk_after(100001, 1) + 550, 0);
    note("another upload starts at 110 s, in the screen-off stage, and ends at 200 s");
    g_oem.ota = 1;
    run_until(200000);
    expect_none("DEEP SLEEP", s1);
    expect_none("net sleep", s1);
    g_oem.ota = 0;
    run_until(240000);
    expect("DEEP SLEEP", s1);                                // at the end of the window that was running
}

static const struct { const char *name; void (*fn)(void); const char *title; } SCEN[] = {
    { "boot_idle_sleep", sc_boot_idle_sleep, "cold boot -> wake screen -> mode page -> idle -> screen off -> BLE window -> deep sleep" },
    { "net_activity", sc_net_activity, "client activity keeps the brush in the screen-off stage" },
    { "no_ssid_window", sc_no_ssid_window, "no Wi-Fi configured: 120 s window" },
    { "wake_session_mode5", sc_wake_session_mode5, "button wake, mode-5 session with force sensor: hum, contact, intensity, over-pressure, cues, auto stop, results" },
    { "mode1", sc_mode1, "mode 1 (gear 54, 120 s), no force sensor" },
    { "mode2", sc_mode2, "mode 2 (gear 47, 180 s), no force sensor" },
    { "mode3", sc_mode3, "mode 3 (gear 50, 120 s), no force sensor" },
    { "mode4", sc_mode4, "mode 4 (gear 48, 150 s), no force sensor" },
    { "app_scheme", sc_app_scheme, "mode 0: phone-app scheme with three steps, single-step profile" },
    { "pause_resume", sc_pause_resume, "pause / resume / pause time-out / swipe-quit after 120 s" },
    { "swipe_quit_short", sc_swipe_quit_short, "swipe-quit of a short session" },
    { "charger_idle", sc_charger_idle, "charger: attach idle, remove, attach, backlight time-out, button, full, remove" },
    { "charger_while_brushing", sc_charger_while_brushing, "charger attached while brushing" },
    { "charger_while_asleep", sc_charger_while_asleep, "charger attached in the screen-off stage" },
    { "asleep_on_charger", sc_asleep_on_charger, "asleep on the charger, button" },
    { "removed_asleep_on_charger", sc_removed_while_asleep_on_charger, "removed while asleep on the charger" },
    { "low_battery_10", sc_low_battery_10, "battery 8 %: warning with buzz at the idle expiry" },
    { "low_battery_idle", sc_low_battery_idle_no_buzz, "battery 10 %: warning without buzz" },
    { "low_battery_0", sc_low_battery_0, "battery 0 %" },
    { "remote_awake", sc_remote_awake, "remote mode / start / stop while awake" },
    { "remote_asleep", sc_remote_asleep, "remote start in the screen-off stage, remote strength" },
    { "remote_during_sleep_seq", sc_remote_during_sleep_seq, "remote start during the screen-off sequence" },
    { "remote_on_charger", sc_remote_on_charger, "remote start on the charger" },
    { "motion_wake", sc_motion_wake, "motion wake, limit, BLE wake" },
    { "first_boot", sc_first_boot, "first boot, holds, factory reset" },
    { "after_factory_reset", sc_after_factory_reset, "boot after a factory reset" },
    { "stock_config", sc_stock_config, "settings of the stock firmware carry over" },
    { "boot_on_charger", sc_boot_on_charger, "boot with the charger attached" },
    { "web_ota", sc_web_ota, "firmware upload: session stopped, no sleep, button locked" },
    { "button_misc", sc_button_misc, "side page, lock popup, OTA lock, touch gate, greeting" },
};

int main(int argc, char **argv)
{
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = true;
        else only = argv[i];
    }
    int failed = 0, ran = 0;
    for (size_t i = 0; i < sizeof SCEN / sizeof SCEN[0]; i++) {
        if (only && strncmp(SCEN[i].name, only, strlen(only)) != 0) continue;
        printf("=== %s: %s\n", SCEN[i].name, SCEN[i].title);
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            SCEN[i].fn();
            printf("  %d checks, %d failed\n", g_checks, g_fail);
            fflush(stdout);
            _exit(g_fail ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        ran++;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed++;
            if (!WIFEXITED(status)) printf("  **FAIL** scenario crashed (status 0x%x)\n", status);
        }
    }
    printf("\n%d scenarios, %d failed\n", ran, failed);
    return failed ? 1 : 0;
}

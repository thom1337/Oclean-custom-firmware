#include <string.h>
#include "oem_api.h"
#include "oem_brush.h"
#include "oem_hal.h"
#include "oem_state.h"

// Main-task application logic of the stock firmware: the "brush_app" task 0x4201cc40
// and what it runs directly. oem_app_boot() is the part before its event loop,
// oem_app_handle() one pass of the loop body.
//
//   10 ms tick   LED tick, charger poll, 1 Hz block (session clock, screen sequencer,
//                gauge, idle timer)
//   button       dispatcher 0x4201cab8, short press 0x4201c790 (wake / start / pause /
//                resume / stop)
//   charger      charge state machine 0x42017a0c
//   idle expiry  screen-off sequence (sleep_brush_task 0x4201b900), 200 ms later the
//                second stage, then the BLE window and deep sleep (0x4201c5d0)
//   wake         motorwakeup 0x4201bd70 / motorwakeup_for_charge 0x4201bee8
//
// Specs: re/spec/input.md 3-4, power.md 2 / 4, ui_flow.md 6.2 / 7.2 / 7.3 / 8,
// led_battery_charge.md 3 / 6, brushing.md 3.5 / 3.6 / 5. The brush engine is in
// oem_brush.c. Not ported: OEM cloud, cloud OTA prompt (screen 97), production test,
// aging, shop demo mode; their flags stay at the normal-user values.

// Stock .data values of the fields this module owns. sys[] is overwritten by the
// configuration load at boot, but the first screen is chosen before that (as stock).
oem_state_t g_oem = {
    .now_ui = 0xff,
    .saved_mode = 0xff,
    .score = 0xff,
    .stop_delay = 200,
    .motor_state = 1,
    .dev_mode = 1,
    .power_state = OEM_PWR_BATTERY,
    .motion_gate = 1,
    .sys = { [2] = 1, [8] = 1, [0x37] = 1, [0x76] = 1, [0x78] = 1, [0x79] = 1, [0x7a] = 0x41, [0x7b] = 0x41 },
};

#define BOOT_TICKS        50    // vTaskDelay(500) in the main-task init
#define SLEEP_SEQ_TICKS   55    // sleep_brush_task: 500 ms delay, then NVS and IMU work before the LEDs
#define BUZZ_TICKS        200   // low-battery buzz: 3 x 400 ms on with 400 ms off in between
#define REMOTE_TRIES      300   // 10 ms ticks a remote "start" may wait for the wake-up / the touch IC

static struct {
    // idle timer ("sleep monitoring")
    uint8_t  idle_on;           // 0x3fc9a71a
    uint8_t  idle_timeout;      // 0x3fc9a71b
    uint32_t idle_t0;           // 0x3fca41a0
    // 10 ms tick
    uint8_t  tick_div;          // 0x3fca4df1
    uint8_t  boot_ticks;        // ticks until the second half of the boot sequence runs
    uint8_t  boot_stage;        // sys[0x34] as found at boot
    uint8_t  buzz;              // low-battery buzz: ticks since its start + 1 (0 = not running)
    // charger
    uint16_t on_chg_ticks;      // 0x3fca4b0e
    uint16_t off_chg_ticks;     // 0x3fca4b0c
    uint16_t unplug_cnt;        // 0x3fc9f304
    uint8_t  close_screen_cnt;  // 0x3fc9ab89  seconds on the charger; backlight off at 30
    uint8_t  pending_screen;    // 0x3fca4b18  show a screen 4 s after the removal
    uint8_t  pend_cnt;          // 0x3fc9ab9f
    // sleep
    uint8_t  sleep_seq;         // ticks until the screen-off sequence runs (stock: a task with a 500 ms delay)
    uint8_t  ble_wake_ok;       // 0x3fc9aba4
    uint8_t  wifi_off_cnt;      // 0x3fc9a1e0  seconds in the screen-off stage; Wi-Fi off at 27
    uint8_t  no_touch_irq;      // 0x3fca5e3f  set when the boot screen is "battery empty"
} s = {
    .idle_on = 1, .idle_timeout = 30,
    .close_screen_cnt = 255, .pend_cnt = 10,
    .ble_wake_ok = 1, .wifi_off_cnt = 200,
};

// Button codes from the driver (ISR / timer context): single producer, single consumer.
static volatile uint8_t s_btn_q[8];
static volatile uint8_t s_btn_w, s_btn_r;

// Remote commands (web UI / MQTT / BLE), protected by the core lock.
enum { REM_BRUSH_ON, REM_BRUSH_OFF, REM_MODE, REM_STRENGTH, REM_ACTIVITY };
static struct {
    struct { uint8_t kind, arg; } q[8];
    uint8_t  head, count;
    uint16_t tries;             // ticks the head command has been waiting
} s_rem;

static void wake(void);
static void wake_for_charge(void);
static void remote_run(void);
static void boot_finish(void);

// ---- fallbacks for requests to other modules -------------------------------------------
// Replaced at link time once the owning module provides the function (NOTES_app.md).

__attribute__((weak)) bool oem_pressure_available(void)
{
    return false;
}

__attribute__((weak)) void oem_ui_page_back(void)
{
    // A swipe up does the same on a side page, except that the UI ignores it while
    // the touch lock is on.
    if (g_oem.subpage == 1) oem_ui_post(10, NULL, 0);
}

// ---- gyro wake counter (RTC 0x50001022, NVS "wakeupcount") -------------------------------

static void gyro_count_store(void)               // 0x42023d34
{
    uint8_t b[10] = { hal_rtc()->gyro_wakeup_count };
    hal_nvs_set("wakeupcount", b, sizeof b);
}

static void gyro_count_clear(void)               // 0x4201b740 "key_wakeup_clear_gyro_count"
{
    oem_rtc_t *r = hal_rtc();
    if (r->gyro_wakeup_count == 0) return;       // stock rewrites the blob every time
    r->gyro_wakeup_count = 0;
    gyro_count_store();
}

static void gyro_count_inc(void)                 // 0x4201b714
{
    oem_rtc_t *r = hal_rtc();
    if (r->gyro_wakeup_count < 20) {
        r->gyro_wakeup_count++;
        gyro_count_store();
    }
}

static bool gyro_limit(void)                     // 0x4201b6f0 "gyro_wakeup_to_up_limit"
{
    return hal_rtc()->gyro_wakeup_count > 4;
}

// ---- idle timer -------------------------------------------------------------------------

void oem_idle_timeout(uint8_t seconds)           // 0x42014278 open_sleep_monitoring
{
    s.idle_timeout = seconds;
    s.idle_t0 = hal_uptime_s();
    s.idle_on = 1;
}

void oem_idle_off(void)                          // 0x42014268
{
    s.idle_on = 0;
}

void oem_idle_kick(void)                         // 0x42014298
{
    s.idle_t0 = hal_uptime_s();
}

// ---- screens ----------------------------------------------------------------------------

void oem_show(uint8_t screen, const void *payload, int len)
{
    g_oem.now_ui = screen;
    g_oem.dwell_s = 0;
    oem_ui_post(screen, payload, len);
}

// 03:01 .. 12:00 (0x4201a3fc)
static bool morning(void)
{
    oem_time_t t;
    hal_time(&t);
    if (t.hour < 4) return t.hour == 3 && t.min != 0;
    if (t.hour > 11) return t.hour == 12 && t.min == 0;
    return true;
}

// Common part of 0x4201a3fc / 0x4201a5ac, ending in 0x4201a3a0 (mode -> mode page)
static void show_mode_page(void)
{
    static const uint8_t MODE_SCREEN[6] = { 81, 79, 77, 76, 78, 80 };
    g_oem.dwell_s = 0;
    if (g_oem.sys[6] == 1) g_oem.profile[6] = morning() ? 1 : 2;      // auto mode by time of day
    if (g_oem.profile[1] == 1)
        g_oem.tod_sub = g_oem.profile[0] == 0 ? 0 : (morning() ? 2 : 3);
    g_oem.profile[7] = g_oem.profile[6];
    uint8_t mode = g_oem.profile[6];
    uint8_t scr = mode < 6 ? MODE_SCREEN[mode] : 78;
    if (scr == 81) {
        uint8_t sub = g_oem.profile[2] ? 4
                    : (g_oem.profile[1] == 1 && (g_oem.tod_sub == 2 || g_oem.tod_sub == 3)) ? g_oem.tod_sub : 1;
        oem_show(81, &sub, 1);
    } else {
        oem_show(scr, NULL, 0);
    }
}

void oem_show_main(void)                         // 0x4201a3fc uipxp_task_show_main_screen
{
    if (g_oem.now_ui == 97) return;              // never interrupts the update prompt
    show_mode_page();
    oem_idle_timeout(g_oem.dev_mode == 2 ? 60 : 30);
}

void oem_show_main_after_session(void)           // 0x4201a5ac
{
    show_mode_page();
    oem_idle_timeout(10);
}

void oem_show_score(uint8_t score)               // 0x4201a774
{
    oem_show(100, &score, 1);
}

void oem_show_history(uint8_t flag)              // 0x4201c1b0 -> 0x4201a730
{
    const oem_rtc_t *r = hal_rtc();
    uint8_t p[8] = {
        (uint8_t)r->hist_seconds, (uint8_t)(r->hist_seconds >> 8),
        (uint8_t)r->hist_score_sum, (uint8_t)(r->hist_score_sum >> 8),
        (uint8_t)r->hist_count, (uint8_t)(r->hist_count >> 8),
        flag, 0,
    };
    oem_show(84, p, 8);
}

// The next three do not restart the dwell counter (stock does not either).
void oem_show_paused(void)                       // 0x4201c73c -> 0x4201a8c0
{
    uint8_t p[2] = { 0, (uint8_t)(g_oem.total_s - g_oem.done_s) };
    g_oem.now_ui = 101;
    oem_ui_post(101, p, 2);
}

void oem_show_brushing(void)                     // 0x4201bb18 -> 0x4201a82c
{
    uint8_t p[2] = { 1, (uint8_t)(g_oem.total_s - g_oem.done_s) };
    g_oem.now_ui = 82;
    oem_ui_post(82, p, 2);
}

static void show_low_battery(void)               // 0x4201a260
{
    g_oem.now_ui = 94;
    oem_ui_post(94, NULL, 0);
}

static void show_charging(uint8_t pct)           // 0x4201a2a4
{
    g_oem.now_ui = 93;
    oem_ui_post(93, &pct, 1);
}

void oem_show_strength(uint8_t level_minus_1)    // 0x4201a270
{
    oem_show(87, &level_minus_1, 1);
}

static void show_complete(void)                  // 0x4201a7a8
{
    oem_show(103, NULL, 0);
}

// 0x420291a4: 4 = birthday, the month number on 1 Jan, 14 Feb, 20 Mar, 1 May, 5 Oct,
// else 99 (also when the clock is not set)
static uint8_t greeting_id(void)
{
    oem_time_t t;
    hal_time(&t);
    if (t.year > 200 || t.month > 12 || t.day > 31) return 99;
    if (g_oem.birthday_month == t.month && g_oem.birthday_day == t.day) return 4;
    if ((t.month == 1 && t.day == 1) || (t.month == 2 && t.day == 14) || (t.month == 3 && t.day == 20)
            || (t.month == 5 && t.day == 1) || (t.month == 10 && t.day == 5)) return t.month;
    return 99;
}

// 0x42029218: first screen after a wake. (Screen 104, the BLE-configured picture, is
// broken in stock and not ported.)
static void wake_screen(void)
{
    if (g_oem.state < 2 && g_oem.batt_pct == 0 && !oem_charger_present()) {
        show_low_battery();
        s.no_touch_irq = 1;
        oem_idle_timeout(0);                     // sleep at once (after the 3 s warning)
        return;
    }
    uint8_t id = greeting_id();
    if (id == 4 || (id != 99 && g_oem.sys[0x76] == 1)) oem_show(85, &id, 1);
    else oem_show_history(0);
}

// ---- BLE window, deep sleep -------------------------------------------------------------

// Step 14 of 0x4201b764 and 0x4201b92c: how long the brush stays in the screen-off
// stage before deep sleep
static void ble_window_start(void)
{
    uint32_t ms = (!hal_wifi_has_ssid() || hal_ble_connected()) ? 120000 : 30000;
    hal_timer_stop(HAL_TMR_BLE_TIMEOUT);
    hal_timer_start(HAL_TMR_BLE_TIMEOUT, ms, false);
}

// 0x4201c5d0 brush_pm_control. On the brush oem_power_deep_sleep() does not return
// unless the power module refuses the sleep.
static void pm_control(bool force)
{
    if (g_oem.power_state != OEM_PWR_BATTERY) return;       // never on the charger
    bool special = g_oem.batt_pct == 0 || g_oem.reset_flag || g_oem.dev_mode == 2;
    hal_log("pm: batt %u asleep %u reset %u force %d", g_oem.batt_pct, g_oem.asleep, g_oem.reset_flag, force);

    if (special && g_oem.asleep && !force) {     // battery empty / shipping state: no BLE window
        hal_delay(100);
        oem_gauge_save();
        oem_imu_amd();
        oem_imu_power_down();
        hal_delay(50);
        oem_power_deep_sleep(false);
        hal_delay(500);
        return;
    }
    if (!force) return;

    bool motion_wake = true;
    if (g_oem.sys[8] == 0 || gyro_limit()) {     // raise-to-wake off, or carried around: button / charger only
        oem_imu_power_down();
        motion_wake = false;
    }
    hal_delay(100);
    oem_gauge_save();
    hal_delay(50);
    oem_power_deep_sleep(motion_wake);
    hal_delay(500);
}

// sleep_brush_task 0x4201b900 after its delay, and brush_work_ooer_to_sleep 0x4201b764
static void screen_off_sequence(void)
{
    if (s.buzz) { s.buzz = 0; oem_motor_off(); }
    brush_user_config_save();
    g_oem.wake_gate = 5;
    // (stock stores the clock in NVS "TimeRTC" here; the clock belongs to the glue)
    // back to the mode of the last brushing start / lock toggle
    if (!g_oem.locked && g_oem.saved_mode != 0xff && g_oem.sys[6] == 0) oem_set_mode(g_oem.saved_mode);
    if (g_oem.power_state == OEM_PWR_BATTERY) {
        oem_imu_amd();
        if (gyro_limit()) oem_imu_power_down();  // too many motion wakes in a row: motion wake off
    }
    hal_lcd_sleep();
    // The fade-out script started 550 ms ago and has ended; a request made while a
    // script runs would be dropped, so make sure (not in stock, which relies on the
    // time its NVS and IMU work takes).
    oem_led_abort_script();
    oem_led_set(4, 1, 4);
    oem_pressure_stop();
    oem_led_set(0, 1, 4);
    oem_led_set(1, 1, 4);
    oem_led_set(2, 1, 4);
    oem_led_set(3, 1, 4);
    oem_led_set(4, 1, 4);
    oem_touch_irq(false);
    oem_touch_set_state(7);
    oem_ui_post(1, NULL, 0);                     // 0x4202149c
    oem_ui_set_enabled(false);
    s.ble_wake_ok = 1;
    oem_gauge_sleep_enter();
    s.wifi_off_cnt = 0;
    hal_log("screen off: wifi %d ble %d", hal_wifi_has_ssid(), hal_ble_connected());
    if (g_oem.power_state == OEM_PWR_BATTERY) ble_window_start();
    g_oem.ui_mode = 0;
    g_oem.show_mode = 0;
    hal_timer_start(HAL_TMR_SLEEP200, 200, false);          // 0x42014374
}

// Event 0x100000, 200 ms after the screen-off sequence (0x4201d2a8)
static void sleep_stage2(void)
{
    if (!g_oem.asleep) return;                   // woken in between (stock arms the motion interrupt even then)
    pm_control(false);                           // deep-sleeps right here in the special cases
    oem_power_prep_screen_off();
    if (g_oem.power_state != OEM_PWR_BATTERY)    // on the charger: only the charge light keeps running
        oem_led_reinit_charge_light();
    oem_power_resume_sleep();
    g_oem.motion_gate = 0;
    oem_motion_irq(true);
}

// Event 0x08: the BLE window is over (0x4201d338)
static void ble_timeout(void)
{
    // Not in stock, which deep-sleeps here unconditionally: the brush may have been
    // woken in the same loop pass, and a firmware upload over Wi-Fi must not be cut.
    if (!g_oem.asleep) return;
    if (g_oem.ota) { ble_window_start(); return; }

    pm_control(true);
    // Only reached without a deep sleep.
    if (g_oem.power_state != OEM_PWR_BATTERY) {  // on the charger (0x4201d500)
        oem_power_prep_screen_off();
        oem_led_reinit_charge_light();
        oem_power_resume_sleep();
        g_oem.motion_gate = 0;
    } else {
        // The power module refused the sleep. Stock has no such case (it would stop
        // the 10 ms tick and stay in the screen-off stage); try again after another
        // window.
        ble_window_start();
    }
}

// What a GATT write does in stock while the screen is off (0x4201b9b0): the brush stays
// up for another window. Here any web / MQTT / BLE client activity does it.
static void net_activity(void)
{
    if (!g_oem.asleep || g_oem.power_state != OEM_PWR_BATTERY) return;
    s.wifi_off_cnt = 0;
    ble_window_start();
}

// ---- wake from the screen-off stage -----------------------------------------------------

// Common first half of 0x4201bd70 / 0x4201bee8. The counter makes the wake functions a
// no-op until the screen-off sequence has run (or after four attempts).
static bool wake_begin(void)
{
    if (g_oem.wake_gate < 4) { g_oem.wake_gate++; return false; }
    g_oem.wake_gate = 0;
    s.sleep_seq = 0;
    oem_power_stay_alive();
    g_oem.asleep = 0;
    g_oem.state = OEM_ST_WOKEN;
    g_oem.motion_gate = 1;
    oem_power_restore_after_screen_off();
    hal_delay(100);
    oem_imu_normal();
    hal_lcd_init();
    oem_pressure_start();
    return true;
}

static void wake_end(void)
{
    hal_timer_start(HAL_TMR_TICK10, 10, true);
    hal_timer_stop(HAL_TMR_BLE_TIMEOUT);
    oem_idle_timeout(30);
}

static void wake(void)                           // 0x4201bd70 "motorwakeup"
{
    if (!wake_begin()) return;
    oem_touch_irq(true);
    oem_motor_off();
    oem_touch_set_state(0);
    oem_touch_set_state(0x21);
    if (g_oem.power_state == OEM_PWR_BATTERY && g_oem.batt_pct != 0) oem_led_set(1, 0, 0);   // wake script
    else oem_led_set(4, 0, 4);
    wake_end();
    brush_hist_update(0, 0);
    wake_screen();
    oem_ui_post(0, NULL, 0);                     // screen on
    oem_motion_irq(false);
    oem_gauge_sleep_exit();
    oem_gauge_rearm(2);
    hal_net_wake();
}

static void wake_for_charge(void)                // 0x4201bee8 "motorwakeup_for_charge"
{
    if (!wake_begin()) return;
    oem_ui_post(0, NULL, 0);
    brush_hist_update(0, 0);
    oem_ui_post(120, NULL, 0);                   // battery screen; stock leaves now_ui as it was
    oem_touch_set_state(0);
    oem_touch_set_state(0x21);
    oem_motor_off();
    oem_touch_irq(true);
    oem_led_set(4, 0, 4);
    if (g_oem.power_state == OEM_PWR_FULL) oem_led_set(2, 0, 4);
    else if (g_oem.power_state == OEM_PWR_CHARGING) oem_led_set(2, 3, 4);
    else if (g_oem.batt_pct == 0) oem_led_set(4, 0, 4);
    else oem_led_set(1, 0, 0);
    wake_end();
    oem_motion_irq(false);
    hal_net_wake();
}

// ---- charger (0x42017a0c, 0x4201840c) ---------------------------------------------------

static void usb_action(bool removed)             // 0x42017a0c
{
    if (g_oem.reset_flag) return;
    if (g_oem.show_mode) { oem_wlc_off(); oem_charge_allow(true); return; }

    if (!removed) {                              // "USB_IN_ACTION", only from the battery state
        if (g_oem.power_state != OEM_PWR_BATTERY) return;
        s.close_screen_cnt = 0;
        oem_gauge_settle_reset();
        g_oem.slew_cnt = 0;
        oem_wlc_off();
        s.unplug_cnt = 0;
        g_oem.full_cnt = 0;
        g_oem.plug_cnt = 100;
        g_oem.power_state = OEM_PWR_CHARGING;
        hal_log("charger attached");
        s.buzz = 0;
        if (g_oem.asleep == 1) {
            wake();
            g_oem.plug_cnt = 0;
        }
        oem_motor_off();
        if (g_oem.session_active) oem_motor_stop();
        oem_gauge_rearm(g_oem.gauge_period);
        oem_idle_timeout(30);
        oem_charge_allow(true);
        oem_led_abort_script();
        oem_led_set(0, 1, 4);
        oem_led_set(1, 1, 4);
        oem_led_set(4, 1, 4);
        oem_led_set(2, 3, 2);                    // charger script, then the charge light breathes
        if (!g_oem.ota && !(g_oem.dev_mode == 2 && g_oem.aging == 1)) show_charging(g_oem.batt_pct);
        return;
    }

    hal_log("charger removed");
    g_oem.plug_cnt = 100;
    oem_wlc_off();
    if (g_oem.asleep == 1) {                     // was asleep on the dock
        oem_led_set(2, 1, 4);
        g_oem.power_state = OEM_PWR_BATTERY;
        wake_for_charge();
        s.pend_cnt = 0;
        if (g_oem.batt_pct == 0) { oem_idle_timeout(1); s.close_screen_cnt = 100; }
        return;
    }
    oem_touch_set_state(6);
    if (g_oem.batt_pct == 0) {
        oem_idle_timeout(1);
        s.close_screen_cnt = 100;
        g_oem.power_state = OEM_PWR_BATTERY;
        return;
    }
    oem_idle_timeout(30);
    if (!(g_oem.dev_mode == 2 && g_oem.aging == 1) && !g_oem.ota) {
        if (s.close_screen_cnt < 30) {
            oem_show_main();
        } else {                                 // the backlight had timed out: wake screen 4 s later
            s.pend_cnt = 0;
            s.pending_screen = 1;
        }
    }
    s.close_screen_cnt = 100;
    oem_gauge_rearm(2);
    oem_gauge_settle_reset();
    g_oem.power_state = OEM_PWR_BATTERY;
    oem_led_abort_script();
    oem_led_set(2, 1, 4);
    oem_led_set(1, 0, 0);                        // wake script
}

static void charger_poll(void)                   // 0x4201840c, every 10 ms
{
    if (g_oem.power_state == OEM_PWR_BATTERY) {
        s.on_chg_ticks = 0;
        if (s.off_chg_ticks < 1000) s.off_chg_ticks++;
        if (s.off_chg_ticks == 400) oem_led_set(4, 0, 4);   // 4 s after boot / removal: backlight full on
    } else {
        s.off_chg_ticks = 0;
        if (s.on_chg_ticks < 1000) s.on_chg_ticks++;
        if (s.on_chg_ticks == 400) oem_led_level(2, 0);     // 4 s on the charger: LED2 forced dark
    }

    bool present = oem_charger_present();
    if (oem_charger_alive_take()) s.unplug_cnt = 0;         // GPIO2 edge: the charger is still there
    // lets the gauge take its first measurement while brushing on the dock
    if (g_oem.session_active && !g_oem.gauge_inited && present) oem_gauge_tick();
    if (!g_oem.gauge_inited) return;

    if (g_oem.power_state == OEM_PWR_CHARGING && !present) {
        if (++s.unplug_cnt > 150) usb_action(true);         // 1.51 s without the charger
    } else if (g_oem.power_state == OEM_PWR_BATTERY) {
        if (present) usb_action(false);
    } else if (g_oem.power_state == OEM_PWR_FULL && !present) {
        if (++s.unplug_cnt > 150) { oem_wlc_off(); usb_action(true); }
    }
    if (present) s.unplug_cnt = 0;
}

static void delay_to_close_screen(void)          // 0x42018250, 1 Hz on the charger
{
    if (s.close_screen_cnt < 200) s.close_screen_cnt++;
    if (s.close_screen_cnt == 30 && g_oem.ui_mode != 0xd7) {
        oem_led_set(4, 0, 4);                    // so that the "off" is not skipped as "no change"
        oem_led_set(4, 1, 4);
    }
}

// ---- session start / end ----------------------------------------------------------------

static void start_session(void)                  // 0x4201c858..0x4201c980
{
    s.buzz = 0;                                  // a low-battery buzz must not switch this session's motor off
    g_oem.saved_mode = g_oem.profile[6];
    oem_led_set(4, 0, 4);
    gyro_count_clear();
    g_oem.state = OEM_ST_SESSION;
    g_oem.user_quit = 0;
    g_oem.brushed_since_wake = 1;
    oem_led_set(2, 1, 4);
    oem_led_set(3, 1, 4);
    brush_session_begin();                       // profile, motor
    hal_log("session: mode %u, %u s, gear %u", g_oem.profile[6], g_oem.total_s, g_oem.gear);
    if (g_oem.profile[6] != 5) {
        oem_touch_irq(false);                    // no gestures while the motor runs
        oem_show_brushing();
    } else {
        oem_show_strength(brush_strength_level() - 1);      // intensity screen, touch stays on
    }
    hal_delay(30);
    oem_power_cpu_lock(true);
    oem_idle_off();
    g_oem.tick_s = 0;
    hal_event_post(OEM_EV_SESSION_START);
    hal_timer_start(HAL_TMR_FAST30, 30, true);   // 0x4201c1d0
    g_oem.sys[0x35] = 1;
    g_oem.stop_delay = 200;
}

static void pause_session(void)                  // "suspend_brush_by_profile"
{
    brush_pause();
    oem_touch_set_state(0x21);                   // full touch re-init now that the motor is off
    oem_show_paused();
    hal_delay(100);
    oem_touch_irq(true);
}

static void resume_session(void)                 // "resume_brush_by_profile"
{
    oem_touch_irq(false);
    hal_delay(50);
    brush_resume();
    oem_idle_off();
    oem_show_brushing();
}

// Event 0x80000: 0x4201c2b8 and the loop part at 0x4201ce36
static void session_end(void)
{
    hal_timer_stop(HAL_TMR_FAST30);              // 0x4201c21c
    oem_idle_off();
    oem_touch_set_state(6);
    bool counted = brush_session_finish();
    hal_log("session end: %u of %u s, score %u%s", g_oem.done_s, g_oem.total_s, g_oem.score,
            counted ? "" : " (not counted)");
    oem_gauge_session_done(g_oem.done_s);
    g_oem.state = OEM_ST_ENDED;
    if (g_oem.power_state == OEM_PWR_BATTERY) {
        // "complete" (103, then the score) when the session ended from the running
        // screens; from the pause screen or after a swipe-quit only from 120 s on
        bool complete;
        if (g_oem.user_quit) complete = g_oem.done_s >= 120;
        else if (g_oem.now_ui == 101) complete = g_oem.done_s >= 120;
        else complete = true;
        if (complete) show_complete();
        else oem_show_main_after_session();
    } else {
        oem_idle_timeout(10);                    // ended by the charger: its screen is already up
    }
    hal_delay(100);
    oem_touch_irq(true);
    g_oem.sys[0x35] = 0;

    oem_power_cpu_lock(false);
    oem_gauge_rearm(2);
    oem_led_set(2, 1, 4);
    oem_led_set(3, 1, 4);
}

// ---- button (0x4201cab8, 0x4201c790) ----------------------------------------------------

void oem_button_push(uint8_t code)
{
    uint8_t w = s_btn_w;
    if ((uint8_t)(w - s_btn_r) < sizeof s_btn_q) {
        s_btn_q[w % sizeof s_btn_q] = code;
        s_btn_w = (uint8_t)(w + 1);
    }
}

// 0x4201b0d8: a short press on these screens only goes back to the mode page
static bool dismiss_screen(void)
{
    uint8_t ui = g_oem.now_ui;
    bool hit = ui == 71 || ui == 72 || ui == 73 || ui == 83 || ui == 92 || ui == 93 || ui == 94
            || ui == 97 || ui == 100
            || (ui == 84 && (g_oem.state == OEM_ST_ENDED || g_oem.state == OEM_ST_SCORED));
    if (!hit || g_oem.asleep) return false;
    if (ui == 97) return true;                   // update prompt: not ported, never shown
    oem_show_main();
    oem_idle_timeout((ui == 83 || ui == 84 || ui == 100) ? 10 : (ui == 92 || ui == 94) ? 5 : 60);
    return true;
}

static void short_press(void)                    // 0x4201c790
{
    hal_log("short press: asleep %u power %u touch %u screen %u", g_oem.asleep, g_oem.power_state,
            oem_touch_state(), g_oem.now_ui);
    s.pending_screen = 0;
    if (g_oem.aging == 1) return;
    if (g_oem.batt_pct == 0 || g_oem.ota) {      // "low power.ota lock key"
        if (g_oem.batt_pct != 0) return;
        oem_idle_timeout(1);
        show_low_battery();
        return;
    }
    if (g_oem.show_mode && g_oem.dev_mode != 2) { oem_leave_show_mode(); return; }
    if (g_oem.asleep == 1) {                     // wake only; the next press starts brushing
        gyro_count_clear();
        wake();
        brush_mode_clamp();
        oem_touch_irq(true);
        return;
    }
    if (dismiss_screen()) return;
    if (!g_oem.session_active) {
        if (oem_touch_state() != 5) return;      // the touch IC must be up and running
        start_session();
        return;
    }
    if (g_oem.score != 0xff) brush_user_config_save();
    if (g_oem.done_s >= g_oem.total_s) { oem_motor_stop(); return; }
    if (g_oem.running) pause_session();
    else resume_session();
}

static void button_event(uint8_t code)           // 0x4201cab8
{
    if (g_oem.ota || g_oem.reset_flag) return;   // key locked
    // 0x4201829c: on the charger a press brings the backlight back for another 30 s
    bool lit = s.close_screen_cnt >= 30;
    if (lit) s.close_screen_cnt = 0;
    if (lit && g_oem.power_state != OEM_PWR_BATTERY) oem_led_set(4, 0, 4);
    g_oem.sys[0x69] = 0;

    if (g_oem.power_state != OEM_PWR_BATTERY) {
        if (g_oem.asleep != 1 || code != 0) return;         // awake on the charger: nothing else
        if (g_oem.batt_pct > 99) oem_wlc_off();
        s.pend_cnt = 0;
        wake_for_charge();
        return;
    }
    switch (code) {
    case 0:                                      // short press
        if (g_oem.dev_mode == 1 && !g_oem.show_mode && g_oem.locked == 1 && g_oem.lock_popup == 1)
            oem_ui_lock_button(0);
        else if (g_oem.subpage == 1)
            oem_ui_page_back();
        else
            short_press();
        break;
    case 1:                                      // 2 s: touch lock
        if (g_oem.dev_mode == 1 && !g_oem.show_mode) {
            oem_ui_lock_button(1);
            g_oem.saved_mode = g_oem.profile[6];
        }
        break;
    case 3:                                      // 5 s on a mode page: info screen
        if (g_oem.now_ui >= 76 && g_oem.now_ui <= 81) {
            uint8_t page = 0;
            g_oem.info_page = 0;
            g_oem.now_ui = 92;                   // 0x4201a94c
            oem_ui_post(92, &page, 1);
            oem_idle_timeout(30);
        }
        break;
    default:
        // 2 (3 s): aging mode / declining the update prompt, not ported.
        // 4 (8 s): factory reset, done by the UI task (oem_factory_reset).
        break;
    }
}

// ---- 1 Hz ---------------------------------------------------------------------------------

// 0x4201babc: three motor pulses of 400 ms, 400 ms apart. Stock blocks the main task for
// these 2 s; here the 10 ms tick runs them, so the UI can draw the warning meanwhile.
static void buzz_tick(void)
{
    if (!s.buzz) return;
    uint8_t t = s.buzz - 1;                      // ticks since the start: on at 0, 80, 160, off 40 ticks later
    if (t % 80 == 0 && t < BUZZ_TICKS) oem_motor_gear(33, false);
    else if (t % 80 == 40) oem_motor_off();
    s.buzz = t >= BUZZ_TICKS ? 0 : s.buzz + 1;
}

// 0x420142d0, returns true when it is time for the screen-off sequence
static bool idle_check(void)
{
    if (!s.idle_on || g_oem.asleep) return false;
    uint32_t el = hal_uptime_s() - s.idle_t0;
    if (el < s.idle_timeout) return false;
    if (g_oem.batt_pct <= 10 && g_oem.power_state == OEM_PWR_BATTERY && g_oem.dev_mode != 2) {
        if (el == s.idle_timeout) {              // exactly at expiry: the warning
            show_low_battery();
            if (g_oem.batt_pct != 0 && g_oem.brushed_since_wake) s.buzz = 1;
            g_oem.brushed_since_wake = 0;
        }
        if (el < (uint32_t)s.idle_timeout + 3) return false;   // the warning stays 3 s
    }
    g_oem.asleep = 1;
    return true;
}

static void sequencer_1hz(void)                  // 0x4201b1e4
{
    if (g_oem.power_state != OEM_PWR_BATTERY || g_oem.asleep) return;
    if (g_oem.now_ui == 88) return;
    if (g_oem.dwell_s < 30) g_oem.dwell_s++;

    uint8_t ui = g_oem.now_ui, d = g_oem.dwell_s, st = g_oem.state;
    bool result_page = ui == 83 || ui == 84 || ui == 100;
    if (ui == 71) {                              // pairing guide
        if (d == 2) oem_show(72, NULL, 0);
    } else if (ui == 72) {
        if (d == 2) { oem_show(73, NULL, 0); oem_idle_timeout(10); }
    } else if (ui == 73) {
        // last guide page: stays until a swipe, a press or the idle timer
    } else if (ui == 87) {                       // intensity screen: 5 s without a swipe
        if (d > 4) { oem_touch_irq(false); g_oem.now_ui = 82; }   // the per-second refresh shows 82
    } else if ((st == OEM_ST_BOOT || st == OEM_ST_WOKEN) && (ui == 84 || ui == 85 || ui == 104)) {
        if (d == 6) oem_show_main();             // wake screen -> mode page
    } else if (st == OEM_ST_ENDED && result_page) {
        if (d == 10) {
            // 0x4201c168: daily goal reached (more than 6 min, score sum above 200,
            // 3 sessions) on a day that began since boot: LED chase
            const oem_rtc_t *r = hal_rtc();
            if (r->hist_seconds > 360 && r->hist_score_sum > 200 && r->hist_count > 2 && brush_hist_new_day())
                oem_led_set(1, 0, 1);
            if (r->hist_count != 0) { oem_show_history(1); g_oem.state = OEM_ST_SCORED; }
            else { oem_show_main(); oem_idle_timeout(10); }
        }
    } else if (st == OEM_ST_SCORED && result_page) {
        if (d == 10) { oem_show_main(); oem_idle_timeout(10); }
    } else if (ui == 103 && d == 1) {
        oem_show_score(g_oem.score);
    }
}

static void block_1hz(void)                      // 0x4201d0ec..0x4201d4cd
{
    // charger removed after the backlight had timed out: wake screen 4 s later
    if (s.pending_screen) s.pend_cnt++;
    if (s.pend_cnt > 3 && s.pending_screen) {
        if (g_oem.power_state == OEM_PWR_BATTERY) wake_screen();
        else show_charging(g_oem.batt_pct);
        s.pend_cnt = 0;
        s.pending_screen = 0;
    }

    brush_tick_1hz();

    if (g_oem.asleep == 1) {                     // 0x4200bae0: Wi-Fi goes off 27 s after the screen
        if (g_oem.ota) s.wifi_off_cnt = 0;       // (not while a firmware upload runs over it)
        if (s.wifi_off_cnt < 100) s.wifi_off_cnt++;
        if (s.wifi_off_cnt == 27) hal_net_sleep();
    } else {
        sequencer_1hz();
    }

    if (!g_oem.session_active) {
        oem_gauge_tick();                        // the percent is frozen during a session
    } else if (g_oem.stop_delay == 200) {
        if (g_oem.now_ui != 87) oem_show_brushing();        // refresh the countdown
    }

    if (g_oem.power_state != OEM_PWR_BATTERY) {
        delay_to_close_screen();
    } else if (!g_oem.show_mode && !g_oem.ota && g_oem.ui_mode != 0xd7 && !g_oem.session_active
               && idle_check()) {
        oem_led_set(1, 1, 3);                    // LEDs and backlight fade out
        oem_touch_set_state(7);
        s.sleep_seq = SLEEP_SEQ_TICKS;
    }
    g_oem.tick_s++;
}

static void tick_10ms(void)
{
    oem_led_tick();
    charger_poll();
    if (s.boot_ticks && --s.boot_ticks == 0) boot_finish();
    buzz_tick();
    // Stock stops a running session when an update starts (event bit 4, 0x4201b638).
    if (g_oem.ota && g_oem.session_active) oem_motor_stop();
    if (s.sleep_seq && --s.sleep_seq == 0 && g_oem.asleep) screen_off_sequence();
    if (s_rem.count) remote_run();               // a remote start waiting for the wake-up
    // stock: 1 Hz reseed watchdog of the touch IC, not while the motor runs
    if (!g_oem.session_active || g_oem.stop_delay != 200) oem_touch_tick_10ms();
    if (++s.tick_div > 99) {
        s.tick_div = 0;
        block_1hz();
    }
}

// ---- remote commands (custom) -----------------------------------------------------------

static void remote_push(uint8_t kind, uint8_t arg)
{
    hal_lock();
    uint8_t last = (uint8_t)((s_rem.head + s_rem.count - 1) % 8);
    bool same = s_rem.count && s_rem.q[last].kind == kind && s_rem.q[last].arg == arg;
    if (!same) {
        if (s_rem.count < 8) {
            uint8_t at = (uint8_t)((s_rem.head + s_rem.count) % 8);
            s_rem.q[at].kind = kind;
            s_rem.q[at].arg = arg;
            s_rem.count++;
        } else {
            hal_log("remote: queue full, command %u dropped", kind);
        }
    }
    hal_unlock();
    hal_event_post(OEM_EV_REMOTE);
}

void oem_remote_brushing(bool on) { remote_push(on ? REM_BRUSH_ON : REM_BRUSH_OFF, 0); }
void oem_remote_mode(uint8_t mode) { remote_push(REM_MODE, mode); }
void oem_remote_strength(uint8_t level) { remote_push(REM_STRENGTH, level); }
void oem_net_activity(void) { remote_push(REM_ACTIVITY, 0); }

// "Brushing on": what a user achieves with the button (a press to wake, a press to
// start), in one go. Returns false to be called again on the next tick.
static bool remote_brush_on(void)
{
    if (g_oem.ota || g_oem.reset_flag || g_oem.aging == 1) return true;
    if (g_oem.session_active) {
        if (!g_oem.running && g_oem.done_s < g_oem.total_s) resume_session();
        return true;
    }
    if (g_oem.power_state != OEM_PWR_BATTERY) {  // as the button: no brushing on the charger
        hal_log("remote: on the charger, not starting");
        return true;
    }
    bool last_try = s_rem.tries >= REMOTE_TRIES;
    if (g_oem.asleep == 1) {
        if (!s.sleep_seq) {                      // else the screen-off sequence is under way: wait for it
            gyro_count_clear();
            wake();
        }
        if (g_oem.asleep == 1) {
            if (last_try) hal_log("remote: the brush did not wake, start dropped");
            return last_try;
        }
        brush_mode_clamp();
    }
    if (g_oem.batt_pct == 0) {                   // as the button: low-battery screen, no start
        if (g_oem.now_ui != 94) { oem_idle_timeout(1); show_low_battery(); }
        return true;
    }
    // Like the button, wait until the touch IC has finished its (re-)init; after the
    // time-out start anyway, the motor does not depend on it.
    if (oem_touch_state() != 5 && !last_try) return false;
    start_session();
    return true;
}

static bool remote_exec(uint8_t kind, uint8_t arg)
{
    switch (kind) {
    case REM_ACTIVITY:
        net_activity();
        return true;
    case REM_BRUSH_ON:
        return remote_brush_on();
    case REM_BRUSH_OFF:
        if (g_oem.session_active) oem_motor_stop();
        return true;
    case REM_MODE:
        if (arg > 5 || g_oem.session_active) return true;   // stock refuses scheme changes during a session too
        oem_set_mode(arg);
        brush_mode_clamp();
        g_oem.saved_mode = g_oem.profile[6];     // so that the screen-off sequence does not undo it
        if (g_oem.asleep) brush_user_config_save();
        else if (g_oem.power_state == OEM_PWR_BATTERY && g_oem.now_ui >= 76 && g_oem.now_ui <= 81) oem_show_main();
        return true;
    case REM_STRENGTH:
        if (arg < 1 || arg > 5) return true;
        hal_rtc()->strength = arg;
        if (g_oem.session_active && g_oem.profile[6] == 5) {
            if (g_oem.now_ui == 87) oem_show_strength(arg - 1);
            brush_strength_set(arg);
        }
        return true;
    }
    return true;
}

static void remote_run(void)
{
    while (s_rem.count) {
        uint8_t kind = s_rem.q[s_rem.head].kind, arg = s_rem.q[s_rem.head].arg;
        if (!remote_exec(kind, arg)) {           // head command waits; the ones behind it keep their order
            s_rem.tries++;
            return;
        }
        s_rem.head = (uint8_t)((s_rem.head + 1) % 8);
        s_rem.count--;
        s_rem.tries = 0;
    }
}

// ---- factory reset, show mode -----------------------------------------------------------

void oem_factory_reset(void)                     // 0x4201c6b8
{
    oem_touch_irq(false);
    hal_delay(20);
    oem_idle_off();
    if (g_oem.session_active) oem_motor_off();   // not in stock: do not restart with the motor running
    g_oem.sys[0x0d] = 0;
    g_oem.sys[0x0e] = 0x16;
    g_oem.sys[0x34] = 11;                        // next boot: defaults, 8 s, then the shipping sleep
    oem_gauge_save();
    brush_sys_config_save();
    hal_delay(100);
    hal_restart();
}

void oem_leave_show_mode(void)                   // 0x4201a9a0
{
    g_oem.ui_mode = 0;
    g_oem.show_mode = 0;
    oem_show_main();
}

// ---- boot, event loop body --------------------------------------------------------------

// The main task before its loop (0x4201cc40), with the configuration loads that stock
// does in app_main. The glue has already run oem_power_boot() and the init functions
// of the other modules (LEDs, motor, touch, button, pressure, UI).
//
// Stock waits 500 ms in the middle (vTaskDelay) while its UI task already draws the
// first screen. Waiting here would keep the core lock, and with it the UI, for that
// time; so the part after the delay runs from the 10 ms tick, 50 ticks later.
void oem_app_boot(int wake_cause)
{
    hal_log("oem app boot, wake cause %d", wake_cause);
    brush_hist_load();                           // app_main: 0x4201c100
    brush_sys_config_load();                     // app_main: 0x42018cf8
    if (wake_cause == OEM_WAKE_BUTTON) gyro_count_clear();   // app_main, EXT1 from GPIO3

    oem_idle_timeout(60);
    oem_gauge_boot();                            // 0x4201831c: first estimate, power_state = battery
    g_oem.power_state = OEM_PWR_BATTERY;
    hal_lcd_init();
    brush_hist_update(0, 0);
    // First screen. As in stock this comes before the user settings are loaded, so the
    // birthday greeting cannot appear here, only after a wake from the screen-off stage.
    s.boot_stage = g_oem.sys[0x34];
    if (s.boot_stage == 11 || s.boot_stage == 0) {          // never set up / just reset: boot animation
        g_oem.now_ui = 70;
        oem_ui_post(70, NULL, 0);
    } else {
        wake_screen();
    }
    oem_led_set(4, 0, 4);                        // backlight on
    brush_config_load();
    brush_mode_clamp();
    g_oem.lang = g_oem.sys[0x67];
    if (g_oem.sys[0x6f] == 0x37) oem_idle_timeout(7);       // set by the BLE factory-reset command
    s.boot_ticks = BOOT_TICKS;
}

static void boot_finish(void)
{
    uint8_t stage = s.boot_stage;
    oem_imu_normal();
    if (stage == 11) {                           // first boot after a factory reset: shipping state
        oem_idle_timeout(8);
        brush_hist_reset();
        g_oem.reset_flag = 1;                    // button ignored; deep sleep right at screen-off
    }
    g_oem.sys[0x34] = stage == 11 ? 0 : stage == 0 ? 1 : 2;
    g_oem.sys[0x6f] = 0;
    brush_sys_config_save();

    if (!s.no_touch_irq) oem_touch_irq(true);
    if (g_oem.power_state != OEM_PWR_CHARGING && g_oem.batt_pct != 0) oem_led_set(1, 0, 4);
    oem_pressure_start();                        // 20 ms force sampling runs whenever the screen is on
    oem_motion_irq(false);
    oem_motor_off();
    g_oem.init_ok = 5;                           // from here on the input interrupts count
    oem_touch_set_state(0);                      // cold start of the touch IC
}

void oem_app_handle(uint32_t bits)
{
    if (bits & OEM_EV_MOTION) {                  // 0x4201cf9d: raise to wake
        if (g_oem.sys[8] == 1 && g_oem.power_state == OEM_PWR_BATTERY) {
            g_oem.motion_gate = hal_anymotion_allowed();
            if (g_oem.wake_gate >= 4 && !gyro_limit()) {
                gyro_count_inc();
                wake();
                g_oem.motion_gate = 1;
            }
        }
    }
    if (bits & OEM_EV_SESSION_END) session_end();
    if (bits & OEM_EV_TOUCH_RDY) oem_touch_step();
    if (bits & OEM_EV_BLE_WAKE) {                // a client connected: screen on
        if (g_oem.asleep == 1 && s.ble_wake_ok && g_oem.power_state == OEM_PWR_BATTERY) wake();
    }
    if (bits & OEM_EV_TICK) tick_10ms();
    if (bits & OEM_EV_FAST) brush_tick_30ms();
    if (bits & OEM_EV_BUTTON) {
        while (s_btn_r != s_btn_w) {
            uint8_t code = s_btn_q[s_btn_r % sizeof s_btn_q];
            s_btn_r = (uint8_t)(s_btn_r + 1);
            button_event(code);
        }
    }
    if (bits & OEM_EV_CHARGER) usb_action(false);
    if (bits & OEM_EV_REMOTE) remote_run();
    if (bits & OEM_EV_SLEEP_STAGE2) sleep_stage2();
    if (bits & OEM_EV_BLE_TIMEOUT) ble_timeout();
    if (bits & OEM_EV_PRESSURE) oem_pressure_sample();
}

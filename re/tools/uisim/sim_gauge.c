// Host test of the battery gauge (main/oem_gauge.c) with virtual time and fakes for
// the ADC, the charge pin, NVS, the LED engine, the UI queue and the IMU.
// Checks it against re/spec/led_battery_charge.md 5.2 - 5.10 and 6.5.
//
//   cc -std=gnu11 -Wall -Wextra -Imain re/tools/uisim/sim_gauge.c -o /tmp/sim_gauge
//   /tmp/sim_gauge [-v]            (-v also prints the gauge's own log lines)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oem_gauge.c"          // white box: the state struct `s` and the static helpers

// ---- fakes -----------------------------------------------------------------------
oem_state_t g_oem;
static int      g_verbose;
static uint32_t now_ms;
static int      f_mv;                   // what the ADC reads
static int      (*f_model)(void);       // or a model of the battery
static uint8_t  f_nvs[32]; static size_t f_nvs_len; static int f_nvs_writes;
static int      f_allow = -1, f_allow_calls; static char f_allow_log[64];
static int      f_led_calls; static int f_led[8][3];
static int      f_ui_posts, f_ui_last = -1;
static float    f_temp = 25; static bool f_temp_ok = true;

uint32_t hal_ms(void) { return now_ms; }
uint32_t hal_uptime_s(void) { return now_ms / 1000; }
void hal_log(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt);
    printf("    [%6u s] ", (unsigned)(now_ms / 1000)); vprintf(fmt, ap); printf("\n");
    va_end(ap);
}
size_t hal_nvs_get(const char *key, void *buf, size_t len)
{
    if (strcmp(key, "brush_battery")) return 0;
    memcpy(buf, f_nvs, len < f_nvs_len ? len : f_nvs_len);
    return f_nvs_len;
}
bool hal_nvs_set(const char *key, const void *buf, size_t len)
{
    if (strcmp(key, "brush_battery") || len > sizeof f_nvs) return false;
    memcpy(f_nvs, buf, len); f_nvs_len = len; f_nvs_writes++;
    return true;
}
int  oem_batt_mv_now(void) { return f_model ? f_model() : f_mv; }
void oem_charge_allow(bool allow)
{
    f_allow = allow;
    if (f_allow_calls < 60) f_allow_log[f_allow_calls] = allow ? 'A' : 'b';
    f_allow_calls++;
}
void oem_led_set(int led, int state, int anim)
{
    if (f_led_calls < 8) { f_led[f_led_calls][0] = led; f_led[f_led_calls][1] = state; f_led[f_led_calls][2] = anim; }
    f_led_calls++;
}
void oem_ui_post(uint16_t id, const void *payload, int len)
{
    if (id == 0x2a && len == 1) { f_ui_posts++; f_ui_last = *(const uint8_t *)payload; }
}
bool oem_imu_temp(float *c) { *c = f_temp; return f_temp_ok; }

// ---- helpers ---------------------------------------------------------------------
static int g_fail, g_checks;
static void check(bool ok, const char *fmt, ...)
{
    va_list ap;
    g_checks++;
    if (!ok) g_fail++;
    printf("  %s ", ok ? "ok  " : "FAIL");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

static void nvs_put(int pct, int mv)    // pct < 0: erase
{
    memset(f_nvs, 0, sizeof f_nvs);
    f_nvs_len = 0;
    if (pct < 0) return;
    f_nvs[0] = (uint8_t)pct; f_nvs[1] = (uint8_t)(mv >> 8); f_nvs[2] = (uint8_t)mv;
    f_nvs_len = 20;
}
// Power cycle: module and shared state back to power-on, NVS kept.
static void reboot(int mv)
{
    s = (gauge_t)GAUGE_POWER_ON;
    memset(&g_oem, 0, sizeof g_oem);
    now_ms = 0;
    f_mv = mv; f_model = NULL;
    f_allow = -1; f_allow_calls = 0; memset(f_allow_log, 0, sizeof f_allow_log);
    f_led_calls = 0; f_ui_posts = 0; f_ui_last = -1; f_nvs_writes = 0;
    f_temp = 25; f_temp_ok = true;
    oem_gauge_boot();
}
static void tick(int n)                 // n gauge ticks, one per second
{
    for (int i = 0; i < n; i++) { now_ms += 1000; oem_gauge_tick(); }
}
static void start(int mv) { reboot(mv); tick(4); }   // through the first measurement

// What the charge state machine (app module, stock 0x42017a0c) does to the gauge.
static void app_attach(bool woke)
{
    oem_gauge_settle_reset();
    g_oem.slew_cnt = 0;
    g_oem.full_cnt = 0;
    g_oem.plug_cnt = woke ? 0 : 100;
    g_oem.power_state = OEM_PWR_CHARGING;
    oem_gauge_rearm(g_oem.gauge_period);
    oem_charge_allow(true);
}
static void app_detach(void)
{
    g_oem.plug_cnt = 100;
    oem_gauge_rearm(2);
    oem_gauge_settle_reset();
    g_oem.power_state = OEM_PWR_BATTERY;
}

// ---- tests -----------------------------------------------------------------------
static void t_table(void)
{
    printf("== voltage -> percent (table 0x3c1192c8, index clamped)\n   mV      %%\n");
    static const int want[] = { 0, 0, 0, 0, 0, 10, 30, 50, 65, 77, 90, 100, 100, 100, 100 };
    bool ok = true;
    for (int i = 0; i <= 14; i++) {
        int mv = 3000 + 100 * i, p = soc_raw((uint32_t)mv);
        printf("  %4d   %3d\n", mv, p);
        ok = ok && p == want[i];
    }
    check(ok, "3400 0, 3500 10, 3600 30, 3700 50, 3800 65, 3900 77, 4000 90, >= 4100 100");
    check(soc_raw(3450) == 5 && soc_raw(3550) == 20 && soc_raw(3750) == 57 && soc_raw(3950) == 83 && soc_raw(4050) == 95,
          "linear between the points: 3450 5, 3550 20, 3750 57, 3950 83, 4050 95");
    check(soc_raw(0) == 0 && soc_raw(2999) == 0 && soc_raw(4199) == 100 && soc_raw(4200) == 100 &&
          soc_raw(5000) == 100 && soc_raw(65535) == 100, "below 3000 -> 0, 4200 and above -> 100 (stock overruns the table there)");

    s = (gauge_t)GAUGE_POWER_ON; memset(&g_oem, 0, sizeof g_oem);
    g_oem.power_state = OEM_PWR_BATTERY;
    check(soc_filtered(3445) == 0 && soc_filtered(3446) == 4 && soc_filtered(4109) == 100 && soc_filtered(4110) == 100,
          "filtered, off charger: <= 3445 mV is 0 %%, 3446 is 4 %%, > 4109 is 100 %%");
    s.first_done = 1; g_oem.power_state = OEM_PWR_CHARGING;
    check(soc_filtered(3445) == 0 && soc_filtered(3446) == 4 && soc_filtered(4000) == 90 && soc_filtered(4110) == 100,
          "filtered, on charger: same clamps, no latch");
    g_oem.power_state = OEM_PWR_BATTERY; s.soc_x100 = 6000;
    check(soc_filtered(3900) == 60 && soc_filtered(3650) == 40 && soc_filtered(3900) == 40,
          "off charger the value only falls (latch): 60 -> 40, stays 40 at 3900 mV");

    printf("== charge slew period (0x42017ca0)\n   mV   period s   s per 1 %%\n");
    static const int per[] = { 21, 21, 17, 23, 17, 13, 15, 6, 6, 6 };
    ok = true;
    for (int i = 0; i < 10; i++) {
        int mv = 3350 + 100 * i, p = charge_period((uint16_t)mv);
        printf("  %4d   %3d       %4d\n", mv, p, p * 10);
        ok = ok && p == per[i];
    }
    check(ok, "< 3500: 21, 35xx: 17, 36xx: 23, 37xx: 17, 38xx: 13, 39xx: 15, >= 4000: 6");
}

static void t_startup(void)
{
    printf("== start-up sequence, no record, 3900 mV, off the charger\n");
    nvs_put(-1, 0);
    reboot(3900);
    check(g_oem.batt_pct == 77 && g_oem.power_state == OEM_PWR_BATTERY && !g_oem.gauge_inited &&
          g_oem.plug_cnt == 100 && g_oem.gauge_period == 64, "boot: 77 %% from one sample, state 2, not inited");
    tick(2);
    check(f_allow_calls == 0 && !g_oem.gauge_inited, "ticks 1, 2 (stock sec 0, 1): nothing");
    tick(1);
    check(f_allow == 0 && f_allow_calls == 1 && !g_oem.gauge_inited, "tick 3 (sec 2): charging blocked");
    tick(1);
    check(g_oem.gauge_inited && f_allow == 1 && !strcmp(f_allow_log, "bbbA"),
          "tick 4 (sec 3): measurement with charging blocked, then allowed (%s)", f_allow_log);
    check(g_oem.batt_pct == 77 && g_oem.batt_mv == 3900 && f_ui_last == 77 && f_ui_posts == 1,
          "77 %%, 3900 mV published, UI message 0x2a sent");
    check(f_nvs_len == 20 && f_nvs[0] == 77 && f_nvs[1] == 0x0f && f_nvs[2] == 0x3c && f_nvs_writes == 1,
          "record saved: 20 bytes { 77, 0x0f, 0x3c }");
    tick(10);
    check(f_ui_posts == 6 && f_nvs_writes == 1 && f_allow_calls == 4, "then an update every 2 s; no further save, charge pin untouched");

    printf("== the same when ticked every 10 ms (stock: brushing on the dock before the first measurement)\n");
    reboot(3900);
    int t_block = -1, t_meas = -1;
    for (int i = 0; i < 400 && t_meas < 0; i++) {
        now_ms += 10; oem_gauge_tick();
        if (t_block < 0 && f_allow == 0) t_block = (int)now_ms;
        if (g_oem.gauge_inited) t_meas = (int)now_ms;
    }
    check(t_block == 1510 && t_meas == 2510, "charging blocked 1.5 s after the first tick, measurement at 2.5 s (%d, %d ms)", t_block, t_meas);
}

static void t_discharge(void)
{
    printf("== discharge: saved 77 %% / 3900 mV, battery now 3800 mV (65 %%)\n");
    nvs_put(77, 3900);
    reboot(3800);
    check(g_oem.batt_pct == 65, "boot estimate from the voltage: 65 %%");
    tick(4);
    check(g_oem.batt_pct == 72, "first measurement trusts the record (77), first update drops the one-shot 5: %u", g_oem.batt_pct);
    printf("   t s    mV   pct\n");
    bool ok = true; int prev = g_oem.batt_pct;
    static const int at[] = { 103, 104, 204, 304, 404, 504, 604, 704, 1500 }, want[] = { 72, 71, 70, 69, 68, 67, 66, 65, 65 };
    for (int i = 0; i < 9; i++) {
        while ((int)(now_ms / 1000) < at[i]) { tick(1); if (g_oem.batt_pct > prev) ok = false; prev = g_oem.batt_pct; }
        printf("  %5d  %4u  %3u\n", at[i], g_oem.batt_mv, g_oem.batt_pct);
        ok = ok && g_oem.batt_pct == want[i];
    }
    check(ok, "one point per 100 s down to the voltage's 65 %%, never below, never up");
    check(f_nvs[0] == 65 && f_nvs_writes == 8, "record follows every change (%d writes)", f_nvs_writes);

    f_mv = 4000; tick(60);
    check(g_oem.batt_pct == 65, "voltage back up to 4000 mV off the charger: percent does not rise");

    // The limiter grants (seconds since the last grant) / 100 points, so after 858 s
    // without a drop the first update at a lower voltage may fall 8 points at once.
    f_mv = 3700; tick(11);
    int before = g_oem.batt_pct;
    check(before == 57, "3700 mV (50 %%) after 858 s without a drop: 8 points at once, then held: %d", before);
    oem_gauge_session_done(120);
    tick(2);
    check(g_oem.batt_pct == 50, "after a session of >= 16 s the next update drops freely: %d -> %u", before, g_oem.batt_pct);
    f_mv = 3650; tick(9);
    before = g_oem.batt_pct;
    oem_gauge_session_done(10);
    tick(2);
    check(before == 50 && g_oem.batt_pct == 47, "after a short session the allowance is 3 points: %d -> %u", before, g_oem.batt_pct);

    printf("== nearly empty: battery 3440 mV\n");
    f_mv = 3440; ok = true; prev = g_oem.batt_pct; int t0 = (int)(now_ms / 1000), t_zero = -1;
    for (int i = 0; i < 60; i++) {
        tick(1);
        if (g_oem.batt_pct > prev) ok = false;
        prev = g_oem.batt_pct;
        if (t_zero < 0 && g_oem.batt_pct == 0) t_zero = (int)(now_ms / 1000) - t0;
    }
    check(ok && t_zero > 0 && t_zero <= 30, "target below 10 %%: falls by a third per update, 0 %% after %d s", t_zero);
    check(f_nvs[0] == 0, "record now says 0 %% (the lock)");
}

static void t_lock(void)
{
    printf("== 0 %% lock\n");
    nvs_put(0, 3400);
    reboot(3450);
    check(g_oem.batt_pct == 0, "saved 0, boot at 3450 mV (5 %%): 0 %% from the first moment");
    nvs_put(0, 3400);
    reboot(3600);
    check(g_oem.batt_pct == 30, "saved 0, boot at 3600 mV: boot estimate 30 %% (>= 10, stock shows it for 3 s)");
    tick(4);
    check(g_oem.batt_pct == 0 && g_oem.gauge_inited, "first measurement below 40 %%: locked at 0");
    tick(120);
    check(g_oem.batt_pct == 0 && f_nvs[0] == 0, "stays 0 at 3600 mV (30 %%)");
    f_mv = 3700; tick(60);
    check(g_oem.batt_pct == 0, "stays 0 at 3700 mV (50 %% is not above 50)");
    f_mv = 3750; tick(6);
    check(g_oem.batt_pct == 56 && f_nvs[0] == 56, "3750 mV: released at the first update that reads above 50 %% (filter at 3745 mV): %u", g_oem.batt_pct);

    nvs_put(0, 3400);
    start(3700);
    check(g_oem.batt_pct == 50 && f_nvs[0] == 50, "saved 0, first measurement 3700 mV (50 %% >= 40): released at once");

    nvs_put(0, 3400);
    start(3500);
    check(g_oem.batt_pct == 0, "saved 0, 3500 mV: locked ...");
    app_attach(false);
    f_mv = 3650;                            // charging: 3650 at the pin, 3600 after the 50 mV compensation
    tick(229);
    int a = g_oem.batt_pct; tick(1); int b = g_oem.batt_pct; tick(229); int c = g_oem.batt_pct; tick(1);
    check(a == 0 && b == 1 && c == 1 && g_oem.batt_pct == 2 && g_oem.gauge_period == 23,
          "... the charge slew raises it: +1 after 230 s, +1 after 460 s (period 23 s)");

    nvs_put(40, 3600);
    start(3250);
    check(g_oem.batt_pct == 0 && f_nvs[0] == 0, "saved 40, first measurement 3250 mV (<= 3299): set to 0 and saved");
}

static void t_charge_const(void)
{
    printf("== charging at a constant 4200 mV from 10 %%\n");
    nvs_put(-1, 0);
    start(3500);
    check(g_oem.batt_pct == 10, "start at 3500 mV: 10 %%");
    app_attach(false);
    f_mv = 4200;
    int n = 0, t99 = -1, t100 = -1, tfull = -1; bool ok = true;
    while (n < 7000 && tfull < 0) {
        int prev = g_oem.batt_pct;
        tick(1); n++;
        if (g_oem.batt_pct < prev) ok = false;
        if (g_oem.batt_pct != prev && g_oem.batt_pct <= 99 && n % 60) ok = false;   // only on every 10th update
        if (g_oem.batt_pct == 99 && t99 < 0) t99 = n;
        if (g_oem.batt_pct == 100 && t100 < 0) t100 = n;
        if (g_oem.power_state == OEM_PWR_FULL && tfull < 0) tfull = n;
    }
    check(ok && t99 == 89 * 60, "one point per 60 s (10 updates of 6 s): 99 %% after %d s", t99);
    check(t100 == t99 + 180, "99 -> 100 needs three confirmations: +%d s", t100 - t99);
    check(tfull == t100 + 5, "FULL after 100 %% held for six ticks: +%d s", tfull - t100);
    check(f_led_calls == 1 && f_led[0][0] == 2 && f_led[0][1] == 0 && f_led[0][2] == 4, "LED call (2, ON, 4) made once");
    check(f_allow == 1, "charging is not switched off when full");
    tick(120);
    check(g_oem.batt_pct == 100 && g_oem.power_state == OEM_PWR_FULL && f_led_calls == 1 && f_nvs[0] == 100, "stays FULL / 100 %%, record 100");

    printf("== taken off the charger at full\n");
    app_detach();
    int posts = f_ui_posts, first = 0;
    f_mv = 4150;
    for (int i = 1; i <= 60; i++) { tick(1); if (!first && f_ui_posts != posts) first = i; }
    check(first == 60 && g_oem.batt_pct == 100,
          "stock quirk: per_cnt is re-armed against the charging period (6), so the first update comes %d s after un-plug", first);
    f_mv = 4000; tick(2);
    check(g_oem.batt_pct == 91, "4000 mV: the limiter had hours of allowance, the percent follows the filter at once: %u", g_oem.batt_pct);
    bool mono = true; int prev = g_oem.batt_pct;
    for (int i = 0; i < 400; i++) { tick(1); if (g_oem.batt_pct > prev) mono = false; prev = g_oem.batt_pct; }
    check(mono && g_oem.batt_pct == 90, "then one point per 100 s: %u %% after 400 s", g_oem.batt_pct);
}

// Battery model for a whole charge: the cell follows the stock charge-time table
// (0x3c1192e2) at twice its speed, so the slew, not the voltage, limits the percent.
static int m_t0;
static int model_charge(void)
{
    double m = 36.0 + 2.0 * ((int)(now_ms / 1000) - m_t0) / 60.0;
    for (int i = 5; i < 12; i++)
        if (m < chg_min[i + 1])
            return 3000 + 100 * i + (int)(100.0 * (m - chg_min[i]) / (chg_min[i + 1] - chg_min[i]));
    return 4200;
}

static void t_charge_full(void)
{
    printf("== a whole charge from 3.5 V (cell follows the charge table at 2x speed)\n");
    nvs_put(-1, 0);
    start(3500);
    app_attach(false);
    m_t0 = (int)(now_ms / 1000);
    f_model = model_charge;
    printf("  t min   pin mV  filt mV  period  pct  state\n");
    int last_inc = -1, last_band = -1, tfull = -1, t = 0;
    bool mono = true, mult = true; int exact[12] = { 0 }, seen[12] = { 0 };
    for (t = 1; t <= 6 * 3600 && tfull < 0; t++) {
        int prev = g_oem.batt_pct;
        tick(1);
        int band = model_charge() / 100 - 29; if (band > 11) band = 11; if (band < 5) band = 5;
        if (g_oem.batt_pct < prev) mono = false;
        if (g_oem.batt_pct > prev) {
            if (g_oem.batt_pct - prev != 1) mono = false;
            // an interval that lies in one voltage band is a whole number of 10 updates
            if (last_inc >= 0 && band == last_band && g_oem.batt_pct < 100) {
                int iv = t - last_inc, unit = 10 * charge_period((uint16_t)(2900 + 100 * band));
                seen[band]++;
                if (iv % unit) mult = false;
                if (iv == unit) exact[band]++;
            }
            last_inc = t; last_band = band;
        }
        if (band != last_band) last_inc = -1;
        if (g_oem.power_state == OEM_PWR_FULL) tfull = t;
        if (t % 600 == 0 || tfull >= 0)
            printf("  %5.1f   %5d   %5u    %3u    %3u    %u\n", t / 60.0, model_charge(), s.rtc_mv, g_oem.gauge_period, g_oem.batt_pct, g_oem.power_state);
    }
    check(mono, "percent never falls and moves one point at a time");
    check(mult, "every step inside one 100 mV band comes a whole number of 10-update slots after the last");
    printf("  steps at exactly 10 x period, per band (period s: 17 23 17 13 15 6):");
    for (int b = 6; b <= 11; b++) printf(" %d/%d", exact[b], seen[b]);
    printf("\n");
    check(exact[11] >= 10, "at >= 4000 mV the slew runs at 60 s per point (%d steps)", exact[11]);
    check(tfull > 0 && g_oem.batt_pct == 100, "full after %.0f min (table time 3500 -> 4200 mV at 2x: 127 min)", tfull / 60.0);
    check(f_led_calls == 1 && f_led[0][0] == 2 && f_led[0][1] == 0, "one LED call (2, ON, 4) at full");
    f_model = NULL;
}

static void t_record(void)
{
    printf("== record save / restore\n");
    nvs_put(-1, 0);
    start(3900);
    check(f_nvs[0] == 77, "first boot, no record: 77 %% saved");
    start(4000);
    check(g_oem.batt_pct == 77 && f_nvs[0] == 77, "next boot at 4000 mV (90 %%): the saved 77 %% wins (no rise off the charger)");
    s.rtc_pct = 55; s.rtc_mv = 3712; oem_gauge_save();
    check(f_nvs_len == 20 && f_nvs[0] == 55 && f_nvs[1] == (3712 >> 8) && f_nvs[2] == (3712 & 0xff), "oem_gauge_save writes { pct, mV hi, mV lo }");
    reboot(3700);
    tick(4);
    check(g_oem.batt_pct == 50 && s.rtc_pct == 50, "restored 55 %% / 3712 mV, then the first update drops to the voltage's 50 %% (one-shot 5)");
    nvs_put(150, 3712);
    start(3800);
    check(g_oem.batt_pct == 65, "a record with percent 150 is treated as no record");
    oem_gauge_reset_record();
    check(f_nvs[0] == 0xff && f_nvs[1] == 0xff && f_nvs[2] == 0xff, "oem_gauge_reset_record writes ff ff ff");
    start(3600);
    check(g_oem.batt_pct == 30, "after that a boot starts without history");

    nvs_put(77, 3900);
    start(3900);
    int w = f_nvs_writes;
    tick(3600);
    check(f_nvs_writes == w, "one hour at a steady 77 %%: no NVS write (%d at start-up)", w);
    oem_gauge_report_reset();
    tick(20);
    check(f_nvs_writes == w + 5, "after oem_gauge_report_reset the next 5 updates save");
}

static void t_invalid(void)
{
    printf("== unusable reading (0 mV: ADC or calibration failure)\n");
    nvs_put(-1, 0);
    reboot(0);
    check(g_oem.batt_pct == 50 && g_oem.batt_fault == 1, "no record: boot assumes 50 %%, fault flag set");
    tick(4);
    check(g_oem.gauge_inited && g_oem.batt_pct == 50 && f_allow == 1 && f_ui_last == 50, "start-up completes (charger detection can run), 50 %%, charging allowed");
    tick(600);
    check(g_oem.batt_pct == 50 && f_nvs_len == 0 && g_oem.batt_fault == 1, "10 min later still 50 %%, nothing written to NVS");
    f_mv = 3900; tick(2);
    check(g_oem.batt_pct == 77 && g_oem.batt_fault == 0 && f_nvs[0] == 77, "reading comes back at 3900 mV: taken as the first measurement, 77 %%");

    nvs_put(0, 3400);
    reboot(0);
    tick(10);
    check(g_oem.batt_pct == 50 && f_nvs[0] == 0, "saved 0 (lock) and no reading: 50 %%, not 0; the record keeps its 0");
    f_mv = 3500; tick(2);
    check(g_oem.batt_pct == 0, "reading back at 3500 mV: the lock applies again");

    nvs_put(63, 3790);
    reboot(0);
    tick(10);
    check(g_oem.batt_pct == 63 && f_nvs[0] == 63 && f_nvs_writes == 0, "saved 63 and no reading: 63 %%");

    nvs_put(77, 3900);
    start(3900);
    tick(10);
    int mv = s.rtc_mv;
    f_mv = 0; tick(300);
    check(g_oem.batt_pct == 77 && s.rtc_mv == mv && g_oem.batt_fault == 1, "reading lost while running: percent and filter frozen for 5 min");
    f_mv = 3900; tick(4);
    check(g_oem.batt_pct == 77 && g_oem.batt_fault == 0, "reading back: normal operation");
    f_mv = 1500; tick(60);
    check(g_oem.batt_pct == 77 && g_oem.batt_fault == 1, "1500 mV is not a battery voltage either: ignored");

    start(3900);
    app_attach(false);
    f_mv = 0; tick(120);
    check(g_oem.batt_pct == 77 && s.rtc_mv == 3900, "on the charger a 0 mV reading does not wrap the filter (stock: 65486 mV)");
}

static void t_thermal(void)
{
    printf("== thermal cut-off (0x42017e84)\n");
    nvs_put(77, 3900);
    start(3900);
    int calls = f_allow_calls;
    f_temp = 80; tick(5);
    check(f_allow_calls == calls, "off the charger: no check");
    app_attach(false); calls = f_allow_calls;
    f_temp = 72; tick(3);
    check(f_allow == 1 && f_allow_calls == calls, "72.0 C: still allowed (needs > 72)");
    f_temp = 72.5f; tick(3);
    check(f_allow == 0 && f_allow_calls == calls + 1, "72.5 C: charging blocked, once");
    f_temp = 67; tick(3);
    check(f_allow == 0, "67.0 C: still blocked (needs < 67)");
    f_temp = 66.9f; tick(3);
    check(f_allow == 1 && f_allow_calls == calls + 2, "66.9 C: allowed again");
    f_temp = 90; f_temp_ok = false; tick(3);
    check(f_allow == 1, "no temperature reading: the pin is left alone");
}

static void t_misc(void)
{
    printf("== plug-in that woke the brush: script 2 again on the third gauge tick\n");
    nvs_put(77, 3900);
    start(3900);
    app_attach(true);
    tick(2);
    check(f_led_calls == 0, "ticks 1, 2: nothing");
    tick(1);
    check(f_led_calls == 3 && f_led[0][0] == 0 && f_led[0][1] == 1 && f_led[0][2] == 4 &&
          f_led[1][0] == 1 && f_led[1][1] == 1 && f_led[2][0] == 2 && f_led[2][1] == 3 && f_led[2][2] == 2,
          "tick 3: (0,OFF,4) (1,OFF,4) (2,BREATHE,2)");
    tick(30);
    check(f_led_calls == 3, "not repeated");

    printf("== un-plug after 30 s at 3950 mV (charging period 15), battery then 3800 mV\n");
    start(3900);
    app_attach(false); f_mv = 3950; tick(30);
    app_detach(); f_mv = 3800;
    int posts = f_ui_posts, first = 0;
    for (int i = 1; i <= 51; i++) { tick(1); if (!first && f_ui_posts != posts) first = i; }
    int a = g_oem.batt_pct; tick(2);
    check(first == 51 && a == 77 && g_oem.batt_pct == 72,
          "first update %d s after un-plug and skipped (settle): %d %%; the next one, 2 s later, drops: %u %%", first, a, g_oem.batt_pct);

    printf("== sleep credits of the drop limiter (0x42017d54 / 0x42017da4)\n");
    nvs_put(90, 4000);
    start(4000);
    f_mv = 3800; tick(6);
    check(g_oem.batt_pct == 85, "one-shot 5 used: 85");
    now_ms += 56 * 1000; oem_gauge_sleep_enter();
    check(s.credit8 == 62 * 15000 / 36000, "sleep_enter after 62 s awake: credit8 = %u", s.credit8);
    now_ms += 3600 * 1000; oem_gauge_sleep_exit();
    check(s.credit16 == 550, "sleep_exit after 1 h asleep: credit16 = %u", s.credit16);
    tick(2);
    check(g_oem.batt_pct == 80, "next update may drop (25 + 550 + 2) / 100 = 5 points: %u", g_oem.batt_pct);
    tick(2);
    check(g_oem.batt_pct == 74, "credit16 is never cleared (stock quirk): (75 + 550 + 2) / 100 = 6 more on the following update: %u", g_oem.batt_pct);
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    t_table();
    t_startup();
    t_discharge();
    t_lock();
    t_charge_const();
    t_charge_full();
    t_record();
    t_invalid();
    t_thermal();
    t_misc();
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

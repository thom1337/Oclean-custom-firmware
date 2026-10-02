#include <string.h>
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_state.h"

// Battery gauge of the stock firmware (0x42017744 .. 0x42018250),
// re/spec/led_battery_charge.md section 5, and the charge thermal cut-off (6.5).
//
// The percentage is not a plain function of the voltage: the voltage is filtered,
// the percentage only falls while off the charger (rate limited) and only rises
// while charging (one point per ten updates, the update period following a
// charge-time table). g_oem.batt_pct is what the UI, the LEDs and the low-battery
// locks use; it is frozen during a brushing session because the app module does not
// call oem_gauge_tick() then.

#define REC_KEY        "brush_battery"
#define REC_LEN        20       // stock blob size; bytes 0..2 are used
#define COMP_MV        50       // 0x3fc9ab90 "buchang": subtracted while charging below 4.0 V
#define FALLBACK_PCT   50       // shown while there is no usable reading and no saved value

// 0x3c1192c8: state of charge in 0.01 % at 3000 mV + 100 mV * i
static const uint16_t soc_x100[13] = {
    0, 0, 0, 0, 0, 1000, 3000, 5000, 6500, 7700, 9000, 10000, 10000 };
// 0x3c1192e2: cumulative charge time in minutes at the same voltages
static const uint16_t chg_min[13] = {
    0, 0, 0, 0, 0, 36, 94, 172, 215, 241, 275, 285, 290 };

// ---- state ---------------------------------------------------------------------------
// One struct so the host test can put the module back to its power-on state. The
// initial values are the stock .data values (everything else starts at 0). The stock
// RTC copies of mV / percent are reloaded from NVS on every boot before they are
// used, so they need no RTC memory here.
typedef struct {
    uint16_t rtc_mv;        // RTC 0x50001016  filtered battery voltage
    uint8_t  rtc_pct;       // RTC 0x50001018  saved percent, 0xff = none
    uint32_t soc_x100;      // 0x3fca4b64      discharge latch
    uint8_t  first_done;    // 0x3fca4b69
    uint16_t per_cnt;       // 0x3fc9ab94      ticks since the last update
    uint8_t  settle;        // 0x3fc9ab92      updates since boot / plug / un-plug
    uint8_t  cnt_99;        // 0x3fca4b60      confirmations for 99 -> 100
    uint8_t  last_rep;      // 0x3fc9ab88      last percent saved / reported
    uint8_t  report_cnt;    // 0x3fc9ab8b
    uint8_t  therm;         // 0x3fc9ab8a      2 = charging allowed, 1 = cut
    uint8_t  extra;         // 0x3fc9ab8d      one-shot extra drop allowance
    uint8_t  fresh;         // 0x3fc9ab8e
    uint32_t t_mark;        // 0x3fca4b50      drop limiter time stamp, s
    uint32_t t_sleep;       // 0x3fca4b54
    uint16_t credit16;      // 0x3fca4b58
    uint8_t  credit8;       // 0x3fca4b5a
    bool     booted;        // oem_gauge_boot() ran
    bool     tick_seen;     // start-up sequence: the first tick was seen ...
    uint32_t tick0_ms;      // ... at this time
    bool     pending_first; // the first measurement had no usable reading yet
} gauge_t;
#define GAUGE_POWER_ON { .rtc_mv = 0xffff, .rtc_pct = 0xff, .per_cnt = 63, .settle = 2, \
                         .last_rep = 0xff, .report_cnt = 10, .therm = 2, .extra = 5, .fresh = 1 }
static gauge_t s = GAUGE_POWER_ON;

static bool mv_valid(int mv) { return mv >= OEM_BATT_MV_MIN_VALID; }

// ---- voltage -> percent --------------------------------------------------------------
// Common part of 0x42017744 / 0x4201783c. Stock indexes the table with
// (mv / 100) % 10 (+10 above 3999 mV) and reads past its end from 4200 mV on;
// here everything from 4100 mV up is 100 %.
static uint32_t soc_lookup_x100(uint32_t mv)
{
    uint32_t v = mv < 3000 ? 3000 : mv;
    if (v >= 4100) return 10000;
    uint32_t i = (v - 3000) / 100;
    return soc_x100[i] + (uint32_t)(soc_x100[i + 1] - soc_x100[i]) * (v - (i * 100 + 3000)) / 100;
}

// 0x4201783c
static uint8_t soc_raw(uint32_t mv)
{
    return (uint8_t)(soc_lookup_x100(mv) / 100);
}

// 0x42017744: clamps (3445 mV and below = 0 %, above 4109 mV = 100 %) and, off the
// charger, a latch that only lets the value fall.
static uint8_t soc_filtered(uint32_t mv)
{
    uint32_t val = soc_lookup_x100(mv);
    if (s.first_done && g_oem.power_state != OEM_PWR_BATTERY) {
        if (mv > 4109) return 100;
        if (mv <= 3445) return 0;
        return (uint8_t)((val < 10000 ? val : 10000) / 100);
    }
    if (!s.first_done || val < s.soc_x100) s.soc_x100 = val;
    if (s.soc_x100 > 10000) s.soc_x100 = 10000;
    if (mv > 4109)       s.soc_x100 = 10000;
    else if (mv <= 3445) s.soc_x100 = 0;
    return (uint8_t)(s.soc_x100 / 100);
}

// 0x42017ca0: seconds between gauge updates while charging. The percent rises one
// point per ten updates, so a point takes (minutes of the 100 mV segment * 60) /
// (percent points of the segment) seconds: 21, 17, 23, 17, 13, 15, 6.
static uint8_t charge_period(uint16_t avg_mv)
{
    int i = (int8_t)(avg_mv / 100 - 29);
    if (i > 11) i = 11;
    if (i < 5) i = 5;
    uint16_t dsoc = (uint16_t)(soc_x100[i] - soc_x100[i - 1]);
    if (dsoc == 0) dsoc = 1;
    uint8_t p = (uint8_t)(((int)(chg_min[i] - chg_min[i - 1]) * 600) / dsoc);
    return p ? p : 1;
}

// 0x42017de0: how many points the percent may fall at this update, off the charger.
// One point per 100 s of up-time since the last grant, plus credits for time spent
// asleep, plus a one-shot allowance (5 after boot, 3 or "unlimited" after a session).
static uint8_t drop_allowance(void)
{
    uint32_t now = hal_uptime_s();
    uint32_t acc = (uint32_t)s.credit8 + s.credit16;
    int32_t el = (int32_t)(now - s.t_mark) + (int32_t)acc;
    uint8_t d = 0;
    if (el > 99) {
        s.credit8 = (uint8_t)(acc % 100);
        d = (uint8_t)(el / 100);
        s.t_mark = now;
        s.fresh = 1;
    }
    if (s.fresh) s.fresh = 0;
    else s.extra = 0;
    return (uint8_t)(d + s.extra);
}

// ---- NVS record ----------------------------------------------------------------------
// "brush_battery": { percent (0xff = none), mV high byte, mV low byte, 17 unused }
static void record_load(void)                    // 0x42024940 as used by 0x420182d0 / 0x42017944
{
    uint8_t b[REC_LEN];
    s.rtc_pct = 0xff;                            // default record ff ff ff (0x3c1192fc)
    s.rtc_mv = 0xffff;
    if (hal_nvs_get(REC_KEY, b, sizeof b) < 3) return;
    // Stock trusts any stored value; a percent above 100 can only be garbage, so it
    // is taken as "no record".
    if (b[0] > 100) return;
    s.rtc_pct = b[0];
    s.rtc_mv = (uint16_t)(b[1] << 8 | b[2]);
}

static void record_write(uint8_t pct, uint16_t mv)   // 0x4202490c
{
    uint8_t b[REC_LEN] = { pct, (uint8_t)(mv >> 8), (uint8_t)mv };
    if (!hal_nvs_set(REC_KEY, b, sizeof b)) hal_log("gauge: battery record not saved");
}

// 0x42017990
void oem_gauge_save(void)
{
    record_write(s.rtc_pct, s.rtc_mv);
}

// 0x420179b8
void oem_gauge_reset_record(void)
{
    record_write(0xff, 0xffff);
}

// ---- hooks for the charge state machine, the session and the sleep code -------------
void oem_gauge_settle_reset(void)                // 0x420179e0
{
    s.settle = 0;
    s.cnt_99 = 0;
}

void oem_gauge_rearm(uint8_t n)                  // 0x420179f4
{
    s.per_cnt = (uint16_t)(g_oem.gauge_period - n);
}

void oem_gauge_session_done(uint16_t secs)       // 0x42017d28
{
    s.extra = secs < 16 ? 3 : 240;
    s.fresh = 1;
}

void oem_gauge_sleep_enter(void)                 // 0x42017d54
{
    uint32_t now = hal_uptime_s();
    s.t_sleep = now;
    s.credit8 = (uint8_t)(s.credit8 + (int32_t)((now - s.t_mark) * 15000) / 36000);
}

void oem_gauge_sleep_exit(void)                  // 0x42017da4
{
    uint32_t now = hal_uptime_s();
    s.t_mark = now;
    s.credit16 = (uint16_t)(s.credit16 + (int32_t)((now - s.t_sleep) * 5500) / 36000);
}

void oem_gauge_report_reset(void)                // 0x42017e74
{
    s.report_cnt = 0;
}

// ---- start-up ------------------------------------------------------------------------
// No usable reading: use the saved percent when there is one and it is not the 0 %
// lock, else a neutral value. Never 0: an unreadable battery must not send the brush
// down the "battery empty" path (see NOTES_led.md).
static uint8_t fallback_pct(void)
{
    return (s.rtc_pct != 0xff && s.rtc_pct != 0) ? s.rtc_pct : FALLBACK_PCT;
}

// brush_app start (0x4201831c): a first estimate until the real first measurement.
// Stock takes one ADC read here; this uses the two-read sample.
void oem_gauge_boot(void)
{
    s.booted = true;
    g_oem.plug_cnt = 100;                        // stock .data values
    g_oem.gauge_period = 64;
    g_oem.gauge_inited = 0;

    int mv = oem_batt_mv_now();
    record_load();
    if (mv_valid(mv)) {
        uint8_t pct = soc_raw((uint32_t)mv);
        if (s.rtc_pct == 0 && pct < 10) pct = 0; // still inside the 0 % lock
        g_oem.batt_pct = pct;
        g_oem.batt_fault = 0;
    } else {
        g_oem.batt_pct = fallback_pct();
        g_oem.batt_fault = 1;
        hal_log("gauge: no battery reading (%d mV), assuming %u %%", mv, g_oem.batt_pct);
    }
    g_oem.power_state = OEM_PWR_BATTERY;
}

// The part of 0x420178b8 that turns the first sample into a percentage.
static void first_sample_apply(uint32_t avg)
{
    s.first_done = 0;
    uint8_t pct = soc_filtered(avg);
    g_oem.batt_pct = pct;
    if (s.rtc_pct == 0xff) {                     // no history: take the measurement
        s.rtc_pct = pct;
        s.rtc_mv = (uint16_t)avg;
    } else if (s.rtc_pct == 0) {                 // 0 % lock: released from 40 % up
        if (pct > 39) s.rtc_mv = (uint16_t)avg;
        else { g_oem.batt_pct = 0; s.soc_x100 = 0; }
    } else {                                     // trust the saved value
        if (avg <= 3299) s.rtc_pct = 0;
        g_oem.batt_pct = s.rtc_pct;
        s.soc_x100 = (uint32_t)s.rtc_pct * 100;
    }
    s.first_done = 1;
    s.pending_first = false;
    g_oem.batt_fault = 0;
}

// 0x420178b8: first real measurement, with charging blocked so the cell is at rest.
static void first_measure(void)
{
    oem_charge_allow(false);
    g_oem.power_state = OEM_PWR_BATTERY;
    int avg = oem_batt_mv_now();
    if (mv_valid(avg)) {
        first_sample_apply((uint32_t)avg);
    } else {
        // Keep the saved record untouched; the first usable sample is taken as the
        // first measurement later (oem_gauge_tick).
        s.pending_first = true;
        s.first_done = 1;
        g_oem.batt_fault = 1;
        g_oem.batt_pct = fallback_pct();
        s.soc_x100 = (uint32_t)g_oem.batt_pct * 100;
        oem_ui_post(0x2a, &g_oem.batt_pct, 1);
        hal_log("gauge: first measurement failed (%d mV), assuming %u %%", avg, g_oem.batt_pct);
    }
    oem_charge_allow(true);
}

// ---- periodic gauge ------------------------------------------------------------------
// The "update" branch of batt_tick, reached every 2 s off the charger and every
// g_oem.gauge_period seconds on it. pct is the percentage of the filtered voltage.
static void gauge_update(uint8_t pct)
{
    uint8_t before = g_oem.batt_pct;
    if (g_oem.power_state == OEM_PWR_CHARGING) {         // slow upward slew only
        if (s.settle < 200) s.settle++;
        if (++g_oem.slew_cnt > 9) {                      // every 10th update
            uint8_t b = g_oem.batt_pct;
            if (b < pct) {
                if (b < 99) g_oem.batt_pct = b + 1;
                else if (pct > 99 && ++s.cnt_99 > 2) g_oem.batt_pct = b + 1;   // 99 -> 100
            }
            g_oem.slew_cnt = 0;
        }
        s.soc_x100 = (uint32_t)g_oem.batt_pct * 100;
    } else {                                             // off the charger, or full
        if (s.settle < 200) s.settle++;
        if (s.settle > 1) {
            uint8_t b = g_oem.batt_pct;
            if (pct < b) {
                if (pct < 10) {                          // nearly empty: fall fast
                    uint8_t d = (uint8_t)((b - pct) / 3);
                    if (d == 0) d = 1;
                    g_oem.batt_pct = (d < b) ? (uint8_t)(b - d) : pct;
                } else {
                    uint8_t d = drop_allowance();
                    if (d) g_oem.batt_pct = (b - pct < d) ? pct : (uint8_t)(b - d);
                }
            }
            uint8_t p2 = soc_raw(s.rtc_mv);
            if (p2 > 50 && g_oem.batt_pct == 0) {        // leave the 0 % lock
                g_oem.batt_pct = p2;
                s.soc_x100 = (uint32_t)p2 * 100;
            }
        }
        if (g_oem.power_state == OEM_PWR_FULL) { g_oem.batt_pct = 100; s.soc_x100 = 10000; }
    }
    s.rtc_pct = g_oem.batt_pct;
    oem_ui_post(0x2a, &g_oem.batt_pct, 1);               // 0x42021704
    if (s.last_rep != g_oem.batt_pct || s.report_cnt < 5) {
        s.last_rep = g_oem.batt_pct;
        s.report_cnt++;
        // (stock also sends the BLE battery notification 0x4200e6c8 here)
        oem_gauge_save();
    }
    g_oem.batt_mv = s.rtc_mv;
    if (before != g_oem.batt_pct)
        hal_log("gauge: %u %% (%u mV, target %u %%)", g_oem.batt_pct, s.rtc_mv, pct);
}

// 0x42017e84: charging is blocked above 72 C and allowed again below 67 C (IMU die
// temperature). Runs at the end of every gauge tick, only on the charger.
void oem_charge_thermal_check(void)
{
    if (g_oem.power_state == OEM_PWR_BATTERY) return;
    float t;
    if (!oem_imu_temp(&t)) return;               // no reading: leave the pin as it is
    if (t > 72.0f && s.therm == 2) {
        hal_log("gauge: %d C, charging blocked", (int)t);
        oem_charge_allow(false);
        s.therm = 1;
    }
    if (t < 67.0f && s.therm == 1) {
        hal_log("gauge: %d C, charging allowed", (int)t);
        oem_charge_allow(true);
        s.therm = 2;
    }
}

// batt_tick 0x42017f08, once per second while not brushing.
void oem_gauge_tick(void)
{
    if (!s.booted) oem_gauge_boot();

    // Third tick after a plug-in that woke the brush: play the "charger" script again.
    if (g_oem.plug_cnt <= 9) g_oem.plug_cnt++;
    if (g_oem.plug_cnt == 3) {
        oem_led_set(0, 1, 4);
        oem_led_set(1, 1, 4);
        oem_led_set(2, 3, 2);
    }

    if (!g_oem.gauge_inited) {
        // Stock keys this on the task's seconds counter: nothing at 0 and 1, charging
        // blocked at 2, measurement at 3. The same on a clock started by the first
        // tick, so it also holds if the caller ticks faster than 1 Hz (stock does,
        // every 10 ms, while brushing on the dock before the first measurement).
        uint32_t now = hal_ms();
        if (!s.tick_seen) { s.tick_seen = true; s.tick0_ms = now; }
        uint32_t el = now - s.tick0_ms;
        if (el < 1500) return;
        oem_charge_allow(false);
        if (el < 2500) return;
        s.t_mark = hal_uptime_s();               // 0x42017d94
        record_load();                           // 0x42017944
        first_measure();
        g_oem.gauge_inited = 1;
    }

    uint8_t st = g_oem.power_state;
    if (st == OEM_PWR_CHARGING || st == OEM_PWR_FULL) {
        if (s.per_cnt >= g_oem.gauge_period) s.per_cnt = 0;
        s.per_cnt++;
    } else if (st == OEM_PWR_BATTERY) {
        if (s.per_cnt >= g_oem.gauge_period) s.per_cnt = (uint16_t)(g_oem.gauge_period - 2);
        s.per_cnt++;
    }

    int avg = oem_batt_mv_now();
    if (mv_valid(avg)) {
        if (s.pending_first) {
            hal_log("gauge: battery reading back (%d mV)", avg);
            first_sample_apply((uint32_t)avg);
        } else if (g_oem.batt_fault) {
            g_oem.batt_fault = 0;
            hal_log("gauge: battery reading back (%d mV)", avg);
        }
        uint16_t mv = (uint16_t)avg;
        if (st == OEM_PWR_BATTERY) {
            g_oem.gauge_period = 64;
        } else {
            g_oem.gauge_period = charge_period((uint16_t)avg);
            if (avg <= 3999) mv -= COMP_MV;      // the charge current lifts the terminal voltage
            if (g_oem.asleep) mv -= 18;          // less load with the screen off
        }
        // Stock computes this in double precision (constants 0x3fd3333333333333 and
        // 0x3fe6666666666666) and truncates; kept as is so the values match.
        s.rtc_mv = (uint16_t)(s.rtc_mv * 0.3 + mv * 0.7);
        uint8_t pct = soc_filtered(s.rtc_mv);
        if (s.per_cnt == g_oem.gauge_period) gauge_update(pct);
    } else if (!g_oem.batt_fault) {
        // A reading that is not a battery voltage is skipped: the filter and the
        // percentage keep their values. (Stock would filter the 0 mV in and fall to
        // 0 % within seconds, or wrap to 65486 mV on the charger.)
        g_oem.batt_fault = 1;
        hal_log("gauge: no battery reading (%d mV), keeping %u %%", avg, g_oem.batt_pct);
    }

    // "Full" is decided here only: 100 % held for six ticks while charging.
    if (g_oem.batt_pct > 99 && g_oem.power_state == OEM_PWR_CHARGING) {
        if (g_oem.full_cnt < 10) g_oem.full_cnt++;
        if (g_oem.full_cnt > 5) {
            g_oem.power_state = OEM_PWR_FULL;
            oem_led_set(2, 0, 4);                // charge light steady
            hal_log("gauge: battery full");
        }
    }
    oem_charge_thermal_check();
}

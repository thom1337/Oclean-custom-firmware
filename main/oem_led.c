#include <string.h>
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_state.h"

// LED pattern module of the stock firmware (0x4201d9a4 .. 0x4201e0c3),
// re/spec/led_battery_charge.md section 2.
//
// Logical LEDs: 0..3 = indicator LEDs (HAL id 1..4), 4 = LCD backlight (HAL id 5),
// 5 = unused "alternate LED3 / LED4" pseudo LED. States: 0 on, 1 off, 2 blink,
// 3 breathe. A request either takes effect at once (anim 4) or after one of four
// scripts (anim 0..3); while a script runs every other request is dropped.
// Everything here runs in the main task with the core lock held; oem_led_tick() is
// called every 10 ms and uses integer arithmetic only.

enum { ST_ON = 0, ST_OFF = 1, ST_BLINK = 2, ST_BREATHE = 3 };
#define ANIM_IDLE     4
#define STEP_INSTANT  60000u

// ---- scripts (stock .data tables, verified against re/seg1_3fc99e00.bin) ----------
typedef struct {
    uint16_t start_ms, end_ms;
    uint8_t  dir;            // 0 = fade out, 1 = fade in
    uint8_t  hal_led;        // 1..5
    uint8_t  skip_if_done;   // 1: do not start when the LED is already there
} led_step_t;
typedef struct { uint8_t count; led_step_t e[12]; } led_script_t;   // stock: 20 slots, the rest zero

// anim 0 @0x3fc9ae44 — wake up
static const led_script_t SCRIPT0 = { 10, {
    {0, 0, 0, 1, 0}, {0, 0, 0, 2, 0}, {0, 0, 0, 3, 0}, {0, 0, 0, 5, 0},   // instant off
    {10, 500, 1, 5, 1},                                                   // backlight soft start
    {700, 1000, 1, 1, 0}, {1000, 1300, 1, 2, 0}, {1300, 1700, 1, 3, 0},
    {1700, 2000, 0, 3, 0}, {2000, 2300, 0, 1, 0} } };
// anim 1, Wi-Fi not connected @0x3fc9ada2 — chase 1 -> 2 -> 3 twice, LED1 off at the end
static const led_script_t SCRIPT1 = { 12, {
    {0, 300, 1, 1, 0}, {200, 300, 0, 2, 0}, {300, 600, 1, 2, 0}, {600, 1000, 1, 3, 0},
    {900, 1000, 0, 1, 0}, {1000, 1300, 1, 1, 0}, {1200, 1300, 0, 2, 0}, {1300, 1600, 1, 2, 0},
    {1500, 1600, 0, 3, 0}, {1600, 2000, 1, 3, 0}, {2200, 2300, 0, 3, 0}, {2200, 2300, 0, 1, 0} } };
// anim 1, Wi-Fi connected @0x3fc9ad00 — the same chase, LED1 stays lit
static const led_script_t SCRIPT1B = { 11, {
    {0, 300, 1, 1, 0}, {200, 300, 0, 2, 0}, {300, 600, 1, 2, 0}, {600, 1000, 1, 3, 0},
    {900, 1000, 0, 1, 0}, {1000, 1300, 1, 1, 0}, {1200, 1300, 0, 2, 0}, {1300, 1600, 1, 2, 0},
    {1500, 1600, 0, 3, 0}, {1600, 2000, 1, 3, 0}, {2200, 2300, 0, 3, 0} } };
// anim 2 @0x3fc9ac5e — put on the charger
static const led_script_t SCRIPT2 = { 11, {
    {0, 100, 0, 1, 0}, {0, 100, 0, 2, 0}, {0, 100, 0, 3, 0}, {0, 100, 0, 5, 0},
    {100, 400, 1, 3, 0}, {400, 700, 1, 2, 0}, {700, 1000, 1, 1, 0}, {1000, 1500, 1, 5, 0},
    {1700, 2000, 0, 3, 0}, {2000, 2300, 0, 2, 0}, {2300, 2600, 0, 1, 0} } };
// anim 3 @0x3fc9abbc — going to sleep: everything fades out in 0.5 s
static const led_script_t SCRIPT3 = { 4, {
    {0, 500, 0, 1, 0}, {0, 500, 0, 2, 0}, {0, 500, 0, 3, 0}, {0, 500, 0, 5, 0} } };

// ---- state (initial values are the stock .data / .bss values) ---------------------
static uint8_t  s_state[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };  // 0x3fc9abb9 .. 0x3fc9abb4
static int      s_anim = ANIM_IDLE;         // 0x3fc9aba8
static const led_script_t *s_script;        // 0x3fca4ef8
static uint16_t s_time;                     // 0x3fca4efc  script clock, ms
static uint8_t  s_pend_led, s_pend_state;   // 0x3fca4f0c, 0x3fca4f08

typedef struct { uint8_t active, led, dir, flag; uint16_t step, cur; } slot_t;
static slot_t   s_slots[5];                 // 0x3fca4ec4

static int32_t  s_breath = 8191;            // 0x3fc9abb0  breathing triangle
static uint8_t  s_breath_down;              // 0x3fca4f04
static uint8_t  s_cnt, s_blink, s_alt;      // 0x3fca4ec2, 0x3fca4ec1, 0x3fca4ec0
static uint8_t  s_wifi_enable = 1;          // 0x3fc9abac
static int32_t  s_wifi_level;               // 0x3fca4f00
static uint8_t  s_wifi_dir;                 // 0x3fca4efe

static bool s_hw_up;                        // oem_led_init() ran once since reset

// ---- state setter 0x4201d9a4 -------------------------------------------------------
static void led_apply_state(unsigned led, unsigned state)
{
    if (led > 5) return;
    if (led == 5) {                         // pseudo LED: breathe on LED3 / LED4 alternately
        if (s_state[5] == state) return;
        s_state[5] = (uint8_t)state;
        if (s_state[5] == ST_OFF || s_state[5] == ST_BREATHE) {
            hal_led_set(3, 0);
            hal_led_set(4, 0);
        }
        if (s_state[5] == ST_BREATHE) { s_breath_down = 0; s_breath = 0; }
        return;
    }
    // A repeated request does nothing, except ON on LED 2 (the charge light), which is
    // always written again: that is what re-lights it after led_init on the charger.
    if (s_state[led] == state && !(led == 2 && state == ST_ON)) return;
    s_state[led] = (uint8_t)state;
    if (state == ST_ON)        hal_led_set((int)led + 1, led == 0 ? 4000 : 8191);
    else if (state <= ST_BREATHE) hal_led_set((int)led + 1, 0);   // off; blink / breathe start dark
    // any other value is stored and nothing is written
}

void oem_led_init(void)
{
    hal_led_pins_release(!s_hw_up);
    s_hw_up = true;
    hal_led_hw_init();
}

void oem_led_park(void)
{
    hal_led_pins_park();
}

// Idle on the charger (0x4201d2ea / 0x4201d51a): the sleep parking holds every LED
// pin; release GPIO19 only, restart LEDC and put the charge light back.
void oem_led_reinit_charge_light(void)
{
    hal_led_pin_release_charge();
    hal_led_hw_init();
    oem_led_set(2, g_oem.power_state == OEM_PWR_FULL ? ST_ON : ST_BREATHE, ANIM_IDLE);
}

// 0x4201dcc8
void oem_led_set(int led, int state, int anim)
{
    if (s_anim != ANIM_IDLE) return;        // a request during a script is lost
    if (anim == ANIM_IDLE) { led_apply_state((unsigned)led, (unsigned)state); return; }

    const led_script_t *sc;
    switch (anim) {
    case 0: sc = &SCRIPT0; break;
    case 1: sc = (g_oem.wifi_status == 1) ? &SCRIPT1B : &SCRIPT1; break;
    case 2: sc = &SCRIPT2; break;
    case 3: sc = &SCRIPT3; break;
    default: return;   // stock would run on with a stale script pointer; no caller does this
    }
    s_anim = anim;
    s_pend_led = (uint8_t)led;
    s_pend_state = (uint8_t)state;
    s_script = sc;
    s_time = 0;
    memset(s_slots, 0, sizeof s_slots);
}

// 0x4201dd48
void oem_led_all(int state)
{
    oem_led_set(0, state, ANIM_IDLE);
    oem_led_set(1, state, ANIM_IDLE);
    oem_led_set(2, state, ANIM_IDLE);
    oem_led_set(3, state, ANIM_IDLE);
}

// 0x4201dcbc: the pending state of the aborted script is not applied
void oem_led_abort_script(void)
{
    s_anim = ANIM_IDLE;
}

// set_led_light_level called directly (0x4201dacc: LED2 forced dark 4 s after plug-in)
void oem_led_level(int hal_id, int level)
{
    hal_led_set(hal_id, (uint32_t)level);
}

// ---- fade engine -------------------------------------------------------------------
// 0x4201dadc: with skip_if_done, a fade towards where the LED already is does not start
static bool step_wanted(uint8_t led, uint8_t dir, uint8_t flag)
{
    if (flag != 1) return true;
    uint32_t lv = hal_led_get(led);
    if (lv >= hal_led_max[led] && dir == 1) return false;
    return !(lv == 0 && dir == 0);
}

// 0x4201db20, t = 0, 10, 20, ... up to and including the last entry's end
static void led_script_step(uint16_t t)
{
    for (unsigned i = 0; i < s_script->count; i++) {        // start the entries due now
        const led_step_t *e = &s_script->e[i];
        if (e->start_ms != t || !step_wanted(e->hal_led, e->dir, e->skip_if_done)) continue;
        slot_t *s = NULL;
        for (int k = 0; k < 5; k++) if (!s_slots[k].active) { s = &s_slots[k]; break; }
        if (!s) continue;
        s->active = 1;
        s->led = e->hal_led;
        s->dir = e->dir;
        s->flag = e->skip_if_done;
        s->cur = (uint16_t)hal_led_get(e->hal_led);
        int dur = e->end_ms - e->start_ms;
        s->step = (dur > 10) ? (uint16_t)(hal_led_max[s->led] / (uint32_t)(dur / 10)) : STEP_INSTANT;
        hal_led_process(s->led, s->cur);
    }
    for (slot_t *s = s_slots; s < s_slots + 5; s++) {       // advance every running fade
        if (s->active != 1) continue;
        if (s->step == STEP_INSTANT) {
            if (s->dir == 0)      hal_led_process(s->led, 0);
            else if (s->dir == 1) hal_led_process(s->led, hal_led_max[s->led] & 0xffff);
            s->cur = 0;
            s->active = 0;
        } else if (s->dir == 0) {
            int16_t v = (int16_t)(s->cur - s->step);
            if (v < 0) { s->cur = 0; s->active = 0; }
            else s->cur = (uint16_t)v;
            hal_led_process(s->led, s->cur);
        } else if (s->dir == 1) {
            int16_t c = (int16_t)s->cur;
            int16_t v = (int16_t)(s->cur + s->step);
            // The backlight ignores its step: +1 per tick up to 20 (soft start), then +199.
            if (s->led == 5) v = (int16_t)(c < 20 ? c + 1 : c + 199);
            s->cur = (uint16_t)v;
            if ((uint32_t)(int32_t)v > hal_led_max[s->led]) {
                s->cur = (uint16_t)hal_led_max[s->led];
                s->active = 0;
            }
            hal_led_process(s->led, s->cur);
        }
    }
}

// ---- Wi-Fi light (HAL LED1) 0x4201dd74 ----------------------------------------------
// mode 0: triangle 0 <-> max (provisioned, not connected); 1: ramp up and hold
// (connected); 2: ramp down and hold (Wi-Fi off, or on the charger).
static void wifi_led_ramp(int32_t max, int32_t step, uint8_t mode)
{
    int32_t v = s_wifi_level;
    if (v >= max) {
        if (mode == 0 || mode == 2) { s_wifi_dir = 1; s_wifi_level = v - step; }   // turn around
        else if (mode == 1)         { s_wifi_level = max; }                        // hold
        return;
    }
    if (v <= 0) {
        if (mode < 2)       { s_wifi_dir = 0; s_wifi_level = v + step; }           // start rising
        else if (mode == 2) { s_wifi_level = 0; }                                  // stay off
        return;
    }
    if (s_wifi_dir == 0)      s_wifi_level = v + step;
    else if (s_wifi_dir == 1) s_wifi_level = v - step;
    hal_led_process(1, s_wifi_level <= 50 ? 0 : (uint32_t)s_wifi_level & 0xffff);
}

// ---- steady tick 0x4201ddf8 (no script running) --------------------------------------
static void led_steady_tick(void)
{
    s_cnt++;
    if (!g_oem.show_mode) {                 // breathing triangle -300 .. 10050, +-50 per tick: 4.15 s
        if (s_breath_down) {
            s_breath -= 50;
            if (s_breath >= -300) goto blink;
            s_breath_down = 0;
        }
        s_breath += 50;
        if (s_breath > 10000) s_breath_down = 1;
    } else {                                // shop demo mode: 0 .. 8191 in steps of 400
        if (s_breath_down) {
            s_breath -= 400;
            if (s_breath >= 0) goto blink;
            s_breath_down = 0;
            s_alt ^= 1;
        }
        s_breath += 400;
        if (s_breath > 8191) s_breath_down = 1;
    }
blink:
    if ((uint8_t)(s_cnt * 225u) < 8) s_blink ^= 1;          // every 33rd tick (330 ms)

    if (g_oem.batt_pct == 0 && g_oem.power_state == OEM_PWR_BATTERY) {
        // empty battery off the charger: every indicator is forced dark
        hal_led_process(1, 0);
        hal_led_process(2, 0);
        hal_led_process(3, 0);
        hal_led_process(4, 0);
    } else if (g_oem.dev_mode == 2) {
        wifi_led_ramp(4000, 40, 1);
    } else if (s_wifi_enable == 1) {
        uint8_t mode = 2;
        if (g_oem.sys[0x0c] == 2 && g_oem.wifi_status != 2 && g_oem.power_state == OEM_PWR_BATTERY)
            mode = (g_oem.wifi_status == 1) ? 1 : 0;
        wifi_led_ramp(4000, 40, mode);
    }

    uint32_t b = s_breath > 8191 ? 8191u : (s_breath < 0 ? 0u : (uint32_t)s_breath);
    for (int idx = 0; idx <= 3; idx++) {
        // Both write through the "process" call, which drops a level above the LED's
        // limit: on LED1..3 (limit 4000) breathing plateaus at 4000 and blink (8191)
        // never lights the LED. Stock only ever blinks LED4.
        if (s_state[idx] == ST_BLINK)        hal_led_process(idx + 1, s_blink ? 8191 : 0);
        else if (s_state[idx] == ST_BREATHE) hal_led_process(idx + 1, b);
    }
    if (s_state[5] == ST_BREATHE) hal_led_process(s_alt ? 4 : 3, b);
}

// 0x4201e03c, every 10 ms
void oem_led_tick(void)
{
    if (s_anim == ANIM_IDLE) { led_steady_tick(); return; }

    if (s_time > s_script->e[s_script->count - 1].end_ms) {   // script finished
        if (s_anim == 3) s_wifi_enable = 0;   // after the sleep script the Wi-Fi light stays off
        s_anim = ANIM_IDLE;
        s_wifi_level = 0;
        if (!(g_oem.batt_pct == 0 && g_oem.power_state == OEM_PWR_BATTERY))
            led_apply_state(s_pend_led, s_pend_state);
        s_breath_down = 0;
        s_breath = 0;
    } else {
        s_wifi_enable = 1;
        led_script_step(s_time);
        s_time += 10;
    }
}

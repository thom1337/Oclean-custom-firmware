// UI of the OEM-behaviour port: the stock "UI_TASK" side (re/spec/ui_flow.md).
//
// Message queue, the handlers for commands 0, 1, 7..12 and screens 70..120, the 50 ms
// blink / animation tick, the page model (mode ring, side pages, lock popup) and the
// composition of the current screen. Pictures are drawn by ui_render.c; this file only
// decides which elements are visible and which variant (digit, frame, language) each
// one shows. Element numbers are indices into STOCK_ELEMS[] (the [n] of the spec).
//
// Everything here runs with the core lock held. oem_ui_handle() is the UI task's loop
// body; oem_ui_post() and oem_ui_lock_button() are called from the main task.

#include <string.h>
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_state.h"
#include "stock_ui_tables.h"
#include "ui_render.h"

// ---- state --------------------------------------------------------------------------

typedef struct { uint16_t id; uint8_t p[10]; } ui_msg_t;          // stock: 14 bytes
#define UI_Q_LEN 10
static ui_msg_t s_q[UI_Q_LEN];             // 0x3fca5012
static uint8_t  s_q_count;                 // 0x3fca509e

static uint8_t  s_ui_now;                  // 0x3fc9c021  UI-side current screen
static bool     s_enabled;                 // frame object +0x14
static const uint8_t *s_list;              // 0x3fca5110  element list of the selected screen
static bool     s_redraw;                  // header flags bit0: full redraw requested
static bool     s_lit;                     // backlight lit at the last pass (not stock, see oem_ui_handle)
static uint8_t  s_dirty[STOCK_ELEM_COUNT]; // element flags bit0

static uint8_t  s_saved_mode_page;         // 0x3fc9aefc  last mode page 76..81 shown
static uint8_t  s_left, s_right;           // 0x3fc9aef9 / 0x3fc9aefa  swipe targets
static uint8_t  s_sub_mode;                // 0x3fca4fec  name variant of screen 81
static uint8_t  s_ui_running;              // 0x3fca50d6  payload[0] of the last 82 / 101
static uint8_t  s_fact[4];                 // 0x3fca50d7  OK / NG flags of the calibration screens

static uint8_t  s_phase, s_c100, s_c250;   // 0x3fca4fd1 / 0x3fca4fd0 / 0x3fca4fcf
static int      s_icon_cnt;                // 0x3fc9aef0  100 per tick, 100..2000
static uint8_t  s_icon_in[4];              // 0x3fc9aef4  inputs of the last status-icon pass

static uint8_t  s_clk_frame, s_clk_t;      // 0x3fc9af08 / 0x3fca4ff1  "loading" animation of page 96
static uint8_t  s_clk_min;                 // 0x3fc9af09  minute last drawn (0xFF = redraw)
static uint8_t  s_clk_applied;             // clock_mode the page 96 elements were laid out for

static bool     s_popup_run;               // lock popup pending (stock: task "show_lock_ui_task")
static uint32_t s_popup_until;

// ---- element helpers ----------------------------------------------------------------

// set_visible 0x421022f4: a change of visibility marks the element dirty.
static void el_show(int el, bool v)
{
    if (ui_el_visible(el) == v) return;
    ui_el_show(el, v);
    s_dirty[el] = 1;
}

// invalidate 0x42102330
static void el_inval(int el) { s_dirty[el] = 1; }

// Writing a variant does not mark the element dirty in stock either: the new picture
// appears with the next redraw of the screen.
static void el_var(int el, uint8_t v) { ui_el_set_var(el, v); }

static void el_x(int el, int x) { ui_el_move(el, x, STOCK_ELEMS[el].y); }

static void el_show_set(const uint8_t *els, size_t n, bool v)
{
    for (size_t i = 0; i < n; i++) el_show(els[i], v);
}
#define SHOW(v, ...) el_show_set((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}), (v))

static uint8_t lang(void) { return g_oem.lang; }

static bool is_mode_page(uint8_t s) { return s >= 76 && s <= 81; }          // 0x4210225c

// 0x420197e8: a session exists and the motor is stopped
static bool paused(void) { return g_oem.session_active && g_oem.stop_delay < 0x1f; }

// A version string digit ("a.b.c.d", characters 0, 2, 4, 6). Stock subtracts '0'
// unchecked; anything else would select a picture outside the digit set.
static uint8_t ver_digit(const char *s, int i)
{
    for (int k = 0; k < i; k++) if (!s[k]) return 0;
    return (s[i] >= '0' && s[i] <= '9') ? (uint8_t)(s[i] - '0') : 0;
}

// ---- message queue (0x4201f840 / 0x4201f86c) ----------------------------------------

void oem_ui_post(uint16_t id, const void *payload, int len)
{
    ui_msg_t m = { .id = id };
    if (payload && len > 0) memcpy(m.p, payload, len > (int)sizeof m.p ? sizeof m.p : (size_t)len);
    if (s_q_count < UI_Q_LEN) s_q[s_q_count++] = m;
    else hal_log("ui: queue full, message %u dropped", id);    // stock drops it silently
    hal_ui_event_post(OEM_UIEV_MSG);
}

static bool q_pop(ui_msg_t *m)
{
    if (!s_q_count) return false;
    *m = s_q[0];
    s_q_count--;
    memmove(&s_q[0], &s_q[1], s_q_count * sizeof s_q[0]);
    return true;
}

// ---- small shared layouts -----------------------------------------------------------

// 0x42020644: the factory-mode banner, never visible in normal use
static void dev_overlay(void)
{
    el_show(170, (g_oem.dev_mode == 2 || g_oem.ui_mode == 0xd7) && !g_oem.show_mode);
}

// 0x420207c8: language of the mode names; on screen 81 one of four names
static void mode_page_update(void)
{
    uint8_t l = lang();
    el_var(163, l); el_var(160, l); el_var(158, l); el_var(156, l);
    dev_overlay();
    el_var(149, l);
    // 0x42020674
    s_sub_mode = g_oem.profile[2] ? 4
               : (g_oem.profile[1] == 1 && (g_oem.tod_sub == 2 || g_oem.tod_sub == 3)) ? g_oem.tod_sub : 1;
    int name = s_sub_mode < 2 ? 154 : s_sub_mode == 2 ? 152 : s_sub_mode == 3 ? 153 : 151;
    el_show(154, name == 154); el_show(153, name == 153);
    el_show(152, name == 152); el_show(151, name == 151);
    el_var(name, l);
}

// Countdown "M:SS" of screens 82 / 99 / 101 / 102, with the stock byte arithmetic
static void countdown_digits(uint16_t rem)
{
    uint8_t m = (uint8_t)(rem / 60);
    uint8_t s = (uint8_t)(rem - m * 60);
    el_var(116, m); el_var(114, s / 10); el_var(113, s % 10);
}

// 0x420224a8 + 0x4201ff94: zone overlays of screens 100 / 101. A zone brushed for
// 5 s or more shows variant 0, 1..4 s variant 1, an untouched zone is hidden.
static void zone_overlays(void)
{
    static const uint8_t ZONE_ELEM[12] = { 15, 14, 9, 8, 10, 11, 5, 4, 13, 12, 7, 6 };   // 0x4201a8c0
    for (int i = 0; i < 12; i++) {
        uint16_t t = g_oem.zone_s[i];
        el_show(ZONE_ELEM[i], t != 0);
        if (t) el_var(ZONE_ELEM[i], t <= 4);
    }
}

// 0x42021284 / 0x420212e0: factory aging overlay of screens 70 / 117 (battery percent;
// the aging pass counter is not part of the port and shows 00)
static void aging_overlay(bool on)
{
    SHOW(on, 178, 177, 176, 175, 174, 173, 172);
}

static void aging_digits(void)
{
    uint8_t p = g_oem.batt_pct;
    el_var(176, p < 100 ? p / 10 : 9); el_var(175, p < 100 ? p % 10 : 9);
    el_var(173, 0); el_var(172, 0);
}

// 0x42021918: battery picture and percent digits of screens 93 / 120. Runs from the
// blink tick every 250 ms, but only while on the charger.
static void battery_layout(void)
{
    if (g_oem.power_state == OEM_PWR_BATTERY) return;
    uint8_t p = g_oem.batt_pct;
    if (p > 99) {
        el_inval(148); el_var(148, 6);
        el_var(146, 1); el_var(145, 0); el_var(144, 0);
        SHOW(false, 139, 138, 142, 141, 140);
        SHOW(true, 146, 145, 144, 143);
        el_show(147, false);                       // no charging strip when full
        return;
    }
    if (p < 16) {
        el_var(148, 0);
        el_inval(148); el_show(147, true);
        if (p < 10) {
            el_var(139, p);
            SHOW(false, 142, 141, 140, 146, 145, 144, 143);
            SHOW(true, 139, 138);
            return;
        }
    } else {
        if (p <= 30)      el_var(148, 1);
        else if (p <= 44) el_var(148, 2);
        else if (p >= 46 && p <= 60) el_var(148, 3);
        else if (p >= 61 && p <= 80) el_var(148, 4);
        else if (p >= 81) el_var(148, 5);          // 45 keeps the previous picture
        el_inval(148); el_show(147, true);
    }
    el_var(142, p / 10); el_var(141, p % 10);
    SHOW(false, 146, 145, 144, 143, 139, 138);
    SHOW(true, 142, 141, 140);
}

// Score badge and digits of screen 100 (and 83)
static void score_digits(uint8_t score, bool high)
{
    if (high) { el_var(96, score / 10); el_var(95, score % 10); }
    else      { el_var(92, score / 10); el_var(91, score % 10); }
}

// 0x4201fde0: greeting screen 85
static void greeting(uint8_t id)
{
    uint8_t art, text;
    switch (id) {
    case 1:  art = 2; text = 0;  break;      // 1 Jan
    case 2:  art = 3; text = 17; break;      // 14 Feb
    case 3:  art = 5; text = 51; break;      // 20 Mar
    case 5:  art = 1; text = 34; break;      // 1 May
    case 10: art = 4; text = 68; break;      // 5 Oct
    case 4:  art = 0; text = 85; break;      // birthday
    default: return;
    }
    el_var(104, art);
    SHOW(id == 4, 103, 102, 101, 100, 99);
    if (id == 4) {                           // date "M-D" above the text
        uint8_t m = g_oem.birthday_month, d = g_oem.birthday_day;
        if (m < 10) {
            el_var(103, m);
            if (d < 10) {
                el_var(100, d);
                el_x(103, 19); el_x(100, 47); el_x(101, 33);
                SHOW(false, 102, 99);
            } else {
                el_var(100, d / 10); el_var(99, d % 10);
                el_x(103, 12); el_x(101, 26); el_x(100, 40); el_x(99, 54);
                el_show(102, false);
            }
        } else {
            el_var(103, m / 10); el_var(102, m % 10);
            if (d < 10) {
                el_var(100, d);
                el_x(103, 12); el_x(102, 26); el_x(101, 40); el_x(100, 54);
                el_show(99, false);
            } else {
                el_var(100, d / 10); el_var(99, d % 10);
                el_x(103, 4); el_x(102, 19); el_x(101, 33); el_x(100, 47); el_x(99, 61);
            }
        }
    }
    el_var(98, (uint8_t)(lang() + text));
}

// ---- clock / weather page 96 --------------------------------------------------------

// variant added to picture 843: digits 0..9, [10] separator, [11] degree, [12] minus, [13] colon
static const uint8_t CLK_GLYPH[14] = { 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 0, 1, 2, 30 };   // 0x3fc9af0a

static void clock_hide_all(void)             // 0x4201f47c
{
    for (int el = 19; el <= 46; el++) el_show(el, false);
}

static void clock_glyph(int el, uint8_t glyph, int x)
{
    el_var(el, CLK_GLYPH[glyph]);
    el_x(el, x);
    el_show(el, true);
}

// 0x4201f6dc: HH:MM at the top, redrawn when the minute changes
static void clock_digits(void)
{
    if (g_oem.clock_mode == 3) return;
    oem_time_t t;
    hal_time(&t);
    if (t.min == s_clk_min && s_clk_min != 0xff) return;
    s_clk_min = t.min;
    SHOW(false, 46, 45, 43, 42);
    clock_glyph(46, (t.hour / 10) % 10, 13);
    clock_glyph(45, t.hour % 10, 25);
    clock_glyph(44, 13, 37);
    clock_glyph(43, (t.min / 10) % 10, 42);
    clock_glyph(42, t.min % 10, 54);
}

static void clock_dashes(void)               // 0x4201f654: "-- ~ --"
{
    clock_glyph(25, 12, 17); clock_glyph(24, 12, 25);
    clock_glyph(26, 10, 33);
    clock_glyph(23, 12, 41); clock_glyph(22, 12, 49);
}

// 0x4201f900: clock, banner, weather icon and the centred line "t2° ~ t1°"
static void clock_weather(uint8_t flag, uint8_t code, int8_t t1, int8_t t2)
{
    static const uint8_t ICON[7] = { 32, 33, 34, 36, 37, 31, 35 };       // 0x4201f8c4
    clock_hide_all();
    clock_digits();
    el_show(flag ? 40 : 41, true);
    if (code < 7) el_show(ICON[code], true);  // stock passes NULL to set_visible for other codes
    if (t1 < -100 || t1 > 100 || t2 < -100 || t2 > 100) return;

    bool two1 = t1 > 9 || t1 < -9, two2 = t2 > 9 || t2 < -9;
    int a1 = t1 < 0 ? -t1 : t1, a2 = t2 < 0 ? -t2 : t2;
    int w = (t2 < 0 ? 8 : 0) + (two2 ? 11 : 0) + (t1 < 0 ? 33 : 25) + (two1 ? 11 : 0);
    int x = ((80 - ((w + 17) & 0xff)) >> 1) & 0xff;

    if (t2 < 0) { clock_glyph(25, 12, x); x += 8; }
    if (two2)   { clock_glyph(30, a2 / 10, x); x += 11; }
    clock_glyph(29, a2 % 10, x);
    clock_glyph(21, 11, x + 11);
    clock_glyph(26, 10, x + 17);
    int x1 = x + 25;
    if (t1 < 0) { clock_glyph(24, 12, x1); x1 += 8; }
    if (two1)   { clock_glyph(28, a1 / 10, x1); x1 += 11; }
    clock_glyph(27, a1 % 10, x1);
    clock_glyph(20, 11, (x1 + 11) > 70 ? x1 + 10 : x1 + 11);
}

// 0x4201fb24: lay the page out for a clock mode
static void clock_set(uint8_t mode)
{
    s_clk_min = 0xff;
    if (g_oem.show_mode) {                   // shop demo: fixed record 0x3fc9b384
        g_oem.clock_mode = 0;
        clock_weather(0, 2, 18, 10);
    } else switch (mode) {
    case 0:
        g_oem.clock_mode = 0;
        clock_weather(g_oem.weather_flag, g_oem.weather_code, g_oem.weather_t1, g_oem.weather_t2);
        break;
    case 1:                                  // loading
        g_oem.clock_mode = 1;
        clock_hide_all(); clock_digits();
        el_show(g_oem.weather_flag ? 40 : 41, true);
        el_var(38, s_clk_frame); el_show(38, true);
        clock_dashes();
        break;
    case 2:                                  // no data: stock also clears the weather record
        g_oem.clock_mode = 2;
        g_oem.weather_flag = 0; g_oem.weather_t1 = 0; g_oem.weather_t2 = 0; g_oem.weather_code = 0;
        clock_hide_all(); clock_digits();
        el_show(41, true);
        el_show(39, true);
        clock_dashes();
        break;
    case 3:                                  // nothing ever received: no clock either
        g_oem.clock_mode = 3;
        clock_hide_all();
        SHOW(true, 39, 19);
        break;
    default:
        return;
    }
    s_clk_applied = g_oem.clock_mode;
}

static void clock_enter(void)                // 0x4201fc28
{
    clock_set(g_oem.clock_mode == 0xff ? 3 : g_oem.clock_mode);
}

// ---- page model ---------------------------------------------------------------------

static uint8_t mode_page(void)               // tail of 0x4201a9bc / 0x4201ac14
{
    switch (oem_mode()) {
    case 0:  return 81;
    case 1:  return 79;
    case 2:  return 77;
    case 3:  return 76;
    case 5:  return 80;
    default: return 78;
    }
}

static uint8_t sub_mode_payload(void)        // 0x4201a328
{
    return g_oem.profile[2] ? 4
         : (g_oem.profile[1] == 1 && (g_oem.tod_sub == 2 || g_oem.tod_sub == 3)) ? g_oem.tod_sub : 1;
}

// Common part of get_next_screen / get_last_screen. Returns true when *out is the
// answer, false when the caller falls through to the mode page.
static bool nav_guard(uint8_t s, uint8_t *out)
{
    if (g_oem.subpage == 1 && (s == 83 || s == 84 || s == 96 || s == 100)) {
        *out = s_saved_mode_page;
        return true;
    }
    return false;
}

// Wake screens and side pages lead straight to the mode page while state is 0 / 1.
static bool nav_wake_page(uint8_t s)
{
    return g_oem.state <= OEM_ST_WOKEN && (s == 83 || s == 84 || s == 85 || s == 96 || s == 100 || s == 104);
}

// The screens that are on neither ring (default branch of both functions).
static uint8_t nav_other(uint8_t s, bool next)
{
    if (!(hal_rtc()->hist_count != 0 && g_oem.state == OEM_ST_ENDED) || s == 98) {
        if (s == 98) {                       // unreachable: zone_flag is never set
            g_oem.zone_flag = 0;
            oem_idle_timeout(1);
            return 0xff;
        }
        if (s != 101 && s != 102 && s != 82) return 0xff;
        if (!paused()) return 0xff;
        if (s == 102) {
            if (!next) return 100;
        } else {
            if (g_oem.done_s >= 120) return s == 82 ? 83 : 100;   // quit after 2 min: score
            g_oem.zone_flag = 0;
        }
    } else if (g_oem.zone_flag) {
        return (s == 83 || s == 100) ? 98 : 0xff;
    }
    return mode_page();
}

static uint8_t get_next_screen(void)         // 0x4201a9bc (swipe up), 0xFF = none
{
    uint8_t s = g_oem.now_ui, r;
    if (g_oem.show_mode) {                   // demo ring
        switch (s) {
        case 80: case 81: return 79;
        case 79: return 77;
        case 77: return 76;
        case 76: return 78;
        case 78: return 96;
        case 96: return 100;
        case 83: case 100: return 84;
        case 84: return 80;
        }
        return 0xff;
    }
    if (nav_guard(s, &r)) return r;
    if (!nav_wake_page(s)) {
        switch (s) {
        case 92: return 92;                  // info: the page index moves in page_commit
        case 71: return 72;
        case 72: return 73;
        case 73: break;
        case 80: return 79;
        case 79: return 77;
        case 77: return 76;
        case 76: return 78;
        case 78: return oem_app_profile() ? 81 : 80;
        case 81: return 80;
        default: return nav_other(s, true);
        }
    }
    return mode_page();
}

static uint8_t get_last_screen(void)         // 0x4201ac14 (swipe down)
{
    uint8_t s = g_oem.now_ui, r;
    if (g_oem.show_mode) {
        switch (s) {
        case 78: return 76;
        case 76: return 77;
        case 77: return 79;
        case 79: return 80;
        case 80: case 81: case 82: return 84;
        case 84: return 100;
        case 83: case 100: return 96;
        case 96: return 78;
        }
        return 0xff;
    }
    if (nav_guard(s, &r)) return r;
    if (!nav_wake_page(s)) {
        switch (s) {
        case 92: return 92;
        case 73: return 72;
        case 72: return 71;
        case 71: return 0xff;
        case 78: return 76;
        case 76: return 77;
        case 77: return 79;
        case 79: return 80;
        case 80: return oem_app_profile() ? 81 : 78;
        case 81: return 78;
        default: return nav_other(s, false);
        }
    }
    return mode_page();
}

static uint8_t left_of(uint8_t saved)        // 0x4201aec0 (swipe left)
{
    uint8_t s = g_oem.now_ui;
    if (g_oem.show_mode || saved == 0xff) return 0xff;
    if (is_mode_page(s)) return 100;
    if (s == 83 || s == 84 || s == 100) return 96;
    if (s == 96) return saved;
    return 0xff;
}

static uint8_t right_of(uint8_t saved)       // 0x4201ae50 (swipe right)
{
    uint8_t s = g_oem.now_ui;
    if (g_oem.show_mode || saved == 0xff) return 0xff;
    if (is_mode_page(s)) return 96;
    if (s == 96 || s == 84) return 100;
    if (s == 83 || s == 100) return saved;
    return 0xff;
}

// 0x42020108: select the element list of a screen and ask for a full redraw. Ids
// without a screen leave the current one selected.
static void screen_switch(uint8_t id)
{
    ui_render_clear();
    if (is_mode_page(id)) {                  // 0x4201ffdc
        g_oem.subpage = 0;
        s_saved_mode_page = id;
    }
    s_left = left_of(s_saved_mode_page);
    s_right = right_of(s_saved_mode_page);

    const uint8_t *list;
    switch (id) {
    case 81:                                 // two lists: app profile art, or the mode-5 art
        if (g_oem.profile[0] == 1 || g_oem.profile[2] == 1) list = ui_screen_list(81);
        else if (g_oem.profile[0] == 0) list = STOCK_SCR_81B;
        else list = NULL;
        break;
    case 84:
        if (g_oem.show_mode) el_var(118, 13);
        list = ui_screen_list(84);
        break;
    case 96:
        clock_enter();
        list = ui_screen_list(96);
        break;
    default:
        list = ui_screen_list(id);
        break;
    }
    if (list) { s_list = list; s_redraw = true; }
}

static void page_commit(uint8_t scr, uint8_t dir)        // 0x4201af48
{
    static const uint8_t SCREEN_MODE[6] = { 3, 2, 4, 1, 5, 0 };   // screens 76..81
    oem_idle_kick();
    if (scr == 0xff) return;
    if (dir == 1 || dir == 2) {
        uint8_t now = g_oem.now_ui;
        if (scr >= 71 && scr <= 73) g_oem.dwell_s = 0;
        if (now == 73 && is_mode_page(scr)) {
            oem_idle_timeout(30);
        } else if ((g_oem.state == OEM_ST_ENDED || g_oem.state == OEM_ST_SCORED) &&
                   (now == 83 || now == 84 || now == 100)) {
            g_oem.dwell_s = 0;
            if (g_oem.state == OEM_ST_ENDED) g_oem.state = OEM_ST_SCORED;
        } else if (now == 101 || now == 102 || now == 82) {      // swipe on a paused session: quit
            g_oem.user_quit = 1;
            oem_motor_stop();
        } else if (now == 92) {
            uint8_t p = g_oem.info_page;
            g_oem.info_page = dir == 1 ? (p ? p - 1 : 3) : (p < 3 ? p + 1 : 0);
            oem_ui_post(92, &g_oem.info_page, 1);
        }
    }
    oem_idle_kick();
    g_oem.now_ui = scr;
    if (is_mode_page(scr)) {
        oem_set_mode(SCREEN_MODE[scr - 76]);
        if (g_oem.state == OEM_ST_WOKEN) oem_idle_timeout(30);
        else if (g_oem.state == OEM_ST_ENDED || g_oem.state == OEM_ST_SCORED) oem_idle_timeout(10);
    }
    if (g_oem.show_mode) oem_led_set(3, 1, 4);
}

// 0x420208f8 "update_now_ui_index": dir 1 up, 2 down, 3 left, 4 right
static void page_update(uint8_t dir)
{
    uint8_t old = g_oem.now_ui;
    bool old_is_mode = is_mode_page(old);
    uint8_t last = get_last_screen();
    uint8_t next = get_next_screen();
    switch (dir) {
    case 1: case 2:
        if (g_oem.subpage == 0) s_ui_now = dir == 1 ? next : last;
        else if (g_oem.subpage == 1) { s_ui_now = s_saved_mode_page; g_oem.subpage = 0; }
        break;
    case 3: case 4:
        if (old_is_mode || g_oem.subpage == 1) {
            g_oem.subpage = 1;
            s_ui_now = dir == 3 ? s_left : s_right;
        }
        break;
    }
    if (old == 96 && s_ui_now != 96) clock_hide_all();
    page_commit(s_ui_now, dir);
    mode_page_update();
    if (g_oem.subpage == 1 && (s_ui_now == 83 || s_ui_now == 100)) {
        oem_show_score(g_oem.score);         // the score page is drawn by its message
    } else if (s_ui_now != 100) {
        screen_switch(s_ui_now);
    }
}

// 0x42020e14 returns false when the swipe has no target. (Stock then pre-renders the
// target into the same buffer, which is overwritten before the next blit.)
static bool prepare_target(uint8_t dir)
{
    return (dir == 1 ? get_next_screen() : get_last_screen()) != 0xff;
}

// 0x42020ab8, main task: a short press on a side page returns to the mode page
void oem_ui_page_back(void)
{
    if (g_oem.subpage == 1) page_update(1);
}

// ---- touch lock (0x4202865c and friends) --------------------------------------------

static bool lock_blocks(void)                // 0x420285e0, table 0x3c11cf24
{
    uint8_t s = g_oem.now_ui;
    return g_oem.locked && (is_mode_page(s) || s == 87 || s == 82);
}

bool oem_ui_swipe_allowed(void)              // 0x4202860c
{
    uint8_t s = g_oem.now_ui;
    return !g_oem.locked || s == 92 || s == 83 || s == 84;
}

static void lock_restore(void)               // 0x420284c4
{
    uint8_t s = g_oem.saved_screen;
    if (s == 81) {
        uint8_t sub = sub_mode_payload();
        oem_show(81, &sub, 1);
    } else if (is_mode_page(s)) {
        oem_show(s, NULL, 0);
    } else if (s == 87) {
        uint8_t level = hal_rtc()->strength;
        oem_show_strength(level - 1);
        oem_strength_gear(level);
    } else if (s == 82) {
        oem_show_paused();                   // the 1 Hz refresh brings 82 back if running
    } else {
        return;
    }
    g_oem.lock_popup = 0;
}

// Stock starts the task "show_lock_ui_task" (0x42028544): lock, show 91, poll 100 x
// 10 ms for a dismissal, restore the saved screen. Here the second half is a deadline
// checked on every pass of the UI loop.
static void lock_popup_start(void)
{
    g_oem.locked = 1;
    g_oem.lock_popup = 1;
    oem_show(91, NULL, 0);
    s_popup_run = true;
    s_popup_until = hal_ms() + 1000;
}

static void lock_popup_poll(void)
{
    if (!s_popup_run || (int32_t)(hal_ms() - s_popup_until) < 0) return;
    s_popup_run = false;
    lock_restore();
}

static void lock_popup_swipe(void)           // 0x42028640: a swipe on a locked screen
{
    g_oem.saved_screen = g_oem.now_ui;
    if (!g_oem.lock_popup) lock_popup_start();
}

// Main task: arg 1 = button held 2 s (toggle), arg 0 = short press while the popup is up.
void oem_ui_lock_button(int arg)
{
    uint8_t now = g_oem.now_ui;
    if (arg == 0) {
        if (g_oem.locked && now != 0 && g_oem.lock_popup) {
            lock_restore();
            if (!g_oem.lock_popup) s_popup_run = false;
        }
        return;
    }
    if (arg != 1) return;
    if (!g_oem.locked) {
        if (!is_mode_page(now)) return;
        g_oem.saved_screen = now;
        if (!g_oem.lock_popup) lock_popup_start();
    } else {
        if (g_oem.locked != 1) return;
        if (now == 82) {
            if (s_ui_running == 1) return;   // not while brushing runs
        } else if (!(is_mode_page(now) || now == 87 || now == 83)) {     // table 0x3c11cf44
            return;
        }
        g_oem.locked = 0;
        g_oem.saved_screen = 0;
        if (g_oem.lock_popup == 1) lock_restore();
    }
    if (g_oem.now_ui != 87) {                // 0x420285bc: 400 ms buzz
        oem_motor_gear(0x35, false);
        hal_delay(400);
        oem_motor_off();
    }
    oem_idle_timeout(30);
}

// ---- status icon (0x42020f94) -------------------------------------------------------

// Lock / network icon at (28,0). Re-evaluated when an input changes and every 500 ms;
// while locked the lock icon alternates with the network icon at 1 Hz.
static void status_icon(void)
{
    if (s_ui_now == 82 && s_ui_running == 1) {
        el_show(180, false);
    } else {
        uint8_t in[4] = { g_oem.wifi_weak, g_oem.cloud_state, g_oem.wifi_status, g_oem.sys[0x0c] };
        if (s_icon_cnt % 1000 == 0 || memcmp(in, s_icon_in, 4) != 0) {
            SHOW(false, 180, 167, 166, 165);
            bool first_half = s_icon_cnt <= 1000;
            bool locked = g_oem.locked == 1;
            int other = 0;
            if (in[3] != 2 || in[2] == 2) other = 167;                    // not bound, or Wi-Fi down
            else if (in[2] == 1 && in[1] == 2) other = 165;
            else if (in[2] == 1 && in[1] == 1 && in[0]) other = 166;
            if (locked && (!other || first_half)) el_show(180, true);
            else if (other) el_show(other, true);
            memcpy(s_icon_in, in, 4);
            el_inval(180); el_inval(167); el_inval(166); el_inval(165);
        }
    }
    s_icon_cnt = (s_icon_cnt % 2000 == 0) ? 100 : s_icon_cnt + 100;
}

// ---- blink / animation tick, every 50 ms (0x42021e9c) -------------------------------

// 0x4201a574(70) -> 0x4201a538: the boot animation has played once
static void boot_anim_done(void)
{
    if (g_oem.reset_flag) return;            // first boot after a reset: the last frame stays
    if (g_oem.sys[0x0c] == 2 || g_oem.sys[0x34] != 1 || g_oem.dev_mode == 2) {
        oem_show_main();
        oem_idle_timeout(60);
    } else {
        oem_show(71, NULL, 0);               // pairing guide
    }
}

static void blink_tick(void)
{
    s_phase ^= 1;

    uint8_t v = ui_el_var(179);              // boot animation, 100 ms per frame
    if (g_oem.aging == 1) {
        el_inval(179);
        aging_overlay(true);
        aging_digits();
        if (s_phase) v++;
        if (v == 19) v = 0;
        el_var(179, v);
    } else if (v < 19 && s_ui_now == 70) {
        el_inval(179);
        if (s_phase) el_var(179, ++v);
        if (v == 19) boot_anim_done();       // (stock loops instead while a BLE test flag, 0x3fca4d8c, is set)
    }

    if ((++s_c100 & 1) == 0 && g_oem.power_state != OEM_PWR_BATTERY) {   // charging strip
        v = ui_el_var(147);
        el_var(147, v <= 20 ? v + 1 : 0);
        el_inval(147);
    }

    if (++s_c250 % 5 == 0) {                 // every 250 ms
        battery_layout();
        if (oem_mode() == 5) {               // brushing background runs as an animation
            v = ui_el_var(117);
            if (v == 14) el_var(117, 0);
            else if (v <= 13 && s_phase) el_var(117, v + 1);
        }
    }

    if (s_ui_now == 88) { el_show(137, true); el_inval(137); }

    bool skip_103 = false;
    if (s_ui_now == 95) {                    // (95 is never posted in this build)
        v = ui_el_var(97);
        if (v <= 18) {
            if (s_phase) el_var(97, ++v);
            el_inval(97);
            if (v == 19) { oem_show_history(1); g_oem.state = OEM_ST_SCORED; }   // 0x4201a574(95)
        }
    } else {
        if (s_ui_now == 84 && !g_oem.show_mode) {        // 50 ms per frame, holds frame 13
            v = ui_el_var(118);
            el_var(118, (v < 12 ? v : 12) + 1);
            el_inval(118);
        }
        if (s_ui_now == 100) {               // perfect-score animation, 100 ms per frame
            v = ui_el_var(3);
            if (v > 18) skip_103 = true;
            else {
                if ((s_c250 & 1) == 0) el_var(3, v + 1);
                el_inval(3);
            }
        }
    }
    if (!skip_103 && s_ui_now == 103) {      // loops frames 0..5
        v = ui_el_var(1);
        el_var(1, v < 5 ? v + 1 : 0);
        el_inval(1);
    }

    status_icon();

    if (g_oem.now_ui == 96) {
        if (g_oem.clock_mode != s_clk_applied) {
            // Stock re-lays the page out from the BLE weather handler (0x42011a60);
            // here whoever feeds the weather only writes g_oem.
            clock_enter();
            s_clk_applied = g_oem.clock_mode;
        }
        if (g_oem.clock_mode == 1) {         // "loading" frames 5..0, 100 ms each
            if (s_clk_t < 100) s_clk_t += 100;
            else {
                s_clk_t = 0;
                s_clk_frame = s_clk_frame ? s_clk_frame - 1 : 5;
                el_show(38, false);
                el_var(38, s_clk_frame);
                el_show(38, true);
            }
        }
        clock_digits();
    }
    // (zone blink 0x42021db8 only acts on screen 99, which needs the zone-guided flag)
}

// ---- message handlers (0x42022564) --------------------------------------------------

// Swipe up (dir 1) / down (dir 2): next mode page, or intensity while brushing runs.
static void swipe_vertical(uint8_t dir, bool lock_check)
{
    if (lock_check) {
        if (lock_blocks()) { lock_popup_swipe(); return; }
        if (!oem_ui_swipe_allowed()) return;
        if (!(oem_motor_playing() || !g_oem.session_active || paused())) return;
    }
    if (g_oem.session_active && !paused()) oem_strength_step(dir == 1);
    else if (prepare_target(dir)) page_update(dir);
    if (lock_check && g_oem.session_active) oem_touch_set_state(6);
}

static void swipe_horizontal(uint8_t dir)
{
    if (lock_blocks()) lock_popup_swipe();
    else if (oem_ui_swipe_allowed()) page_update(dir);
}

// Factory calibration screens 105..119: only what keeps the element state consistent.
static void factory_screen(const ui_msg_t *m)
{
    static const uint8_t MAIN_EL[15] = { 68, 0, 58, 56, 0, 0, 57, 54, 53, 52, 51, 50, 48, 49, 47 };   // 105..119
    uint8_t id = (uint8_t)m->id;
    int marks = id == 106 ? 4 : (id == 109 || id == 112) ? 2 : (id == 110 || id == 113) ? 1 : 0;
    if (MAIN_EL[id - 105]) el_var(MAIN_EL[id - 105], 0);
    if (marks) {
        // OK / NG mark pairs per row: [66]/[62], [65]/[61], [64]/[60], [63]/[59]
        SHOW(true, 66, 65, 64, 63, 62, 61, 60, 59);
        for (int i = 0; i < marks; i++) {
            if (id != 110) s_fact[i] = m->p[i];          // 110 reuses the previous flag
            int ok = 66 - i, ng = 62 - i;
            if (s_fact[i]) { el_show(ok, false); el_var(ng, 0); }
            else           { el_show(ng, false); el_var(ok, 0); }
        }
    }
    if (id == 117) aging_digits();
}

static void handle_msg(const ui_msg_t *m)
{
    uint8_t id = (uint8_t)m->id, p0 = m->p[0];
    if (m->id > 120) return;

    switch (id) {
    // ---- commands
    case 0:                                  // screen on
        hal_lcd_init();
        s_enabled = true;
        return;
    case 1:                                  // screen off
        s_enabled = false;
        hal_lcd_sleep();
        return;
    case 7:  swipe_horizontal(3); return;
    case 9:  swipe_horizontal(4); return;
    case 10: swipe_vertical(1, true); return;
    case 8:  swipe_vertical(2, true); return;
    case 12: swipe_vertical(2, false); return;           // shop demo auto page
    case 11: return;                         // long touch: stock stores the byte and calls an empty stub

    // ---- screens
    case 70:
        el_var(179, 0xff);
        aging_overlay(g_oem.aging != 0);
        if (g_oem.aging) aging_digits();
        break;
    case 71: case 72: case 73:
    case 104:
        break;
    case 76: case 77: case 78: case 79: case 80:
        mode_page_update();
        break;
    case 81:
        s_sub_mode = p0;                     // recomputed by mode_page_update
        mode_page_update();
        break;
    case 82: {
        s_ui_running = p0;
        uint16_t rem = oem_brush_remaining();
        if (s_ui_running == 1) {
            SHOW(false, 111, 110, 109, 108, 107);
            // progress art, 15 frames over the session; mode 5 animates it instead.
            // Stock divides by total_s / 15 unguarded (a divide-by-zero trap below 15 s).
            if (oem_mode() != 5 && g_oem.total_s / 15 != 0) {
                unsigned f = ((unsigned)(g_oem.total_s - rem) & 0xff) / (g_oem.total_s / 15u);
                el_var(117, f > 13 ? 14 : (uint8_t)f);
            }
        } else {                             // "paused" layout, not used by this build
            SHOW(true, 112, 111, 110, 109, 108, 107);
        }
        countdown_digits(rem);
        break;
    }
    case 83:                                 // alternative score screen, unreachable in this build
        SHOW(true, 111, 110, 109, 108);
        el_show(94, p0 == 100);
        SHOW(p0 != 100 && p0 >= 90, 96, 95, 93);
        SHOW(p0 < 90, 92, 91, 90);
        if (p0 != 100) score_digits(p0, p0 >= 90);
        break;
    case 84:
        el_var(118, 0xff);
        break;
    case 85:
        greeting(p0);
        break;
    case 87:
        el_var(106, p0);
        el_var(105, lang());
        goto no_overlay;
    case 88:
        el_var(137, p0 / 10);
        el_var(136, lang());
        goto no_overlay;
    case 89: el_var(132, lang()); goto no_overlay;
    case 90: el_var(134, lang()); goto no_overlay;
    case 91:
        el_var(130, lang());
        // The popup counts from the moment it is drawn: the main task may have held
        // the core lock for the 400 ms buzz since it was requested.
        if (s_popup_run) s_popup_until = hal_ms() + 1000;
        goto no_overlay;
    case 92:
        el_var(75, ver_digit(g_oem.fw_version, 0)); el_var(73, ver_digit(g_oem.fw_version, 2));
        el_var(71, ver_digit(g_oem.fw_version, 4)); el_var(69, ver_digit(g_oem.fw_version, 6));
        el_var(77, p0);
        if (p0 <= 3) SHOW(p0 == 0, 75, 73, 71, 69, 76, 74, 72, 70);      // "V a.b.c.d" on page 0 only
        break;
    case 93:
        el_var(148, 0xff); el_var(147, 0xff);
        if (p0 >= 100) {
            el_var(146, 1); el_var(145, 0); el_var(144, 0);
            SHOW(false, 139, 138, 142, 141, 140, 147);
        } else if (p0 < 10) {
            el_var(139, p0);
            SHOW(false, 142, 141, 140, 146, 145, 144, 143);
        } else {
            el_var(142, p0 / 10); el_var(141, p0 % 10);
            SHOW(false, 146, 145, 144, 143, 139, 138);
        }
        battery_layout();
        break;
    case 94: el_var(89, 0); goto no_overlay;
    case 95: el_var(97, 0xff); goto no_overlay;
    case 97:
        el_var(87, lang());
        el_var(85, ver_digit(g_oem.ota_version, 0)); el_var(83, ver_digit(g_oem.ota_version, 2));
        el_var(81, ver_digit(g_oem.ota_version, 4)); el_var(79, ver_digit(g_oem.ota_version, 6));
        el_var(78, lang());
        goto no_overlay;
    case 98:
        el_var(18, lang());
        break;
    case 99:                                 // zone-guided brushing: needs zone_flag, which is never set
                                             // (the planned-zone overlays 0x420223f0 are not ported)
        countdown_digits(oem_brush_remaining());
        el_show(16, true);
        break;
    case 100:
        if (p0 == 100) {                     // perfect: only the animation
            SHOW(false, 96, 95, 93, 92, 91, 90, 94, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4);
            el_show(3, true);
            el_var(3, 0xff);
        } else {
            bool high = p0 >= 90;
            el_show(3, false);
            el_show(94, false);
            SHOW(high, 96, 95, 93);
            SHOW(!high, 92, 91, 90);
            zone_overlays();
            score_digits(p0, high);
        }
        break;
    case 101:
        s_ui_running = p0;
        zone_overlays();
        countdown_digits(oem_brush_remaining());
        break;
    case 102:                                // paused, zone-guided: unreachable (see 99)
        countdown_digits(oem_brush_remaining());
        break;
    case 103: el_var(1, 0xff); goto no_overlay;
    case 105: case 106: case 107: case 108: case 109: case 110: case 111: case 112:
    case 113: case 114: case 115: case 116: case 117: case 118: case 119:
        factory_screen(m);
        goto no_overlay;
    case 120: {
        uint8_t p = g_oem.batt_pct;
        if (p >= 100) {                      // the battery picture is not updated here
            el_var(146, 1); el_var(145, 0); el_var(144, 0);
            SHOW(false, 139, 138, 142, 141, 140);
        } else if (p < 10) {
            el_var(148, 0);
            el_var(139, p);
            SHOW(false, 142, 141, 140, 146, 145, 144, 143);
        } else {
            if (p < 16)       el_var(148, 0);
            else if (p <= 30) el_var(148, 1);
            else if (p <= 44) el_var(148, 2);
            else if (p >= 46 && p <= 60) el_var(148, 3);
            else if (p >= 61 && p <= 80) el_var(148, 4);
            else if (p >= 81) el_var(148, 5);
            el_var(142, p / 10); el_var(141, p % 10);
            SHOW(false, 146, 145, 144, 143, 139, 138);
        }
        break;
    }
    default:                                 // 2..6, 13..69, 74, 75, 86, 96: not handled
        return;
    }
    dev_overlay();
no_overlay:
    s_ui_now = id;
    screen_switch(id);
}

// ---- composition (0x42023b5c) -------------------------------------------------------

// Draws when the screen was (re)selected or an element of its list is dirty, like
// stock; every draw recomposes the whole frame.
static bool screen_draw(void)
{
    if (!s_list) return false;
    bool need = s_redraw;
    for (const uint8_t *e = s_list; !need && *e != 0xff; e++) need = s_dirty[*e];
    if (!need) return false;
    s_redraw = false;
    ui_render_clear();
    for (const uint8_t *e = s_list; *e != 0xff; e++) {
        ui_render_elem(*e);
        s_dirty[*e] = 0;
    }
    return true;
}

// ---- entry points -------------------------------------------------------------------

void oem_ui_init(uint8_t *fb)                // 0x42021368
{
    ui_render_init(fb, hal_res_read);
    ui_render_set_lang(g_oem.lang);
    ui_render_clear();
    memset(s_dirty, 1, sizeof s_dirty);      // stock elements start visible + dirty
    s_q_count = 0;
    s_ui_now = 0xff;
    s_enabled = true;
    s_list = NULL;
    s_redraw = false;
    s_lit = false;
    s_saved_mode_page = s_left = s_right = 0xff;
    s_sub_mode = 0;
    s_ui_running = 0;
    memset(s_fact, 0, sizeof s_fact);
    s_phase = s_c100 = s_c250 = 0;
    s_icon_cnt = 100;
    memset(s_icon_in, 0xff, sizeof s_icon_in);
    s_clk_frame = 5;
    s_clk_t = 0;
    s_clk_min = 0xff;
    s_clk_applied = 0xff;
    s_popup_run = false;
    g_oem.clock_mode = 0xff;
    g_oem.wifi_weak = 1;                     // stock initial value, until the first RSSI reading
}

bool oem_ui_handle(uint32_t bits)            // one pass of 0x42022564
{
    if ((bits & OEM_UIEV_GESTURE) && g_oem.power_state == OEM_PWR_BATTERY) oem_gesture_end();

    lock_popup_poll();

    if (bits & OEM_UIEV_MSG) {
        ui_msg_t m;
        if (q_pop(&m)) {
            ui_render_set_lang(g_oem.lang);
            handle_msg(&m);
            if (s_q_count) hal_ui_event_post(OEM_UIEV_MSG);
        }
    }
    if (bits & OEM_UIEV_BLINK) blink_tick();
    if (bits & OEM_UIEV_LCD_REINIT) hal_lcd_init();
    if (bits & OEM_UIEV_FACTORY) {           // button held 8 s
        oem_leave_show_mode();
        hal_nvs_set("IntoShow", &g_oem.show_mode, 1);
        oem_factory_reset();
    }
    // Not in stock, which keeps composing and pushing frames behind a dark backlight
    // (on the charger 12 a second, for as long as the brush is docked). A newly
    // selected screen is still drawn once, as before; after that nothing is drawn
    // while the backlight is dark. All of the above still runs and the dirty flags
    // stay set; the first pass that finds the light on redraws the whole screen.
    bool lit = oem_led_backlight_lit();
    if (lit && !s_lit) s_redraw = true;
    s_lit = lit;
    return s_enabled && (lit || s_redraw) && screen_draw();
}

uint8_t oem_ui_now(void) { return s_ui_now; }
bool oem_ui_enabled(void) { return s_enabled; }
void oem_ui_set_enabled(bool on) { s_enabled = on; }

// Not a stock function: stock keeps the side-page state across the screen-off stage and
// loses it only with the reset that follows deep sleep.
void oem_ui_page_reset(void)
{
    g_oem.subpage = 0;
    s_saved_mode_page = s_left = s_right = 0xff;
}

// Host test of the OEM UI module (main/oem_ui.c): fakes for the HAL and for the app
// functions the UI calls, virtual time, and scripted sequences. Frames that reach the
// "panel" are written as PPMs (tile them with sheet.py); behaviour is checked with
// CHECK().
//   cc -std=gnu11 -Wall -Wextra -Imain re/tools/uisim/sim_ui.c main/oem_ui.c main/ui_render.c -o sim_ui
//   sim_ui res.bin outdir            (res.bin: picture partition dump, or the stand-in from mkres.py)
//   for g in a_wake b_ring c_side d_brush e_mode5 f_batt g_info h_lock i_ota j_misc k_guard; do
//       python3 re/tools/uisim/sheet.py outdir/sheet_$g.png outdir/${g}_*.ppm; done
// Exit status is non-zero when a CHECK fails.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_state.h"
#include "ui_render.h"

oem_state_t g_oem;
static oem_rtc_t s_rtc;
static FILE *s_res;
static const char *s_out;
static uint8_t s_fb[UI_FB_BYTES], s_panel[UI_FB_BYTES];
static uint32_t s_ms, s_ui_bits;
static oem_time_t s_time = { 26, 10, 2, 9, 41, 0, 5 };
static int s_blits, s_lcd_inits, s_lcd_sleeps, s_gestures, s_idle = -1, s_kicks, s_stops, s_buzz, s_factory;
static int s_touch_state = -1, s_fail, s_checks, s_verbose;
static uint8_t s_next_score = 87;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- HAL fakes ----------------------------------------------------------------------
void hal_lock(void) {}
void hal_unlock(void) {}
uint32_t hal_ms(void) { return s_ms; }
uint32_t hal_uptime_s(void) { return s_ms / 1000; }
void hal_delay(uint32_t ms) { s_ms += ms; s_ui_bits |= OEM_UIEV_BLINK; }   // the UI task waits for the lock
void hal_time(oem_time_t *t) { *t = s_time; }
void hal_log(const char *fmt, ...)
{
    if (!s_verbose) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
}
void hal_event_post(uint32_t bits) { (void)bits; }
void hal_ui_event_post(uint32_t bits) { s_ui_bits |= bits; }
void hal_timer_start(hal_timer_t t, uint32_t ms, bool periodic) { (void)t; (void)ms; (void)periodic; }
void hal_timer_stop(hal_timer_t t) { (void)t; }
size_t hal_nvs_get(const char *key, void *buf, size_t len) { (void)key; (void)buf; (void)len; return 0; }
bool hal_nvs_set(const char *key, const void *buf, size_t len) { (void)buf; (void)len; if (s_verbose) printf("nvs_set %s\n", key); return true; }
oem_rtc_t *hal_rtc(void) { return &s_rtc; }
void hal_lcd_init(void) { s_lcd_inits++; memset(s_panel, 0, sizeof s_panel); }
void hal_lcd_sleep(void) { s_lcd_sleeps++; }
void hal_lcd_blit(const uint8_t *fb) { s_blits++; memcpy(s_panel, fb, sizeof s_panel); }
bool hal_res_read(uint32_t off, void *dst, size_t len)
{
    return fseek(s_res, off, SEEK_SET) == 0 && fread(dst, 1, len, s_res) == len;
}

// ---- app fakes (the parts of oem_app.c / oem_brush.c the UI calls or depends on) ------
void oem_gesture_end(void) { s_gestures++; }
void oem_idle_timeout(uint8_t s) { s_idle = s; }
void oem_idle_off(void) { s_idle = -1; }
void oem_idle_kick(void) { s_kicks++; }
void oem_show(uint8_t screen, const void *payload, int len)
{
    g_oem.now_ui = screen; g_oem.dwell_s = 0;
    oem_ui_post(screen, payload, len);
}
bool oem_app_profile(void) { return g_oem.profile[0] == 1 || g_oem.profile[2] == 1; }
uint8_t oem_mode(void) { return g_oem.profile[6]; }
void oem_set_mode(uint8_t m) { g_oem.profile[6] = g_oem.profile[7] = m; }
static void show_mode_page(void)
{
    static const uint8_t MODE_SCREEN[6] = { 81, 79, 77, 76, 78, 80 };
    uint8_t scr = MODE_SCREEN[oem_mode() > 5 ? 5 : oem_mode()];
    uint8_t sub = g_oem.profile[2] ? 4 : (g_oem.profile[1] == 1 && (g_oem.tod_sub == 2 || g_oem.tod_sub == 3)) ? g_oem.tod_sub : 1;
    oem_show(scr, scr == 81 ? &sub : NULL, scr == 81);
}
void oem_show_main(void)
{
    if (g_oem.now_ui == 97) return;
    g_oem.profile[7] = g_oem.profile[6];
    show_mode_page();
    oem_idle_timeout(30);
}
void oem_show_main_after_session(void) { oem_show_main(); oem_idle_timeout(10); }
void oem_show_score(uint8_t score) { oem_show(100, &score, 1); }
void oem_show_history(uint8_t flag)
{
    uint16_t p[4] = { s_rtc.hist_seconds, s_rtc.hist_score_sum, s_rtc.hist_count, flag };
    oem_show(84, p, 8);
}
uint16_t oem_brush_remaining(void)
{
    if (oem_mode() == 5) return g_oem.done_s;
    return g_oem.total_s >= g_oem.done_s ? g_oem.total_s - g_oem.done_s : 0;
}
void oem_show_paused(void)
{
    uint8_t p[2] = { 0, (uint8_t)(g_oem.total_s - g_oem.done_s) };
    uint8_t dwell = g_oem.dwell_s;
    oem_show(101, p, 2);
    g_oem.dwell_s = dwell;
}
void oem_show_brushing(void)
{
    uint8_t p[2] = { 1, (uint8_t)(g_oem.total_s - g_oem.done_s) };
    uint8_t dwell = g_oem.dwell_s;
    oem_show(82, p, 2);
    g_oem.dwell_s = dwell;
}
void oem_show_strength(uint8_t l) { oem_show(87, &l, 1); }
static const uint8_t GEAR[6] = { 0, 0x33, 0x34, 0x01, 0x18, 0x20 };
void oem_strength_gear(uint8_t level) { g_oem.gear = GEAR[level >= 1 && level <= 5 ? level : 3]; }
void oem_strength_step(bool up)
{
    g_oem.dwell_s = 0;
    int cur = 3;
    for (int i = 1; i <= 5; i++) if (g_oem.gear == GEAR[i] && i != 3) cur = i;
    int n = up ? (cur + 1 > 5 ? 5 : cur + 1) : (cur - 1 == 0 ? 1 : cur - 1);
    s_rtc.strength = (uint8_t)n;
    if (g_oem.now_ui == 87) { oem_show_strength((uint8_t)(n - 1)); g_oem.gear = GEAR[n]; }
}
static void session_end(bool while_running)
{
    bool show_done = while_running || g_oem.done_s >= 120;
    g_oem.session_active = 0; g_oem.running = 0; g_oem.stop_delay = 0;
    g_oem.score = s_next_score;
    g_oem.state = OEM_ST_ENDED;
    s_rtc.hist_count++;
    if (g_oem.power_state != OEM_PWR_BATTERY) { oem_idle_timeout(10); return; }
    if (show_done) oem_show(103, NULL, 0);
    else oem_show_main_after_session();
}
// handle_motor_stop 0x42019528 only posts the session-end event; the main task ends
// the session on its next pass (main_step below).
static int s_end_pending;
void oem_motor_stop(void)
{
    s_stops++;
    if (g_oem.session_active) s_end_pending = 1 + g_oem.running;
    g_oem.session_active = 0; g_oem.stop_delay = 200;
}
static void main_step(void)
{
    if (!s_end_pending) return;
    bool running = s_end_pending == 2;
    s_end_pending = 0;
    session_end(running);
}
void oem_factory_reset(void) { s_factory++; }
void oem_leave_show_mode(void) { g_oem.show_mode = 0; }
void oem_led_set(int led, int state, int anim) { (void)led; (void)state; (void)anim; }
static bool s_backlight = true;
bool oem_led_backlight_lit(void) { return s_backlight; }
void oem_touch_set_state(uint8_t st) { s_touch_state = st; }
bool oem_motor_playing(void) { return g_oem.session_active && g_oem.running; }
void oem_motor_gear(uint8_t gear, bool app) { (void)app; if (gear == 0x35) s_buzz++; }
void oem_motor_off(void) {}

static void start_brushing(void)             // 0x4201c790, UI-relevant part
{
    g_oem.saved_mode = oem_mode();
    g_oem.state = OEM_ST_SESSION;
    g_oem.session_active = 1; g_oem.running = 1; g_oem.stop_delay = 200;
    g_oem.done_s = g_oem.elapsed_s = 0;
    g_oem.total_s = oem_mode() == 5 ? 180 : 120;
    memset(g_oem.zone_s, 0, sizeof g_oem.zone_s);
    if (oem_mode() == 5) {
        if (s_rtc.strength < 1 || s_rtc.strength > 5) s_rtc.strength = 3;
        oem_show_strength(s_rtc.strength - 1);
        oem_strength_gear(s_rtc.strength);
    } else {
        oem_show_brushing();
    }
}

static void short_press(void)                // 0x4201cab8 case 0
{
    uint8_t n = g_oem.now_ui;
    if (g_oem.locked && g_oem.lock_popup) { oem_ui_lock_button(0); return; }
    if (g_oem.subpage == 1) { oem_ui_page_back(); return; }
    if ((n >= 71 && n <= 73) || n == 83 || n == 92 || n == 93 || n == 94 || n == 100 ||
        (n == 84 && (g_oem.state == OEM_ST_ENDED || g_oem.state == OEM_ST_SCORED))) {
        oem_show_main();
        oem_idle_timeout(n == 83 || n == 84 || n == 100 ? 10 : n == 92 || n == 94 ? 5 : 60);
        return;
    }
    if (!g_oem.session_active) start_brushing();
    else if (g_oem.running) { g_oem.running = 0; g_oem.stop_delay = 0; oem_show_paused(); }
    else { g_oem.running = 1; g_oem.stop_delay = 200; oem_show_brushing(); }
}

static void session_clock_1hz(void)
{
    if (!(g_oem.session_active && g_oem.running)) return;
    g_oem.done_s++; g_oem.elapsed_s++;
    g_oem.zone_s[(g_oem.done_s / 9) % 12] += 1;
    if (oem_mode() != 5 && g_oem.done_s >= g_oem.total_s) session_end(true);
    else if (g_oem.now_ui != 87) oem_show_brushing();
}

static void sequencer_1hz(void)              // 0x4201b1e4 (no shop demo, no OTA watchdog)
{
    if (g_oem.power_state != OEM_PWR_BATTERY || g_oem.asleep) return;
    if (g_oem.now_ui == 88) return;
    if (g_oem.dwell_s < 30) g_oem.dwell_s++;
    uint8_t n = g_oem.now_ui, st = g_oem.state, d = g_oem.dwell_s;
    if (n == 71) { if (d == 2) oem_show(72, NULL, 0); }
    else if (n == 72) { if (d == 2) { oem_show(73, NULL, 0); oem_idle_timeout(10); } }
    else if (n == 73) {}
    else if (n == 87) { if (d >= 5) g_oem.now_ui = 82; }
    else if (st <= 1 && (n == 84 || n == 85 || n == 104)) { if (d == 6) oem_show_main(); }
    else if (st == 3 && (n == 83 || n == 84 || n == 100)) {
        if (d == 10) {
            if (s_rtc.hist_count) { oem_show_history(1); g_oem.state = OEM_ST_SCORED; }
            else oem_show_main_after_session();
        }
    } else if (st == 4 && (n == 83 || n == 84 || n == 100)) { if (d == 10) oem_show_main_after_session(); }
    else if (n == 103 && d == 1) oem_show_score(g_oem.score);
}

// ---- driver -------------------------------------------------------------------------
static void pump(void)
{
    while (s_ui_bits) {
        uint32_t b = s_ui_bits;
        s_ui_bits = 0;
        if (oem_ui_handle(b)) hal_lcd_blit(s_fb);
    }
}
static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        s_ms += 10;
        if (s_ms % 50 == 0) s_ui_bits |= OEM_UIEV_BLINK;
        if (s_ms % 1000 == 0) { sequencer_1hz(); session_clock_1hz(); }
        main_step();
        pump();
    }
}
static void post(uint16_t id) { oem_ui_post(id, NULL, 0); pump(); main_step(); pump(); }
static void show(uint8_t scr, int p0) { uint8_t p = (uint8_t)p0; oem_show(scr, p0 < 0 ? NULL : &p, p0 >= 0); pump(); }

static char s_group[16] = "g";
static int s_seq;
static void group(const char *g) { snprintf(s_group, sizeof s_group, "%s", g); s_seq = 0; }
static void snap(const char *fmt, ...)
{
    char name[96], path[600];
    va_list ap; va_start(ap, fmt); vsnprintf(name, sizeof name, fmt, ap); va_end(ap);
    snprintf(path, sizeof path, "%s/%s_%02d_%s.ppm", s_out, s_group, s_seq++, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
    for (int i = 0; i < UI_W * UI_H; i++) {
        unsigned v = (s_panel[2 * i] << 8) | s_panel[2 * i + 1];
        unsigned char px[3] = { ((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31 };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void reset_world(void)
{
    memset(&g_oem, 0, sizeof g_oem);
    memset(&s_rtc, 0, sizeof s_rtc);
    g_oem.now_ui = 0xff; g_oem.saved_mode = 0xff; g_oem.score = 0xff;
    g_oem.power_state = OEM_PWR_BATTERY; g_oem.batt_pct = 80;
    g_oem.dev_mode = 1; g_oem.profile[6] = g_oem.profile[7] = 5; g_oem.profile[1] = 2;
    g_oem.lang = 2; g_oem.sys[0x0c] = 2; g_oem.wifi_status = 1;
    g_oem.birthday_month = g_oem.birthday_day = 0xff;
    strcpy(g_oem.fw_version, "1.2.3.4");
    s_rtc.strength = 3; s_rtc.hist_count = 1;
    s_ui_bits = 0; s_idle = -1;
    memset(s_panel, 0, sizeof s_panel);
    oem_ui_init(s_fb);
}

// wake() 0x4201bd70, UI part: wake screen, then "screen on"
static void wake(void)
{
    g_oem.state = OEM_ST_WOKEN; g_oem.asleep = 0;
    oem_idle_timeout(30);
    oem_show_history(0);
    oem_ui_post(0, NULL, 0);
    pump();
}

static void to_mode_page(void) { wake(); run(6200); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s res.bin outdir [-v]\n", argv[0]); return 2; }
    s_res = fopen(argv[1], "rb");
    if (!s_res) { perror(argv[1]); return 1; }
    s_out = argv[2];
    s_verbose = argc > 3;

    // ---- A: wake screen -> mode page, drawing gate, blit rate -------------------------
    group("a_wake");
    reset_world();
    CHECK(oem_ui_enabled() && oem_ui_now() == 0xff);
    CHECK(!oem_ui_handle(OEM_UIEV_BLINK));                   // nothing selected: no frame
    oem_ui_set_enabled(false);                               // as left by the screen-off sequence
    wake();
    CHECK(s_lcd_inits == 1 && oem_ui_now() == 84 && s_blits == 1);   // drawn once, after "screen on"
    snap("wake84_f0");
    run(300); snap("wake84_300ms");
    run(700); snap("wake84_1s");
    { int b = s_blits; run(2000); CHECK(s_blits - b == 40); }  // frame 13 holds; stock keeps invalidating it
    run(3200);
    CHECK(g_oem.now_ui == 80 && oem_ui_now() == 80 && s_idle == 30);
    snap("mode80");
    { int b = s_blits; run(2000); CHECK(s_blits - b == 4); }   // status icon re-evaluated every 500 ms
    // screen off: message 1 after the immediate disable
    oem_ui_post(1, NULL, 0); oem_ui_set_enabled(false); pump();
    CHECK(!oem_ui_enabled() && s_lcd_sleeps == 1);
    { int b = s_blits; run(1000); CHECK(s_blits == b); }
    // gesture end only on battery
    oem_ui_handle(OEM_UIEV_GESTURE); CHECK(s_gestures == 1);
    g_oem.power_state = OEM_PWR_CHARGING; oem_ui_handle(OEM_UIEV_GESTURE); CHECK(s_gestures == 1);
    g_oem.power_state = OEM_PWR_BATTERY;
    oem_ui_handle(OEM_UIEV_FACTORY); CHECK(s_factory == 1);
    // queue: 10 slots, FIFO, one message per pass
    for (int i = 0; i < 14; i++) oem_ui_post(11, NULL, 0);
    { int passes = 0; while (s_ui_bits) { uint32_t b = s_ui_bits; s_ui_bits = 0; oem_ui_handle(b); passes++; } CHECK(passes == 10); }

    // ---- B: vertical ring, both directions, with and without the app profile ---------
    group("b_ring");
    reset_world(); to_mode_page();
    static const uint8_t UP[] = { 79, 77, 76, 78, 80 }, DOWN[] = { 78, 76, 77, 79, 80 };
    static const uint8_t MODE_OF[] = { 3, 2, 4, 1, 5, 0 };
    for (int i = 0; i < 5; i++) {
        post(10);
        CHECK(g_oem.now_ui == UP[i] && oem_ui_now() == UP[i] && oem_mode() == MODE_OF[UP[i] - 76]);
        snap("up_%d", UP[i]);
    }
    for (int i = 0; i < 5; i++) { post(8); CHECK(g_oem.now_ui == DOWN[i] && oem_mode() == MODE_OF[DOWN[i] - 76]); }
    CHECK(s_idle == 30);
    g_oem.profile[0] = 1;                                    // app profile: page 81 joins the ring
    post(8); CHECK(g_oem.now_ui == 81 && oem_mode() == 0); snap("down_81_app");
    post(8); CHECK(g_oem.now_ui == 78);
    post(10); CHECK(g_oem.now_ui == 81);
    g_oem.profile[1] = 1; g_oem.tod_sub = 2; show(81, 2); snap("81_sub2");
    g_oem.tod_sub = 3; show(81, 3); snap("81_sub3");
    g_oem.profile[2] = 1; show(81, 4); snap("81_sub4");
    g_oem.profile[0] = 0; g_oem.profile[1] = 2; g_oem.profile[2] = 0; show(81, 1); snap("81_alt_list");
    post(10); CHECK(g_oem.now_ui == 80);
    for (int l = 0; l <= 16; l += 8) {                       // mode pages in other languages
        g_oem.lang = (uint8_t)l;
        for (int scr = 76; scr <= 80; scr++) { show((uint8_t)scr, -1); snap("lang%d_%d", l, scr); }
    }

    // ---- C: horizontal ring, side pages, clock variants ------------------------------
    group("c_side");
    reset_world(); to_mode_page();
    g_oem.score = 87; g_oem.state = OEM_ST_WOKEN;
    for (int i = 0; i < 12; i++) g_oem.zone_s[i] = (uint16_t)(i % 3 == 0 ? 0 : i % 3 == 1 ? 3 : 12);
    post(9); CHECK(g_oem.now_ui == 96 && g_oem.subpage == 1); snap("right_96_mode3");
    post(9); CHECK(g_oem.now_ui == 100 && oem_ui_now() == 100); snap("right_100_score87");
    post(9); CHECK(g_oem.now_ui == 80 && g_oem.subpage == 0); snap("right_80");
    post(7); CHECK(g_oem.now_ui == 100);
    post(7); CHECK(g_oem.now_ui == 96);
    post(7); CHECK(g_oem.now_ui == 80);
    post(7); post(10); CHECK(g_oem.now_ui == 80 && g_oem.subpage == 0);      // swipe up on a side page
    post(9); short_press(); pump(); CHECK(g_oem.now_ui == 80 && !g_oem.session_active);   // button on a side page
    g_oem.score = 95; post(7); run(100); snap("left_100_score95");
    g_oem.score = 100; show(100, 100); snap("100_perfect_f0");
    { int b = s_blits; run(1000); CHECK(s_blits - b == 20); }  // animation: invalidated on every tick
    snap("100_perfect_1s"); run(1500); snap("100_perfect_hold");
    { int b = s_blits; run(1000); CHECK(s_blits == b); }       // frame 19 reached: static
    g_oem.score = 42; show(100, 42); snap("100_score42");
    post(7); CHECK(g_oem.now_ui == 96);
    g_oem.clock_mode = 2; run(100); snap("96_mode2_0941");
    s_time.hour = 23; s_time.min = 5; run(100); snap("96_mode2_2305");
    g_oem.clock_mode = 1; run(100); snap("96_loading_a"); run(300); snap("96_loading_b");
    g_oem.weather_code = 3; g_oem.weather_t1 = 12; g_oem.weather_t2 = -5; g_oem.clock_mode = 0; run(100); snap("96_w_-5_12");
    post(7); post(9);                                          // leave and re-enter
    g_oem.weather_code = 0; g_oem.weather_t1 = 25; g_oem.weather_t2 = 18; g_oem.weather_flag = 1; post(9); post(7); snap("96_w_18_25");
    post(7); g_oem.weather_code = 6; g_oem.weather_t1 = -3; g_oem.weather_t2 = -12; post(9); snap("96_w_-12_-3");
    post(7); g_oem.weather_t1 = 7; g_oem.weather_t2 = 2; post(9); snap("96_w_2_7");
    post(10); CHECK(g_oem.now_ui == 80);
    post(9); CHECK(g_oem.now_ui == 96); post(8); CHECK(g_oem.now_ui == 80);

    // ---- D: brushing with countdown: 82, pause 101, done 103, score 100, history 84 ---
    group("d_brush");
    reset_world(); to_mode_page();
    post(10); post(10); post(10);                              // mode 3 (screen 76)
    CHECK(oem_mode() == 3);
    short_press(); pump(); CHECK(g_oem.now_ui == 82 && oem_ui_now() == 82); snap("82_start");
    post(10); CHECK(g_oem.now_ui == 82 && s_touch_state == 6);   // swipe while running: strength step, no page change
    { int b = s_blits; run(3000); CHECK(s_blits - b == 3); }     // one frame per second
    run(27000); snap("82_30s");
    short_press(); pump(); CHECK(g_oem.now_ui == 101 && oem_ui_now() == 101); snap("101_pause_30s");
    run(3000);
    short_press(); pump(); CHECK(g_oem.now_ui == 82);
    run(30000); snap("82_60s");
    run(50000); snap("82_110s");
    run(9000); snap("82_119s");
    run(1000); CHECK(g_oem.now_ui == 103 && oem_ui_now() == 103); snap("103_a");
    run(100); snap("103_b"); run(150); snap("103_c");
    { int b = s_blits; run(500); CHECK(s_blits - b == 10); }   // 50 ms per frame
    run(250); CHECK(g_oem.now_ui == 100 && oem_ui_now() == 100); snap("100_after");
    run(10000); CHECK(g_oem.now_ui == 84 && g_oem.state == OEM_ST_SCORED); run(1000); snap("84_after");
    run(9000); CHECK(g_oem.now_ui == 76 && s_idle == 10); snap("mode76_after");
    // quit a paused session before 2 minutes: straight to the mode page
    short_press(); run(20000); short_press(); pump(); CHECK(g_oem.now_ui == 101);
    { int st = s_stops; post(8); CHECK(s_stops == st + 1 && g_oem.user_quit == 1 && !g_oem.session_active); }
    CHECK(g_oem.now_ui == 76 && oem_ui_now() == 76); snap("quit_lt120");

    // ---- E: mode 5: intensity 87, count-up, quit after 2 minutes ---------------------
    group("e_mode5");
    reset_world(); to_mode_page();
    short_press(); pump(); CHECK(g_oem.now_ui == 87 && oem_ui_now() == 87); snap("87_level3");
    post(10); CHECK(s_rtc.strength == 4); snap("87_level4");
    post(10); post(10); CHECK(s_rtc.strength == 5); snap("87_level5");
    post(8); post(8); post(8); post(8); post(8); CHECK(s_rtc.strength == 1); snap("87_level1");
    run(6000); CHECK(g_oem.now_ui == 82 && oem_ui_now() == 82); snap("82_m5_a");
    run(1000); snap("82_m5_b"); run(1000); snap("82_m5_c"); run(60000); snap("82_m5_68s");
    run(60000);
    short_press(); pump(); CHECK(g_oem.now_ui == 101); snap("101_m5_128s");
    post(10);                                                 // quit after >= 120 s: done -> score
    CHECK(!g_oem.session_active && g_oem.now_ui == 103); run(100); snap("103_m5");
    run(1000); CHECK(oem_ui_now() == 100); snap("100_m5");

    // ---- F: charging 93 / battery 120 / low battery 94 --------------------------------
    group("f_batt");
    reset_world();
    g_oem.power_state = OEM_PWR_CHARGING;
    static const uint8_t PCT[] = { 0, 5, 10, 15, 16, 31, 45, 46, 61, 81, 99, 100 };
    for (unsigned i = 0; i < sizeof PCT; i++) {
        g_oem.batt_pct = PCT[i];
        show(93, PCT[i]); run(300); snap("93_%d", PCT[i]);
    }
    g_oem.batt_pct = 50; show(93, 50); run(250); snap("93_50_a"); run(500); snap("93_50_b");
    { int b = s_blits; run(1000); CHECK(s_blits - b == 12); }   // strip every 100 ms + battery every 250 ms
    // backlight dark: no frame, the layout still follows batt_pct; light back on: one fresh frame at the next pass
    { int b = s_blits; s_backlight = false; g_oem.batt_pct = 51; run(1000); CHECK(s_blits == b);
      s_backlight = true; run(50); CHECK(s_blits == b + 1); snap("93_51_relit");
      b = s_blits; run(1000); CHECK(s_blits - b == 12); g_oem.batt_pct = 50; run(300); }
    // 120 after the charger was removed: the digits are those the charging layout left
    static const uint8_t PCT2[] = { 0, 7, 15, 45, 81, 100 };
    for (unsigned i = 0; i < sizeof PCT2; i++) {
        g_oem.power_state = OEM_PWR_CHARGING; g_oem.batt_pct = PCT2[i]; run(300);
        g_oem.power_state = OEM_PWR_BATTERY; show(120, -1); snap("120_%d", PCT2[i]);
    }
    g_oem.batt_pct = 100; g_oem.power_state = OEM_PWR_FULL; show(120, -1); run(300); snap("120_100_full");
    g_oem.power_state = OEM_PWR_BATTERY;
    show(94, -1); snap("94_low");

    // ---- G: info pages 92 ---------------------------------------------------------------
    group("g_info");
    reset_world(); to_mode_page();
    g_oem.info_page = 0; show(92, 0); oem_idle_timeout(30); snap("92_p0");
    post(8); pump(); CHECK(g_oem.info_page == 1 && g_oem.now_ui == 92); snap("92_p1");
    post(8); pump(); snap("92_p2");
    post(8); pump(); CHECK(g_oem.info_page == 3); snap("92_p3");
    post(8); pump(); CHECK(g_oem.info_page == 0); snap("92_p0_again");
    post(10); pump(); CHECK(g_oem.info_page == 3);
    short_press(); pump(); CHECK(g_oem.now_ui == 80 && s_idle == 5);

    // ---- H: touch lock: popup 91, blocked swipes, icon, unlock -------------------------
    group("h_lock");
    reset_world(); to_mode_page();
    { uint32_t t0 = s_ms; oem_ui_lock_button(1); CHECK(s_ms - t0 == 400 && s_buzz == 1 && s_idle == 30); }
    CHECK(g_oem.locked == 1 && g_oem.lock_popup == 1 && g_oem.now_ui == 91 && g_oem.saved_screen == 80);
    pump(); CHECK(oem_ui_now() == 91); snap("91_popup");
    run(900); CHECK(g_oem.now_ui == 91);
    run(200); CHECK(g_oem.now_ui == 80 && g_oem.lock_popup == 0 && oem_ui_now() == 80);
    run(400); snap("80_locked_a"); run(500); snap("80_locked_b");
    post(10); CHECK(g_oem.now_ui == 91 && g_oem.saved_screen == 80);        // blocked swipe
    run(500); short_press(); pump(); CHECK(g_oem.now_ui == 80 && !g_oem.lock_popup && !g_oem.session_active);
    run(2000); CHECK(g_oem.now_ui == 80);                                    // no second restore later
    post(9); CHECK(g_oem.now_ui == 91); run(1100); CHECK(g_oem.now_ui == 80 && g_oem.subpage == 0);
    short_press(); pump(); CHECK(g_oem.now_ui == 87);                        // the button still works
    post(10); CHECK(g_oem.now_ui == 91 && g_oem.saved_screen == 87);
    run(1100); CHECK(g_oem.now_ui == 87 && s_rtc.strength == 3);
    run(6000); CHECK(g_oem.now_ui == 82);
    oem_ui_lock_button(1); CHECK(g_oem.locked == 1);                         // no unlock while brushing runs
    post(8); CHECK(g_oem.now_ui == 91 && g_oem.saved_screen == 82);
    run(2100); CHECK(g_oem.now_ui == 82);                                    // 101, then the 1 Hz refresh
    short_press(); pump(); CHECK(g_oem.now_ui == 101); run(600); snap("101_locked");
    post(8); CHECK(g_oem.now_ui == 101 && g_oem.session_active);             // 101 is not in the popup list: ignored
    short_press(); pump(); short_press(); pump();                            // resume, pause again
    g_oem.now_ui = 82; s_buzz = 0;                                           // (as left by a popup restore) paused on 82
    oem_ui_lock_button(1); CHECK(g_oem.locked == 0 && s_buzz == 1);
    oem_motor_stop(); run(200);
    show(80, -1); run(600); snap("80_unlocked");
    g_oem.sys[0x0c] = 0; run(1100); snap("80_unbound_icon");
    oem_ui_lock_button(1); run(1200);                          // locked + network icon: alternate at 1 Hz
    while (s_ms % 1000 != 300) run(10);
    snap("80_locked_unbound_a"); run(500); snap("80_locked_unbound_b");
    oem_ui_page_reset(); post(9); CHECK(g_oem.now_ui == 91);   // still locked

    // ---- I: OTA screens -----------------------------------------------------------------
    group("i_ota");
    reset_world(); oem_ui_post(0, NULL, 0); pump();
    strcpy(g_oem.ota_version, "2.0.1.7");
    show(97, -1); snap("97_prompt");
    show(88, 0); snap("88_0"); show(88, 55); run(100); snap("88_55"); show(88, 100); run(100); snap("88_100");
    show(89, -1); snap("89_ok"); show(90, -1); snap("90_fail");
    g_oem.lang = 9; show(88, 30); snap("88_30_lang9"); show(89, -1); snap("89_lang9"); show(91, -1); snap("91_lang9");

    // ---- J: boot animation, pairing guide, greetings, custom picture ------------------
    group("j_misc");
    reset_world();
    g_oem.sys[0x34] = 0; show(70, -1); snap("70_f0"); run(1000); snap("70_1s");
    run(1000); CHECK(g_oem.now_ui == 80 && s_idle == 60); snap("70_done_80");
    reset_world(); g_oem.sys[0x0c] = 0; g_oem.sys[0x34] = 1;
    show(70, -1); run(2000); CHECK(g_oem.now_ui == 71); snap("71");
    run(2000); CHECK(g_oem.now_ui == 72); snap("72"); run(2000); CHECK(g_oem.now_ui == 73); snap("73");
    post(8); CHECK(g_oem.now_ui == 72); post(10); post(10); CHECK(g_oem.now_ui == 80 && s_idle == 30);
    reset_world(); g_oem.reset_flag = 1; show(70, -1); run(3000); CHECK(g_oem.now_ui == 70); snap("70_first_boot_holds");
    reset_world(); g_oem.state = OEM_ST_WOKEN;
    static const uint8_t GREET[] = { 1, 2, 3, 5, 10 };
    for (unsigned i = 0; i < sizeof GREET; i++) { show(85, GREET[i]); snap("85_id%d", GREET[i]); }
    g_oem.birthday_month = 3; g_oem.birthday_day = 7; show(85, 4); snap("85_bday_3-7");
    g_oem.birthday_month = 3; g_oem.birthday_day = 17; show(85, 4); snap("85_bday_3-17");
    g_oem.birthday_month = 11; g_oem.birthday_day = 7; show(85, 4); snap("85_bday_11-7");
    g_oem.birthday_month = 12; g_oem.birthday_day = 24; show(85, 4); snap("85_bday_12-24");
    show(85, 1); snap("85_id1_again");
    post(10); CHECK(g_oem.now_ui == 80);                       // wake screen: swipe goes to the mode page
    show(104, -1); snap("104_custom");
    show(98, -1); snap("98"); show(83, 93); snap("83_alt_score");
    show(106, 1); snap("106_factory"); show(117, -1); snap("117_factory");
    g_oem.dev_mode = 2; show(80, -1); snap("80_dev_overlay"); g_oem.dev_mode = 1;
    show(96, -1); CHECK(oem_ui_now() == 80);                   // 96 is not a message
    show(86, -1); CHECK(oem_ui_now() == 80);

    // ---- K: inputs stock does not guard --------------------------------------------------
    group("k_guard");
    reset_world(); to_mode_page();
    post(10); short_press(); pump();                           // mode 1 session
    g_oem.total_s = 10; g_oem.done_s = 4; oem_show_brushing(); pump(); snap("82_total10");   // total / 15 == 0
    g_oem.total_s = 700; g_oem.done_s = 30; oem_show_brushing(); pump(); snap("82_11min");
    oem_motor_stop(); run(100);
    strcpy(g_oem.fw_version, "v1.x"); show(92, 0); snap("92_bad_version");
    g_oem.now_ui = 80; g_oem.subpage = 0; show(80, -1);
    g_oem.clock_mode = 0; g_oem.weather_code = 9; g_oem.weather_t1 = 100; g_oem.weather_t2 = -100;
    post(9); CHECK(g_oem.now_ui == 96); snap("96_w_-100_100_code9");
    g_oem.weather_t1 = 101; post(9); post(7); snap("96_w_out_of_range");
    g_oem.score = 0xff; post(9); CHECK(g_oem.now_ui == 100); run(100); snap("100_no_score_yet");

    printf("%d checks, %d failed, %d blits\n", s_checks, s_fail, s_blits);
    return s_fail != 0;
}

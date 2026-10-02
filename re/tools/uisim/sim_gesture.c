// Host check of the touch gesture decoder (main/oem_gesture.c).
//   cc -std=gnu11 -Wall -Wextra -I main re/tools/uisim/sim_gesture.c main/oem_gesture.c -o sim_gesture && ./sim_gesture
//
// Feeds synthetic trackpad reports (raw X / Y as the IQS7222D delivers them, 0..255,
// 0xFFFF = no finger) into the per-sample function, fires the 70 ms end-of-touch
// event and checks which UI message comes out:
//   7 swipe left, 8 swipe down, 9 swipe right, 10 swipe up, 11 long touch.
// The decoder works on x = 255 - X and y = 255 - Y; x is the up / down axis.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_input.h"
#include "oem_state.h"

oem_state_t g_oem;
static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- fakes for what oem_gesture.c calls --------------------------------------------
static struct { uint16_t id; int len; uint8_t payload; } g_msg[16];
static int g_msgs, g_timer_starts, g_timer_ms, g_timer_periodic, g_idle_kicks;

void oem_ui_post(uint16_t id, const void *payload, int len)
{
    if (g_msgs < 16) {
        g_msg[g_msgs].id = id;
        g_msg[g_msgs].len = len;
        g_msg[g_msgs].payload = (payload && len > 0) ? *(const uint8_t *)payload : 0xEE;
    }
    g_msgs++;
}
void hal_timer_start(hal_timer_t t, uint32_t ms, bool periodic)
{
    if (t == HAL_TMR_GESTURE70) { g_timer_starts++; g_timer_ms = (int)ms; g_timer_periodic = periodic; }
}
void oem_idle_kick(void) { g_idle_kicks++; }

// ---- helpers -----------------------------------------------------------------------
static void fresh(void)
{
    // finish whatever a previous case left behind, on battery so that the state resets
    memset(&g_oem, 0, sizeof g_oem);
    g_oem.power_state = OEM_PWR_BATTERY;
    oem_gesture_end();
    g_oem.now_ui = 80;
    g_oem.stop_delay = 200;          // stock value outside a paused session
    g_msgs = g_timer_starts = g_idle_kicks = 0;
}

// n reports moving linearly from (x0, y0) to (x1, y1) in decoder coordinates
static void stroke(int n, int x0, int y0, int x1, int y1)
{
    for (int i = 0; i < n; i++) {
        int x = n > 1 ? x0 + (x1 - x0) * i / (n - 1) : x0;
        int y = n > 1 ? y0 + (y1 - y0) * i / (n - 1) : y0;
        oem_gesture_sample((uint16_t)(255 - x), (uint16_t)(255 - y));
    }
}
static void points(const int *x, const int *y, int n)
{
    for (int i = 0; i < n; i++) oem_gesture_sample((uint16_t)(255 - x[i]), (uint16_t)(255 - y[i]));
}
static void lift(void) { oem_gesture_sample(0xFFFF, 0xFFFF); }

// Expect exactly one message `id` (0 = none) after the end-of-touch event.
static void expect(const char *what, int id)
{
    oem_gesture_end();
    if (id == 0) CHECK(g_msgs == 0, "%s: expected no message, got %d (first id %d)", what, g_msgs, g_msg[0].id);
    else CHECK(g_msgs == 1 && g_msg[0].id == id, "%s: expected message %d, got %d message(s), first id %d",
               what, id, g_msgs, g_msgs ? g_msg[0].id : 0);
}

static void test_directions(void)
{
    printf("swipe directions\n");
    fresh(); stroke(12, 60, 120, 180, 120); lift();
    expect("x rising", 10);
    CHECK(g_msg[0].len == 1 && g_msg[0].payload == 120, "swipe up carries start_y (got %d, len %d)", g_msg[0].payload, g_msg[0].len);
    CHECK(g_idle_kicks == 1, "idle timer restarted once (%d)", g_idle_kicks);
    fresh(); stroke(12, 180, 120, 60, 120); lift();
    expect("x falling", 8);
    CHECK(g_msg[0].len == 1 && g_msg[0].payload == 120, "swipe down carries start_y");
    fresh(); stroke(12, 120, 60, 120, 180); lift();
    expect("y rising", 9);
    CHECK(g_msg[0].len == 0, "swipe right has no payload");
    fresh(); stroke(12, 120, 180, 120, 60); lift();
    expect("y falling", 7);
    // raw coordinates: the chip's X counts against the decoder's x
    fresh();
    for (int i = 0; i < 10; i++) oem_gesture_sample((uint16_t)(200 - 10 * i), 128);
    expect("raw X falling = swipe up", 10);
    fresh();
    for (int i = 0; i < 10; i++) oem_gesture_sample(128, (uint16_t)(60 + 10 * i));
    expect("raw Y rising = swipe left", 7);
}

static void test_sample_count(void)
{
    printf("sample count and timer\n");
    fresh(); stroke(5, 60, 120, 180, 120);
    expect("5 samples", 0);
    CHECK(g_idle_kicks == 0, "too short a touch does not restart the idle timer");
    fresh(); stroke(6, 60, 120, 180, 120);
    expect("6 samples", 10);
    fresh(); stroke(9, 60, 120, 180, 120);
    CHECK(g_timer_starts == 9 && g_timer_ms == 70 && !g_timer_periodic, "70 ms one-shot restarted per sample (%d, %d ms)",
          g_timer_starts, g_timer_ms);
    CHECK(oem_gesture_touching(), "finger reported while touching");
    lift();
    CHECK(g_timer_starts == 9, "a no-finger report does not restart the timer");
    CHECK(!oem_gesture_touching(), "no finger after the lift report");
    expect("after lift", 10);
    // no-finger needs both coordinates above 0xEA5F
    fresh(); stroke(8, 60, 120, 180, 120);
    oem_gesture_sample(0xFFFF, 100);
    CHECK(oem_gesture_touching() && g_timer_starts == 9, "X alone out of range still counts as a sample");
    // the state is cleared by the end event: a second, short touch gives nothing
    fresh(); stroke(10, 60, 120, 180, 120); expect("first", 10);
    g_msgs = 0; stroke(4, 60, 120, 180, 120); expect("second, 4 samples", 0);
}

static void test_travel(void)
{
    printf("travel thresholds (the x extreme lags one sample)\n");
    { fresh(); int x[] = {100, 100, 100, 100, 100, 120}, y[] = {50, 50, 50, 50, 50, 50}; points(x, y, 6);
      expect("x step only in the last sample", 0); }
    { fresh(); int x[] = {100, 100, 100, 100, 120, 120}, y[] = {50, 50, 50, 50, 50, 50}; points(x, y, 6);
      expect("x travel 20 seen", 10); }
    { fresh(); int x[] = {100, 100, 100, 100, 119, 119}, y[] = {50, 50, 50, 50, 50, 50}; points(x, y, 6);
      expect("x travel 19", 0); }
    { fresh(); int x[] = {100, 100, 100, 100, 80, 80}, y[] = {50, 50, 50, 50, 50, 50}; points(x, y, 6);
      expect("x travel -20", 8); }
    { fresh(); int x[] = {100, 100, 100, 100, 100, 100}, y[] = {50, 50, 50, 50, 50, 70}; points(x, y, 6);
      expect("y travel 20 (current sample counts)", 9); }
    { fresh(); int x[] = {100, 100, 100, 100, 100, 100}, y[] = {50, 50, 50, 50, 50, 69}; points(x, y, 6);
      expect("y travel 19", 0); }
    { fresh(); int x[] = {100, 100, 100, 100, 100, 100}, y[] = {50, 50, 50, 50, 50, 30}; points(x, y, 6);
      expect("y travel -20", 7); }
    // the start position is the second sample
    { fresh(); int x[] = {10, 100, 100, 100, 100, 100, 100}, y[] = {50, 50, 50, 50, 50, 50, 50}; points(x, y, 7);
      expect("jump before sample 2 is not travel", 0); }
    // out and back: the farthest excursion counts for x, the end position for y
    { fresh(); int x[] = {100, 100, 110, 140, 140, 110, 100, 100}, y[] = {50, 50, 50, 50, 50, 50, 50, 50}; points(x, y, 8);
      expect("x out and back", 10); }
    { fresh(); int x[] = {100, 100, 100, 100, 100, 100, 100, 100}, y[] = {50, 50, 60, 90, 90, 60, 50, 50}; points(x, y, 8);
      expect("y out and back", 0); }
}

// dx / dy combinations at the end of a 10-sample touch. x reaches its extreme two
// samples before the end so the lag does not matter.
static void combo(const char *what, int dx, int dy, int id)
{
    fresh();
    int x[10], y[10];
    for (int i = 0; i < 10; i++) {
        x[i] = 120 + (i >= 6 ? dx : 0);
        y[i] = 120 + (i >= 6 ? dy : 0);
    }
    points(x, y, 10);
    expect(what, id);
}

static void test_cross_axis(void)
{
    printf("cross-axis cases\n");
    combo("dx 30 dy 25: mostly x", 30, 25, 10);
    combo("dx -30 dy 25", -30, 25, 8);
    combo("dx 30 dy 30: tie", 30, 30, 0);
    combo("dx 30 dy 40: mostly y, x within 49", 30, 40, 9);
    combo("dx 30 dy -40", 30, -40, 7);
    combo("dx 49 dy 60", 49, 60, 9);
    combo("dx 50 dy 60: x beyond 49", 50, 60, 0);
    combo("dx -50 dy -60", -50, -60, 0);
    combo("dx 19 dy 20", 19, 20, 9);
    combo("dx 19 dy 19", 19, 19, 0);
    combo("dx 19 dy -25", 19, -25, 7);
    combo("dx 60 dy 59", 60, 59, 10);
    combo("dx 20 dy 0", 20, 0, 10);
    combo("dx 0 dy 20", 0, 20, 9);
}

static void test_long_touch(void)
{
    printf("long touch\n");
    fresh(); stroke(79, 100, 100, 100, 100);
    CHECK(g_msgs == 0, "nothing before sample 80");
    stroke(1, 100, 100, 100, 100);
    CHECK(g_msgs == 1 && g_msg[0].id == 11 && g_msg[0].len == 1 && g_msg[0].payload == 2, "message 11 {2} at sample 80 (%d)", g_msgs);
    stroke(200, 100, 100, 100, 100);
    CHECK(g_msgs == 1, "reported once per touch (%d)", g_msgs);
    g_msgs = 0;
    expect("end of a long touch", 0);
    // with 50 or more of x travel there is no long touch, and the swipe is reported at the end
    fresh(); stroke(40, 60, 100, 160, 100); stroke(60, 160, 100, 160, 100);
    CHECK(g_msgs == 0, "no long touch after 100 samples with x travel");
    expect("long swipe", 10);
    // the finger moves away just before sample 80 (the extreme lags: seen from sample 80 on)
    fresh(); stroke(78, 100, 100, 100, 100);
    { int x[] = {160, 160, 160}, y[] = {100, 100, 100}; points(x, y, 3); }
    CHECK(g_msgs == 0, "sample 80 passed with travel: no long touch");
    // ... one sample later it is too late for the excursion to count
    fresh(); stroke(79, 100, 100, 100, 100);
    { int x[] = {160, 160, 160}, y[] = {100, 100, 100}; points(x, y, 3); }
    CHECK(g_msgs == 1 && g_msg[0].id == 11, "travel first seen at sample 81: long touch at 80");
    // a swipe after a long touch in the same contact is not reported
    fresh(); stroke(80, 100, 100, 100, 100); g_msgs = 0; stroke(10, 100, 100, 100, 200);
    expect("swipe after the long touch fired", 0);
}

static void test_gates(void)
{
    printf("gates\n");
    fresh(); g_oem.sys[0x69] = 1; stroke(10, 60, 120, 180, 120);
    expect("touch switched off (sys[0x69])", 0);
    fresh(); g_oem.now_ui = 94; stroke(10, 60, 120, 180, 120);
    expect("low-battery screen", 0);
    CHECK(g_idle_kicks == 0, "no idle restart on the low-battery screen");
    fresh(); g_oem.reset_flag = 1; stroke(10, 60, 120, 180, 120);
    expect("after factory reset: no up/down", 0);
    fresh(); g_oem.reset_flag = 1; stroke(10, 120, 60, 120, 180);
    expect("after factory reset: left/right still work", 9);

    // on the charger the event is dropped and nothing is reset
    fresh(); stroke(4, 100, 100, 100, 100);
    g_oem.power_state = OEM_PWR_CHARGING;
    expect("on the charger", 0);
    g_oem.power_state = OEM_PWR_BATTERY;
    { int x[] = {130, 130}, y[] = {100, 100}; points(x, y, 2); }
    expect("count carried over from before", 10);
}

static void test_brushing(void)
{
    printf("during a session\n");
    // motor running: the swipe is reported with payload 0 (strength step in mode 5)
    fresh(); g_oem.session_active = 1; g_oem.stop_delay = 200; stroke(10, 60, 120, 180, 120);
    expect("running", 10);
    CHECK(g_msg[0].payload == 0, "payload 0 while brushing (%d)", g_msg[0].payload);
    // touched while running, paused before the touch ended: dropped
    fresh(); g_oem.session_active = 1; g_oem.stop_delay = 200; stroke(10, 60, 120, 180, 120);
    g_oem.stop_delay = 0;
    expect("touch began while running, ended paused", 0);
    CHECK(g_idle_kicks == 1, "idle timer is restarted all the same");
    // paused all along: reported
    fresh(); g_oem.session_active = 1; g_oem.stop_delay = 5; stroke(10, 180, 120, 60, 120);
    expect("paused", 8);
    CHECK(g_msg[0].payload == 0, "payload 0 while paused");
    // the "was running" mark is cleared by the end event
    fresh(); g_oem.session_active = 1; g_oem.stop_delay = 200; stroke(10, 60, 120, 180, 120);
    g_oem.stop_delay = 0; expect("dropped", 0);
    g_msgs = 0; stroke(10, 120, 60, 120, 180);
    expect("next touch while paused", 9);
    // pause counter 30 still counts as paused, 31 not (stock 0x420197e8)
    fresh(); g_oem.session_active = 1; g_oem.stop_delay = 200; stroke(10, 60, 120, 180, 120);
    g_oem.stop_delay = 31;
    expect("stop_delay 31 is not 'paused'", 10);
}

int main(void)
{
    test_directions();
    test_sample_count();
    test_travel();
    test_cross_axis();
    test_long_touch();
    test_gates();
    test_brushing();
    if (g_fail) printf("\n%d of %d checks FAILED\n", g_fail, g_checks);
    else        printf("\nall %d checks passed\n", g_checks);
    return g_fail != 0;
}

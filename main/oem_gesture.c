#include <stdlib.h>
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_input.h"
#include "oem_state.h"

// Touch gesture decoder (input.md section 2). The IQS7222D only delivers X / Y;
// swipes are recognised here. oem_gesture_sample() runs in the main task for every
// trackpad report, oem_gesture_end() in the UI task 70 ms after the last report with
// a finger. Both run under the core lock, like every core function.
//
// Naming follows the stock firmware: the chip's X axis (after the 255 - x inversion)
// is the UI's up / down axis, its Y axis the left / right axis. Which way that is on
// the handle cannot be told from the code.

#define NO_FINGER   60000u
#define RAW_NONE    0xEA5Fu     // both raw coordinates above this: no finger
#define MIN_SAMPLES 6
#define SWIPE_MIN   20          // travel on the 0..255 axis
#define CROSS_MAX   49          // largest up/down travel that still allows left/right
#define LONG_COUNT  80          // samples until a still finger counts as a long touch

static uint32_t s_x, s_y;                 // 0x3fca5a3c / 0x3fca5a40: 255 - raw; NO_FINGER when lifted
static uint16_t s_hist[3];                // 0x3fca5a1a: x of this sample, the one before, two before
static uint16_t s_count;                  // 0x3fca5a32: samples in this touch
static int16_t  s_start_x, s_start_y;     // 0x3fca5a26 / 0x3fca5a28: position of sample 2
static int16_t  s_min_x = 1000;           // 0x3fc9c480 (lags one sample)
static int16_t  s_max_x;                  // 0x3fca5a24
static int16_t  s_dx, s_dy;               // 0x3fca5a2a / 0x3fca5a2c
static uint8_t  s_long_fired;             // 0x3fca5a20
static uint8_t  s_was_running;            // 0x3fca5a18: a sample arrived while the motor ran

// 0x420197e8: session exists and the motor is paused
static bool brush_paused(void)
{
    return g_oem.stop_delay < 31 && g_oem.session_active;
}

bool oem_gesture_touching(void)
{
    return s_y != NO_FINGER;
}

// 0x42025ca4
void oem_gesture_sample(uint16_t raw_x, uint16_t raw_y)
{
    if (raw_x > RAW_NONE && raw_y > RAW_NONE) {
        s_x = s_y = NO_FINGER;
        return;
    }
    s_x = 255u - raw_x;
    s_y = 255u - raw_y;
    s_hist[2] = s_hist[1];
    s_hist[1] = s_hist[0];
    s_hist[0] = (uint16_t)s_x;
    hal_timer_start(HAL_TMR_GESTURE70, 70, false);      // restart the end-of-touch timer
    s_count++;
    if (g_oem.session_active && !brush_paused()) s_was_running = 1;
    if (s_count < 3) {
        s_start_y = (int16_t)s_y;
        s_start_x = (int16_t)s_x;
        return;
    }
    // Extremes of x. Stock stores the previous sample, not the current one.
    if (s_x < (uint32_t)(int32_t)s_min_x) s_min_x = (int16_t)s_hist[1];
    if ((uint32_t)(int32_t)s_max_x < s_x) s_max_x = (int16_t)s_hist[1];
    int to_min = abs(s_min_x - s_start_x);
    int to_max = abs(s_max_x - s_start_x);
    int16_t ext = (to_max < to_min) ? s_min_x : s_max_x;
    s_dy = (int16_t)((int16_t)s_y - s_start_y);         // current y
    s_dx = (int16_t)(ext - s_start_x);                  // farthest x excursion so far
    if (!s_long_fired && abs(s_dx) <= CROSS_MAX) {
        if (s_count == LONG_COUNT) {
            uint8_t payload = 2;
            oem_ui_post(11, &payload, 1);               // long touch (no effect in the stock UI)
            s_count = 0;
            s_long_fired = 1;
        } else if (s_count > 100) {
            s_count = 0;
        }
    }
}

// 0x42025afc. The stock UI task calls it only while running on battery; on the
// charger the event is dropped and the state below is not reset. The test is
// repeated here so that the behaviour does not depend on the caller.
void oem_gesture_end(void)
{
    if (g_oem.power_state != OEM_PWR_BATTERY) return;

    if (s_count >= MIN_SAMPLES && !s_long_fired && g_oem.sys[0x69] != 1 && g_oem.now_ui != 94) {
        oem_idle_kick();
        bool brushing = g_oem.session_active != 0;
        if (!(brushing && brush_paused() && s_was_running == 1)) {
            unsigned adx = (unsigned)abs(s_dx), ady = (unsigned)abs(s_dy);
            // payload byte: 0 while brushing, else the low byte of start_y (unused by the UI)
            uint8_t p = brushing ? 0 : (uint8_t)s_start_y;
            if (adx >= SWIPE_MIN && !g_oem.reset_flag && ady < adx) {
                if (s_dx > 0)      oem_ui_post(10, &p, 1);          // swipe up
                else if (s_dx < 0) oem_ui_post(8, &p, 1);           // swipe down
            } else if ((adx < SWIPE_MIN && ady >= SWIPE_MIN) ||
                       (adx >= SWIPE_MIN && adx <= CROSS_MAX && ady > adx)) {
                if (s_dy > 0)      oem_ui_post(9, NULL, 0);         // swipe right
                else if (s_dy < 0) oem_ui_post(7, NULL, 0);         // swipe left
            }
        }
    }
    s_count = 0;
    s_was_running = 0;
    s_min_x = 1000;
    s_long_fired = 0;
    s_max_x = 0;
}

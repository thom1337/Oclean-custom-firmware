// Motor waveform: gear table, generator and playback logic of the stock firmware
// (re/spec/brushing.md section 1). Platform independent; hw_motor.c owns the I2S
// channel, the task and the amp pin.
//
// The brush head is a voice coil behind an audio amplifier. Stock streams a
// synthesised wave to it: one period is computed (table wave or sine, scaled by the
// amplitude 0..50, full scale 16383 = half of int16), sent 9 times, then the next
// period is computed. Amplitude can therefore only change every 9 periods.
#include "oem_wave.h"
#include <math.h>
#include <string.h>
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_state.h"

// ---- stock tables ----------------------------------------------------------------

// rodata 0x3c11ad93: one period, 100 points, peak +-98 (a flattened sine)
static const int8_t oc_wave100[100] = {
       0,   18,   33,   45,   55,   63,   70,   75,   79,   83,
      86,   88,   90,   92,   93,   94,   95,   96,   97,   97,
      97,   98,   98,   98,   98,   98,   98,   98,   98,   98,
      97,   97,   97,   96,   95,   94,   93,   92,   90,   88,
      86,   83,   79,   75,   69,   63,   54,   44,   32,   17,
       0,  -18,  -33,  -45,  -55,  -63,  -70,  -75,  -79,  -83,
     -86,  -88,  -90,  -92,  -93,  -94,  -95,  -96,  -97,  -97,
     -97,  -98,  -98,  -98,  -98,  -98,  -98,  -98,  -98,  -98,
     -97,  -97,  -97,  -96,  -95,  -94,  -93,  -92,  -90,  -88,
     -86,  -83,  -79,  -75,  -69,  -63,  -54,  -44,  -32,  -17,
};

// rodata 0x3c11abf3: swell step sizes
static const uint8_t oc_swell_step[3] = { 1, 2, 1 };

// rodata 0x3c1194b8, index = gear id - 1. freq = f10 * 10 + frac / 10.
static const oem_gear_t oc_gear[OEM_GEAR_COUNT] = {
    /*  1 */ { 19, 50, 18, 40, 0x50, 10, 50 },  /* 195 Hz, strength 3 */
    /*  2 */ { 19, 50, 18, 90, 0x50, 10, 50 },
    /*  3 */ { 19, 50, 19, 40, 0x50, 10, 50 },
    /*  4 */ { 19, 50, 19, 90, 0x50, 10, 50 },
    /*  5 */ { 19, 50, 20, 40, 0x50, 10, 50 },
    /*  6 */ { 19, 50, 20, 90, 0x50, 10, 50 },
    /*  7 */ { 19, 50, 21, 40, 0x50, 10, 50 },
    /*  8 */ { 19, 50, 21, 90, 0x50, 10, 50 },
    /*  9 */ { 19, 50, 22,  6, 0x50, 10, 50 },
    /* 10 */ { 19, 50, 25,  6, 0x50, 10, 50 },  /* mode 5 placeholder */
    /* 11 */ { 19, 50, 24,  6, 0x50, 10, 50 },
    /* 12 */ { 19, 50, 25,  6, 0x50, 10, 50 },
    /* 13 */ { 19, 50, 26,  6, 0x50, 10, 50 },
    /* 14 */ { 19, 50, 27,  6, 0x50, 10, 50 },
    /* 15 */ { 19, 50, 28,  6, 0x50, 10, 50 },
    /* 16 */ { 19, 50, 29,  6, 0x50, 10, 50 },
    /* 17 */ { 19, 50, 29,  6, 0x50, 10, 50 },
    /* 18 */ { 19, 50, 30,  0, 0x50, 10, 50 },
    /* 19 */ { 19, 50, 31,  6, 0x50, 10, 50 },
    /* 20 */ { 19, 50, 32,  6, 0x50, 10, 50 },
    /* 21 */ { 19, 50, 33,  6, 0x50, 10, 50 },
    /* 22 */ { 19, 50, 34,  6, 0x50, 10, 50 },
    /* 23 */ { 19, 50, 35,  6, 0x50, 10, 50 },
    /* 24 */ { 19, 50, 36,  6, 0x50, 10, 50 },  /* strength 4 */
    /* 25 */ { 19, 50, 37,  6, 0x50, 10, 50 },
    /* 26 */ { 19, 50, 38,  6, 0x50, 10, 50 },
    /* 27 */ { 19, 50, 39,  6, 0x50, 10, 50 },
    /* 28 */ { 19, 50, 40,  6, 0x50, 10, 50 },
    /* 29 */ { 19, 50, 41,  6, 0x50, 10, 50 },
    /* 30 */ { 19, 50, 42,  6, 0x50, 10, 50 },
    /* 31 */ { 19, 50, 43,  6, 0x50, 10, 50 },
    /* 32 */ { 19, 50, 44,  6, 0x50, 10, 50 },  /* strength 5 */
    /* 33 */ { 22, 50, 24, 20, 0x1f, 30,  0 },  /* 225 Hz, low-battery buzz */
    /* 34 */ { 22, 50, 28, 60, 0x1f, 30,  0 },
    /* 35 */ { 22, 50, 30, 60, 0x1f, 20,  0 },
    /* 36 */ { 22, 50, 32, 60, 0x1f, 30,  0 },
    /* 37 */ { 13,  0, 25, 50, 0x20, 25, 10 },  /* 130 Hz */
    /* 38 */ { 13,  0, 28, 50, 0x20, 25, 10 },
    /* 39 */ { 13,  0, 32, 50, 0x20, 25, 10 },
    /* 40 */ { 13,  0, 45,  0, 0x20, 15, 10 },
    /* 41 */ { 26,  0, 18,  0, 0x50, 25, 10 },  /* 260 Hz */
    /* 42 */ { 23, 80, 13,  6, 0x50, 10, 50 },  /* 238 Hz */
    /* 43 */ { 18,  0, 14,  0, 0x50, 20, 10 },  /* 180 Hz */
    /* 44 */ { 40, 10, 10, 10, 0x50, 20, 10 },  /* 401 Hz, 70 ms tick */
    /* 45 */ { 35,  0, 42, 10, 0x50, 20, 10 },  /* 350 Hz */
    /* 46 */ { 23, 50, 38,  0, 0x50, 10, 50 },  /* 235 Hz */
    /* 47 */ { 21,  0, 48,  0, 0x50, 10, 50 },  /* 210 Hz, mode 2 */
    /* 48 */ { 22, 50, 15,  0, 0x21, 10, 50 },  /* 225 Hz swell, mode 4 */
    /* 49 */ { 16,  0,  2,  0, 0x51, 10, 50 },  /* 160 Hz sine, idle hum */
    /* 50 */ { 25, 50, 20,  6, 0x22, 10, 50 },  /* 255 Hz, mode 3 */
    /* 51 */ { 22,  0, 10, 40, 0x50, 10, 50 },  /* 220 Hz, strength 1 */
    /* 52 */ { 22,  0, 14, 40, 0x50, 10, 50 },  /* 220 Hz, strength 2 */
    /* 53 */ { 22,  0, 10, 20, 0x1f, 30,  0 },  /* 220 Hz, lock buzz */
    /* 54 */ { 23,  0, 38, 20, 0x1f, 30,  0 },  /* 230 Hz pulse, mode 1 */
};
_Static_assert(sizeof(oem_gear_t) == 7, "gear entry is 7 bytes");

const oem_gear_t *oem_wave_gear(uint8_t gear_id)
{
    return (gear_id >= 1 && gear_id <= OEM_GEAR_COUNT) ? &oc_gear[gear_id - 1] : NULL;
}

// ---- generator (stock 0x40377f70) ------------------------------------------------

void oem_wave_set(oem_wave_t *w, uint16_t freq_hz, uint8_t duty, uint8_t type)
{
    w->freq = freq_hz;
    w->base = duty;
    w->type = type;
    w->amp = duty;
    w->dir = 1;
}

int oem_wave_period_len(uint16_t freq_hz)
{
    if (freq_hz == 0) return 0;                  // stock divides by zero here
    int n = OEM_WAVE_RATE / freq_hz;
    return (n >= 1 && n <= OEM_WAVE_MAX_PERIOD) ? n : 0;
}

int oem_wave_period(oem_wave_t *w, uint16_t elapsed_s, int16_t *buf, int cap)
{
    int n = oem_wave_period_len(w->freq);
    if (n == 0 || n > cap) return 0;

    const uint8_t type = w->type;
    int amp = w->amp;
    bool clamped = false;

    // The arithmetic is kept in the stock order and precision (single-precision
    // multiply / divide, truncation towards zero) so the samples are the stock ones.
    for (int k = 0; k < n; k++) {
        float shape;
        int extra;
        if (type == OEM_WAVE_T_SINE) {
            shape = (float)sin((3.14159265 * (double)(2 * k)) / (double)n) * 16383.0f;
            extra = 0;
        } else {
            int idx = (100 * k + 100) / n - 1;   // 0..99; -1 for the first sample when n > 100
            if (idx < 0) idx = 0;
            shape = ((float)oc_wave100[idx] / 98.0f) * 16383.0f;
            extra = (type == OEM_WAVE_T_BOOST) ? 6 : 0;
        }
        if (amp + extra > 44) { amp = 50; extra = 0; clamped = true; }   // never above 50
        buf[k] = (int16_t)(int)(((float)(amp + extra) * shape) / 50.0f);
    }
    if (clamped) w->amp = 50;

    // Modulation step: once per call, for the next call.
    switch (type) {
    case OEM_WAVE_T_SWELL:                       // base .. base+20, hold 30 calls, back to base
        if (w->sw_state == 0) {
            w->sw_off = (int8_t)(w->sw_off + oc_swell_step[w->sw_idx % 3]);
            w->sw_idx++;
            if (w->sw_off > 19) { w->sw_state = 1; w->sw_off = 20; w->sw_idx = 0; }
        } else if (w->sw_state == 1) {
            if (++w->sw_hold > 29) { w->sw_state = 2; w->sw_hold = 0; }
        } else if (w->sw_state == 2) {
            w->sw_off = (int8_t)(w->sw_off - oc_swell_step[w->sw_idx % 3]);
            w->sw_idx++;
            if (w->sw_off < 1) { w->sw_state = 0; w->sw_off = 0; w->sw_idx = 0; }
        }
        w->amp = (int16_t)(w->sw_off + w->base);
        break;
    case OEM_WAVE_T_PULSE:                       // steady and wobbling sections
        if (elapsed_s < 5) { w->pulse_on = 0; break; }
        if (elapsed_s % 6 == 0) w->pulse_on ^= 1;   // on every call during such a second (stock)
        if (!w->pulse_on) { w->amp = w->base; break; }
        // fall through
    case OEM_WAVE_T_TRIANGLE:                    // down to base-16, back up to base
        if (w->dir) { w->amp--; if (w->amp < (int)w->base - 15) w->dir = 0; }
        else        { w->amp++; if (w->amp >= (int)w->base)     w->dir = 1; }
        break;
    default:                                     // 0x50, 0x51, 0x22, 0x00: constant amplitude
        break;
    }
    return n;
}

// ---- requests from the main task --------------------------------------------------

// Stock passes the parameters to the music task in plain globals that the generator
// reads and writes while the main task may be rewriting them (a clamp or modulation
// write-back can then overwrite a new amplitude with an old one). Here the request
// is one word plus a generation counter, and the generator applies it at the start
// of its next chunk: the same result as stock whenever stock's race does not hit.
static volatile uint32_t s_req;          // freq | duty << 16 | type << 24
static volatile uint32_t s_req_gen;      // bumped after s_req is written
static volatile uint8_t  s_play_req;     // 0x3fca4fc3: 0 none, 1 wave (2 = voice clip, not ported)

static oem_gear_t s_app_gear[4];         // 0x3fca33e8, set over BLE (command 0x08)

// 0x4201f074 motor_wave
static void motor_wave(uint16_t freq_hz, uint8_t duty, uint8_t type)
{
    // Stock would divide by zero (frequency 0: an app gear entry that was never set)
    // or allocate a huge buffer. Ignore such a request; whatever plays goes on.
    if (oem_wave_period_len(freq_hz) == 0) {
        hal_log("motor: %u Hz refused", freq_hz);
        return;
    }
    s_req = (uint32_t)freq_hz | (uint32_t)duty << 16 | (uint32_t)type << 24;
    s_req_gen++;
    s_play_req = 1;
    hw_motor_post(OEM_WAVE_EV_START);
}

// 0x42018edc motor_gear + 0x4201e7c8
void oem_motor_gear(uint8_t gear_id, bool use_app_table)
{
    const oem_gear_t *e;
    if ((uint8_t)(gear_id - 1) > OEM_GEAR_COUNT - 1) gear_id = 2;
    if (!use_app_table) {
        e = &oc_gear[gear_id - 1];
    } else {
        // The app table is indexed by the session's step counter, not by the gear.
        uint8_t step = oem_brush_step_index();
        if (step > 3) return;
        e = &s_app_gear[step];
    }
    uint16_t hz = (uint16_t)(e->f10 * 10 + e->frac / 10);
    uint8_t duty = e->duty;
    if (g_oem.motor_state == 2 && g_oem.dev_mode != 2) duty >>= 1;   // over-pressure
    hal_log("motor gear %u%s: %u Hz duty %u type 0x%02x", gear_id, use_app_table ? " (app)" : "",
            hz, duty, e->type);
    motor_wave(hz, duty, e->type);
}

// 0x4201e834 motor_off
void oem_motor_off(void)
{
    oem_motor_amp(false);
    s_play_req = 0;
    hw_motor_post(OEM_WAVE_EV_STOP);
}

// 0x4201f178
bool oem_motor_playing(void)
{
    return s_play_req == 1;
}

// BLE command 0x08 (0x4200eaa8) fills up to 4 entries {f10, frac, duty, b3, 0, 0, 0}.
// Stock does not check the count; an index outside the table is ignored here.
void oem_motor_app_gear_set(int index, const uint8_t entry[7])
{
    if (index < 0 || index > 3) return;
    memcpy(&s_app_gear[index], entry, sizeof s_app_gear[index]);
}

// ---- playback (stock music task 0x4201f24c), runs in the motor task --------------

static oem_wave_t s_wave = { .freq = 250, .base = 20, .type = OEM_WAVE_T_BOOST, .dir = 1 };
static uint32_t s_gen_seen;
static uint8_t  s_running;               // 0x3fca4f6e: the task keeps itself going
static int16_t  s_buf[OEM_WAVE_MAX_PERIOD];

// One stock chunk: one period computed, sent 9 times.
static void wave_chunk(void)
{
    uint32_t gen = s_req_gen;
    if (gen != s_gen_seen) {
        uint32_t r = s_req;
        s_gen_seen = gen;
        oem_wave_set(&s_wave, (uint16_t)(r & 0xffff), (uint8_t)(r >> 16), (uint8_t)(r >> 24));
    }
    int n = oem_wave_period(&s_wave, g_oem.elapsed_s, s_buf, OEM_WAVE_MAX_PERIOD);
    if (n == 0) {
        // Not reachable (motor_wave() refuses such a frequency); stay silent but
        // keep consuming time so the task does not spin.
        n = OEM_WAVE_RATE / 100;
        memset(s_buf, 0, (size_t)n * sizeof s_buf[0]);
    }
    for (int i = 0; i < OEM_WAVE_PERIODS; i++) hw_motor_write(s_buf, n);
}

void oem_wave_task_step(uint32_t bits)
{
    // Start / new parameters. Stock skips this unless a voice clip is idle or nothing
    // runs; without clips that is always true, so a gear change while the wave runs
    // takes the same path: clock restart, 5 chunks, amp on. A request on the charger
    // is dropped silently.
    bool started = false;
    if ((bits & OEM_WAVE_EV_START) && hw_motor_start_allowed()) {
        hw_motor_restart();
        s_running = 1;
        hw_motor_post(OEM_WAVE_EV_NEXT);
        if (s_play_req == 1) {
            for (int i = 0; i < OEM_WAVE_PREROLL; i++) wave_chunk();
        }
        oem_motor_amp(true);
        started = true;
    }
    // Stop. Stock handles the bits in this fixed order, so "off, then gear" arriving
    // in one pass (the task was busy writing) starts the wave and stops it again,
    // leaving it dead although a wave is requested. Deviation: a stop that an
    // executed start has overtaken (the request is "wave" again) is dropped.
    if ((bits & OEM_WAVE_EV_STOP) && !(started && s_play_req == 1)) {
        oem_motor_amp(false);
        s_running = 0;
    }
    if (bits & OEM_WAVE_EV_NEXT) {
        if (s_play_req == 1) wave_chunk();
        if (s_running) hw_motor_post(OEM_WAVE_EV_NEXT);
    }
}

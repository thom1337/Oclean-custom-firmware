// Host check of the motor waveform (main/oem_wave.c): the numbers that reach the amp.
//   cc -std=gnu11 -Wall -Wextra -ffp-contract=off -I main re/tools/uisim/sim_wave.c main/oem_wave.c -lm -o sim_wave && ./sim_wave
//
// 1. Every strength gear and mode gear: period length / frequency, peak against the
//    spec formula (brushing.md 1.4), mean over whole periods, continuity where one
//    period (and one chunk) joins the next.
// 2. The modulated types over time (pulse, triangle, swell): amplitude range.
// 3. All 54 gears, normal and with the over-pressure halving: no sample above 16383.
// 4. The playback logic with fake hardware: start sequence, gear change while
//    playing, stop, charger gate, app gear table.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_state.h"
#include "oem_wave.h"

oem_state_t g_oem;
static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- fakes for what oem_wave.c calls ----------------------------------------------

static bool g_verbose;
void hal_log(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt); printf("    [log] "); vprintf(fmt, ap); printf("\n"); va_end(ap);
}

static uint8_t g_step_index;
uint8_t oem_brush_step_index(void) { return g_step_index; }

// Fake hardware: an event word instead of the event group, a sample log instead of I2S.
static uint32_t g_pending;
static bool     g_allowed = true;
static int      g_amp;                 // GPIO48 level
static int      g_amp_calls, g_restarts;
static long     g_written;             // samples written so far
static long     g_amp_on_at;           // g_written when the amp was last switched on
static int      g_peak;                // peak of the samples written since the last reset
static int16_t  g_last[OEM_WAVE_MAX_PERIOD];
static int      g_last_n;

void oem_motor_amp(bool on) { g_amp = on; g_amp_calls++; if (on) g_amp_on_at = g_written; }
void hw_motor_post(uint32_t bits) { g_pending |= bits; }
void hw_motor_restart(void) { g_restarts++; }
bool hw_motor_start_allowed(void) { return g_allowed; }
void hw_motor_write(const int16_t *buf, int n)
{
    for (int i = 0; i < n; i++) if (abs(buf[i]) > g_peak) g_peak = abs(buf[i]);
    memcpy(g_last, buf, (size_t)n * sizeof buf[0]);
    g_last_n = n;
    g_written += n;
}

// One wake-up of the motor task: take the pending bits (clear on exit), run the body.
static uint32_t task_pass(void)
{
    uint32_t bits = g_pending & OEM_WAVE_EV_ALL;
    g_pending = 0;
    if (bits) oem_wave_task_step(bits);
    return bits;
}
static void reset_counters(void) { g_written = 0; g_peak = 0; g_amp_calls = 0; g_restarts = 0; g_amp_on_at = -1; }

// ---- helpers ------------------------------------------------------------------------

static int effective_amp(int amp, uint8_t type)
{
    int extra = (type == OEM_WAVE_T_BOOST) ? 6 : 0;
    return (amp + extra > 44) ? 50 : amp + extra;
}
// Spec formula: peak = (amp [+6]) / 50 * 16383, truncated.
static int formula_peak(int amp, uint8_t type) { return effective_amp(amp, type) * OEM_WAVE_FULL / 50; }

typedef struct { int n, peak, min, max, max_step, wrap_step; double mean; } period_stats_t;

static period_stats_t stats(const int16_t *p, int n)
{
    period_stats_t s = { .n = n, .min = 32767, .max = -32768 };
    long sum = 0;
    for (int k = 0; k < n; k++) {
        sum += p[k];
        if (p[k] < s.min) s.min = p[k];
        if (p[k] > s.max) s.max = p[k];
        if (k && abs(p[k] - p[k - 1]) > s.max_step) s.max_step = abs(p[k] - p[k - 1]);
    }
    s.peak = s.max > -s.min ? s.max : -s.min;
    s.mean = (double)sum / n;
    s.wrap_step = abs(p[0] - p[n - 1]);      // last sample of a period -> first of the next
    return s;
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case OEM_WAVE_T_BOOST: return "0x50 table+6";
    case OEM_WAVE_T_SINE: return "0x51 sine";
    case OEM_WAVE_T_PULSE: return "0x1f pulse";
    case OEM_WAVE_T_TRIANGLE: return "0x20 triangle";
    case OEM_WAVE_T_SWELL: return "0x21 swell";
    case OEM_WAVE_T_PLAIN22: return "0x22 table";
    default: return "0x00 table";
    }
}

// ---- 1. the gears the firmware uses ---------------------------------------------------

static void gear_row(int gear, const char *use)
{
    const oem_gear_t *e = oem_wave_gear((uint8_t)gear);
    int hz = e->f10 * 10 + e->frac / 10;
    oem_wave_t w = { 0 };
    oem_wave_set(&w, (uint16_t)hz, e->duty, e->type);

    // Three chunks at brushing second 0 (the pulse type is steady there), written
    // out as the task does: each period 9 times.
    static int16_t stream[3 * OEM_WAVE_PERIODS * OEM_WAVE_MAX_PERIOD];
    static int16_t buf[OEM_WAVE_MAX_PERIOD];
    int len = 0, n = 0, period_step = 0;
    period_stats_t first = { 0 };
    int amp0 = w.amp;
    for (int c = 0; c < 3; c++) {
        n = oem_wave_period(&w, 0, buf, OEM_WAVE_MAX_PERIOD);
        period_stats_t s = stats(buf, n);
        if (c == 0) first = s;
        if (s.max_step > period_step) period_step = s.max_step;   // the swell grows from chunk to chunk
        for (int r = 0; r < OEM_WAVE_PERIODS; r++) { memcpy(stream + len, buf, (size_t)n * sizeof buf[0]); len += n; }
    }

    // Frequency, measured on the stream: one rising zero crossing per period.
    int crossings = 0;
    for (int i = 1; i < len; i++) if (stream[i - 1] < 0 && stream[i] >= 0) crossings++;
    crossings++;                                  // the stream starts on one
    double period = (double)len / crossings;
    double hz_real = OEM_WAVE_RATE / period;

    // Largest step anywhere in the stream, including period and chunk joins.
    int stream_step = 0;
    for (int i = 1; i < len; i++) if (abs(stream[i] - stream[i - 1]) > stream_step) stream_step = abs(stream[i] - stream[i - 1]);
    long sum = 0;
    for (int i = 0; i < len; i++) sum += stream[i];

    int want = formula_peak(amp0, e->type);
    printf("%4d  %-11s %4d %4d %7.2f  %-13s %4d %4d  %6d %6d  %+7.2f %+6.2f%%  %5d %5d\n",
           gear, use, hz, n, hz_real, type_name(e->type), e->duty, effective_amp(amp0, e->type),
           want, first.peak, first.mean, 100.0 * first.mean / first.peak, first.max_step, first.wrap_step);

    CHECK(n == OEM_WAVE_RATE / hz, "gear %d: period %d", gear, n);
    CHECK(fabs(period - n) < 1e-9, "gear %d: measured period %.3f, expected %d", gear, period, n);
    // The period is a whole number of samples, so the pitch is 24000 / n: at or
    // above the nominal value, by less than one sample of period.
    CHECK(hz_real >= hz && hz_real < (double)OEM_WAVE_RATE / (OEM_WAVE_RATE / (double)hz - 1), "gear %d: %.2f Hz, nominal %d", gear, hz_real, hz);
    if (e->type == OEM_WAVE_T_SINE) {
        // A sampled sine may miss the crest by up to half a sample.
        CHECK(first.peak <= want && first.peak >= (int)(want * cos(M_PI / n)) - 1, "gear %d: sine peak %d, formula %d", gear, first.peak, want);
    } else {
        CHECK(first.peak == want, "gear %d: peak %d, formula %d", gear, first.peak, want);
    }
    CHECK(first.max == -first.min, "gear %d: asymmetric, max %d min %d", gear, first.max, first.min);
    CHECK(first.peak <= OEM_WAVE_FULL, "gear %d: peak %d above full scale", gear, first.peak);
    // Zero mean: the table is sampled at n points, so a period does not sum to
    // exactly zero; it has to stay far below one percent of the peak.
    CHECK(fabs(first.mean) <= 0.005 * first.peak + 0.5, "gear %d: mean %.2f", gear, first.mean);
    CHECK(fabs((double)sum / len - first.mean) < 1.0, "gear %d: stream mean %.2f", gear, (double)sum / len);
    // Continuity: joining periods must not jump more than the wave itself does.
    CHECK(first.wrap_step <= first.max_step, "gear %d: wrap step %d > %d", gear, first.wrap_step, first.max_step);
    CHECK(stream_step <= period_step, "gear %d: stream step %d > %d", gear, stream_step, period_step);
}

// ---- 2. modulated types over time -----------------------------------------------------

static void modulation(int gear, int start_s, double seconds)
{
    const oem_gear_t *e = oem_wave_gear((uint8_t)gear);
    int hz = e->f10 * 10 + e->frac / 10;
    oem_wave_t w = { 0 };
    oem_wave_set(&w, (uint16_t)hz, e->duty, e->type);
    static int16_t buf[OEM_WAVE_MAX_PERIOD], prev[OEM_WAVE_MAX_PERIOD];
    int amp_min = 999, amp_max = -999, peak_min = 99999, peak_max = 0, chunks = 0, changes = 0, prev_n = 0;
    int prev_amp = w.amp, worst_join = 0, worst_step = 0;
    double worst_mean = 0, t = 0;
    period_stats_t ps = { 0 };
    while (t < seconds) {
        int amp = w.amp;                           // amplitude this chunk is generated with
        int n = oem_wave_period(&w, (uint16_t)(start_s + (int)t), buf, OEM_WAVE_MAX_PERIOD);
        period_stats_t s = stats(buf, n);
        int eff = effective_amp(amp, e->type);
        CHECK(s.peak == abs(eff) * OEM_WAVE_FULL / 50, "gear %d chunk %d: peak %d at amp %d", gear, chunks, s.peak, amp);
        if (eff < amp_min) amp_min = eff;
        if (eff > amp_max) amp_max = eff;
        if (s.peak < peak_min) peak_min = s.peak;
        if (s.peak > peak_max) peak_max = s.peak;
        if (fabs(s.mean) > fabs(worst_mean)) worst_mean = s.mean;
        if (amp != prev_amp) changes++;
        // Amplitude moves by 1 or 2 per chunk. Exceptions, both stock: the pulse type
        // snaps back to the duty when a wobble section ends, and above 44 the
        // generator plays 50 whatever the triangle does.
        CHECK(abs(amp - prev_amp) <= 2 || e->type == OEM_WAVE_T_PULSE || amp > 44 || prev_amp > 44,
              "gear %d chunk %d: amplitude jumped %d -> %d", gear, chunks, prev_amp, amp);
        if (chunks) {                              // chunk boundary: last sample of the old period -> first of the new
            int join = abs(buf[0] - prev[prev_n - 1]);
            int allowed = s.max_step > ps.max_step ? s.max_step : ps.max_step;
            if (join > worst_join) worst_join = join;
            if (allowed > worst_step) worst_step = allowed;
            CHECK(join <= allowed, "gear %d chunk %d: join step %d > %d", gear, chunks, join, allowed);
        }
        memcpy(prev, buf, sizeof buf); prev_n = n; ps = s; prev_amp = amp;
        t += (double)OEM_WAVE_PERIODS * n / OEM_WAVE_RATE;
        chunks++;
    }
    printf("%4d  %-13s %4d  from %3d s, %5.1f s, %4d chunks: amp %3d..%-3d peak %5d..%-5d  %3d amp changes, "
           "worst join %4d (in-period step %4d), worst mean %+.2f\n",
           gear, type_name(e->type), e->duty, start_s, seconds, chunks, amp_min, amp_max, peak_min, peak_max,
           changes, worst_join, worst_step, worst_mean);
    CHECK(peak_max <= OEM_WAVE_FULL, "gear %d: peak %d", gear, peak_max);
    if (e->type == OEM_WAVE_T_SWELL) CHECK(amp_min == e->duty && amp_max == e->duty + 20, "gear %d: swell range %d..%d", gear, amp_min, amp_max);
    if (e->type == OEM_WAVE_T_TRIANGLE && e->duty <= 44) CHECK(amp_min == e->duty - 16 && amp_max == e->duty, "gear %d: triangle range %d..%d", gear, amp_min, amp_max);
    if (e->type == OEM_WAVE_T_PULSE && start_s + seconds < 5) CHECK(amp_min == e->duty && amp_max == e->duty, "gear %d: pulse must be steady before 5 s", gear);
    // A wobble section that ends while the triangle is rising leaves the direction
    // "up", so the next section starts with one step above the duty.
    if (e->type == OEM_WAVE_T_PULSE) CHECK(amp_min >= e->duty - 16 && amp_max <= e->duty + 1, "gear %d: pulse range %d..%d", gear, amp_min, amp_max);
}

// ---- 3. every gear, also with the duty halved -----------------------------------------

static void sweep(void)
{
    static int16_t buf[OEM_WAVE_MAX_PERIOD];
    int worst = 0, worst_gear = 0, lowest_amp = 0;
    double worst_rel_mean = 0;
    for (int gear = 1; gear <= OEM_GEAR_COUNT; gear++) {
        for (int half = 0; half < 2; half++) {
            const oem_gear_t *e = oem_wave_gear((uint8_t)gear);
            oem_wave_t w = { 0 };
            oem_wave_set(&w, (uint16_t)(e->f10 * 10 + e->frac / 10), half ? e->duty >> 1 : e->duty, e->type);
            double t = 0;
            for (int c = 0; c < 3000; c++) {
                if (w.amp < lowest_amp) lowest_amp = w.amp;
                int n = oem_wave_period(&w, (uint16_t)t, buf, OEM_WAVE_MAX_PERIOD);
                CHECK(n == OEM_WAVE_RATE / (e->f10 * 10 + e->frac / 10), "gear %d: n %d", gear, n);
                period_stats_t s = stats(buf, n);
                if (s.peak > worst) { worst = s.peak; worst_gear = gear; }
                if (s.peak > 200 && fabs(s.mean) / s.peak > worst_rel_mean) worst_rel_mean = fabs(s.mean) / s.peak;
                CHECK(s.peak <= OEM_WAVE_FULL, "gear %d: sample %d above full scale", gear, s.peak);
                CHECK(s.wrap_step <= s.max_step, "gear %d: wrap step %d > %d", gear, s.wrap_step, s.max_step);
                t += (double)OEM_WAVE_PERIODS * n / OEM_WAVE_RATE;
            }
        }
    }
    printf("all 54 gears x {duty, duty/2} x 3000 chunks: largest sample %d (gear %d), lowest amplitude %d, "
           "largest |mean|/peak %.4f\n", worst, worst_gear, lowest_amp, worst_rel_mean);
    CHECK(worst == OEM_WAVE_FULL, "strongest gear should reach exactly %d", OEM_WAVE_FULL);
    CHECK(worst_rel_mean < 0.005, "mean/peak %.4f", worst_rel_mean);
}

// ---- 4. playback logic ---------------------------------------------------------------

static void playback(void)
{
    printf("start: gear 1 (195 Hz, n = 123)\n");
    g_oem.motor_state = 1; g_oem.dev_mode = 1;
    reset_counters();
    CHECK(!oem_motor_playing(), "playing before any request");
    oem_motor_gear(1, false);
    CHECK(oem_motor_playing(), "not playing after motor_gear");
    CHECK(g_pending == OEM_WAVE_EV_START, "pending 0x%x", g_pending);
    CHECK(g_written == 0 && g_amp == 0, "something happened before the task ran");
    task_pass();
    printf("  restarts %d, %ld samples = %ld periods queued before amp on, amp %d, peak %d\n",
           g_restarts, g_amp_on_at, g_amp_on_at / 123, g_amp, g_peak);
    CHECK(g_restarts == 1, "restart count %d", g_restarts);
    CHECK(g_amp_on_at == 45L * 123 && g_written == 45L * 123, "pre-roll %ld samples", g_amp_on_at);
    CHECK(g_amp == 1 && g_amp_calls == 1, "amp %d after %d calls", g_amp, g_amp_calls);
    CHECK(g_peak == 7863, "strength 3 peak %d", g_peak);
    CHECK(g_pending == OEM_WAVE_EV_NEXT, "pending 0x%x", g_pending);
    for (int i = 0; i < 10; i++) task_pass();
    CHECK(g_written == 45L * 123 + 10L * 9 * 123, "10 chunks: %ld samples", g_written);
    CHECK(g_pending == OEM_WAVE_EV_NEXT && g_amp_calls == 1 && g_restarts == 1, "steady state");

    printf("gear change while playing: gear 24 (strength 4)\n");
    reset_counters();
    oem_motor_gear(24, false);
    CHECK(g_pending == (OEM_WAVE_EV_START | OEM_WAVE_EV_NEXT), "pending 0x%x", g_pending);
    task_pass();                                   // start path, then the pending "next" chunk
    printf("  restarts %d, %ld periods written in the pass, amp re-asserted after %ld, peak %d\n",
           g_restarts, g_written / 123, g_amp_on_at / 123, g_peak);
    CHECK(g_restarts == 1 && g_amp == 1 && g_amp_calls == 1, "restart %d amp %d calls %d", g_restarts, g_amp, g_amp_calls);
    CHECK(g_amp_on_at == 45L * 123 && g_written == 54L * 123, "written %ld", g_written);
    CHECK(g_peak == 13761, "strength 4 peak %d (no old-gear sample may follow the change)", g_peak);

    printf("over-pressure: motor_state 2 halves the duty (36 -> 18, +6 = 24)\n");
    reset_counters();
    g_oem.motor_state = 2;
    oem_motor_gear(24, false);
    task_pass();
    printf("  peak %d\n", g_peak);
    CHECK(g_peak == 7863, "halved peak %d", g_peak);
    reset_counters();
    g_oem.dev_mode = 2;                            // factory mode: no halving
    oem_motor_gear(24, false);
    task_pass();
    CHECK(g_peak == 13761, "factory mode peak %d", g_peak);
    g_oem.motor_state = 1; g_oem.dev_mode = 1;

    printf("gear ids outside 1..54 fall back to gear 2\n");
    for (int g = 0; g < 2; g++) {
        reset_counters();
        oem_motor_gear(g ? 55 : 0, false);
        task_pass();
        CHECK(g_last_n == 123 && g_peak == 7863, "gear %d -> n %d peak %d", g ? 55 : 0, g_last_n, g_peak);
    }

    printf("idle hum: gear 49 (160 Hz sine)\n");
    reset_counters();
    oem_motor_gear(49, false);
    task_pass();
    printf("  n %d, peak %d\n", g_last_n, g_peak);
    CHECK(g_last_n == 150 && g_peak == 655, "hum n %d peak %d", g_last_n, g_peak);

    printf("stop\n");
    reset_counters();
    oem_motor_off();
    CHECK(g_amp == 0 && g_amp_calls == 1, "amp must drop in the caller, before the task runs");
    CHECK(!oem_motor_playing(), "still playing");
    int passes = 0;
    while (task_pass() && passes < 10) passes++;
    printf("  amp %d, %ld samples written after the stop, task idle after %d passes\n", g_amp, g_written, passes);
    CHECK(g_amp == 0 && g_written == 0 && g_pending == 0 && passes <= 2, "stop: amp %d written %ld passes %d", g_amp, g_written, passes);

    printf("start refused on the charger (gate closed)\n");
    reset_counters();
    g_allowed = false;
    oem_motor_gear(1, false);
    while (task_pass()) { }
    printf("  amp %d, written %ld, playing %d\n", g_amp, g_written, oem_motor_playing());
    CHECK(g_amp == 0 && g_written == 0 && g_restarts == 0, "gate: amp %d written %ld", g_amp, g_written);
    CHECK(oem_motor_playing(), "stock still reports the wave as requested");
    g_allowed = true;
    oem_motor_off();
    while (task_pass()) { }

    printf("gear then off in one pass: stopped; off then gear in one pass: running (deviation)\n");
    reset_counters();
    oem_motor_gear(1, false); oem_motor_off();
    passes = 0; while (task_pass() && passes < 10) passes++;
    CHECK(g_amp == 0 && g_pending == 0 && g_written == 0, "gear+off: amp %d written %ld", g_amp, g_written);
    reset_counters();
    oem_motor_gear(1, false); task_pass();
    oem_motor_off(); oem_motor_gear(52, false);
    task_pass();
    CHECK(g_amp == 1 && (g_pending & OEM_WAVE_EV_NEXT), "off+gear: amp %d pending 0x%x", g_amp, g_pending);
    long before = g_written; task_pass();
    CHECK(g_written == before + 9L * 109 && g_peak == 7863, "off+gear: stream stalled or wrong (n %d)", g_last_n);
    oem_motor_off(); while (task_pass()) { }

    printf("app gear table\n");
    static const uint8_t app0[7] = { 20, 0, 20, 0, 0, 0, 0 };     // 200 Hz, duty 20, type 0
    static const uint8_t app3[7] = { 25, 55, 60, 0, 0, 0, 0 };    // 255 Hz, duty 60 -> clamps to 50
    oem_motor_app_gear_set(0, app0);
    oem_motor_app_gear_set(3, app3);
    oem_motor_app_gear_set(4, app3);               // outside the table: ignored
    oem_motor_app_gear_set(-1, app3);
    reset_counters(); g_step_index = 0;
    oem_motor_gear(32, true); task_pass();
    printf("  step 0: n %d peak %d\n", g_last_n, g_peak);
    CHECK(g_last_n == 120 && g_peak == 20 * OEM_WAVE_FULL / 50, "app 0: n %d peak %d", g_last_n, g_peak);
    reset_counters(); g_step_index = 3;
    oem_motor_gear(32, true); task_pass();
    printf("  step 3: n %d peak %d\n", g_last_n, g_peak);
    CHECK(g_last_n == 94 && g_peak == OEM_WAVE_FULL, "app 3: n %d peak %d", g_last_n, g_peak);
    reset_counters(); g_step_index = 1;            // never set: 0 Hz, stock would divide by zero
    oem_motor_gear(32, true);
    CHECK(!(g_pending & OEM_WAVE_EV_START), "0 Hz request was posted");
    task_pass();
    CHECK(g_last_n == 94 && g_restarts == 0, "0 Hz entry changed the wave (n %d)", g_last_n);
    reset_counters(); g_step_index = 4;            // stock: nothing
    oem_motor_gear(32, true);
    CHECK(!(g_pending & OEM_WAVE_EV_START), "step 4 posted a start");
    oem_motor_off(); while (task_pass()) { }
    CHECK(oem_wave_period_len(9) == 0 && oem_wave_period_len(10) == 2400 && oem_wave_period_len(0) == 0, "period limits");
}

// -d: dump "gear half chunk elapsed_s n: samples" for every gear, to compare the
// generator against another implementation of the stock routine.
static void dump(void)
{
    static int16_t buf[OEM_WAVE_MAX_PERIOD];
    for (int gear = 1; gear <= OEM_GEAR_COUNT; gear++) {
        for (int half = 0; half < 2; half++) {
            const oem_gear_t *e = oem_wave_gear((uint8_t)gear);
            oem_wave_t w = { 0 };
            oem_wave_set(&w, (uint16_t)(e->f10 * 10 + e->frac / 10), half ? e->duty >> 1 : e->duty, e->type);
            for (int c = 0; c < 400; c++) {
                int n = oem_wave_period(&w, (uint16_t)(c / 20), buf, OEM_WAVE_MAX_PERIOD);
                printf("%d %d %d %d %d:", gear, half, c, c / 20, n);
                for (int k = 0; k < n; k++) printf(" %d", buf[k]);
                printf("\n");
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-d")) { dump(); return 0; }
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");

    printf("== 1. gears used by the firmware (first chunk; brushing second 0)\n");
    printf("gear  use           Hz    n  Hz real  type          duty  amp  peak:formula/got  mean LSB, %%peak   step  wrap\n");
    gear_row(51, "strength 1"); gear_row(52, "strength 2"); gear_row(1, "strength 3");
    gear_row(24, "strength 4"); gear_row(32, "strength 5");
    gear_row(54, "mode 1");     gear_row(47, "mode 2");     gear_row(50, "mode 3");
    gear_row(48, "mode 4");     gear_row(49, "idle hum");   gear_row(10, "mode 5 base");
    gear_row(33, "low battery"); gear_row(53, "lock buzz"); gear_row(44, "tick");

    printf("\n== 2. modulated types over time\n");
    modulation(54, 0, 4.5);      // mode 1 before second 5: steady
    modulation(54, 5, 120.0);    // mode 1: steady / triangle sections
    modulation(48, 0, 150.0);    // mode 4: swell
    modulation(37, 0, 30.0);     // triangle
    modulation(40, 0, 30.0);     // triangle with duty 45: clamps to 50
    modulation(53, 5, 30.0);     // pulse with duty 10: the triangle dips below zero amplitude

    printf("\n== 3. sweep\n");
    sweep();

    printf("\n== 4. playback logic\n");
    playback();

    printf("\n%s (%d failed checks)\n", g_fail ? "FAILED" : "ALL CHECKS PASSED", g_fail);
    return g_fail ? 1 : 0;
}

#include <stddef.h>
#include <string.h>
#include "oem_input.h"

// Brushing-force value of the AW8686X (brushing.md section 4). The stock firmware
// gets it from a closed Awinic library ("aw8686x_sw_algorithm"): the driver hands it
// one raw ADC sample every 20 ms, and reads back
//
//     force = (filtered sample - baseline) * coef / 100
//
// where coef comes from the factory calibration (20000 / ADC counts of the test
// weight, so the unit is roughly grams if that weight was 200 g) and the baseline is
// the zero-force level the library tracks while nothing presses on the brush head.
// The baseline tracker is most of the code. It is ported here from the stock image
// (0x42071e64 reset, 0x42071e9c core, 0x42072b44 parameters, 0x42072c44 per-sample
// entry) statement by statement, one channel only, and checked against the original
// machine code on the host (re/tools/uisim/sim_force.c).
//
// The state block has the layout of the library's 236-byte context, so that the
// host test can compare it byte for byte. Field names are mine; "unused" fields are
// read by the stock code but never written (they stay 0).

typedef struct {
    uint8_t  r00;
    uint8_t  pressed_out;     // 0x01  copy of `pressed` after each sample
    uint8_t  r02;
    uint8_t  touch_rel_n;     // 0x03  samples without press that end a "touch" (1)
    uint8_t  touch_rel_cnt;   // 0x04
    uint8_t  r05;
    uint8_t  ch_type;         // 0x06  1
    uint8_t  r07[3];
    uint16_t touch_cnt;       // 0x0a  pressed samples towards a "touch" (3 needed)
    uint16_t r0c;
    uint16_t coef;            // 0x0e  calibration coefficient
    uint8_t  enabled;         // 0x10
    uint8_t  channels;        // 0x11
    uint16_t press_cap;       // 0x12  54
    uint16_t rel_cap;         // 0x14  34: samples after a release until the press state is cleared
    uint16_t thr_press;       // 0x16  20: least press threshold (force units)
    int16_t  thr_release;     // 0x18  10
    uint16_t thr_track;       // 0x1a  10: below this the baseline follows the signal
    uint16_t quiet_lim;       // 0x1c  20
    uint16_t filt_pct;        // 0x1e  60: weight of the previous output in the input low-pass
    int16_t  thr_heavy;       // 0x20  1600
    uint16_t heavy_n;         // 0x22  8
    uint16_t noise;           // 0x24  calibrated noise (ADC counts)
    uint16_t init_n;          // 0x26  20: samples before the first baseline
    uint32_t pos_timeout;     // 0x28  60000 samples above thr_track -> baseline reset
    uint32_t neg_timeout;     // 0x2c  60000 samples below -thr_track -> baseline reset
    uint16_t r30;             // 0x30  1
    uint16_t r32;
    // ---- channel 0
    uint16_t over_cnt;        // 0x34  consecutive samples with force > 1.5 * thr_track
    int16_t  peak;            // 0x36
    uint16_t since_rel;       // 0x38  samples since the last release (max 80)
    int16_t  slope;           // 0x3a  leaky derivative of the input
    int16_t  median;          // 0x3c  median of the last 30 quiet samples
    uint16_t init_cnt;        // 0x3e
    uint16_t heavy_cnt;       // 0x40
    uint16_t pos_cnt;         // 0x42
    uint16_t neg_cnt;         // 0x44
    uint16_t r46, r48;
    int16_t  unused4a;        // 0x4a
    int16_t  diff;            // 0x4c  sample - baseline
    int16_t  unused4e;        // 0x4e
    int16_t  baseline;        // 0x50
    int16_t  baseline0;       // 0x52  first baseline
    int16_t  diff_scaled;     // 0x54  diff * max(coef, 200) / 100
    int16_t  force;           // 0x56  diff * coef / 100
    int16_t  hist[5];         // 0x58  last five samples, newest last
    int16_t  quiet[30];       // 0x62  samples for the median
    uint16_t r9e;
    int16_t  unuseda0, unuseda2;   // 0xa0, 0xa2
    uint8_t  ra4[8];
    int16_t  cand;            // 0xac  baseline candidate
    int16_t  slope_acc;       // 0xae
    int16_t  prev;            // 0xb0  previous filtered sample
    int16_t  f_out;           // 0xb2  input filter: previous output
    int16_t  f_mid;           // 0xb4  input filter: sample held back for the spike test
    int16_t  f_new;           // 0xb6
    uint32_t rb8;
    uint32_t have_prev;       // 0xbc
    uint32_t f_count;         // 0xc0
    uint8_t  heavy;           // 0xc4  force stayed above thr_heavy
    uint8_t  negative;        // 0xc5  signal well below the baseline
    uint8_t  rc6;
    uint8_t  pressed;         // 0xc7
    uint8_t  released;        // 0xc8  a release happened less than rel_cap samples ago
    uint8_t  unusedc9;
    uint8_t  rca;
    uint8_t  rebase;          // 0xcb  baseline was reset by a timeout
    uint8_t  rcc, rcd, rce, rcf;
    uint16_t rel_cnt;         // 0xd0
    uint16_t press_cnt;       // 0xd2
    uint8_t  rd4[3];
    uint8_t  touch;           // 0xd7  debounced press ("touch" event state)
    uint8_t  touch_end;       // 0xd8
    uint8_t  rd9, rda, rdb;
    uint32_t rdc;
    uint32_t re0;
    uint8_t  out[5];          // 0xe4  report: touch, sample lo/hi, baseline lo/hi
    uint8_t  re9[3];
} aw_state_t;

_Static_assert(sizeof(aw_state_t) == OEM_FORCE_STATE_SIZE, "library context is 236 bytes");
_Static_assert(offsetof(aw_state_t, coef) == 0x0e && offsetof(aw_state_t, noise) == 0x24 &&
               offsetof(aw_state_t, over_cnt) == 0x34 && offsetof(aw_state_t, baseline) == 0x50 &&
               offsetof(aw_state_t, quiet) == 0x62 && offsetof(aw_state_t, cand) == 0xac &&
               offsetof(aw_state_t, f_count) == 0xc0 && offsetof(aw_state_t, pressed) == 0xc7 &&
               offsetof(aw_state_t, rel_cnt) == 0xd0 && offsetof(aw_state_t, touch) == 0xd7 &&
               offsetof(aw_state_t, out) == 0xe4, "library context layout");

static aw_state_t s;

// Input filter settings the stock driver passes with every sample (rodata
// 0x3c11c8da: 01 03 10 00): enabled, mode 3 (spike removal + low-pass), spike
// threshold 16 counts.
#define SPIKE_THR 16

// 0x42103878 / 0x42103890: value * mul / div in 16 bits, signed and unsigned
static int16_t scale_s(int16_t v, uint16_t mul, uint16_t div)
{
    return (int16_t)((int32_t)v * (int32_t)mul / (int32_t)div);
}
static uint16_t scale_u(uint16_t v, uint16_t mul, uint16_t div)
{
    return (uint16_t)((uint32_t)v * mul / div);
}

static void add_base(int delta) { s.baseline = (int16_t)(s.baseline + delta); }

// 0x42071e64
static void alg_reset(void)
{
    memset((uint8_t *)&s + 0xa0, 0, 0x24);
    memset((uint8_t *)&s + 0x34, 0, 0x6a);
    memset((uint8_t *)&s + 0xc4, 0, 0x20);
}

// No press: let the baseline follow the signal. The step depends on how far the
// signal is from the baseline (ds, in scaled units), how fast it moves (slope) and
// what happened recently. All divisions truncate toward zero, as in the original.
static void track_baseline(int ds)
{
    const int noise = s.noise, half = s.noise >> 1, thr = s.thr_track;
    const int slope = s.slope, d = s.diff;

    if (slope <= half && -half <= slope) {                  // input is quiet
        double q = s.quiet_lim * 1.5, t = thr * 1.5;
        if (s.unused4e < q && s.unused4e > -q && ds > -t && ds < t) {
            if (d < -half || half < d)                       add_base(d / 8);
            else if ((noise >> 2) < d || d < -(noise >> 2))  add_base(d / 4);
            else                                             add_base(d / 2);
        }
        return;
    }
    if (ds < 0) {                                           // below the baseline
        if (s.heavy == 1) {
            add_base(s.median < s.baseline ? d / 12 : d / 128);
            return;
        }
        if ((uint16_t)(s.since_rel - 1) < 24) {             // shortly after a release: recover fast
            if (s.median < s.baseline) {
                if (s.since_rel < 6) {
                    if (s.median < s.hist[4]) add_base(d / 2);
                    else                      s.baseline = s.median;
                } else {
                    add_base(d / 4);
                }
            } else {
                add_base(d / 8);
            }
            return;
        }
        if (ds >= -2) {
            if (slope < 40) {
                if (d <= -noise || noise < d) add_base((ds + 1) >> 1);
                else if (d < 0)               add_base(-1);
                return;
            }
        } else if (ds >= -5 && slope < 40) {
            add_base(-1);
            return;
        }
        if (-(thr >> 1) < ds) { add_base(d / 48); return; }
        if (slope < 40 && ds < -(thr >> 1) && -thr < ds) { add_base(d / 112); return; }
        double lim = -(thr * 1.5);
        if (ds < -thr && lim < ds) { add_base(d / 144); return; }
        if (!(lim > ds) || ds <= -2 * thr) {
            if (ds < -2 * thr && -3 * thr < ds) add_base(d / 576);
            return;
        }
        add_base(d / 288);
        return;
    }
    if (ds >= thr) return;                                  // on its way to a press: hold
    if (ds <= 4) {
        if (-noise < d && d <= noise) {
            if (d >= 1) add_base(1);
        } else {
            add_base(ds >> 1);
        }
    } else if (ds <= 6) {
        add_base((s.coef > 199 ? d : ds) / 3);
    } else if (d >= 1 && d <= 4) {
        add_base(d >> 1);
    } else if (d < 9) {
        if (d >= 5) add_base(d >> 2);
    } else if (d < 17) {
        add_base(d >> 3);
    } else if (d < 33) {
        add_base(s.coef < 200 ? d >> 3 : d >> 5);
    } else if (d < 65) {
        add_base(d >> 6);
    } else if (d <= 128) {
        add_base(d >> 7);
    }
}

// A positive / negative excursion that lasts too long resets the baseline to the
// current sample.
static void count_timeout(uint16_t *cnt, uint32_t limit)
{
    if (*cnt < limit) {
        (*cnt)++;
    } else {
        *cnt = (uint16_t)limit;
        s.rebase = 1;
        s.baseline = s.hist[4];
    }
}

// 0x42071e9c, channel 0
static void alg_core(int16_t x)
{
    // First init_n samples: the baseline is the mean of the last five of them.
    if (s.init_cnt < s.init_n) {
        s.quiet[s.init_cnt] = x;
        if ((int)s.init_cnt < (int)s.init_n - 5) { s.init_cnt++; return; }
        int k = (int16_t)(s.init_cnt - s.init_n + 5);
        if (k > 4) return;
        s.hist[k] = x;
        s.init_cnt++;
        if (k != 4) return;
        int sum = s.hist[0] + s.hist[1] + s.hist[2] + s.hist[3] + s.hist[4];
        s.baseline0 = s.baseline = (int16_t)(sum / 5);
        return;
    }
    // While nothing touches and the input is quiet: median of the last 30 samples.
    if (s.touch == 0 && s.slope < (int)s.noise && -(int)s.noise < s.slope &&
        -2 * (int)s.noise < s.unused4e && s.unused4e < 2 * (int)s.noise) {
        int16_t sorted[30];
        memmove(&s.quiet[0], &s.quiet[1], 29 * sizeof(int16_t));
        s.quiet[29] = x;
        memcpy(sorted, s.quiet, sizeof(sorted));
        for (int i = 1; i < 30; i++) {
            int16_t v = sorted[i];
            int j = i - 1;
            for (; j >= 0 && sorted[j] > v; j--) sorted[j + 1] = sorted[j];
            sorted[j + 1] = v;
        }
        s.median = (int16_t)((sorted[14] + sorted[15]) / 2);
    }

    const uint8_t was_pressed = s.pressed;
    s.diff = (int16_t)(x - s.baseline);
    memmove(&s.hist[0], &s.hist[1], 4 * sizeof(int16_t));
    s.hist[4] = x;
    s.force = scale_s(s.diff, s.coef, 100);
    s.diff_scaled = scale_s(s.diff, s.coef < 200 ? 200 : s.coef, 100);
    const int ds = s.diff_scaled;

    if (!s.pressed) {
        const int thr = s.thr_track;
        if (ds >= thr) {
            count_timeout(&s.pos_cnt, s.pos_timeout);
            s.neg_cnt = 0;
        } else if (ds > -thr) {
            s.pos_cnt = 0;
            s.neg_cnt = 0;
        } else {
            count_timeout(&s.neg_cnt, (uint16_t)s.neg_timeout);
            s.pos_cnt = 0;
        }
        track_baseline(ds);
    } else {
        count_timeout(&s.pos_cnt, s.pos_timeout);
        s.neg_cnt = 0;
    }

    // (Here the original can re-derive `cand` from the fields at 0xa0 / 0xa2. Nothing
    // writes those, and with both 0 that code never runs.)
    if ((s.slope < (s.noise >> 1) || s.since_rel < 25) && ds >= -3 && ds <= 3) s.cand = s.baseline;
    if (s.over_cnt > 2 && s.rebase == 0) s.baseline = s.cand;

    // Press threshold: the calibrated noise in force units, at least thr_press.
    uint16_t press_thr = scale_u(s.noise, s.coef, 100);
    if (press_thr < s.thr_press) press_thr = s.thr_press;

    if (s.negative == 0) {
        s.negative = (ds < -(int)press_thr && ds <= -(int)s.thr_release) ? 1 : 0;
    } else if (s.negative == 1) {
        if (ds > -(int)s.thr_release) s.negative = 0;
    }

    if (s.rel_cap <= s.rel_cnt) {
        s.rcd = 0;
        s.rel_cnt = 0;
        s.press_cnt = 0;
        s.pressed = 0;
        s.released = 0;
        s.rc6 = 0;
    }

    const int force = s.force;
    s.over_cnt = (force > s.thr_track * 1.5) ? (uint16_t)(s.over_cnt + 1) : 0;

    if (!s.pressed && (int)press_thr < force) {
        s.pressed = 1;
        if (!s.released) s.rebase = 0;
        s.press_cnt = 0;
        s.rel_cnt = 0;
        s.released = 0;
    }

    if (scale_s(s.thr_heavy, s.coef, 100) < ds) {
        s.heavy_cnt++;
        if (s.heavy_n < s.heavy_cnt) s.heavy = 1;
    } else {
        s.heavy_cnt = 0;
    }

    bool bump = true;                 // count this sample in since_rel
    if (s.pressed == 1) {
        int rel_thr;
        if (s.press_cnt < 9) {
            s.peak = 0;
            rel_thr = s.thr_release;
        } else {
            // Release threshold rises with 5 % of a peak value. The original computes
            // the peak from the field at 0x4a, which nothing writes.
            float k = (float)s.coef / 100.0f;
            float v = -(float)s.thr_press + k * (float)(s.unused4a - s.baseline);
            int16_t pk = (int16_t)(int)v;
            if (s.peak < pk) s.peak = pk;
            rel_thr = (int16_t)(int)(s.peak * 0.05 + (double)(uint16_t)s.thr_release);
        }
        if (force < rel_thr) {
            s.pressed = 0;
            s.released = 1;
            s.rel_cnt = 0;
            s.over_cnt = 0;
            if (s.press_cnt > 1) {
                // A real press ended: the baseline may only go down to the current sample.
                if (s.hist[4] < s.baseline) {
                    s.since_rel = 1;
                    bump = false;
                } else {
                    s.since_rel = 0;
                    s.baseline = s.hist[4];
                }
            }
        }
    } else {
        s.peak = 0;
    }
    if (bump) s.since_rel = (s.since_rel + 1 > 79) ? 80 : (uint16_t)(s.since_rel + 1);

    if (s.released) {
        if (s.rel_cnt < s.rel_cap) {
            s.rel_cnt++;
        } else {
            s.rel_cnt = s.rel_cap;
            s.heavy = 0;
        }
    }
    if (s.pressed) {
        if (s.unusedc9 == 0) {
            if (s.press_cnt < s.press_cap) s.press_cnt++;
            else                           s.press_cnt = s.press_cap;
        }
        if (was_pressed == 0 && s.pressed == 1) s.heavy = 0;
    }
}

// Input filter of 0x42072c44, mode 3: the output runs one sample behind. A sample
// that sticks out from both neighbours by more than SPIKE_THR is replaced by their
// mean; steps smaller than the noise are low-passed (filt_pct of the old value).
static int16_t alg_filter(int16_t x)
{
    if (s.f_count < 2) {
        if (s.f_count == 0) s.f_out = x; else s.f_mid = x;
        s.f_count++;
        return x;
    }
    int mid = s.f_mid, out = s.f_out;
    s.f_new = x;
    if ((SPIKE_THR < mid - out && SPIKE_THR < mid - x) || (SPIKE_THR < out - mid && SPIKE_THR < x - mid)) {
        mid = (int16_t)((out + x) / 2);
        s.f_mid = (int16_t)mid;
    }
    if (mid - out < (int)s.noise && -(int)s.noise < mid - out) {
        mid = (int16_t)((100 - (int)s.filt_pct) * mid / 100 + out * (int)s.filt_pct / 100);
    }
    s.f_out = (int16_t)mid;
    s.f_mid = s.f_new;
    return (int16_t)mid;
}

// 0x42072c44
int16_t oem_force_step(int16_t raw)
{
    int16_t x = alg_filter(raw);

    // slope = 0.95 * (slope + change): a leaky derivative of the filtered input
    if (!s.have_prev) {
        s.slope_acc = x;
        s.have_prev = 1;
    } else {
        s.slope_acc = (int16_t)(int)((x - s.prev) * 0.95 + s.slope_acc * 0.95);
        s.slope = s.slope_acc;
    }
    s.prev = x;

    alg_core(x);

    // Debounced "touch" state of the library (3 pressed samples in, 1 out). The
    // brush logic does not use it; kept so the state matches the original.
    uint8_t touch = s.touch;
    s.pressed_out = s.pressed;
    if (!s.pressed) {
        if (!touch) {
            s.touch_cnt = 0;
        } else {
            uint8_t n = s.touch_rel_cnt;
            if (s.touch_rel_n <= n) {
                s.touch_end = 1;
                s.touch = 0;
                s.touch_cnt = 0;
                touch = 0;
                n = s.touch_rel_n;
            }
            s.touch_rel_cnt = (uint8_t)(n + 1);
        }
    } else if (!touch) {
        if (s.touch_cnt < 2) {
            s.touch_cnt++;
        } else {
            s.touch = 1;
            if (!s.touch_end) s.rdb = 0;
            s.re0 = 0;
            s.touch_end = 0;
            touch = 1;
            s.touch_cnt = 3;
        }
        s.touch_rel_cnt = 0;
    }
    s.out[0] = touch;
    s.out[1] = (uint8_t)x;
    s.out[2] = (uint8_t)((uint16_t)x >> 8);
    s.out[3] = (uint8_t)s.baseline;
    s.out[4] = (uint8_t)((uint16_t)s.baseline >> 8);
    return s.force;
}

// What the stock init does with the library (0x42027894): clear, reset
// (0x42072bf0), parameters (0x42072b44, literals 0x42002d2c..), coefficient
// (0x42103838), reset again (0x42072b30).
void oem_force_init(uint16_t noise, uint16_t coef)
{
    memset(&s, 0, sizeof(s));
    s.touch_rel_n = 1;
    s.enabled = 1;
    s.channels = 1;
    s.ch_type = 1;
    s.press_cap = 54;
    s.rel_cap = 34;
    s.thr_press = 20;
    s.thr_release = 10;
    s.thr_track = 10;
    s.quiet_lim = 20;
    s.filt_pct = 60;
    s.thr_heavy = 1600;
    s.heavy_n = 8;
    s.noise = noise;
    s.init_n = 20;
    s.pos_timeout = 60000;
    s.neg_timeout = 60000;
    s.r30 = 1;
    s.coef = coef;
    alg_reset();
}

int16_t  oem_force_baseline(void) { return s.baseline; }
int16_t  oem_force_filtered(void) { return s.hist[4]; }
uint16_t oem_force_coef(void)     { return s.coef; }
bool     oem_force_pressed(void)  { return s.pressed != 0; }
const uint8_t *oem_force_state(void) { return (const uint8_t *)&s; }

// Record layout (0x42026c08): ':' 0xA0 len(6) | noise u16 | coef u16 | 5A 5A |
// byte sum of the six data bytes as u32 | carry byte | '\r' '\n'.
// 0x42027894 falls back to noise 9 / coef 23 when the record does not check out.
bool oem_force_parse_cal(const uint8_t rec[16], uint16_t *noise, uint16_t *coef)
{
    *noise = OEM_FORCE_DEF_NOISE;
    *coef = OEM_FORCE_DEF_COEF;
    if (rec[0] != ':' || rec[1] != 0xA0 || rec[14] != '\r' || rec[15] != '\n') return false;
    unsigned len = rec[2];
    if (len != 6) return false;       // the only length the factory code writes; keeps the reads inside the record
    uint32_t sum = 0;
    for (unsigned i = 0; i < len; i++) sum += rec[3 + i];
    uint32_t stored = (uint32_t)rec[len + 3] | (uint32_t)rec[len + 4] << 8 |
                      (uint32_t)rec[len + 5] << 16 | (uint32_t)rec[len + 6] << 24;
    if (sum != stored || rec[len + 7] != 0 || sum == 0) return false;
    if (rec[7] != 0x5A || rec[8] != 0x5A) return false;
    *noise = (uint16_t)(rec[3] | rec[4] << 8);
    *coef = (uint16_t)(rec[5] | rec[6] << 8);
    return true;
}

float oem_force_temp_coef(int now_c, uint8_t base_c)
{
    int base = base_c ? base_c : (uint8_t)now_c;
    float k = (float)((now_c - base) / 100.0 + 1.0);
    if (k >= 1.25f) return 1.25f;
    if (k <= 0.75f) return 0.75f;
    return k;
}

int16_t oem_force_scale(int16_t force, float temp_coef)
{
    if (force < 0) force = 0;
    int16_t v = (int16_t)(int)((float)force * temp_coef);
    return v < 0 ? 0 : v;
}

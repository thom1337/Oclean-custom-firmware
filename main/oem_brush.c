#include <string.h>
#include "oem_api.h"
#include "oem_brush.h"
#include "oem_hal.h"
#include "oem_state.h"

// Brush engine of the stock firmware: configuration blobs, mode profiles, the session
// clock with its step advance and auto stop, the 30 s zone cue, pause / resume, the
// pressure state machine, score, session record and the daily history totals.
// re/spec/brushing.md sections 2..6 and 8; the main-loop side is in oem_app.c.
//
// Everything here runs in the main task with the core lock held, except the few
// functions the UI task calls (oem_strength_step, oem_strength_gear, oem_motor_stop,
// oem_set_mode, oem_brush_remaining), which hold the same lock.

#define GEAR_HUM        49      // anti-splash idle hum: 160 Hz sine, duty 2
#define USE_APP_GEARS   false   // BLE gear table (brushing.md 2.5) is not ported, see NOTES_app.md

typedef struct { uint8_t a, gear, seconds; } step_t;

// Session variables that only the engine uses (stock "ses" 0x3fca4c90 and neighbours).
// The ones other modules read are in g_oem.
static struct {
    uint8_t    id;              // ses+0x2a   profile id, goes into the record
    step_t     step[13];        // ses+0x74
    uint16_t   next_step_s;     // 0x3fca4c7a elapsed_s at which the next step starts
    uint16_t   cue_ticks;       // 0x3fca4c7e 30 ms ticks since the last zone cue
    uint8_t    cue_active;      // 0x3fca4c80
    uint8_t    cue_count;       // 0x3fca4c7c
    uint16_t   ring[17];        // 0x3fca4d5e contact filter: the last 17 pressure samples
    uint8_t    ring_idx;        // 0x3fca4d5d
    uint8_t    ring_full;       // 0x3fca4d5c
    uint8_t    release_cnt;     // 0x3fca4d80 consecutive samples below 390
    uint8_t    plog[240];       // 0x3fca4b81 pressure log, one byte per 2 s
    uint16_t   plog_idx;        // 0x3fca4c72
    oem_time_t start;           // ses+0x22
} ses;

static uint8_t s_sys_raw[100];  // images of the two blobs as stored, so bytes this
static uint8_t s_ucfg[100];     // firmware does not use survive a save
static bool    s_hist_new_day;  // 0x3fca4dfe
static uint8_t s_record[182];   // last session record (0x42019f20), RAM only
static uint8_t s_record_len;

// ---- small helpers ---------------------------------------------------------------------

// NVS write that leaves the flash alone when the stored blob is already identical.
// (Stock rewrites its blobs on every sleep and every boot.)
static void blob_put(const char *key, const uint8_t *img, size_t len)
{
    uint8_t cur[100];
    if (len <= sizeof cur && hal_nvs_get(key, cur, len) == len && memcmp(cur, img, len) == 0) return;
    if (!hal_nvs_set(key, img, len)) hal_log("brush: %s not saved", key);
}

// 03:01 .. 12:00, the stock "morning" window (0x4201a3fc, 0x42019050, 0x42019f20)
static bool morning(void)
{
    oem_time_t t;
    hal_time(&t);
    if (t.hour < 4) return t.hour == 3 && t.min != 0;
    if (t.hour > 11) return t.hour == 12 && t.min == 0;
    return true;
}

// Force value the engine works with: never negative, 0 without a sensor.
static int pressure_now(void)
{
    if (!oem_pressure_available()) return 0;
    return g_oem.pressure > 0 ? g_oem.pressure : 0;
}

static void ring_clear(void)                     // 0x420198dc
{
    memset(ses.ring, 0, sizeof ses.ring);
    ses.ring_idx = 0;
    ses.ring_full = 0;
}

// ---- sys_config (0x42018cf8 / 0x42018d74) ----------------------------------------------
// Blob "sys_config", 100 bytes written, 11 used:
//   0 sys[0x34] boot stage   1 sys[0x0d]   2 sys[0x0e]   3 time byte 7   4 sys[0x0c] bound
//   5 sys[0x67] language     6 sys[0x6e]   7 sys[0x6f]   9 sys[0x68] (0xAA = language valid)
//   10 voice[3]

void brush_sys_config_load(void)
{
    memset(s_sys_raw, 0, sizeof s_sys_raw);      // key absent: all zero, as stock
    hal_nvs_get("sys_config", s_sys_raw, sizeof s_sys_raw);
    const uint8_t *b = s_sys_raw;
    g_oem.sys[0x34] = b[0];
    g_oem.sys[0x0d] = b[1];
    g_oem.sys[0x0e] = b[2];
    g_oem.sys[0x0c] = b[4];
    g_oem.sys[0x67] = b[5];
    g_oem.sys[0x6e] = b[6];
    g_oem.sys[0x6f] = b[7];
    g_oem.sys[0x68] = b[9];
    // Language 2 unless the BLE "set language" marker is there. Stock does not range
    // check the index; one above 16 would read past the picture tables.
    if (b[9] != 0xAA || g_oem.sys[0x67] > 16) g_oem.sys[0x67] = 2;
}

void brush_sys_config_save(void)
{
    uint8_t *b = s_sys_raw;
    b[0] = g_oem.sys[0x34];
    b[1] = g_oem.sys[0x0d];
    b[2] = g_oem.sys[0x0e];
    b[4] = g_oem.sys[0x0c];
    b[5] = g_oem.sys[0x67];
    b[6] = g_oem.sys[0x6e];
    b[7] = g_oem.sys[0x6f];
    b[9] = g_oem.sys[0x68];
    blob_put("sys_config", s_sys_raw, sizeof s_sys_raw);
}

// ---- user_config (0x42018a74 / 0x420185b4 / 0x420187b4) --------------------------------
// Blob "user_config", 100 bytes written, 80 used. Bytes this firmware has a variable for:
//   0 sys[1]      1 sys[0x2e]   2 sys[2] zone-cue type     4 profile[0]   5 sys[0x37] anti-splash
//   6 sys[0]      17, 18 birthday month, day               19 mode        20 sys[3]   21 sys[4]
//   22 sys[0x2c]  23 sys[0x2b]  24 sys[0x36]  25, 26 sys[0x3a..0x3b]      33 sys[0x72]
//   34 sys[0x3c]  36 sys[0x69]  37 sys[0x6b]  38 sys[5]    39 sys[6] auto mode by time
//   40 sys[7]     41 sys[8] raise-to-wake     43..47 profile[1..5]        52 touch lock
//   53 lock popup saved screen  54..61, 64..67 zone seconds (low bytes)   62 last score
//   63 sys[0x76] greeting pages
// The rest (record pointers 8..11, app gear flag 12, voice flags 48..50, OEM-cloud
// counters) is kept as loaded.

// Order of the zone bytes: 54..61 = zones 0,1,2,3,5,4,6,7 (4 and 5 swapped, 0x4201a8c0)
static const uint8_t ZONE_BYTE[12] = { 54, 55, 56, 57, 59, 58, 60, 61, 64, 65, 66, 67 };

static void ucfg_unpack(void)
{
    const uint8_t *b = s_ucfg;
    uint8_t *sys = g_oem.sys;
    sys[1] = b[0];
    sys[0x2e] = b[1]; sys[0x2f] = 0;
    sys[2] = b[2];
    g_oem.profile[0] = b[4];
    sys[0x37] = b[5];
    sys[0] = b[6];
    g_oem.birthday_month = b[17];
    g_oem.birthday_day = b[18];
    g_oem.profile[6] = b[19];
    sys[3] = b[20];
    sys[4] = b[21];
    sys[0x2c] = b[22];
    sys[0x2b] = b[23];
    sys[0x36] = b[24];
    sys[0x3a] = b[25]; sys[0x3b] = b[26];
    sys[0x72] = b[33];
    sys[0x3c] = b[34];
    sys[0x69] = b[36];
    sys[0x6b] = b[37];
    sys[5] = b[38];
    sys[6] = b[39];
    sys[7] = (uint8_t)(b[40] - 0x2e) > 2 ? 0x2f : b[40];
    sys[8] = b[41];
    memcpy(&g_oem.profile[1], &b[43], 5);
    g_oem.locked = b[52];
    g_oem.saved_screen = b[53];
    for (int i = 0; i < 12; i++) g_oem.zone_s[i] = b[ZONE_BYTE[i]];
    g_oem.score = b[62];
    sys[0x76] = b[63];
}

static void ucfg_pack(void)
{
    uint8_t *b = s_ucfg;
    const uint8_t *sys = g_oem.sys;
    b[0] = sys[1];
    b[1] = sys[0x2e];
    b[2] = sys[2];
    b[4] = g_oem.profile[0];
    b[5] = sys[0x37];
    b[6] = sys[0];
    b[17] = g_oem.birthday_month;
    b[18] = g_oem.birthday_day;
    b[19] = g_oem.profile[6];
    b[20] = sys[3];
    b[21] = sys[4];
    b[22] = sys[0x2c];
    b[23] = sys[0x2b];
    b[24] = sys[0x36];
    b[25] = sys[0x3a]; b[26] = sys[0x3b];
    b[33] = sys[0x72];
    b[34] = sys[0x3c];
    b[36] = sys[0x69];
    b[37] = sys[0x6b];
    b[38] = sys[5];
    b[39] = sys[6];
    b[40] = sys[7];
    b[41] = sys[8];
    memcpy(&b[43], &g_oem.profile[1], 5);
    b[52] = g_oem.locked;
    b[53] = g_oem.saved_screen;
    for (int i = 0; i < 12; i++) b[ZONE_BYTE[i]] = (uint8_t)g_oem.zone_s[i];
    b[62] = g_oem.score;
    b[63] = sys[0x76];
}

void brush_user_config_save(void)                // 0x420185b4
{
    ucfg_pack();
    blob_put("user_config", s_ucfg, sizeof s_ucfg);
}

// 0x420187b4: factory defaults, written back at once
static void ucfg_defaults(void)
{
    static const struct { uint8_t at, val; } DEF[] = {
        { 0, 1 }, { 1, 0xde }, { 5, 1 }, { 6, 0x0c }, { 15, 0xff }, { 16, 4 }, { 17, 0xff }, { 18, 0xff },
        { 19, 5 },                               // mode 5
        { 21, 0x12 }, { 28, 0xf0 }, { 33, 0xff }, { 37, 1 }, { 40, 0x2f },
        { 41, 1 },                               // raise-to-wake on
        { 43, 2 }, { 45, 180 }, { 46, 1 }, { 47, 16 },   // profile[1..5] = 2, 0, 180, 1, 16
        { 48, 1 }, { 49, 1 }, { 50, 1 }, { 51, 1 },
        { 63, 1 },                               // greeting pages on
    };
    memset(s_ucfg, 0, sizeof s_ucfg);
    for (size_t i = 0; i < sizeof DEF / sizeof DEF[0]; i++) s_ucfg[DEF[i].at] = DEF[i].val;
    ucfg_unpack();
    blob_put("user_config", s_ucfg, sizeof s_ucfg);
}

// 0x42018dd8: the user settings. A boot stage of 3 or more (11 = factory reset pending)
// and an absent / all-zero blob both give the defaults.
void brush_config_load(void)
{
    bool use_defaults = g_oem.sys[0x34] >= 3;
    if (!use_defaults) {
        memset(s_ucfg, 0, sizeof s_ucfg);
        hal_nvs_get("user_config", s_ucfg, sizeof s_ucfg);
        unsigned sum = 0;
        for (int i = 0; i < 80; i++) sum += s_ucfg[i];
        if ((uint16_t)sum == 0) use_defaults = true;
        else ucfg_unpack();
    }
    if (use_defaults) ucfg_defaults();
    g_oem.sys[0x2e] = 0xde; g_oem.sys[0x2f] = 0x03;   // u16 0x3de, set on every boot
    g_oem.sys[0x6b] = 1;
}

// ---- modes -----------------------------------------------------------------------------

bool oem_app_profile(void)                       // 0x42018fe8
{
    return g_oem.profile[0] == 1 || g_oem.profile[2] == 1;
}

void oem_set_mode(uint8_t mode)
{
    g_oem.profile[6] = g_oem.profile[7] = mode;
}

uint8_t oem_mode(void)
{
    return g_oem.profile[6];
}

// 0x42019008: mode 0 exists only with an app profile; anything else unknown is mode 5
void brush_mode_clamp(void)
{
    if (g_oem.profile[6] > 5) g_oem.profile[6] = 5;
    if (!oem_app_profile() && g_oem.profile[6] == 0) g_oem.profile[6] = 5;
    g_oem.profile[7] = g_oem.profile[6];
}

// ---- daily history totals --------------------------------------------------------------
// NVS "shuanhuan" (0x32 bytes): BE u16 seconds, BE u16 score sum, BE u16 count, u8 day.

static void hist_save(void)                      // 0x42023df0
{
    const oem_rtc_t *r = hal_rtc();
    uint8_t b[0x32] = { 0 };
    b[0] = (uint8_t)(r->hist_seconds >> 8);   b[1] = (uint8_t)r->hist_seconds;
    b[2] = (uint8_t)(r->hist_score_sum >> 8); b[3] = (uint8_t)r->hist_score_sum;
    b[4] = (uint8_t)(r->hist_count >> 8);     b[5] = (uint8_t)r->hist_count;
    b[6] = r->hist_day;
    blob_put("shuanhuan", b, sizeof b);
}

void brush_hist_load(void)                       // 0x4201c100
{
    uint8_t b[0x32] = { 0 };                     // key absent: zero totals, day 0 (as stock)
    hal_nvs_get("shuanhuan", b, sizeof b);
    oem_rtc_t *r = hal_rtc();
    r->hist_seconds = (uint16_t)(b[0] << 8 | b[1]);
    r->hist_score_sum = (uint16_t)(b[2] << 8 | b[3]);
    r->hist_count = (uint16_t)(b[4] << 8 | b[5]);
    r->hist_day = b[6];
}

void brush_hist_reset(void)                      // 0x4201c0cc
{
    oem_rtc_t *r = hal_rtc();
    r->hist_seconds = r->hist_score_sum = r->hist_count = 0;
    r->hist_day = 0xff;
    hist_save();
}

void brush_hist_update(uint16_t seconds, uint8_t score)   // 0x4201bc88
{
    oem_rtc_t *r = hal_rtc();
    oem_time_t t;
    hal_time(&t);
    if (seconds < 15) {                          // no session to add: new-day check
        if (t.day != r->hist_day) {
            if (r->hist_day < 40 && t.year < 200) {
                r->hist_seconds = r->hist_score_sum = r->hist_count = 0;
                s_hist_new_day = true;
            }
            r->hist_day = t.day;
        }
    } else {
        r->hist_seconds = (uint16_t)(r->hist_seconds + seconds);
        r->hist_score_sum = (uint16_t)(r->hist_score_sum + score);
        r->hist_count++;
    }
    hist_save();
}

bool brush_hist_new_day(void)
{
    return s_hist_new_day;
}

// ---- profiles (0x42019050) -------------------------------------------------------------
// Record: id, flags (bits 7..5) | step count (bits 4..0), then {a, gear, seconds} per
// step. Only the first 21 bytes of each stock table are copied; what follows the used
// steps is left-over data.

static const uint8_t PROFILE_A[21] = {           // mode 1: gear 54, 120 s   (0x3c1196ee)
    0x00, 0x01, 0x00, 0x36, 0x78, 0x00, 0x2e, 0x32, 0x00, 0x12, 0x32, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00 };
static const uint8_t PROFILE_B[21] = {           // modes 0 / 5: gear 10, 150 s + 150 s   (0x3c119716)
    0x00, 0x02, 0x00, 0x0a, 0x96, 0x00, 0x0a, 0x96, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00 };
static const uint8_t PROFILE_C[21] = {           // mode 2: gear 47, 180 s   (0x3c11973e)
    0x00, 0x01, 0x00, 0x2f, 0xb4, 0x00, 0x21, 0x2d, 0x00, 0x12, 0x2d, 0x00, 0x21, 0x2d, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00 };
static const uint8_t PROFILE_D[21] = {           // mode 4: gear 48, 150 s   (0x3c119766)
    0x00, 0x01, 0x00, 0x30, 0x96, 0x00, 0x30, 0x96, 0x00, 0x12, 0x2d, 0x00, 0x21, 0x2d, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00 };
static const uint8_t PROFILE_E[21] = {           // mode 3: gear 50, 120 s   (0x3c11978e)
    0x00, 0x01, 0x00, 0x32, 0x78, 0x00, 0x30, 0x96, 0x00, 0x12, 0x2d, 0x00, 0x21, 0x2d, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00 };

// 38 bytes of the phone-app scheme blob "motor_data" (255 bytes) at the given offset
static void motor_data_read(uint8_t *rec, size_t off)   // 0x420247e8
{
    uint8_t blob[255];
    memset(blob, 0, sizeof blob);
    if (hal_nvs_get("motor_data", blob, sizeof blob) == 0) return;   // absent: the table stays
    memcpy(rec, blob + off, 38);
}

static void load_profile(uint8_t mode)
{
    uint8_t rec[44];                             // stock: 40; padded so 13 steps stay inside
    const uint8_t *tab = mode == 1 ? PROFILE_A : mode == 2 ? PROFILE_C
                       : mode == 3 ? PROFILE_E : mode == 4 ? PROFILE_D : PROFILE_B;
    memset(rec, 0, sizeof rec);
    memcpy(rec, tab, 21);
    rec[0] = g_oem.profile[6];
    if (mode == 0) {
        if (g_oem.profile[2] == 1) {             // single-step profile set over BLE (cmd 0x30)
            rec[0] = 6;
            rec[1] = 1;
            rec[3] = g_oem.profile[5];           // gear, default 16
            rec[4] = g_oem.profile[3];           // seconds, default 180
        } else if (g_oem.profile[0] != 0) {      // scheme stored by the phone app
            size_t off = 0;
            if (g_oem.profile[1] == 1) off = morning() ? 0x3c : 0x78;
            motor_data_read(rec, off);
        }
    }

    ses.id = rec[0];
    uint8_t n = rec[1] & 0x1f;
    g_oem.step_count = n > 13 ? 13 : n;
    memset(ses.step, 0, sizeof ses.step);
    g_oem.total_s = 0;
    for (int i = 0; i < g_oem.step_count; i++) {
        ses.step[i].a = rec[2 + 3 * i];
        ses.step[i].gear = rec[3 + 3 * i];
        ses.step[i].seconds = rec[4 + 3 * i];
        g_oem.total_s = (uint16_t)(g_oem.total_s + ses.step[i].seconds);
    }
}

// ---- strength (mode 5) -----------------------------------------------------------------

static uint8_t strength_to_gear(uint8_t level)   // 0x42019368
{
    switch (level) {
    case 1: return 51;
    case 2: return 52;
    case 4: return 24;
    case 5: return 32;
    default: return 1;                           // level 3 and anything else
    }
}

static uint8_t gear_to_strength(uint8_t gear)    // 0x42019398
{
    switch (gear) {
    case 51: return 1;
    case 52: return 2;
    case 24: return 4;
    case 32: return 5;
    default: return 3;
    }
}

uint8_t brush_strength_level(void)
{
    oem_rtc_t *r = hal_rtc();
    if ((uint8_t)(r->strength - 1) > 4) r->strength = 3;
    return r->strength;
}

void oem_strength_gear(uint8_t level)            // 0x42019368
{
    ses.step[0].gear = g_oem.gear = strength_to_gear(level);
}

void brush_strength_set(uint8_t level)           // 0x420192e8 (the caller shows screen 87)
{
    oem_strength_gear(level);
    if (g_oem.motor_state != 0 && g_oem.running) oem_motor_gear(g_oem.gear, USE_APP_GEARS);
}

// Swipe up / down while the motor runs. The new level is always stored; it is applied
// only while the intensity screen 87 is up (the first seconds of a mode-5 session).
void oem_strength_step(bool up)                  // 0x4201b50c
{
    g_oem.dwell_s = 0;
    uint8_t cur = gear_to_strength(g_oem.gear);
    uint8_t n = up ? (cur >= 5 ? 5 : cur + 1) : (cur <= 1 ? 1 : cur - 1);
    hal_rtc()->strength = n;
    if (g_oem.now_ui == 87) {
        oem_show_strength(n - 1);
        brush_strength_set(n);
    }
}

// ---- session start, step advance, clock ------------------------------------------------

uint8_t oem_brush_step_index(void)
{
    return g_oem.step_index;
}

// 0x4201bb34: what the countdown shows. Mode 5 counts up. (Stock also requests voice
// clip 6 here when contact_s reaches 120 in mode 5; clips are not ported.)
uint16_t oem_brush_remaining(void)
{
    if (g_oem.profile[6] == 5) return g_oem.done_s;
    return g_oem.done_s <= g_oem.total_s ? (uint16_t)(g_oem.total_s - g_oem.done_s) : 0;
}

// Program finished (0x420193c0 with brush_state 0x14): the motor stops by itself.
static void program_done(void)
{
    g_oem.sys[0x35] = 0;
    g_oem.done_s = g_oem.total_s;
    g_oem.session_active = 0;
    oem_motor_off();
    hal_event_post(OEM_EV_SESSION_END);
}

static void step_advance(void)                   // 0x420193f8
{
    if (g_oem.step_index < g_oem.step_count) {
        if (g_oem.profile[6] != 5) g_oem.gear = ses.step[g_oem.step_index].gear;   // mode 5 keeps the strength gear
        if (g_oem.motor_state == 0) oem_motor_gear(GEAR_HUM, false);
        else oem_motor_gear(g_oem.gear, USE_APP_GEARS);
        g_oem.step_index++;
        g_oem.sys[0x2d] = g_oem.step_index == 1 ? 0 : 0x20;
    } else {
        program_done();
    }
}

// The engine part of start_session (0x4201c858.. in 0x4201c790): flags, start time,
// pressure log, then session_init 0x4201ba38 -> 0x420194a8, which starts the motor.
void brush_session_begin(void)
{
    ring_clear();
    g_oem.sys[0x37] = g_oem.dev_mode != 2;       // anti-splash is forced on at every start
    g_oem.session_active = 1;
    g_oem.muted = 0;
    g_oem.running = 1;
    ses.cue_count = 0;
    ses.release_cnt = 0;
    hal_time(&ses.start);
    g_oem.elapsed_s = 0;
    memset(ses.plog, 0, sizeof ses.plog);        // 0x42018514
    ses.plog_idx = 0;

    memset(g_oem.zone_s, 0, sizeof g_oem.zone_s);
    g_oem.zone_flag = 0;
    g_oem.profile[6] = g_oem.dev_mode == 2 ? 5 : g_oem.profile[7];
    load_profile(g_oem.profile[6]);
    g_oem.step_index = 0;
    g_oem.done_s = 0;
    g_oem.contact_s = 0;
    ses.cue_ticks = 0;
    ses.next_step_s = ses.step[0].seconds;
    g_oem.gear = ses.step[0].gear;
    // Mode 5: the profile's gear 10 is a placeholder for the strength gear. Stock
    // patches it after the first step_advance (0x42019368 from 0x4201c790), which is
    // the same as long as the motor starts in the idle hum; without a force sensor it
    // starts on the gear directly, so the gear has to be right before.
    if (g_oem.profile[6] == 5) oem_strength_gear(brush_strength_level());
    // Anti-splash: wait in the idle hum until the bristles touch. Without a working
    // force sensor that contact would never be seen.
    g_oem.motor_state = (g_oem.sys[0x37] == 0 || !oem_pressure_available()) ? 1 : 0;
    step_advance();
}

void oem_motor_stop(void)                        // 0x42019528 handle_motor_stop
{
    g_oem.session_active = 0;
    g_oem.stop_delay = 200;
    hal_event_post(OEM_EV_SESSION_END);
    oem_motor_off();
    oem_touch_irq(true);
}

void brush_pause(void)                           // 0x4201c790 "suspend_brush_by_profile" + 0x420197c0
{
    g_oem.motor_state = 0;
    ring_clear();
    g_oem.running = 0;
    g_oem.muted = 1;
    g_oem.stop_delay = 0;                        // counted up at 1 Hz; 30 ends the session
    oem_motor_off();
}

void brush_resume(void)                          // 0x4201c790 "resume_brush_by_profile" + 0x4201977c
{
    ring_clear();
    g_oem.motor_state = oem_pressure_available() ? 0 : 1;   // hum until contact, if that can be seen
    g_oem.running = 1;
    g_oem.stop_delay = 200;
    // Stock passes step[step_index - 1].gear here and in the pressure paths below; in
    // mode 5 that is the placeholder gear 10 once the second step has started. The
    // chosen strength gear is kept instead (g_oem.gear is the same value in every
    // other mode).
    if (g_oem.motor_state == 0) oem_motor_gear(GEAR_HUM, false);
    else oem_motor_gear(g_oem.gear, USE_APP_GEARS);
    g_oem.muted = 0;
}

// One byte of the pressure log per 2 s of brushing: pressure / 4, 0xff from 1000 up
static void pressure_log(void)
{
    if (!g_oem.running || (g_oem.done_s & 1) != 0) return;
    if (ses.plog_idx < 239) {
        int p = pressure_now();
        ses.plog[ses.plog_idx] = p > 999 ? 0xff : (uint8_t)(p / 4);
    }
    ses.plog_idx++;
}

void brush_tick_1hz(void)                        // 0x42019560 and the session part of the 1 Hz block
{
    if (g_oem.session_active && g_oem.stop_delay == 200) {
        g_oem.elapsed_s++;
        if (g_oem.elapsed_s == ses.next_step_s) {
            if (g_oem.step_index < g_oem.step_count)
                ses.next_step_s = (uint16_t)(ses.next_step_s + ses.step[g_oem.step_index].seconds);
            if (g_oem.step_count - 1 == g_oem.step_index) ses.next_step_s += 2;   // several steps: 2 s past the total
            step_advance();
        }
        g_oem.done_s = g_oem.elapsed_s;
        if (pressure_now() > 49) g_oem.contact_s++;
        if (g_oem.total_s < g_oem.elapsed_s) g_oem.done_s = g_oem.total_s;
        // Not stock: a scheme with a zero-length step never reaches its next step and
        // would run until the counter wraps (18 h). No valid profile gets here.
        if (g_oem.session_active && g_oem.elapsed_s >= g_oem.total_s + 2u) program_done();
    }
    if (g_oem.stop_delay < 60) {                 // paused: 30 s, then the session ends
        g_oem.stop_delay++;
        if (g_oem.stop_delay == 30) oem_motor_stop();
    }
    if (g_oem.session_active) {
        pressure_log();
        g_oem.score = brush_score();
    } else if (ses.plog_idx > 150) {
        ses.plog_idx = 0;
    }
}

// ---- zone cue (0x4201962c), every 30 ms while the motor runs -----------------------------
// Type 0 (default): at every multiple of 30 s, not in the first or the last 10 s, the
// amp is switched off for two ticks (about 60 ms) while the wave keeps streaming.
// Type 1 is a voice clip (not ported: no cue).
static void zone_cue(void)
{
    if (g_oem.sys[2] != 0) return;
    if (g_oem.elapsed_s % 30 == 0 && g_oem.elapsed_s > 10 && !ses.cue_active
            && ses.cue_ticks > 50 && oem_brush_remaining() > 10) {
        g_oem.muted = 1;
        oem_motor_amp(false);
        ses.cue_active = 1;
        ses.cue_ticks = 0;
    } else if (ses.cue_ticks == 1) {
        if (ses.cue_active) g_oem.muted = 1;
    } else if (ses.cue_ticks > 1 && ses.cue_active) {
        if (g_oem.motor_state == 0) oem_motor_gear(GEAR_HUM, false);
        else oem_motor_amp(true);
        g_oem.muted = 0;
        ses.cue_active = 0;
        ses.cue_ticks = 0;
        ses.cue_count = ses.cue_count == 255 ? 60 : ses.cue_count + 1;
    }
    ses.cue_ticks++;
}

// ---- pressure (0x42018530, 0x420198fc) -------------------------------------------------

// 0x42019860: the bristles are on the teeth when the last 17 samples (510 ms) vary by
// more than 29 and add up to more than 510
static bool contact_seen(uint16_t p)
{
    ses.ring[ses.ring_idx] = p;
    if (ses.ring_idx + 1 < 17) ses.ring_idx++;
    else { ses.ring_idx = 0; ses.ring_full = 1; }
    uint16_t mx = 0, mn = 0xffff, sum = 0;
    for (int i = 0; i < 17; i++) {
        if (ses.ring[i] > mx) mx = ses.ring[i];
        if (ses.ring[i] < mn) mn = ses.ring[i];
        sum = (uint16_t)(sum + ses.ring[i]);
    }
    if ((uint16_t)(mx - mn) <= 29 || sum <= 510) return false;
    return ses.ring_full;
}

// 0x42019830: 34 consecutive samples (about 1 s) below the limit
static bool released(int p, int limit)
{
    if (p < limit) {
        if (++ses.release_cnt > 33) { ses.release_cnt = 0; return true; }
    } else {
        ses.release_cnt = 0;
    }
    return false;
}

static void pressure_tick(void)
{
    int p = pressure_now();
    // pressure lights: led 2 = "pressure fine", led 3 = "too hard" (steady, blinking from 600)
    if (p <= 400) {
        if (g_oem.motor_state != 2) {            // state 2: the lights stay as they are
            oem_led_set(2, g_oem.motor_state == 1 ? 0 : 1, 4);
            oem_led_set(3, 1, 4);
        }
    } else {
        oem_led_set(2, 1, 4);
        oem_led_set(3, p < 600 ? 0 : 2, 4);
    }
    if (!oem_pressure_available()) return;       // no sensor: the motor stays in state 1

    switch (g_oem.motor_state) {
    case 0:                                      // idle hum, waiting for contact
        if (!contact_seen((uint16_t)p)) return;
        ring_clear();
        g_oem.motor_state = 1;
        break;
    case 1:
        if (p <= 400) return;
        g_oem.motor_state = 2;                   // over-pressure: oem_motor_gear halves the duty
        break;
    case 2:
        // (stock asks for voice clip 1 above 600 here)
        if (!released(p, 390)) return;
        g_oem.motor_state = 1;
        break;
    default:
        return;
    }
    oem_motor_gear(g_oem.gear, USE_APP_GEARS);   // see brush_resume() about the gear
}

void brush_tick_30ms(void)
{
    if (g_oem.session_active && g_oem.running) zone_cue();
    if (!g_oem.session_active) return;
    if (!g_oem.running) {                        // paused
        g_oem.motor_state = 0;
        oem_led_set(2, 1, 4);
        oem_led_set(3, 1, 4);
    } else {
        pressure_tick();
    }
}

// ---- score, record ---------------------------------------------------------------------

// 0x4201c260 with the constants of 0x4201c71c: 12 zones, 5 s each, weight 84
uint8_t brush_score(void)
{
    unsigned sum = 0;
    bool zones = false;
    for (int i = 0; i < 12; i++) {
        unsigned z = g_oem.zone_s[i];
        if (z) zones = true;
        unsigned v = (z < 5 ? z : 5) * 84 / 5;
        sum += v < 125 ? v : 125;
    }
    if (!zones) {
        // SUBSTITUTE, NOT STOCK. The zone tracker (IMU posture classifier) is not
        // ported, so zone_s[] stays zero, the stock score would always be 0 and no
        // session would ever count. Without zone data the score is the part of the
        // programme that was brushed.
        if (g_oem.total_s == 0) return 0;
        unsigned s = (unsigned)g_oem.done_s * 100 / g_oem.total_s;
        return (uint8_t)(s < 100 ? s : 100);
    }
    unsigned s = sum / 10;
    return (uint8_t)(s < 100 ? s : 100);
}

// 0x42019f20: the session record in the stock layout. Stock appends it to the record
// area of the picture partition (0x42024e5c); this firmware never writes that
// partition, the record is kept in RAM for whoever wants to fetch it.
static void record_build(void)
{
    unsigned len = g_oem.done_s / 2u + 51;
    if (len > sizeof s_record) len = sizeof s_record;
    uint8_t *r = s_record;
    memset(r, 0xff, sizeof s_record);
    r[0] = 0;
    r[1] = (uint8_t)len;
    if (ses.start.year > 200) hal_time(&ses.start);      // clock was not set at the start
    r[2] = ses.start.year; r[3] = ses.start.month; r[4] = ses.start.day;
    r[5] = ses.start.hour; r[6] = ses.start.min;   r[7] = ses.start.sec;
    r[8] = ses.id;
    r[9] = (uint8_t)(g_oem.total_s >> 8);  r[10] = (uint8_t)g_oem.total_s;
    r[11] = (uint8_t)(g_oem.done_s >> 8);  r[12] = (uint8_t)g_oem.done_s;
    r[13] = 10; r[14] = 20; r[15] = 70;
    r[16] = r[17] = r[18] = 0;
    r[19] = s_sys_raw[3];
    r[20] = (uint8_t)g_oem.zone_s[0];
    r[28] = g_oem.score;
    r[29] = (g_oem.profile[1] == 1 && g_oem.profile[6] == 0) ? (morning() ? 1 : 2) : 3;
    r[30] = g_oem.profile[2];
    r[31] = 0;                                   // voice clip 2 count
    for (int i = 0; i < 4; i++) r[32 + i] = (uint8_t)g_oem.zone_s[8 + i];
    unsigned n = len - 51;
    memcpy(&r[51], ses.plog, n);
    s_record_len = (uint8_t)len;
}

const uint8_t *oem_brush_record(size_t *len)
{
    if (len) *len = s_record_len;
    return s_record_len ? s_record : NULL;
}

// End of a session (the engine part of 0x4201c2b8): final score; a session of at least
// 15 s with a non-zero score gets a record and goes into the daily totals.
bool brush_session_finish(void)
{
    g_oem.score = brush_score();
    if (g_oem.done_s < 15 || g_oem.score == 0) return false;
    record_build();
    brush_hist_update(g_oem.done_s, g_oem.score);
    return true;
}

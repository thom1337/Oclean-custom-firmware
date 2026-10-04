#pragma once
#include <stdbool.h>
#include <stdint.h>

// Shared state of the OEM-behaviour port ("oem core").
//
// The stock firmware keeps its application state in plain globals that the main
// task ("brush_app") and the UI task both touch. The port keeps the same variables,
// under the names the specs in re/spec/ use, in one struct. The comment after each
// field is the stock address and the module that owns (writes) it:
//   A = oem_app.c   B = oem_brush.c (inside oem_app agent's scope)   U = oem_ui.c
//   L = oem_led.c / oem_gauge.c   I = input   P = power
// Other modules may read any field. Access happens with the core lock held (see
// oem_hal.h: hal_lock), so no field needs to be atomic.

// Power state values (stock 0x3fca4b74)
#define OEM_PWR_CHARGING 1
#define OEM_PWR_BATTERY  2
#define OEM_PWR_FULL     3

// `state` values (stock 0x3fca4e8f)
#define OEM_ST_BOOT     0
#define OEM_ST_WOKEN    1
#define OEM_ST_SESSION  2
#define OEM_ST_ENDED    3
#define OEM_ST_SCORED   4

typedef struct {
    // ---- screens / UI bookkeeping
    uint8_t  now_ui;          // 0x3fc9ab9a  A  brush-app-side current screen id (0xFF = none)
    uint8_t  state;           // 0x3fca4e8f  A  OEM_ST_*
    uint8_t  dwell_s;         // 0x3fca4de5  A  seconds since the current screen was posted (saturates 30)
    uint8_t  info_page;       // 0x3fca4dcb  U  page 0..3 of info screen 92
    uint8_t  locked;          // 0x3fca5d9c  U  touch lock
    uint8_t  lock_popup;      // 0x3fca5d94  U  lock popup (screen 91) is up
    uint8_t  saved_screen;    // 0x3fca5d98  U  screen to restore after the popup
    uint8_t  subpage;         // 0x3fca4fda  U  screen_mode: 1 = on a left/right side page
    uint8_t  zone_flag;       // 0x3fca4e7a  A  zone-guided mode (no stock writer; stays 0)
    uint8_t  user_quit;       // 0x3fca4dc8  U  session ended by a swipe
    uint8_t  saved_mode;      // 0x3fc9ab9c  A  mode at the last brushing start / lock toggle (0xFF none)

    // ---- power
    uint8_t  asleep;          // 0x3fca41a4  A  1 = screen-off stage entered
    uint8_t  power_state;     // 0x3fca4b74  A  OEM_PWR_*
    uint8_t  batt_pct;        // 0x3fca4b7a  L  battery percent 0..100
    uint16_t batt_mv;         //             L  filtered battery millivolts
    uint8_t  wake_gate;       // 0x3fc9f308  A  5 after the screen-off sequence ran
    uint8_t  init_ok;         // 0x3fc9f309  A  5 once the main task is up (ISRs ignore edges before)

    // ---- modes of operation
    uint8_t  ota;             // 0x3fca4194  A  non-zero while a firmware update runs (set by the web OTA too)
    uint8_t  reset_flag;      // 0x3fca4ea0  A  first boot after a factory reset
    uint8_t  dev_mode;        // 0x3fca4e9b  A  1 normal, 2 factory production test
    uint8_t  aging;           // 0x3fca4e91  A  factory aging mode
    uint8_t  show_mode;       // 0x3fca4dca  A  shop demo mode (NVS "IntoShow")
    uint8_t  ui_mode;         // 0x3fca4d8d  A  0xD7 = demo / production UI mode

    // ---- brushing session
    uint8_t  session_active;  // 0x3fca4d5b  B  session exists (running or paused)
    uint8_t  running;         // 0x3fca4e03  B  1 = motor running, 0 = paused
    uint8_t  stop_delay;      // 0x3fc9aba5  B  200 = running; 0.. counts up at 1 Hz while paused
    uint8_t  motor_state;     // 0x3fc9ab97  B  0 idle hum (waiting for contact), 1 normal, 2 over-pressure
    uint16_t total_s;         // ses+0x6e    B  planned seconds
    uint16_t done_s;          // ses+0x70    B  elapsed seconds (clamped to total)
    uint16_t elapsed_s;       // 0x3fca4c82  B  brushing seconds (frozen while paused)
    uint16_t zone_s[12];      // 0x3fca4cda  B  seconds brushed per zone
    uint8_t  score;           // 0x3fc9aba3  B  last score 0..100 (0xFF = none yet)
    int16_t  pressure;        // 0x3fca4c74  I  latest brushing-force value
    uint8_t  gear;            // ses+0x21    B  current motor gear id
    uint8_t  brushed_since_wake; // 0x3fca4197 B

    // ---- configuration (mirrors of the stock NVS blobs, loaded at boot)
    uint8_t  sys[0x7c];       // 0x3fc9a69e  A  sys config: [6] auto mode by time, [8] raise-to-wake,
                              //                [0x0c] bound, [0x34] boot stage, [0x37] anti-splash,
                              //                [0x67] language, [0x69] touch off, [0x76] greeting pages
    uint8_t  profile[8];      // 0x3fca4c85  B  [0] app profile, [1], [2], [3] secs, [5] gear, [6] mode, [7] mode shown
    uint8_t  lang;            // 0x3fca5114  A  UI language index 0..16
    uint8_t  birthday_month, birthday_day;   // 0x3fca33e6/7  A  0xFF = unset
    char     fw_version[16];  //             glue  "a.b.c.d" shown on info page 0

    // ---- network status for the LED module (stock 0x3fca2ad9): 0 reset, 1 connected, 2 down
    uint8_t  wifi_status;     //             glue

    // ---- additions by the UI module (agent U may add fields below this line)
    uint8_t  tod_sub;         // 0x3fca4de4  A  time-of-day sub profile of screen 81 (0, 2 morning, 3 evening); written by oem_show_main
    uint8_t  cloud_state;     // 0x3fca3870  glue  status-icon input: 1 = link up, 2 = cloud session (stays 0 without a cloud)
    uint8_t  wifi_weak;       // 0x3fc9aef8  glue  status-icon input: RSSI below -83 dBm (cleared at -79 dBm or better)
    uint8_t  clock_mode;      // 0x3fc9b380  U  page 96: 0 weather, 1 loading, 2 no data (clock only), 3 / 0xFF nothing received
    uint8_t  weather_flag;    // 0x3fca4ff4  glue  page 96 banner: 0 = picture 846, else 847
    int8_t   weather_t1;      // 0x3fca4ff5  glue  right-hand temperature of the "t2 ~ t1" line
    int8_t   weather_t2;      // 0x3fca4ff6  glue  left-hand temperature
    uint8_t  weather_code;    // 0x3fca4ff8  glue  weather icon 0..6
    char     ota_version[8];  // 0x3fca3ac8+200  glue  "a.b.c.d" offered by the update prompt (screen 97)

    // ---- additions by the app/brush module (agent A may add fields below this line)
    uint8_t  step_index;      // ses+0x20    B  steps started so far (oem_brush_step_index())
    uint8_t  step_count;      // ses+0x2b    B  steps of the running profile (at most 13)
    uint16_t contact_s;       // ses+0x72    B  seconds with pressure >= 50
    uint8_t  muted;           // 0x3fca4c7d  B  1 while paused and during the zone-cue stutter
    uint16_t tick_s;          // 0x3fca4e2a  A  seconds counted by the 1 Hz block (0 at boot and at session start)

    // ---- additions by the LED/gauge module (agent L may add fields below this line)
    // Gauge variables that the charge state machine (stock 0x42017a0c, app module) and
    // the charger ISR also touch. oem_gauge_boot() loads the stock .data values.
    uint8_t  gauge_inited;    // 0x3fca4b80  L    first battery measurement done; charger detection waits for it
    uint8_t  plug_cnt;        // 0x3fc9ab8c  L+A  gauge ticks since a plug-in that woke the brush (boot: 100).
                              //                  A writes 100 on attach / removal, 0 when the attach woke the brush
    uint8_t  slew_cnt;        // 0x3fca4b5b  L+A  charge slew divider (A writes 0 on attach)
    uint8_t  full_cnt;        // 0x3fca4b68  L+A  gauge ticks at 100 % while charging (A writes 0 on attach)
    uint8_t  gauge_period;    // 0x3fc9ab96  L    gauge update period in ticks (boot: 64); see oem_gauge_rearm()
    uint8_t  batt_fault;      //             L    custom: 1 while the battery reading is unusable (ADC / calibration
                              //                  failure); batt_pct is then the saved value or a neutral 50
    uint16_t batt_raw_mv;     //             L    custom, diagnostics: the last oem_batt_mv_now() of the gauge tick,
                              //                  unfiltered and uncompensated (0 = no reading)

    // ---- additions by the input module (agent I may add fields below this line)
    // Diagnostics for the web UI / log; no core logic reads them.
    int16_t  force_raw;       // 0x3fca3294  I  last AW8686X sample (ADC value - 0x2000, before the input filter)
    int16_t  force_base;      // 0x3fca3292  I  zero-force level the force algorithm tracks (same unit)
    uint16_t force_coef;      // 0x3fca3290  I  calibration in use: pressure = (sample - base) * coef / 100
    uint16_t touch_x;         //             I  last trackpad report, IQS7222D register 0x14 (0xFFFF = no finger)
    uint16_t touch_y;         //             I  register 0x15

    // ---- additions by the power module (agent P may add fields below this line)
    uint8_t  motion_gate;     // 0x3fc9f307  A  non-zero = the GPIO8 ISR must not post OEM_EV_MOTION

    // ---- additions by the motor module (agent M may add fields below this line)

} oem_state_t;

extern oem_state_t g_oem;      // defined in oem_app.c

// Variables the stock firmware keeps in RTC slow memory (0x50001000..): they survive
// deep sleep only — the bootloader reloads them on every other reset, as on stock.
// The instance is provided by the HAL (hal_rtc()); on the brush it is the
// RTC_DATA_ATTR variable behind hw_power_rtc().
typedef struct {
    uint32_t magic;            // OEM_RTC_MAGIC once initialised
    uint8_t  ota_oneshot;      // 0x50001001
    uint8_t  strength;         // 0x50001019  intensity level 1..5 (invalid -> 3)
    uint8_t  hist_day;         // 0x5000101a  day of month of the history totals
    uint16_t hist_count;       // 0x5000101c  sessions today
    uint16_t hist_score_sum;   // 0x5000101e
    uint16_t hist_seconds;     // 0x50001020
    uint8_t  gyro_wakeup_count;// 0x50001022  consecutive motion wakes (limit 4, cap 20)
    uint8_t  reserved[16];     // modules may claim bytes here (document the use in your NOTES file)
} oem_rtc_t;
#define OEM_RTC_MAGIC 0x0c1ea200u

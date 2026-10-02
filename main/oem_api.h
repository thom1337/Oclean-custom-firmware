#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "oem_state.h"

// Functions the modules of the OEM-behaviour port call on each other. Each is the
// port of a stock function (address in the comment; details in re/spec/*.md). The
// block header names the owning module / file. All of them run with the core lock
// held (see oem_hal.h) and, unless noted, in the main ("brush_app") task.

// ===================================================================================
// UI — oem_ui.c (re/spec/ui_flow.md). Runs in the UI task unless noted.
// ===================================================================================
void    oem_ui_init(uint8_t *fb);                 // 0x42021368 ui_task_init; fb = 25600-byte frame buffer
// Queue a message for the UI task (any task; stock 0x4201f840). Ids below 70 are
// commands (0 screen on, 1 screen off, 7 swipe left, 8 swipe down, 9 swipe right,
// 10 swipe up, 12 auto page); ids 70..120 mean "show screen id".
void    oem_ui_post(uint16_t id, const void *payload, int len);
// One pass of the UI task loop body for the UI event bits that were set
// (stock 0x42022564): handles one queued message, the 50 ms blink tick, the gesture
// end and the factory-reset request, then composes the current screen. Returns true
// when the frame buffer holds a new frame that must be pushed to the panel.
bool    oem_ui_handle(uint32_t ui_event_bits);
uint8_t oem_ui_now(void);                         // UI-task-side current screen (0x3fc9c021)
bool    oem_ui_enabled(void);                     // drawing enabled (msg 0 / msg 1)
void    oem_ui_set_enabled(bool on);              // 0x4202149c clears it at once when going to sleep
// Touch lock (0x4202865c): arg 1 = toggle (2 s button hold), 0 = dismiss the popup.
void    oem_ui_lock_button(int arg);
bool    oem_ui_swipe_allowed(void);               // 0x4202860c
// Called by the main task at 1 Hz: the UI-owned part of the sequencer, if any.
void    oem_ui_page_reset(void);                  // forget side page / saved page (wake)
// Short press while on a side page (96 / 100): back to the mode page (0x42020ab8,
// main task). Added by the UI module; the button handler 0x4201cab8 needs it.
void    oem_ui_page_back(void);

// ===================================================================================
// app + brush engine — oem_app.c, oem_brush.c (input.md §3-4, power.md §2/§4,
// ui_flow.md §7.2/§8, brushing.md §2-6, led_battery_charge.md §3/§6.3/§6.4)
// ===================================================================================
void    oem_app_boot(int wake_cause);             // 0x4201cc40 up to the event loop
void    oem_app_handle(uint32_t event_bits);      // one pass of the 0x4201cc40 loop body
void    oem_button_push(uint8_t code);            // called by the button driver before posting OEM_EV_BUTTON
                                                  // code: 0 short, 1 = 2 s, 2 = 3 s, 3 = 5 s, 4 = 8 s
// idle timer ("sleep monitoring")
void    oem_idle_timeout(uint8_t seconds);        // 0x42014278 open_sleep_monitoring
void    oem_idle_off(void);                       // 0x42014268
void    oem_idle_kick(void);                      // 0x42014298
// screen wrappers (set g_oem.now_ui, dwell_s, then post)
void    oem_show(uint8_t screen, const void *payload, int len);
void    oem_show_main(void);                      // 0x4201a3fc uipxp_task_show_main_screen
void    oem_show_main_after_session(void);        // 0x4201a5ac
void    oem_show_score(uint8_t score);            // 0x4201a774 (posts 100)
void    oem_show_history(uint8_t flag);           // posts 84
void    oem_show_paused(void);                    // 0x4201c73c (posts 101 in this build)
void    oem_show_brushing(void);                  // 0x4201bb18 (posts 82)
void    oem_show_strength(uint8_t level_minus_1); // 0x4201a270 (posts 87)
// session control
void    oem_motor_stop(void);                     // 0x42019528 handle_motor_stop (ends the session)
void    oem_strength_step(bool up);               // 0x4201b50c
bool    oem_app_profile(void);                    // 0x42018fe8: profile[0]==1 || profile[2]==1
void    oem_set_mode(uint8_t mode);               // page_commit: profile[6] = profile[7] = mode
uint8_t oem_mode(void);                           // g_oem.profile[6]
void    oem_factory_reset(void);                  // 0x4201c6b8 (UI event 0x200 path)
void    oem_leave_show_mode(void);                // 0x4201a9a0
// Custom additions: commands from the web UI / MQTT / BLE. Thread-safe; they queue
// the command and post OEM_EV_REMOTE, the main task executes it.
void    oem_remote_brushing(bool on);             // like a short press that starts / stops a session
void    oem_remote_mode(uint8_t mode);            // 0..5
void    oem_remote_strength(uint8_t level);       // 1..5
void    oem_net_activity(void);                   // a client is using the web UI / BLE: keep the brush up

// ===================================================================================
// LED patterns + battery gauge — oem_led.c, oem_gauge.c (+ hw_led.c, hw_battery.c,
// hw_charge.c) (re/spec/led_battery_charge.md)
// ===================================================================================
// led: 0..3 = indicator LEDs (GPIO17..20), 4 = LCD backlight. state: 0 on, 1 off,
// 2 blink, 3 breathe. anim: 4 = immediate, 0..3 = run that script first.
void    oem_led_init(void);                       // led_init + channels
void    oem_led_set(int led, int state, int anim);// 0x4201dcc8
void    oem_led_all(int state);                   // 0x4201dd48
void    oem_led_abort_script(void);               // 0x4201dcbc
void    oem_led_tick(void);                       // 0x4201e03c, every 10 ms
void    oem_led_level(int hal_id, int level);     // set_led_light_level (direct call at 0x42018398)
void    oem_led_reinit_charge_light(void);        // idle on the charger: release GPIO19, LEDC on (0x4201d2ea)
// gauge
void    oem_gauge_boot(void);                     // start-up sequence (§5.6): restore record, first reading
void    oem_gauge_tick(void);                     // batt_tick 0x42017f08 (called from the 1 Hz block)
void    oem_gauge_save(void);                     // 0x42017990 save_battery_record
int     oem_batt_mv_now(void);                    // one fresh reading in mV (two reads averaged), 0 on failure
void    oem_charge_thermal_check(void);           // 0x42017e84 (1 Hz, on the charger)
// charger hardware
bool    oem_charger_present(void);                // GPIO9 low
void    oem_charge_allow(bool allow);             // set_CHARGE_EN_IO_level 0x4200d650
// ---- additions by the LED / gauge module (details: re/spec/NOTES_led.md)
// oem_led_init() is the boot / wake form: it releases the holds of GPIO17..21, puts
// the pins at "off" and runs led_init (as 0x4200d77c and 0x4200df88 do). It does not
// touch the pattern state. Call it once at boot before the first oem_led_set().
void    oem_led_park(void);                       // LED part of 0x4200dbbc / 0x4200dda4: GPIO17..20 plain outputs
                                                  // at "off", GPIO21 open-drain high, all held (LEDC detached)
// oem_led_reinit_charge_light() also re-applies the charge light itself
// (led 2: ON when power_state is FULL, else BREATHE), as stock does at 0x4201d2ea.
// Gauge hooks for the charge state machine (0x42017a0c), the session and the sleep
// code. The counters they also write directly are in g_oem (plug_cnt, slew_cnt,
// full_cnt); g_oem.gauge_inited is stock 0x4201838c.
void    oem_gauge_settle_reset(void);             // 0x420179e0: settle = 0, 99 -> 100 counter = 0
void    oem_gauge_rearm(uint8_t n);               // 0x420179f4: per_cnt = period - n (n = g_oem.gauge_period: restart)
void    oem_gauge_session_done(uint16_t secs);    // 0x42017d28: after a session the percent may drop freely once
void    oem_gauge_sleep_enter(void);              // 0x42017d54 (sleep_brush_task)
void    oem_gauge_sleep_exit(void);               // 0x42017da4 (wake 0x4201bd70)
void    oem_gauge_report_reset(void);             // 0x42017e74: BLE connected -> save / report the next 5 updates
void    oem_gauge_reset_record(void);             // 0x420179b8: write the "no record" default (factory reset)
// oem_batt_mv_now() returns 0 when the ADC or its calibration failed. A value below
// this is not a battery voltage (the CPU cannot run from it): treat it as "no
// reading", never as "empty" (boot gate, low-battery checks).
#define OEM_BATT_MV_MIN_VALID 2000
void    oem_batt_adc_init(void);                  // ADC part of brush_gpio_cfg 0x4200d77c (oem_batt_mv_now does it on first use)
// Charger pins (hw_charge.c). oem_charge_pins_init: GPIO45 low + held, GPIO26
// released, GPIO9 input with the any-edge interrupt that posts OEM_EV_CHARGER (level
// low, g_oem.init_ok >= 5 and g_oem.gauge_inited, as 0x40377c98), GPIO2 input with
// the falling-edge interrupt. The other three do it themselves on first use.
void    oem_charge_pins_init(void);
void    oem_wlc_off(void);                        // set_WLC_EN_IO_level(0) 0x4200d6b0: GPIO45 = 0, then oem_charge_allow(true)
bool    oem_charger_alive_take(void);             // a falling edge on GPIO2 since the last call (stock: clears the
                                                  // un-plug debounce counter 0x3fc9f304); call it from the 10 ms poll

// ===================================================================================
// input — hw_touch.c, oem_gesture.c, hw_button.c, hw_pressure.c (re/spec/input.md)
// ===================================================================================
void    oem_touch_init(void);
void    oem_touch_set_state(uint8_t st);          // 0x4201b430 (5 = running, 6, 7 = sleep, 0x21 ...)
uint8_t oem_touch_state(void);                    // 0x3fca4ded
void    oem_touch_irq(bool enable);               // 0x4200cc58 add / remove the GPIO12 ISR
void    oem_touch_step(void);                     // 0x4201b458: one state-machine step per RDY edge
void    oem_touch_tick_10ms(void);                // periodic housekeeping (3 s reseed), if needed
void    oem_gesture_end(void);                    // 0x42025afc, UI task on OEM_UIEV_GESTURE
void    oem_button_init(void);
void    oem_pressure_init(void);
void    oem_pressure_start(void);                 // start sampling (brush_notify_task_start_sensor)
void    oem_pressure_stop(void);
void    oem_pressure_sample(void);                // on OEM_EV_PRESSURE: updates g_oem.pressure

// ===================================================================================
// motor — oem_wave.c, hw_motor.c (re/spec/brushing.md §1)
// ===================================================================================
void    oem_motor_init(void);
void    oem_motor_gear(uint8_t gear_id, bool use_app_table);   // 0x4201e7c8 motor_gear
void    oem_motor_off(void);                      // stop the waveform, amp off
bool    oem_motor_playing(void);                  // 0x4201f178 (wave playing)
void    oem_motor_amp(bool on);                   // GPIO48 with hold (0x4200d148); used by the zone cue
void    oem_motor_app_gear_set(int index, const uint8_t entry[7]);  // BLE gear table (§2.5)

// ===================================================================================
// power + IMU — hw_power.c, hw_imu.c (re/spec/power.md)
// ===================================================================================
// Wake cause as app_main sees it (4.1)
#define OEM_WAKE_COLD    0
#define OEM_WAKE_BUTTON  1   // EXT1 GPIO3
#define OEM_WAKE_CHARGER 2   // EXT1 GPIO9
#define OEM_WAKE_MOTION  3   // EXT0 GPIO8
#define OEM_WAKE_TIMER   4
// Boot-time dispatch (app_main 0x4200c034 + brush_gpio_cfg 0x4200d77c): configures
// the GPIOs, applies the "go straight back to sleep" checks (empty battery, motion
// wake over the limit) — in which case it does not return — and returns the cause.
int     oem_power_boot(void);
void    oem_power_prep_screen_off(void);          // 0x4200dbbc
void    oem_power_restore_after_screen_off(void); // 0x4200df88
void    oem_power_deep_sleep(bool motion_wake);   // gpio_prep_deep_sleep 0x4200dda4 + "deep sleep2" (true) / "deep sleep1" (false)
void    oem_power_stay_alive(void);               // 0x420140f4
void    oem_power_resume_sleep(void);             // 0x42014134
void    oem_power_cpu_lock(bool take);            // 0x42014184 / 0x420141a8
void    oem_motion_irq(bool enable);              // 0x4200d75c add / remove the GPIO8 ISR
void    oem_imu_normal(void);                     // 0x4201e730
void    oem_imu_amd(void);                        // 0x4201e74c any-motion mode
void    oem_imu_power_down(void);                 // 0x4201e6f0
bool    oem_imu_temp(float *celsius);

// ===================================================================================
// Requests: a function you need from another module that is not listed above.
// Declare it here with the stock address and describe it in your NOTES file; the
// integrator assigns it.
// ===================================================================================
// motor (oem_wave.c) -> app / brush engine:
uint8_t  oem_brush_step_index(void);              // ses+0x20 step_index (steps started so far): motor_gear 0x42018edc
                                                  // indexes the app gear table with it when use_app_table is set
// UI (oem_ui.c) -> app / brush engine:
uint16_t oem_brush_remaining(void);               // 0x4201bb34: seconds shown on 82/99/101/102 (mode 5: done_s, else
                                                  // total_s - done_s; requests clip 6 at contact_s == 120 in mode 5)
void     oem_strength_gear(uint8_t level);        // 0x42019368: step[0].gear = ses.gear = strength_gear[level]
                                                  // (lock-popup restore of the intensity screen 87)
// app / brush engine (oem_app.c, oem_brush.c) -> input (details: re/spec/NOTES_app.md).
// oem_app.c holds weak fallbacks for these two so the firmware links before they are
// assigned: "no force sensor" and "post UI message 10".
bool     oem_pressure_available(void);            // the force sensor (AW8686X) answered at init and delivers samples.
                                                  // false: sessions start with motor_state 1 (no anti-splash wait)
                                                  // and the pressure reactions are skipped
// app -> UI:
void     oem_ui_page_back(void);                  // 0x42020ab8, main task: if g_oem.subpage == 1, page_update(1)
                                                  // (short press on a side page: back to the mode page, no lock check)
// app / brush engine offers (web UI / BLE; not called inside the core):
const uint8_t *oem_brush_record(size_t *len);     // last session that counted, stock record layout (brushing.md 6.2),
                                                  // RAM only; NULL before the first one

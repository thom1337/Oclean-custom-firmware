# NOTES_app — main-task application logic and brush engine

Module: `main/oem_app.c`, `main/oem_brush.c` (+ private `main/oem_brush.h`), host test
`re/tools/uisim/sim_app.c`. Port of the stock `brush_app` task (0x4201cc40) and everything it
runs directly. Nothing here was run on the device.

## 1. Files and entry points

| File | Content |
|---|---|
| `main/oem_app.c` | `g_oem`; `oem_app_boot`, `oem_app_handle`; idle timer (`oem_idle_*`); screen wrappers (`oem_show*`); button queue + dispatcher + short press; session start / pause / resume / end handler; charger poll + charge state machine; wake functions; screen-off sequence, second stage, BLE window, deep-sleep decision; 1 Hz block + screen sequencer; remote commands (`oem_remote_*`, `oem_net_activity`); `oem_factory_reset`, `oem_leave_show_mode` |
| `main/oem_brush.c` | `sys_config` / `user_config` blobs and defaults, mode clamp, `oem_set_mode`, `oem_mode`, `oem_app_profile`; history totals; profiles for modes 0..5; strength; session begin, step advance, 1 Hz clock, auto stop; zone cue; pause / resume / `oem_motor_stop`; pressure state machine; score; session record; `oem_strength_step`, `oem_strength_gear`, `oem_brush_remaining`, `oem_brush_step_index`, `oem_brush_record` |
| `main/oem_brush.h` | interface between the two files only |
| `re/tools/uisim/sim_app.c` | host scenarios (section 7) |

`oem_app_handle(bits)` order per pass, as stock: motion, session end, touch RDY, BLE wake,
10 ms tick (LED tick, charger poll, ..., every 100th: 1 Hz block), 30 ms fast tick, button,
charger event, remote (custom), sleep stage 2, BLE timeout, pressure sample.
`OEM_EV_SESSION_START` is posted and has no handler (as stock).

1 Hz block order (0x4201d0ec..): pending screen after a charger removal; session clock
(`brush_tick_1hz`); screen sequencer, or the Wi-Fi-off counter while asleep; gauge tick when no
session, else the per-second 82 refresh; backlight time-out on the charger, else the idle check;
`tick_s++`.

## 2. Stock functions ported

| Stock | Here |
|---|---|
| 0x4201cc40 main task: init part / loop body | `oem_app_boot` + `boot_finish` / `oem_app_handle`, `tick_10ms`, `block_1hz` |
| 0x4200c034 app_main: `load_daily_totals` 0x4201c100, `load_sys_config` 0x42018cf8, button-wake counter clear | `oem_app_boot` |
| 0x4201cab8 button dispatcher, 0x4201829c | `button_event` |
| 0x4201c790 short press (incl. start 0x4201c858.., pause, resume) | `short_press`, `start_session`, `pause_session`, `resume_session` |
| 0x4201b0d8 dismiss_screen | `dismiss_screen` |
| 0x4201b680 post_button_event | `oem_button_push` (queue; the busy marker is gone, see D3) |
| 0x42014278 / 0x42014268 / 0x42014298 / 0x420142d0 idle timer | `oem_idle_timeout` / `oem_idle_off` / `oem_idle_kick` / `idle_check` |
| 0x4201babc low-battery buzz | `buzz_tick` |
| 0x4201b1e4 1 Hz sequencer, 0x4201c168 | `sequencer_1hz` |
| 0x4201a3fc, 0x4201a5ac, 0x4201a3a0 (+0x4201a2b8..0x4201a364) | `oem_show_main`, `oem_show_main_after_session`, `show_mode_page` |
| 0x4201a774, 0x4201c1b0/0x4201a730, 0x4201c73c/0x4201a8c0, 0x4201bb18/0x4201a82c, 0x4201a270, 0x4201a260, 0x4201a2a4, 0x4201a7a8, 0x4201a94c | `oem_show_score`, `oem_show_history`, `oem_show_paused`, `oem_show_brushing`, `oem_show_strength`, `show_low_battery`, `show_charging`, `show_complete`, info screen in `button_event` |
| 0x42029218 wake screen, 0x420291a4 greeting id | `wake_screen`, `greeting_id` |
| 0x4201b900 sleep_brush_task, 0x4201b764 brush_work_ooer_to_sleep, 0x42014374 | `screen_off_sequence` (tick-delayed) |
| event 0x100000 handler (0x4201d2a8) | `sleep_stage2` |
| event 0x08 handler (0x4201d338, 0x4201d500) | `ble_timeout` |
| 0x4201c5d0 brush_pm_control | `pm_control` |
| 0x4201b92c / 0x4201b9b0 ble_timeout_in_sleep(_data), step 14 of 0x4201b764 | `ble_window_start`, `net_activity` |
| 0x4200bae0 / 0x4200bad0 Wi-Fi-off counter | in `block_1hz` / `screen_off_sequence` |
| 0x4201bd70 motorwakeup, 0x4201bee8 motorwakeup_for_charge | `wake`, `wake_for_charge` (`wake_begin`, `wake_end`) |
| 0x4201cf9d motion event, 0x4201b6f0 / 0x4201b714 / 0x4201b740 / 0x42023d34 gyro wake count | `oem_app_handle`, `gyro_limit` / `gyro_count_inc` / `gyro_count_clear` / `gyro_count_store` |
| 0x4201840c charger poll (0x42018398, 0x420183d0), 0x42017a0c usb_action, 0x42018250 | `charger_poll`, `usb_action`, `delay_to_close_screen` |
| 0x4201c2b8 session end + loop part 0x4201ce36 | `session_end` + `brush_session_finish` |
| 0x4201c6b8 factory reset, 0x4201a9a0 | `oem_factory_reset`, `oem_leave_show_mode` |
| 0x42018cf8 / 0x42018d74 sys_config | `brush_sys_config_load` / `_save` |
| 0x42018dd8, 0x42018a74, 0x420185b4, 0x420187b4 user_config | `brush_config_load`, `ucfg_unpack`, `brush_user_config_save` / `ucfg_pack`, `ucfg_defaults` |
| 0x42019008, 0x42018fe8, 0x42018fd0 | `brush_mode_clamp`, `oem_app_profile`, `oem_mode() == 5` |
| 0x4201c100, 0x4201c0cc, 0x4201bc88, 0x42023df0 / 0x42023e24 history | `brush_hist_load`, `brush_hist_reset`, `brush_hist_update`, `hist_save` |
| 0x42019050 load_profile, 0x420247e8 motor_data | `load_profile`, `motor_data_read` |
| 0x4201ba38 / 0x420194a8 session init, 0x420193f8 step advance, 0x420193c0 | `brush_session_begin`, `step_advance`, `program_done` |
| 0x42019560 1 Hz clock, pressure log, 0x42018514 | `brush_tick_1hz`, `pressure_log` |
| 0x4201962c zone cue, 0x4201bb34 remaining | `zone_cue`, `oem_brush_remaining` |
| 0x42019528 handle_motor_stop, 0x420197c0 pause, 0x4201977c resume | `oem_motor_stop`, `brush_pause`, `brush_resume` |
| 0x42018530, 0x420198fc, 0x42019860, 0x42019830, 0x420198dc pressure | `pressure_tick`, `contact_seen`, `released`, `ring_clear` |
| 0x4201b50c, 0x420192e8, 0x42019368, 0x42019398 strength | `oem_strength_step`, `brush_strength_set`, `oem_strength_gear`, `gear_to_strength` |
| 0x4201c260 (+0x4201c71c) score | `brush_score` |
| 0x42019f20 record (without the flash write 0x42024e5c) | `record_build`, `oem_brush_record` |

Not ported (as decided): OEM cloud jobs, cloud OTA prompt (screen 97, RTC 0x50001001), OTA
progress watchdog of the sequencer (0x3fc9ab99), production test, aging, shop-demo ring and its
3 s auto page, voice clips, zone tracker (0x420149e4), magnetometer calibration save
(0x4201492c), BLE state notifications (0x42015af0), charge-session log (0x3fca4b1a), "TimeRTC"
blob (clock persistence is the glue's), `sys[0x72]` / `sys[0x3a]` day counters at session end,
the "spa bubble" counters in `show_main`, the 5 s press counter 0x3fca4e92, event bits 0x04 /
0x2000 / 0x10000 / 0x400000 / 0x800000.

## 3. Persistent data

Same keys and layouts as stock, namespace `storage`:

* `sys_config` (100 bytes, 11 used) and `user_config` (100 bytes, 80 used): the byte maps are in
  the comments above `brush_sys_config_load` / `ucfg_unpack` (read from 0x42018a74 / 0x420185b4 /
  0x420187b4). Both blobs are kept as loaded and only the bytes this firmware has a variable for
  are replaced on a save, so stock-only bytes survive (record pointers 8..11, app gear flag 12,
  voice flags 48..50, cloud counters).
* `shuanhuan` (0x32 bytes): BE u16 seconds, score sum, count, u8 day. `wakeupcount` (10 bytes).
  `motor_data` (255 bytes) is only read (mode 0 schemes).
* A blob is not written when the stored content is already identical (stock rewrites
  `user_config` at every screen-off and `sys_config` at every boot).
* RTC (`hal_rtc()`): `strength`, `hist_*`, `gyro_wakeup_count`. No `reserved[]` byte claimed. The
  history totals are reloaded from NVS at every boot as in stock; `strength` only lives in RTC
  memory (the glue / `hw_power_rtc()` must give it 3 after a power loss; a value outside 1..5 is
  turned into 3 at use).
* `g_oem.zone_s[]` and `g_oem.score` are restored from `user_config` at boot (the UI's score side
  page reads them) and zeroed at every session start.

## 4. Deviations from stock, and why

D1. **Score substitute (decided).** `brush_score()`: with zone data the stock formula; with
`zone_s[]` all zero `min(100, done_s * 100 / total_s)`. Marked in the code.

D2. **No force sensor (decided).** `oem_pressure_available() == false`: `motor_state = 1` at start
and resume (no idle hum), the motor state machine is skipped, the pressure value is taken as 0
(led 2 "pressure fine" is on while the motor runs, as in a normal stock session).
In mode 5 the strength gear is put into `step[0]` *before* the first `step_advance` (stock
patches it afterwards, which only works because stock always starts in the hum).

D3. **Two blocking waits became tick-driven**, because `hal_delay()` keeps the core lock and with
it the UI task: (a) the 500 ms `vTaskDelay` in the main-task init: `oem_app_boot()` returns at
once, the rest (`boot_finish`: IMU init, boot stage, touch IRQ, LED 1, pressure start,
`init_ok = 5`, touch state 0) runs 50 ticks later, so the first screen is drawn immediately as
in stock; (b) the low-battery buzz (3 x 400 ms, 0x4201babc) runs from the 10 ms tick so the
warning screen is visible during it. The stock "busy" marker 0x3fca4198 (a press during the
buzz aborts that sleep check; the charger is ignored meanwhile) is not needed any more: a
press is handled at once and restarts the idle timer through `dismiss_screen`. The buzz is
cancelled by a session start, a charger attach and the screen-off sequence.
The other waits are `hal_delay()` as in stock: wake 100 ms, session start 30 ms, pause 100 ms,
resume 50 ms, session end 100 ms, deep sleep 100 + 50 ms, factory reset 20 + 100 ms.

D4. **`sleep_brush_task` is a countdown of 55 ticks**, not a task: touch state 7 at once, 550 ms
later the rest. 550 instead of 500 ms because the stock task still does NVS and IMU work before
its LED calls, and the fade-out script (520 ms) must be over or those calls are dropped by the
LED engine; `oem_led_abort_script()` is called before them as a guard. If the brush is woken
before the countdown ends, the sequence is not run (stock would run it on an awake brush).

D5. **Mode 5 after 150 s (decided):** the restore paths (contact, over-pressure release, resume)
use the strength gear `g_oem.gear`, not `step[step_index-1].gear` (= placeholder gear 10).

D6. **Stage 2 and BLE timeout check `asleep`.** Stock arms the motion interrupt in stage 2 even
when the brush was woken in the 200 ms before, and deep-sleeps on the BLE-timeout bit even if a
wake was handled earlier in the same loop pass. Both are skipped when not asleep.

D7. **Deep sleep refused / firmware upload.** `ble_timeout()` restarts the window instead of
sleeping while `g_oem.ota` is set; if `oem_power_deep_sleep()` returns (charger just attached,
QEMU), the window is started again (stock would stop the 10 ms tick and stay in the screen-off
stage). While asleep with `g_oem.ota` set the Wi-Fi-off counter is held at 0.

D8. **`g_oem.ota`**: honoured where stock tests its flag (button dispatcher, short press, idle
check, charging screen, removal screen). In addition the 10 ms tick ends a running session when
it becomes set (stock: event bit 4 posted by the OTA start).

D9. **Record**: built in RAM in the stock layout, not written to the picture partition
(decided). `oem_brush_record()` returns the last one. Byte 31 (clip-2 count) is 0.

D10. **App gear table** (`brushing.md` 2.5): `oem_motor_gear(..., use_app_table = false)` always.
The flag is `user_config` byte 12 and is preserved, but the table itself is RAM-only in stock
and nothing in this firmware fills it.

D11. **Safety nets, not stock:** a language index above 16 becomes 2; a profile with more than 13
steps is clamped and padded (stock reads one byte past its 40-byte record); a session whose
elapsed time reaches `total_s + 2` ends even if no step boundary matched (a scheme with a
zero-length step would otherwise run until the 16-bit counter wraps); `oem_factory_reset()`
switches a running motor off before the restart; the wake functions cancel a pending
screen-off countdown.

D12. **NVS writes only on change** (section 3), `gyro_count_clear()` only when the count is not 0.

D13. `oem_ui_page_reset()` is not called at wake: the UI notes say stock keeps the side-page
state across the screen-off stage.

D14. **Remote start** (custom): does in one go what the button needs two presses for (wake, then
start), waits up to 3 s for the wake and for the touch IC to reach state 5 and then starts
anyway; it is not blocked by a dismissable screen (same as the stock BLE remote start). Remote
mode also sets `saved_mode` (otherwise the screen-off sequence would restore the previous mode)
and saves `user_config` when the screen is off. Remote strength applies at once during a mode-5
session (stock applies a swipe only while screen 87 is up). The stock "remote start" flag
0x3fca308e (blocks the idle sleep forever, changes the end screen) is not reproduced.

## 5. What the code says where the specs are silent or differ

1. **First screen at boot is chosen before `user_config` is loaded** (0x4201cc40: 0x42029218 is
   called before 0x42018dd8). So at boot the birthday is still unset and `sys[0x76]` has its
   `.data` value 1: no birthday greeting after a deep-sleep wake, holiday greetings regardless
   of the stored setting. Kept (the `.data` values of `sys[]` are in the initialiser of `g_oem`).
2. **Session end rules** (0x4201c2b8): `user_quit` set (swipe): 103 if `done_s >= 120`, else mode
   page with 10 s; not set and `now_ui == 101` (pause time-out, press at the end): the same;
   otherwise (ended from 82 / 87, i.e. auto stop or a stop request): always 103, even after a
   few seconds. The record / history condition `done_s >= 15 && score != 0` is the same in all
   branches.
3. **Contact filter** (0x42019860, disassembly): the ring keeps its "filled once" flag, so after
   the first 510 ms of a session contact is recognised as soon as the 17-sample window varies by
   more than 29 and sums above 510: a few samples, not a full 510 ms.
4. **Over-pressure release** (0x420198fc): with `sys[0x37] == 0` stock never leaves state 2;
   unreachable because every start sets it to 1. The port always goes back to state 1.
5. **Wake screen after a charger removal with the backlight timed out** is posted at the top of
   the 1 Hz block, before the sequencer of the same block: the mode page follows 5 s later, not 6.
6. **`wake_for_charge` posts screen 120 without changing `now_ui`** (0x4202151c): a short press
   afterwards acts on the screen id from before the sleep. Kept.
7. **Daily-goal LED chase** (0x4201c168) is evaluated at "state 3, dwell 10" before the history
   screen, and needs a day roll-over since boot (0x3fca4dfe is never cleared). Ported.
8. **Asleep on the charger** is only reachable when the charger is attached in the ~0.5 s between
   the idle expiry and the screen-off sequence (the wake is refused by the wake counter). Then:
   no IMU any-motion mode, no BLE window, charge light re-initialised in stage 2, button ->
   screen 120.
9. **The 1 Hz block is counted in 10 ms ticks**, and ticks that fall into a blocking handler merge
   (event bit), so every wake / start / pause shifts the second boundary by up to the handler's
   delay. The idle timer uses the uptime in seconds, compared at those blocks. Same in the port.
10. **`0x4201829c`** resets the charger backlight counter on any button event when it is >= 30,
    also on battery (boot value 255).
11. `led_battery_charge.md` 6.2: the GPIO2 edge that clears the un-plug debounce comes through
    `oem_charger_alive_take()` in the poll.

## 6. Shared headers, requests, glue

`oem_state.h` (app block): `step_index`, `step_count`, `contact_s`, `muted`, `tick_s`.
`oem_hal.h`: nothing. No timer added.
`oem_api.h` Requests block:

* `bool oem_pressure_available(void)` (input) — now provided by `hw_pressure.c`.
* `void oem_ui_page_back(void)` (UI, 0x42020ab8) — now provided by `oem_ui.c`.
  `oem_app.c` keeps weak fallbacks for both ("no sensor", "post UI message 10") so a build
  without those files still links.
* offered: `const uint8_t *oem_brush_record(size_t *len)`.

Requests of others implemented here: `oem_brush_step_index` (motor), `oem_brush_remaining`,
`oem_strength_gear` (UI).

Calls into other modules beyond the original list: `oem_gauge_settle_reset`, `oem_gauge_rearm`,
`oem_gauge_session_done`, `oem_gauge_sleep_enter`, `oem_gauge_sleep_exit`, `oem_wlc_off`,
`oem_charger_alive_take` (LED / gauge module's hooks), `hal_anymotion_allowed` (power).
`oem_charge_thermal_check()` is not called here (the gauge tick does it); "full" is decided by
the gauge tick.

What the glue has to do:

1. Before `oem_app_boot(cause)`: `cause = oem_power_boot()`, the `*_init()` of the other modules
   (`oem_led_init`, `oem_motor_init`, `oem_touch_init`, `oem_button_init`, `oem_charge_pins_init`,
   `oem_ui_init(fb)`); `oem_app_boot` calls none of them. It calls `oem_gauge_boot()`,
   `hal_lcd_init()`, and 500 ms later `oem_imu_normal()`, `oem_touch_irq(true)`,
   `oem_pressure_start()`, `oem_motion_irq(false)`, `oem_motor_off()`, `oem_touch_set_state(0)`.
2. Start `HAL_TMR_TICK10` (periodic 10 ms) and call `oem_app_handle(bits)` with the lock held for
   every set of main event bits. The second half of the boot needs the tick.
3. `hal_net_sleep()` is called 27 s after the screen went off (stock counter), i.e. it should
   stop Wi-Fi right then; `hal_net_wake()` at every wake from the screen-off stage.
4. `hal_rtc()` must return initialised values (strength 3, hist_day 0xff, ...).
5. Web / MQTT / BLE: `oem_remote_*` and `oem_net_activity()` take the core lock themselves (it
   is recursive) and may be called from any task; they can block for as long as a main-task
   handler runs (at most about 0.6 s). `oem_net_activity()` on every client request keeps the
   brush in the screen-off stage (BLE window and Wi-Fi-off counter restart); while the screen is
   on it does nothing (the idle timer is not touched).
6. Stock's Wi-Fi / BLE event handlers, which are glue here: Wi-Fi STA connected -> if no session
   `oem_idle_timeout(60)`, `if (g_oem.batt_pct) oem_led_set(1, 0, 0)`, post `OEM_EV_BLE_WAKE`;
   BLE connect -> post `OEM_EV_BLE_WAKE`.
7. Web OTA: set `g_oem.ota` for the duration (button locked, no idle sleep, session stopped, no
   deep sleep). Stock refuses an update below 20 % battery. Clearing the flag lets an expired
   idle timer turn the screen off at once.

## 7. Host test

```
cc -std=gnu11 -Wall -Wextra -I main re/tools/uisim/sim_app.c main/oem_app.c main/oem_brush.c -o sim_app
./sim_app [-v] [scenario prefix]
```

Virtual time (1 ms steps), real timers of the HAL list, fakes for all other modules that record
their calls; each scenario runs in its own process. 30 scenarios, 400 checks, all pass (also with
`-fsanitize=address,undefined`). Both core files pass `re/tools/esp_syntax.sh`.

| Scenario | Result (times in s after boot) |
|---|---|
| boot_idle_sleep | screen 84 at 0, second boot half at 0.5, mode page 80 at 6.0, fade-out at 36.0, sequence at 36.55 (IMU any-motion, SLPIN, LEDs off, UI msg 1, BLE window 30 s), stage 2 at 36.75, Wi-Fi off 27 s later, deep sleep2 at 66.7 |
| net_activity, no_ssid_window | activity restarts the 30 s window and the Wi-Fi-off counter; 120 s window without an SSID |
| wake_session_mode5 (force sensor) | first press wakes only (100 ms, script 0, screen 84, gyro count cleared); second press: screen 87 {2}, gear 49 hum; contact -> gear 1; swipe up -> 87 {3}, gear 24; 5 s later touch IRQ off and screen 82; swipe on 82 only stores; force 500 -> gear re-issued in state 2, led 3 on, 700 -> blink, 34 samples below 390 -> state 1; cue: amp off 60 ms at 30, 60 .. 300 s; gear 24 again at 150 s (no gear 10); stop at 302 s; 103 -> 100 {100} after 1 s -> 84 after 10 s -> 80 after 10 s -> sleep after 10 s; history 1 / 300 s / 100; record bytes |
| mode1..mode4 (no sensor) | gears 54 / 47 / 50 / 48 at once, one gear command, stop at 120 / 180 / 120 / 150 s, 3 / 5 / 3 / 4 cues, score 100 |
| app_scheme | `motor_data` scheme with 3 steps: gears at 0 / 20 / 30 s, stop at total + 2; single-step BLE profile |
| pause_resume, swipe_quit_short | pause (motor off, touch 0x21, 101, clock frozen), resume (gear, 82), 30 s time-out -> mode page + 10 s, share-of-time score; swipe-quit at 130 s -> 103 -> score 72; at 12 s -> mode page, not counted |
| charger_idle | attach: LEDs off, script 2, screen 93, LED2 dark after 4 s; removal after 1.51 s -> mode page, script 0; backlight off after 30 s; button -> backlight only; full after 6 gauge ticks; removal after the time-out -> wake screen 4 s later -> mode page |
| charger_while_brushing / _while_asleep / asleep_on_charger / removed_asleep_on_charger / boot_on_charger | session stopped and counted, screen 93, no start on the charger; wake + 93 + second script on the third gauge tick; the race of 5.8, button -> 120; removal -> 120, sleep 30 s later; boot on the dock -> 93 after the gauge's first measurement, never sleeps |
| low_battery_10 / _idle / _0 | 94 at the idle expiry, 3 pulses (400 / 400 ms) only after a session, screen-off 3 s later; at 0 %: 94 at boot, no touch IRQ, press -> 94, deep sleep1 right after stage 2, IMU powered down |
| remote_awake / _asleep / _during_sleep_seq / _on_charger | mode + start + stop; start from the screen-off stage 120 ms after the command (fake touch IC), strength applied live; start requested during the sequence waits for it; refused on the charger |
| motion_wake | motion -> wake, count 1 (NVS); over the limit: IMU powered down at screen-off, motion ignored, BLE wake works, deep sleep1 |
| first_boot, after_factory_reset | empty NVS: screen 70, defaults, stage 0 -> 1; 2 s / 5 s holds; reset writes stage 11 and restarts; next boot: defaults, 8 s, button dead, deep sleep1 right after stage 2 |
| stock_config, web_ota, button_misc | stock blobs are read and survive a save; auto mode by time; raise-to-wake off; OTA flag; side page, lock popup, touch gate, greeting 85 after a wake |

An extra run with the real `main/oem_led.c` instead of the LED fake (not kept as a test) showed
the backlight on / off where expected in the boot, wake, charger and sleep scenarios.

Not tested: anything with real drivers or the glue; the real UI / gauge / input modules (only
fakes); timing of the touch IC (the fake reaches state 5 after three RDY edges); task
concurrency of `oem_remote_*`; clock changes; the factory / show / aging flags.

## 8. Unverified on hardware — watch for on first boot

1. **Touch gate.** As in stock a short press starts a session only when `oem_touch_state() == 5`.
   If the touch driver does not get there on the real chip, the button cannot start brushing
   (wake, pause, resume and the remote start still work; the remote start gives up waiting after
   3 s). The log line `short press: asleep .. touch <state> ..` shows the state.
2. **Anti-splash with a force sensor that answers but reads wrong.** With
   `oem_pressure_available()` true every session starts in the idle hum (gear 49) and switches
   to the real gear only when the contact filter fires (window spread > 29, sum > 510 in sensor
   units). If the scale is off the motor hums for the whole session. The units are an open
   question of `brushing.md`.
3. **Auto off.** Untouched, the brush turns the screen off 36 s after a wake and deep-sleeps
   30 s later (120 s without Wi-Fi credentials or with a BLE client); the web UI / MQTT are gone
   then until the next button / motion / charger wake. Client activity through
   `oem_net_activity()` is what keeps it up.
4. **First boot over existing stock settings**: `sys_config` byte 0 decides (1 / 2: normal;
   absent or 0: boot animation and, if never bound, the pairing guide 71..73 once; 11: shipping
   state: 8 s, then deep sleep with the button ignored until the next wake).
5. **Factory reset (8 s hold)** leads to that shipping state: after the restart the brush shows
   the boot animation for 8 s, ignores the button and deep-sleeps; the next wake is a first boot.
6. **Backlight after a button wake** stays at the script level (raw 2191) until a
   `led_set(4, ON, 4)` gets through; a session start within 2.3 s of the wake is dropped by the
   running script (stock behaviour).
7. **Seconds drift**: session seconds are counted in ticks (5.9); a 120 s programme lasts 120
   tick-seconds plus the handler delays.
8. `hal_net_sleep()` 27 s after screen-off against the custom web server / MQTT: check that
   nothing blocks in it (it runs in the main task with the lock held).

# NOTES — UI module (`main/oem_ui.c`)

Port of the stock UI task side (`re/spec/ui_flow.md`; stock `0x42022564` and the functions it
calls). Nothing here ran on the brush; it was exercised on the host with `re/tools/uisim/sim_ui.c`.

## 1. What is implemented

File: `main/oem_ui.c` (one file, no private header). Host test: `re/tools/uisim/sim_ui.c`.

| Entry point | Stock | Notes |
|---|---|---|
| `oem_ui_init(fb)` | `0x42021368` | calls `ui_render_init(fb, hal_res_read)`, resets all UI state, drawing enabled (stock `+0x14 = 1`), sets `g_oem.clock_mode = 0xFF`, `g_oem.wifi_weak = 1` (stock initial values) |
| `oem_ui_post(id, payload, len)` | `0x4201f840` | 10-slot FIFO, dropped when full (logged), payload zero-filled, posts `OEM_UIEV_MSG` |
| `oem_ui_handle(bits)` | `0x42022564` loop body | order as stock: gesture end (only on battery) -> one message (re-posts `OEM_UIEV_MSG` while the queue is not empty) -> blink tick -> LCD re-init (0x20) -> factory reset (0x200: `oem_leave_show_mode`, NVS `IntoShow`, `oem_factory_reset`) -> compose (not behind a dark backlight, 3.10). Returns true when a frame was composed |
| `oem_ui_now / enabled / set_enabled` | `0x3fc9c021`, frame object `+0x14` | |
| `oem_ui_lock_button(arg)` | `0x4202865c` | main task; includes the 400 ms buzz (`oem_motor_gear(0x35,false)`, `hal_delay(400)`, `oem_motor_off()`) and `oem_idle_timeout(30)` |
| `oem_ui_swipe_allowed()` | `0x4202860c` | |
| `oem_ui_page_back()` | `0x42020ab8` | main task: `if (subpage == 1) page_update(1)`. Added to the UI block (the app module asked for the same function under the same name in the Requests block) |
| `oem_ui_page_reset()` | none | see 3.9 |

Inside: message handlers for commands 0, 1, 7..12 and every screen id 70..120 that stock handles
(74, 75, 86, 96 and everything below 70 not listed are ignored, as in stock); `page_update`
`0x420208f8`, `page_commit` `0x4201af48`, `get_next_screen` `0x4201a9bc`, `get_last_screen`
`0x4201ac14`, left / right targets `0x4201aec0` / `0x4201ae50`, `screen_switch` `0x42020108` (both
lists of screen 81), `mode_page_update` `0x420207c8`, status icon `0x42020f94`, battery layout
`0x42021918`, greeting `0x4201fde0`, clock page `0x4201fb24` / `0x4201f900` / `0x4201f6dc` /
`0x4201f654` (all four clock modes including the temperature line), blink tick `0x42021e9c`, lock
`0x420285e0` / `0x42028640` / `0x420284c4` / `0x42028544`, screen draw `0x42023b5c` (dirty logic).

## 2. What the stock code says where the spec or the task text was unclear

1. **Frames are not pushed on every loop pass.** `0x42023b5c` draws (and the loop blits) only when
   the selected screen has its redraw flag set (set by every `screen_switch`) or an element of its
   list is dirty; `set_visible` marks an element dirty only when the visibility changes,
   `invalidate` always, writing a variant byte never. The port keeps one dirty flag per element and
   `oem_ui_handle` returns true only in that case. Consequences that are stock behaviour and were
   measured in the simulator:
   * mode pages: 2 frames/s (the status icon invalidates its four elements every 500 ms);
   * history 84: 20 frames/s for as long as it is shown (frame 13 keeps being invalidated);
     done 103: 20 frames/s; perfect-score animation: 20 frames/s for 2 s, then none;
   * charging 93: 12 frames/s (strip every 100 ms, battery picture every 250 ms); in the port
     only while the backlight is lit (3.10);
   * brushing 82: 1 frame/s (the per-second message). In mode 5 the background variant advances
     every 500 ms but only becomes visible with that 1 Hz redraw, so it moves two frames at a time.
2. Element visibility after `oem_ui_init` is the stock power-on state: everything visible. The four
   status icons overlap at (28,0) until the first blink tick (50 ms) sorts them out.
3. Message 0 while drawing is already enabled blanks the panel (`hal_lcd_init`) and nothing is
   redrawn until something gets dirty. Stock avoids that by order: `wake()` posts the screen first
   (not drawn, drawing is off) and then 0; `wake_for_charge()` posts 0 and then 120.
4. A variant of 0xFF is turned into 0 only by a draw. If the blink tick runs between the 84 message
   and its first draw (possible only while drawing is off), the animation jumps to frame 13. Same
   for 100 (animation never starts until drawn). Kept.
5. `get_next_screen` / `get_last_screen` compare `state` with 97 (`0x4201ab25`); a state is 0..4, so
   the term is dead and left out.
6. Lock task `0x42028544`: after a dismissal by short press (`0x4202865c(0)` restores the screen and
   clears the popup flag) the task notices the cleared flag within 10 ms and restores a second
   time. The port restores once.
7. Unlock list `0x3c11cf44` = {76..81, 87, 82, 83}; 82 only while `ui_running != 1`. Because
   `now_ui` is 91 while the popup is up, the "unlock while the popup is up" branch is dead.
8. Charging screen: at exactly 45 % `0x42021918` writes no battery level, and the 93 handler has
   just set the variant to 0xFF, so a charger plugged in at 45 % shows the empty picture until the
   percentage changes. The 120 handler never shows digit elements, it only hides the wrong sets;
   the digits visible on 120 are those the last charging layout left.
9. Score page with `score == 0xFF` (no session yet): the handler takes the 90..99 branch and uses
   digit variants 25 and 5, i.e. one wrong picture (#210 read as 14x24). Kept; see 6.
10. Countdown minutes are a single digit element; 10 minutes or more selects a non-digit picture.
11. `prepare_target` `0x42020e14` pre-renders the target screen into the same frame buffer, which
    is overwritten before the next blit; its result is true whenever the target is not 0xFF.
12. Descriptors written by stock that belong to no element (0x3fc9c425 in message 0, 0x3fc9c443 in
    89 / 90, 0x3fc9c431 in the blink tick, 0x3fc9c34d in 110) are skipped.
13. `0x42020ab8` passes `subpage` (= 1) as the direction, i.e. `page_update(1)`.

## 3. Deviations from stock, and why

1. **Blit only when dirty** (2.1) instead of "on every pass" as the task text put it: that is what
   the stock code does. The frame buffer is cleared only when a frame is composed (stock clears it
   on every pass and leaves it black when nothing is drawn); the panel sees the same frames and
   the buffer always holds the last frame.
2. **Lock popup without a task**: `lock_popup_start()` sets `locked`, `lock_popup`, shows 91 and
   arms a 1 s deadline that is checked on every `oem_ui_handle` pass (so at least every 50 ms).
   The deadline is re-armed when message 91 is handled, because `oem_ui_lock_button` keeps the
   core lock during the 400 ms buzz and the UI task cannot draw the popup before that; the popup
   is visible for 1.0..1.05 s as in stock. Single restore on dismissal (2.6).
3. **Guards where stock is unsafe** (all commented in the code):
   * 82: stock divides by `total_s / 15`; below 15 s that is a divide-by-zero trap. The progress
     frame is left unchanged in that case.
   * 96: weather codes above 6 give a NULL element in stock; no icon is shown.
   * 92 / 97: version characters that are not digits (or a string shorter than 7) give digit 0
     instead of an arbitrary picture. Clock digits use `hour % 10`, `(hour / 10) % 10`.
   * message payloads are zero-filled (stock: stack garbage behind `len`).
4. **Zone overlays** of 100 / 101 read `g_oem.zone_s[]` when the message is handled. Stock reads
   UI-side copies (`0x3fca4dcc..`) that the app fills when it posts 101 (`0x4201a8c0`) and before
   the score (`0x4201b170`); the mapping zone index -> element is the same (`ZONE_ELEM`).
5. **Clock page layout** is redone on the blink tick when `g_oem.clock_mode` differs from the
   mode the page was laid out for. Stock calls `0x4201fb24` from the BLE weather handler; here
   whoever provides weather only writes `g_oem`.
6. `0x4201a574` / `0x4201a538` (boot animation finished: mode page with 60 s idle, or pairing
   guide 71) and the completion of 95 are app-side functions in stock that only the blink tick
   calls; they are ported inside `oem_ui.c` (`boot_anim_done()`).
7. The Wi-Fi RSSI read inside the status icon (`esp_wifi_sta_get_ap_info`) is not in the core file;
   the hysteresis result is the field `g_oem.wifi_weak` (see 4).
8. Not ported (dead or factory only): message 2 / UI bit 0x100 slide animation; the side-page
   usage counters (`0x3fca4fed`, `0x4201fc54`); planned-zone overlays and zone blink of 99 / 102
   (`0x420223f0`, `0x42021db8`; `zone_flag` is never set); the history update `0x4201d5a4` on the
   98 branch; the aging pass counter on 70 / 117 (shows 00); the "loop the boot animation" flag
   `0x3fca4d8c` (BLE test command); the pre-render side effects of `0x42020acc` /`0x42020014`
   (shop demo); stock `printf` logging. Calibration screens 105..119 only set the variants and
   OK / NG marks the message table sets.
9. `oem_ui_page_reset()` is in the contract but has no stock counterpart: stock keeps `subpage`
   and the saved mode page across the screen-off stage (so after sleeping on a side page the
   first short press on the wake screen goes "back to the mode page" instead of starting a
   session) and loses them only with the reset after deep sleep. For stock behaviour do not call
   it on a wake from the screen-off stage.
10. **Nothing is drawn behind a dark backlight.** Stock keeps composing and pushing frames with
    the backlight off: on the charger 12 a second (2.1) for as long as the brush is docked,
    although the backlight goes off after 30 s. Here `oem_ui_handle` asks
    `oem_led_backlight_lit()` before it composes (`oem_led.c`: the backlight level the LED
    driver holds is not 0; the level and not the LED state, because the scripts fade the
    backlight without touching the state). A newly selected screen is still drawn once in the
    dark, as before (the redraw flag of `screen_switch`); after that nothing is drawn while the
    backlight is dark. Messages, the blink tick and the lock popup run as usual and the dirty
    flags stay set; the first pass that finds the light on sets the redraw flag and so redraws
    the whole screen. When a button press on the dock switches the backlight back on,
    `button_event` (`oem_app.c`) also posts `OEM_UIEV_MSG`: a pass without a message, so that
    the redraw comes at once and not with the next 50 ms tick.
11. **Setup AP passcode screen** (not stock). While `hal_setup_ap_code()` is non-NULL and
    neither `g_oem.session_active` nor `g_oem.ota` is set, `screen_draw` draws none of the
    selected screen's elements but the 9 digits on black, as three rows of three 18 x 28
    7-segment digits made of `ui_render_fill()` rectangles (no OEM pictures needed), in
    x 8..71, y 26..133, clear of the panel's rounded corners; a "1" is centred in its cell.
    It redraws when the code appears, changes or goes (compared with `s_code_drawn`). A
    first version drew small digits over the bottom of the current screen: on the brush the
    corners cut off the outer digits.

## 4. Additions to the shared headers

`oem_state.h`, UI block of `g_oem`:

| Field | Stock | Writer | Use |
|---|---|---|---|
| `tod_sub` | `0x3fca4de4` | **app**, in `oem_show_main` (`profile[1]==1`: `!profile[0] ? 0 : morning ? 2 : 3`) | name of screen 81; stays 0 -> name #399 |
| `cloud_state` | `0x3fca3870` | glue (optional) | status icon: with Wi-Fi up, 2 shows #709, 1 + weak RSSI shows #733 |
| `wifi_weak` | `0x3fc9aef8` | glue (optional) | 1 below -83 dBm, 0 at -79 dBm or better |
| `clock_mode`, `weather_flag`, `weather_t1`, `weather_t2`, `weather_code` | `0x3fc9b380`, `0x3fca4ff4` | glue (optional), after `oem_ui_init` | page 96. Default 0xFF = stock default: no clock, pictures #849 / #848. `clock_mode = 2` shows HH:MM (from `hal_time`) with "-- ~ --"; 0 shows the weather record |
| `ota_version[8]` | `0x3fca3ac8+200` | whoever posts 97 | "a.b.c.d" on the update prompt |

`oem_api.h`: `oem_ui_page_back()` appended to the UI block; `bool oem_led_backlight_lit(void)`
in the LED block (not stock, implemented in `oem_led.c`; see 3.10). Requests block:

* `uint16_t oem_brush_remaining(void)` — `0x4201bb34`, called when 82 / 99 / 101 / 102 are handled
  (stock calls it from the UI task; it also requests voice clip 6 at `contact_s == 120` in mode 5).
* `void oem_strength_gear(uint8_t level)` — `0x42019368`, called by the lock-popup restore of
  screen 87 after `oem_show_strength(strength - 1)`.

One HAL addition (not stock): `const char *hal_setup_ap_code(void)` (`oem_hal.h`; the setup AP's passcode, or NULL). No timers, no RTC bytes claimed.

## 5. What the UI expects from the other modules

* Glue: call `oem_ui_init` before anything posts a message (it empties the queue). UI loop:
  wait for bits, take the core lock, `if (oem_ui_handle(bits)) hal_lcd_blit(fb);`. The 50 ms
  `BLINK50` timer must run whenever the UI task runs (animations, lock popup deadline).
* App: `oem_show*` wrappers set `now_ui` / `dwell_s` and post, as in stock. The UI task itself
  writes these app-owned fields, exactly where stock does: `now_ui`, `dwell_s`, `state` (3 -> 4
  in `page_commit`, 4 after screen 95), `zone_flag` (cleared), and it calls `oem_set_mode`,
  `oem_idle_timeout`, `oem_idle_kick`, `oem_motor_stop`, `oem_strength_step`, `oem_show_score`,
  `oem_show_main`, `oem_show_history`, `oem_show_paused`, `oem_show_strength`, `oem_show`,
  `oem_touch_set_state(6)`, `oem_motor_playing`, `oem_led_set(3,1,4)` (shop demo),
  `oem_gesture_end`, `oem_leave_show_mode`, `oem_factory_reset`, and, not stock,
  `oem_led_backlight_lit` (3.10).
* `oem_motor_stop()` must not show a screen synchronously (stock only posts the session-end
  event): `page_commit` writes `now_ui` after it returns.
* The app persists `g_oem.locked` / `g_oem.saved_screen` (`user_config` 0x34 / 0x35) and should
  restore `g_oem.score` and `g_oem.zone_s[]` (low bytes in `user_config`) at boot, otherwise the
  score side page shows 2.9 until the first session.
* Going to sleep: `oem_ui_post(1, NULL, 0); oem_ui_set_enabled(false);` (`0x4202149c`).
* Web / BLE OTA: `oem_show(88, &percent, 1)` per progress step (the picture is `percent / 10`,
  100 % shows the "done" art), `oem_show(89, NULL, 0)` / `oem_show(90, NULL, 0)` for the result.
* Language: `g_oem.lang` is read at every use.

## 6. Unverified on hardware — watch on first boot

* Everything visual: the simulator has real art only for pictures 0..25 (battery, low battery,
  intensity levels, pairing guide, greetings); all other pictures were placeholders, so overlaps
  that depend on transparent areas (mode name over mode art, lock icon over the tooth map on 101,
  badge behind the score digits) could not be judged.
* An unbound brush (`sys[0x0c] != 2`) shows picture #732 at the top of every mode page; with the
  lock on it alternates with the lock icon at 1 Hz. If that looks wrong, the inputs are
  `g_oem.sys[0x0c]`, `g_oem.wifi_status`, `g_oem.cloud_state`.
* Score side page before the first session (2.9); charging picture at exactly 45 % (2.8).
* SPI load: 20 full frames per second on screens 84 / 103 (stock does the same at 1000 Hz tick).
* Dark backlight (3.10): on the dock, after the 30 s time-out, a button press must bring the
  charging screen back with the current percentage and the strip moving. A picture that stays
  frozen with the light on would be this gate.
* Lock: the popup should appear together with the buzz in stock; here it appears when the buzz
  ends (the main task holds the core lock for those 400 ms) and then stays 1 s.
* Page 96 shows no clock by default (stock default without weather); set `clock_mode = 2` for a clock.
* Which swipe direction is "up" is decided by the gesture decoder, not here.

## 7. Host test

`re/tools/uisim/sim_ui.c` (build / run lines in its header) provides fakes for the HAL and a
small model of the app (show wrappers, short press, session clock, 1 Hz sequencer, session end)
with virtual time, 115 checks, and dumps 145 frames (the last group, `l_apcode`, checks the
setup AP passcode screen: the whole screen black but the digits, hidden while brushing, back on
the mode page, the mode page restored when the AP goes); built with `-fsanitize=address,undefined`
and at `-O2`, all checks pass. Scripted and looked at as contact sheets: wake 84 (frames 0..13)
-> mode page after 6 s; both vertical rings with and without the app profile, screen 81 with its
four names and the alternative list; mode pages in languages 0, 2, 8, 16; right / left ring
(96, 100, back), swipe up / down and button press on a side page; score 42 / 87 / 95 / 100 (animation
to frame 19); clock page in modes 3, 2, 1 and weather with one- and two-digit, negative and
limit temperatures; mode-3 session with countdown and progress frame, pause 101 with zone
overlays, resume, done 103, score 100, history 84, mode page; quit while paused before and after
2 minutes; mode 5 with intensity 1..5, hand-over to 82, count-up; charging 93 at 0..100 % with
strip animation, no frame while the backlight is dark and one fresh frame when it is lit again
(3.10), 120, 94; info pages 0..3 with version; lock toggle, popup timing, blocked
swipes on 80 / 87 / 82, dismissal, unlock rules, icons; OTA 97 / 88 / 89 / 90; boot animation
with its three exits, pairing guide, greetings and birthday date layouts, 104, 98, 83, factory
106 / 117, dev overlay; queue overflow, drawing gate, gesture / factory bits; the guards of 3.3.
Not tested: the real app and glue, real pictures above id 25, shop-demo ring, calibration
screens beyond a smoke test, timing on the device.

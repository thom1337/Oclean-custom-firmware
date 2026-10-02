# Stock Oclean X Ultra 20 — UI state machine, pages, menu/settings (`ui_flow.md`)

Scope: which screen is shown when, what the user can do on each, timers, wake/sleep of the
display, renderer compositing rules and the data the UI reads. Source: stock app image
(IDF v5.1.1), Ghidra output in `$S/decomp/f2`, verified against Xtensa disassembly (`fd.py`)
where the decompilation dropped arguments. Stock tick is 1000 Hz, so `vTaskDelay(n)` = n ms.

Marking: **[C]** = CONFIRMED (read from code/data at the address given), **[I]** = INFERRED
(reasoning given). Element numbers `[n]` are indices into `STOCK_ELEMS[]` of
`$S/re/stock_ui_tables.h`; `#id` is a picture id (index into `STOCK_PIC_OFF[]`).
"desc" = the 6-byte `*C#` descriptor of an element; `desc[5]` is its *variant* byte.

Nothing in this document was observed on a device. Pictures were not available, so every
statement about what a picture *looks like* is [I] unless it follows from code.

---------------------------------------------------------------------------------------------

## 0. The ten most important findings

1. **There is no on-device settings menu.** The only things the user can change on the device
   are: brushing mode (swipe up/down on the mode page), intensity 1..5 (swipe up/down, only in
   mode 5 while the intensity screen 87 is showing), and the touch lock (hold button 2 s).
   Language, auto-mode-by-time, raise-to-wake, greeting pages, weather, custom picture are set
   only over BLE. [C] (all writers of the relevant globals are BLE handlers; see §10)
2. **Input devices are the button (GPIO3) and a touch strip** (Azoteq IQS IC, I2C addr 0x44 on
   the bit-bang bus SCL13/SDA14, RDY/INT = GPIO12). Touch produces only four gestures
   (up/down/left/right swipe); there is no tap. [C] 0x42025ca4 / 0x42025afc
3. **Page model:** a vertical ring of *mode pages* (screens 76..81 = modes) and a horizontal
   ring `mode page <-> clock/weather 96 <-> score 100`. Swipe up/down changes the mode, swipe
   left/right moves along the horizontal ring. [C] 0x4201a9bc, 0x4201ac14, 0x4201aec0,
   0x4201ae50, 0x420208f8
4. **Swipes are instant page switches.** The slide animation code (UI msg 2, event bit 0x100,
   0x42020e90, second frame buffer) is dead: nothing posts msg 2 or sets bit 0x100 and the
   second buffer is never allocated. [C]
5. **Backlight = LEDC channel 4 / GPIO21, active low.** Stock uses only: off (raw duty 8191),
   "pattern" level (raw 2191 = brightness 6000/8191) reached by a ~0.5 s fade-in on wake, and
   full (raw 0). Sleep = 500 ms fade-out, then SLPIN (0x10), raw 8191. The earlier claim
   "stock has no backlight control" is wrong. [C] §8
6. **Idle timeouts are per-state seconds values** passed to 0x42014278 (30 s on the mode page
   after a wake, 60 s after boot, 10 s after a brushing session, 5 s after dismissing
   info/low-battery, ...). Full table in §7.3. [C]
7. **First screen after a wake is not the mode page**: it is history 84 (or greeting 85, custom
   picture 104, low battery 94); the mode page follows after 6 s (or on swipe). After a cold
   first boot it is the boot animation 70. [C] 0x42029218, 0x4201b1e4
8. **Brushing screens in this build:** running = 82 (re-posted every second); paused = 101
   (zone map), not the "82 paused" variant; end = 103 (6 frames at 50 ms, looping, ~1 s) ->
   score 100 (10 s) -> history 84 (10 s) -> mode page -> sleep 10 s later. Zone-guided
   screens 99 / 102 / 98 need flag 0x3fca4e7a which no code sets non-zero. [C]/[I] §9
9. **Lock** (2 s button hold on a mode page) blocks swipes on screens 76..82 and 87; a blocked
   swipe shows popup 91 for 1 s. It is persisted (user_config[0x34]). The button still works
   while locked. [C] 0x4202865c, 0x42028544
10. **Language index** = NVS `storage/sys_config` byte 5, valid only if byte 9 == 0xAA,
    otherwise **2**. 17 languages (0..16). Only a BLE command changes it. [C] 0x42018cf8,
    0x42010e88

---------------------------------------------------------------------------------------------

## 1. Architecture

### 1.1 Tasks, timers, events

| Item | Value | Source |
|---|---|---|
| UI task | `xTaskCreatePinnedToCore(0x42022564, "UI_TASK", stack 0xC00, NULL, prio 3, NULL, tskNO_AFFINITY)` | [C] 0x42021368 |
| UI event group | handle at 0x3fca50e4; task waits `xEventGroupWaitBits(eg, 0x3FF, clearOnExit=1, waitAll=0, portMAX_DELAY)` | [C] 0x42022564 |
| blink timer | esp_timer "blink_timer", **periodic 50 000 us**, callback 0x4201f63c sets event bit 0x02 | [C] 0x42021368 |
| gesture timer | esp_timer "touch_timer" (`ui.gesture_timer`, handle 0x3fca50b0), **one-shot 70 000 us**, restarted on every touch sample (0x42021348); callback 0x4201f648 sets event bit 0x10 | [C] |
| "1s_timer" | created with an empty callback (0x421022c4) and never started — dead | [C] |
| 1 Hz sequencer | 0x4201b1e4, called from the brush_app main loop's 1-second section (0x4201d0ec; the main loop counts 100 x 10 ms ticks of the "periodic" esp_timer, event bit 0x40) | [C] |
| frame buffer | `malloc(0x6400)` = 80x160 RGB565, plus a 160-byte row buffer (0x3fca5100); object at *0x3fca5104 = {buf, buf2(=NULL), draw_target(=buf), u8, fill, u8 enabled @+0x14} | [C] 0x42023928 |
| ui_task_init | 0x42021368: alloc buffers, create event group + 3 timers, create task, start blink timer, `lang = sys_config[0x67]` (0x3fca5114 = 0x3fc9a69e[0x67]) | [C] |

Event bits of the UI event group:

| Bit | Set by | Action in UI task |
|---|---|---|
| 0x001 | message post 0x4201f840 (also re-set by the task while the queue is not empty) | pop ONE message and handle it (§5) |
| 0x002 | blink timer, every 50 ms | `blink_tick()` 0x42021e9c (§7.1) (skipped if 0x3fca4fe0 == 1; never set) |
| 0x010 | gesture timer (70 ms after the last touch sample) | if power state (0x3fca4b6c+8) == 2 (on battery): gesture decode 0x42025afc (§3.2) |
| 0x020 | 0x4201fd48 (BLE "set panel id" handler 0x42010214) | LCD re-init 0x42029120 |
| 0x100 | nobody | dead (slide animation 0x42020e90) |
| 0x200 | 0x4201fd54 (button held 8 s; also BLE 0x420111d0) | leave show mode, write NVS `IntoShow`, factory reset + restart (0x4201c6b8) |

After handling the bits, every loop iteration ends with (0x42022698..0x420226a7) [C]:

```c
if (fb->enabled) {                 /* +0x14, set by msg 0, cleared by msg 1 / 0x4202149c */
    memset(fb->buf, 0, 0x6400);    /* 0x42023af4 – always, even if nothing is drawn      */
    if (screen_draw(current_screen, 0))      /* 0x42023c34 -> 0x42023b5c                  */
        lcd_blit(0, 0, 80, 160, fb->buf);    /* 0x420239e0 -> 0x420288a8                  */
}
```

### 1.2 Message queue (0x4201f840 / 0x4201f80c / 0x4201f86c) [C]

```c
typedef struct { uint16_t id; uint16_t pad; uint8_t payload[10]; } ui_msg_t;   /* 14 bytes */
static ui_msg_t q[10];      /* 0x3fca5012 */
static uint8_t  q_count;    /* 0x3fca509e */
static ui_msg_t cur;        /* 0x3fca5004 */

void ui_post(uint16_t id, const void *payload, int len) {   /* 0x4201f840 */
    ui_msg_t m; m.id = id; if (len) memcpy(m.payload, payload, len);   /* rest is stack garbage */
    if (q_count < 10) q[q_count++] = m;        /* silently dropped when full; no lock          */
    xEventGroupSetBits(ui_eg, 1);
}
/* pop: cur = q[0]; memmove(q, q+1, (count-1)*14); count--  (FIFO) */
```

Message ids below 70 are commands; ids 70..120 are "show screen N" (the id is the screen id).
Every `uipxp_task_show_*` wrapper in the brush app first writes the screen id to
`now_ui_show` (0x3fc9ab9a, the brush-app-side "current screen") and then posts the message.
The UI task keeps its own copy `ui_now` (0x3fc9c021) set when the message is handled.

---------------------------------------------------------------------------------------------

## 2. Renderer (verified)

### 2.1 Screen / element model [C] 0x42023b5c, 0x42023c74, 0x421022f4, 0x42102330

* Screen header = `{u8 *flags, elem **list (NULL-terminated), ?, callback}`. All 49 headers
  have `flags = 0x01`, `callback = NULL`, and contain only picture elements (type 3). The
  status-bar screen pointer (0x3fca5108) is NULL. (checked by dumping every header)
* Element = `{u8 x, y, w, h; u32 type; desc*; ...; u8 *flags}`; flags bit0 = dirty,
  bit1 = visible, bit4 = set by `invalidate`. Initial flags of all 181 elements = 0x03
  (visible + dirty).
* `set_visible(elem, v)` (0x421022f4): if the visible bit changes, write it and set dirty.
* `invalidate(elem)` (0x42102330 with arg 1): set bit4 and dirty (bit0), regardless of visibility.
* `screen_switch(id)` (0x42020108): clears the frame buffer, updates the left/right targets
  (§6.3), selects the header and sets its flags bit0 (full redraw). Ids without a header
  (74, 75, 86, >120, 0xFF) leave the current screen unchanged.
* `screen_draw(scr, force=0)`: draw if the screen's flags bit0 is set (then clear it) **or any
  element in its list has the dirty bit** — including invisible ones. Then every element of
  the list is processed in list order: if visible, draw picture; clear its dirty bit.
  Consequence: every draw is a full-screen recomposition followed by a full 80x160 blit.
* Element **visibility and variant bytes are global state shared between screens** (many
  elements appear in several lists: digits [113..116] in 82/99/101/102, zone overlays in
  99/100/101/102, quadrants [108..111] in 82/83, lock icon [180], [170], ...). A port must keep
  one visibility flag and one variant per element, not per screen.

### 2.2 Picture drawer 0x40378440 [C]

```c
/* elem (x,y,w,h), desc = "*C#", hi, lo, variant */
void draw_pic(x, y, desc, w, h, variant /* = desc[5]; 0xFF is first rewritten to 0 at 0x42023c88 */)
{
    base = (hi<<8 | lo) - (hi==1 ? 1 : hi==2 ? 2 : hi==3 ? 3 : 0);
    g_key_id = base;                                 /* 0x3fca50fc, used by the compositor */
    if      (variant == 100) idx = lang + 609;       /* legacy codes – no stock caller uses them:  */
    else if (variant == 101) idx = lang + 592;       /* 0x420207c8 writes the language directly    */
    else if (variant == 102) idx = lang + 626;       /* as the variant of elements #609/#592/#626  */
    else                     idx = base + variant;
    if (h > 160 || w > 80) return;                   /* whole picture skipped                       */
    if (base == 998) { off = (*(u8*)0x3fc9a719 == 'B') ? 0x76E000 : 0x75E000; rows of 2*w bytes }
    else if (base < 927 || FUN_4201c71c() != 12) {   /* FUN_4201c71c() is hard-wired to return 12  */
        off = PIC_OFF[idx];
        for (row = 0; row < h; row++, off += 2*w) {  /* loop counter is u8: (y+row)&0xFF           */
            part_read(rowbuf, off, 2*w);             /* 0x40378794: partition type 0x40/0x00,      */
            composite_row(x, y+row, w, rowbuf);      /*   offsets > 0x800000 are not read          */
        }
    } else {                                         /* ids 927..950: zone overlays                 */
        off = 0x77E000 + ZONE_OFF[idx - 927];
        yy  = (base <= 937) ? 20 : 64;               /* element y is ignored                        */
        for (r = 0; r < 44; r++, yy++, off += 240) { /* 80 px x {b0,b1,alpha}                       */
            read 240 bytes; draw each run of pixels with alpha != 0 at (x+run_start, yy)
            /* stock quirk: a run starting at column 0 loses its first pixel, and a run still open
               at column 79 is not drawn (run start 0 doubles as "no run") */
        }
    }
}
```

`composite_row` = 0x40378298 called with h = 1:

* If `x + w > 80` or `y + 1 > 160` the row is dropped completely (so a picture that overhangs
  the bottom is cut at row 159, a picture that overhangs the right edge is not drawn at all).
* Pixels are compared/stored as the little-endian u16 loaded from the flash byte stream
  (flash order = panel order = high byte first; do not swap).
* Transparency depends on the **base id** (descriptor id, not base+variant). Verified from the
  disassembly 0x40378319..0x4037843c — the earlier list is correct:

```c
static bool key_black(uint16_t b) {          /* pixel 0x0000 is not drawn */
    switch (b) { case 0: case 53: case 118: case 297: case 331: case 348: case 365: case 382:
                 case 416: case 592: case 609: case 626: case 661: case 708: return true; }
    if (b >= 129 && b <= 133) return true;
    if (b >= 709 && b <= 735) return (0x05800041u >> (b - 709)) & 1;   /* 709,715,732,733,735 */
    if (b >= 842 && b <= 847) return true;
    if (b >= 849 && b <= 874) return (0x03FDFFEFu >> (b - 849)) & 1;   /* all but 853 and 866 */
    if (b > 937) return true;
    return false;
}
/* base 185: pixel value 0x4646 not drawn; base 195: pixel value 0xC7F9 (as loaded LE) not drawn */
```

Glyph/text elements (types 0 and 2, font code 0x4202568c) are not used by any screen. [C]

### 2.3 Language index [C]

`lang` = 0x3fca5114, copied from `sys_config[0x67]` (0x3fc9a69e+0x67) in ui_task_init and
updated by the BLE language command 0x42010e88. `sys_config[0x67]` is loaded from NVS
namespace `storage`, blob `sys_config` (100 bytes), **byte 5**; byte 9 must be 0xAA, otherwise
the firmware forces `lang = 2` (0x42018cf8). Range 0..16 (17 picture banks; the factory picture
test 0x42021c24 counts lang 0..16). BLE app-language-code -> index map (0x42010dd8):

```c
/* index by app code 0..17; 0xFF = invalid */
static const uint8_t APP_LANG_TO_UI[18] =
  {0xFF, 0, 1, 2, 11, 7, 12, 15, 5, 3, 4, 6, 16, 13, 10, 8, 9, 14};
```

Which human language each index is cannot be read from the code ([I]: index 2, the fallback,
is most likely English). Language-dependent elements simply get `variant = lang`.

---------------------------------------------------------------------------------------------

## 3. Inputs

### 3.1 Button (GPIO3, active low) -> events 0..4 [C] 0x40377dc0, 0x40377e74, 0x42019c84..0x42019ccc, 0x4201cab8

On press four one-shot timers start; on release they are stopped.

| Event | Condition | Handler effect (0x4201cab8) |
|---|---|---|
| 0 short | released after 80 ms <= t < 1500 ms (`(t-80) < 1420`) | see below |
| 1 | still pressed at **2.0 s** | lock toggle (§3.3) if normal mode and not show mode; also `saved_mode(0x3fc9ab9c) = mode` |
| 2 | still pressed at **3.0 s** | factory mode: aging-mode toggle. Normal mode: only if `now_ui == 97` (update prompt): decline -> 0x42011968 + mode page with 10 s timeout (0x4201a5ac) |
| 3 | still pressed at **5.0 s** | if `now_ui` in 76..81: show info screen 92 page 0 (0x4201a94c), idle timeout 30 s |
| 4 | still pressed at **8.0 s** | log "BTN_REBOOT_PRESS"; UI bit 0x200 -> exit show mode, NVS `IntoShow`=0, config reset (sys_config[0xd]=0,[0xe]=0x16,[0x34]=11), `esp_restart()` (0x4201c6b8) |

The events are cumulative: a hold passes through 2 s, 3 s, 5 s and 8 s in turn and each event
fires when its timer expires (a release after 1.5 s or more generates no short press). So
opening the info screen (5 s) always toggles the lock at 2 s first (popup 91 for 1 s when it
locks, then the mode page is back before the 5 s event); this is why screen 92 is exempt from
the lock in §5.1. The events are delivered through main-loop event bit 0x80 to the registered
callback (0x42019e0c -> 0x4201cab8). [C]

Common gate at the top of 0x4201cab8 [C]:

```c
if (ota_in_progress()) return;                    /* 0x42013a50 */
if (first_boot_flag /*0x3fca4ea0*/) return;
if (charge_bl_timeout_hit() /*0x4201829c: counter 0x3fc9ab89 >= 30, then reset to 0*/
    && power_state != 2) backlight_on();          /* 0x4201dcc8(4, 0, 4) */
sys_config[0x69] = 0;
if (asleep /*0x3fca41a4 == 1*/) {
    if (power_state != 2) {                       /* on the charger, display asleep */
        if (ev == 0) wake_for_charge();           /* 0x4201bee8 -> battery screen 120 */
        return;
    }                                             /* on battery: fall through */
} else if (power_state != 2) return;              /* awake on the charger: button does nothing else */
if (*(u8*)0x3fca4dff) return;
switch (ev) ...
```

Short press (event 0) [C] 0x4201cab8 / 0x4201c790 / 0x4201b0d8:

```c
if (normal_mode && !show_mode && locked && lock_popup_active) lock_popup_dismiss();   /* 0x4202865c(0) */
else if (screen_mode == 1) page_update(1);        /* on a side page (96/100): back to the mode page */
else short_press();                               /* 0x4201c790 */

short_press():
  if (aging_mode) return;
  if (battery_pct == 0 || ota) { if (battery_pct == 0) { idle_timeout(1); show(94); } return; }
  if (show_mode && !factory) { exit_show_mode(); return; }          /* 0x4201a9a0 */
  if (asleep) { clear_gyro_wake_count(); wake(); return; }          /* first press only wakes: 0x4201bd70 */
  /* dismiss: 0x4201b0d8 */
  if (now_ui in {71,72,73, 83, 92, 93, 94, 97, 100} || (now_ui == 84 && state in {3,4})) {
      if (now_ui == 97) start_ota();                                 /* 0x42011a00 */
      else { show_main(); idle_timeout(now_ui in {83,84,100} ? 10 : now_ui in {92,94} ? 5 : 60); }
      return;                                    /* (unless 0x42011340()==1) */
  }
  if (!session_active) { if (touch_ic_state != 5) return; start_brushing(); }   /* §9, screens 82/87 */
  else if (running) pause -> screen 101; else resume -> screen 82 (or stop if total time reached)
```

Note: on the wake screens 84/85/104 (state 0/1) a short press starts brushing directly.

### 3.2 Touch gestures [C] 0x42025ca4 (per sample), 0x42025afc (gesture end)

The touch driver (state machine 0x4201b458, sample read 0x420263fc: 12 bytes from register
0x10 of the IC at I2C 0x44; u16 at +8 and +10 are the two coordinates) calls
`touch_sample(p1, p2)` for every report. The UI only needs this decoder:

```c
/* state */ int cnt; int16_t startL, startS, minL = 1000, maxL = 0, hist[3]; bool longp, began_running;
int16_t dL, dS;

void touch_sample(uint32_t p1, uint32_t p2) {            /* 0x42025ca4 */
    if (p1 > 59999 && p2 > 59999) return;                /* "no touch" report */
    int L = 255 - p1, S = 255 - p2;                      /* L = long axis, S = short axis */
    hist[2] = hist[1]; hist[1] = hist[0]; hist[0] = L;
    restart_gesture_timer(70 ms);
    cnt++;
    if (session_active && !paused) began_running = true;
    if (cnt < 3) { startS = S; startL = L; return; }
    if (L < minL) minL = hist[1];                        /* previous sample, as in stock */
    if (L > maxL) maxL = hist[1];
    int16_t ext = (abs(maxL - startL) < abs(minL - startL)) ? minL : maxL;
    dS = S - startS;  dL = ext - startL;
    if (!longp && abs(dL) < 50) {
        if (cnt == 80) { ui_post(0x0B, "\x02", 1); cnt = 0; longp = true; }   /* no visible effect */
        else if (cnt > 100) cnt = 0;
    }
}

void gesture_end(void) {                                 /* 0x42025afc, 70 ms after the last sample */
    if (cnt >= 6 && !longp && sys_config[0x69] != 1 && now_ui != 94) {
        idle_timer_refresh();                            /* 0x42014298 */
        if (!(session_active && paused && began_running)) {
            unsigned A = abs(dL), B = abs(dS);
            if (A >= 20 && !first_boot_flag && B < A) {
                if (dL > 0)      ui_post(10, &startS_or_0, 1);   /* UI_CMD_SWIPE_SCREEN_UP   */
                else if (dL < 0) ui_post(8,  &startS_or_0, 1);   /* UI_CMD_SWIPE_SCREEN_down */
            } else if ((A < 20 && B >= 20) || (A >= 20 && B > A && A <= 49)) {
                if (dS > 0)      ui_post(9, 0, 0);               /* "ui_task_swipe_screen_right" */
                else if (dS < 0) ui_post(7, 0, 0);               /* "ui_task_swipe_screen_left"  */
            }
        }
    }
    cnt = 0; began_running = false; minL = 1000; maxL = 0; longp = false;
}
```

`gesture_end` runs only when power state == 2 (not on the charger). Which physical direction
"up" is on the handle is not derivable from the code (open question).

The touch interrupt (GPIO12 ISR) is **removed while brushing runs** (`0x4200cc58(0)`): at
brushing start in modes 0..4, and in mode 5 when the intensity screen times out; it is
re-installed on pause/stop/wake (`0x4200cc58(1)`). [C] 0x4201c9b4, 0x4201b1e4

### 3.3 Touch lock [C] 0x4202865c, 0x42028544, 0x420284c4, 0x420285e0, 0x4202860c

State: `locked` 0x3fca5d9c (persisted), `popup_active` 0x3fca5d94, `popup_saved_screen`
0x3fca5d98.

* Toggle = button event 1 (2 s), only if `0x3fca4e9b == 1` (normal mode) and not show mode.
  * Not locked: only when `now_ui` in 76..81: save `now_ui`, start task "show_lock_ui_task"
    (stack 0x1000, prio 3) which sets `locked = 1`, shows screen **91** and after 100 x 10 ms
    (1 s, or earlier when dismissed by a short press) restores the saved screen.
  * Locked: allowed when `now_ui` in {76..81, 87, 83} or (`now_ui == 82` and brushing is not
    running, 0x3fca50d6 != 1): `locked = 0`; if the popup is up, restore.
  * Both directions then: unless `now_ui == 87`: motor buzz `0x42018edc(0x35,0)`, 400 ms, stop;
    idle timeout 30 s.
* While locked, a swipe on a screen in `{76,77,78,79,80,81,87,82}` (table 0x3c11cf24) shows the
  popup 91 for 1 s instead of acting. Swipes are still processed on 92, 83 and 84; on every
  other screen they are ignored while locked.
* Restore (0x420284c4): 76..81 -> re-show that mode page; 87 -> intensity screen with the
  stored level; 82 -> 0x4201c73c (posts the paused screen 101; the 1 Hz refresh re-posts 82
  if brushing is running).
* Lock icon: element [180] `#842` at (28,0), see §9 "status icon".

---------------------------------------------------------------------------------------------

## 4. Global state used by the UI

| Name used here | Address | Type | Meaning | Src |
|---|---|---|---|---|
| `now_ui` | 0x3fc9ab9a | u8 | brush-app-side current screen id (init 0xFF) | [C] 0x4201a13c |
| `ui_now` | 0x3fc9c021 | u8 | UI-task-side current screen id (init 0xFF) | [C] |
| `screen_mode` | 0x3fca4fda | u8 | 0 = on the vertical (mode) ring, 1 = on a side page | [C] 0x420208f8 |
| `saved_mode_page` | 0x3fc9aefc | u32 | last mode page 76..81 shown (init 0xFF) | [C] 0x4201ffdc |
| `left_target` / `right_target` | 0x3fc9aef9 / 0x3fc9aefa | u8 | screens for swipe left / right, recomputed at every screen switch | [C] |
| `mode` | 0x3fca4c85+6 (copy at +7) | u8 | brushing mode 0..5 (default 5) | [C] |
| `profile[0..5]` | 0x3fca4c85+0..5 | u8 | [0] use_app_brush_profile, [1] (default 2; 1 = time-of-day sub profile), [2] (default 0) | [C] |
| `sub_mode` | 0x3fca4fec | u8 | name variant of screen 81 (1..4) | [C] 0x42020674 |
| `strength` | RTC slow 0x50001019 | u8 | intensity level 1..5 (invalid -> 3) | [C] |
| `state` | 0x3fca4e8f | u8 | 0 boot, 1 woken, 2 brushing session, 3 session ended, 4 after score | [C] |
| `dwell_s` | 0x3fca4de5 | u8 | seconds since the current screen was posted (saturates 30) | [C] |
| `session_active` | 0x3fca4d5b | u8 | brushing session exists (running or paused) | [C] |
| `run_flag` | 0x3fc9aba5 | u8 | 200 = running, 0 = paused | [C] |
| `ui_running` | 0x3fca50d6 | u8 | payload[0] of the last 82/101 message (1 = running) | [C] |
| `total_s` / `elapsed_s` | 0x3fca4c90+0x6e / +0x70 | u16 | planned session length / elapsed seconds | [C] |
| `zone_t[12]` | 0x3fca4cda | u16[12] | seconds brushed per zone (source for 100/101) | [C] |
| `score` | 0x3fc9aba3 | u8 | last score 0..100 (persisted; 0xFF in .data) | [C] |
| `batt_pct` | 0x3fca4b6c+0x0e | u8 | battery percent 0..100 | [C] |
| `power_state` | 0x3fca4b6c+8 | u32 | 2 = on battery, 1 = charging, 3 = charged/full | [C] |
| `asleep` | 0x3fca41a4 | u8 | display/system in the sleep-pending state | [C] |
| `locked` | 0x3fca5d9c | u8 | touch lock | [C] |
| `show_mode` | 0x3fca4dca | u8 | shop demo mode ("guitai"), NVS `storage/IntoShow` | [C] |
| `dev_mode` | 0x3fca4e9b | u8 | 1 normal, 2 factory test | [C] 0x42027c98 |
| `aging` | 0x3fca4e91 | u8 | factory aging ("cooker") mode | [C] |
| `lang` | 0x3fca5114 | u8 | language index 0..16 | [C] |
| `first_boot_flag` | 0x3fca4ea0 | u8 | stock name "button_long_reset_flag": 1 during the first boot after a factory reset (`sys_config[0x34]` was 11): buttons ignored, vertical swipes ignored, boot animation stays, sleep after 8 s | [C] 0x4201cc40, 0x4201cab8 |
| `sys_config[]` | 0x3fc9a69e | u8[0x7c] | [6] auto mode by time, [8] raise-to-wake (default 1), [0x0c] bound (2 = bound), [0x34] boot/guide state, [0x67] language, [0x69] touch off, [0x76] greeting pages | [C] |
| `info_page` | 0x3fca4dcb | u8 | page 0..3 of info screen 92 | [C] |
| `clock_mode` | 0x3fc9b380 | u32 | 0 weather, 1 loading, 2 no data, 3/0xFF none (init 0xFF) | [C] |
| weather | 0x3fca4ff4 | {u8 flag; s8 t1; s8 t2; u8; u32 code} | set over BLE (0x42011a60) | [C] |
| hist | RTC 0x50001020 / 0x5000101e / 0x5000101c / 0x5000101a | u16,u16,u16,u8 | day totals: seconds, score sum, session count, day-of-month | [C] 0x4201bc88 |
| birthday | 0x3fca33e4+2 / +3 | u8 | month / day (0xFF = unset) | [C] |

---------------------------------------------------------------------------------------------

## 5. UI task message table (0x42022564)

`P[n]` = payload byte n. "-> S" means `screen_switch(S)` is called at the end of the handler
(for all 70..120 handlers `ui_now = id` first; the handlers that end at label 0x42022d5f also
call `dev_overlay()` 0x42020644, which shows element [170] `#710` only in factory mode).

### 5.1 Commands

| Id | Name (log string / role) | Posted by | Handler |
|---|---|---|---|
| 0 | screen on | wake 0x4201bd70 / 0x4201bee8 | `lcd_init()` 0x42029120 (reset + init table + black fill), `fb->enabled = 1` |
| 1 | screen off | 0x4202149c (from brush_work_ooer_to_sleep; the poster also clears `fb->enabled` at once) | `fb->enabled = 0`; panel command 0x10 (SLPIN) via 0x42028918 |
| 2 | (slide-up animation) | nobody | dead |
| 7 | swipe left | gesture | lock check; `page_update(3)` |
| 8 | `UI_CMD_SWIPE_SCREEN_down` | gesture | lock check; §5.2 with dir 2 |
| 9 | swipe right | gesture | lock check; `page_update(4)` |
| 10 | `UI_CMD_SWIPE_SCREEN_UP` | gesture | lock check; §5.2 with dir 1 |
| 11 | long touch | touch_sample (P[0]=2) | stores P[0] in 0x3fca509f, calls an empty stub — no effect |
| 12 | auto page (show mode) | sequencer every 3 s in show mode | like 8 without lock checks |
| 0x2A, 0x31, 0x32, 0x33 | (battery / sensor values) | 0x42017f08, 0x4201baa4 | **not handled** by the UI task (ignored) |

"lock check" (msgs 7..10) [C] 0x420226d1..:

```c
if (locked && now_ui in {76..81, 87, 82}) { show_lock_popup(); break; }      /* 0x42028640 */
if (!( !locked || now_ui == 92 || now_ui == 83 || now_ui == 84 )) break;
```

### 5.2 Swipe up / down [C] 0x42022c39.. / 0x420226d1..

```c
/* dir: 1 = up (msg 10), 2 = down (msg 8) */
if (!(motor_wave_playing /*0x3fca4fc3==1*/ || !session_active || paused)) break;
if (session_active && !paused)
    strength_step(dir == 1);          /* 0x4201b50c */
else if (prepare_target(dir))         /* 0x42020e14: next/last screen != 0xFF */
    page_update(dir);                 /* 0x420208f8 */
if (session_active) touch_state(6);   /* 0x4201b430(6) */

void strength_step(bool up) {         /* 0x4201b50c */
    dwell_s = 0;
    int cur = level_from_code(motor_code);     /* 0x42019398: 0x33->1, 0x34->2, 0x18->4, 0x20->5, else 3 */
    int n = up ? (cur + 1 > 5 ? 5 : cur + 1) : (cur - 1 == 0 ? 1 : cur - 1);
    strength = n;                              /* RTC 0x50001019, written even if not applied */
    if (now_ui == 87) { show(87, n - 1); motor_code = code[n]; apply_to_motor_if_running(); }  /* 0x420192e8 */
}
/* level -> motor profile code (0x42019368): 1->0x33, 2->0x34, 3->0x01, 4->0x18, 5->0x20 */
```

So intensity can only be changed while screen 87 is on the display, i.e. in mode 5 during the
5 s after brushing starts (the dwell restarts with every step).

### 5.3 Screen messages 70..120 (id = screen)

| Id | Payload | What the handler sets before `-> id` |
|---|---|---|
| 70 | – | [179].var=0xFF(->0). If `aging==0`: hide [172..178]; else show them and update battery digits (0x420212e0) |
| 71,72,73 | – | nothing |
| 76..79 | – | `mode_page_update()` (0x420207c8, §9) |
| 80 | – | `mode_page_update()`; [149].var = lang |
| 81 | P[0]=sub mode | `sub_mode = P[0]` (then recomputed by mode_page_update) |
| 82 | P[0]=running(1)/paused(0), P[1]=(total-elapsed)&0xFF | §9 screen 82 |
| 83 | P[0]=score | §9 (unreachable in this build) |
| 84 | 8 bytes (hist seconds, score sum, count, flag) | [118].var = 0xFF (->0) |
| 85 | P[0]=greeting id | 0x4201fde0, §9 |
| 87 | P[0]=level-1 | [106].var = P[0]; [105].var = lang |
| 88 | P[0]=percent | [137].var = P[0]/10; [136].var = lang |
| 89 | – | [132].var = lang |
| 90 | – | [134].var = lang |
| 91 | – | [130].var = lang |
| 92 | P[0]=page 0..3 | §9 |
| 93 | P[0]=battery % | §9 |
| 94 | – | [89].var = 0 |
| 95 | – | [97].var = 0xFF (->0) |
| 96 | (never posted; only reached through page_update) | 0x4201fc28, §9 |
| 97 | – | [87].var = lang, [78].var = lang, new-version digits, §9 |
| 98 | – | [18].var = lang |
| 99 | – | zone overlays from plan (0x420223f0), countdown digits, show [16] |
| 100 | P[0]=score | §9 |
| 101 | P[0]=running/paused, P[1] | `ui_running = P[0]`; zone overlays from `zone_t` (0x420224a8); countdown digits |
| 102 | P[0] | zone overlays from plan; countdown digits |
| 103 | – | [1].var = 0xFF (->0) |
| 104 | – | nothing (picture 998) |
| 105..119 | factory / calibration | variants set to 0, OK/NG marks (0x42026f5c only) |
| 120 | – | battery layout from `batt_pct`, §9 |


---------------------------------------------------------------------------------------------

## 6. Page model and navigation

### 6.1 Modes and mode pages [C] 0x4201a3a0, 0x4201af48

```c
/* mode index -> screen id */
static const uint8_t MODE_SCREEN[6] = { 81, 79, 77, 76, 78, 80 };
/* screen 76..81 -> mode index */
static const uint8_t SCREEN_MODE[6] = { 3 /*76*/, 2 /*77*/, 4 /*78*/, 1 /*79*/, 5 /*80*/, 0 /*81*/ };
/* mode art / name pictures (elements of the screen lists) */
/* mode 0 (81): art #90 [155] + name #399/#609/#592/#626 by sub_mode   (app profile in use)
                art #103 [150] + name #399 [154]                        (alternative list, see §9)
   mode 1 (79): art #77  [157], name #382+lang [156]
   mode 2 (77): art #51  [161], name #348+lang [160]
   mode 3 (76): art #770 [164], name #771+lang [163]
   mode 4 (78): art #64  [159], name #365+lang [158]
   mode 5 (80): art #103 [150], name #715+lang [149] */
```

* Mode 0 ("exclusive"/app profile, screen 81) is only reachable when
  `app_profile() := profile[0]==1 || profile[2]==1` (0x42018fe8). At boot and after a key wake
  0x42019008 forces `mode = 5` if `mode > 5` or (`mode == 0` and `!app_profile()`).
* Mode 5 is special: count-up timer instead of countdown, animated progress art, intensity
  screen 87 at start with 5 adjustable levels (§9). [C] 0x42018fd0, 0x4201bb34
* If `sys_config[6] == 1` (auto mode by time of day, BLE setting) `show_main()` overrides the
  mode before showing: `mode = 1` if 03:01 <= time <= 12:00, else `mode = 2`. [C] 0x4201a3fc
  ([I]: mode 1 = a morning programme, mode 2 = an evening programme.)
* What the six modes are called (the text is in the name pictures) is not derivable from code.

### 6.2 `show_main()` = 0x4201a3fc ("uipxp_task_show_main_screen") [C]

```c
void show_main(void) {
    if (now_ui == 97) return;                    /* never interrupts the update prompt */
    dwell_s = 0;
    if (sys_config[6] == 1) mode = time_in(03:01..12:00) ? 1 : 2;
    if (profile[1] == 1)                         /* time-of-day sub profile for screen 81 */
        tod_sub = !profile[0] ? 0 : (time_in(03:01..12:00) ? 2 : 3);     /* 0x3fca4de4 */
    profile[7] = mode;
    show_mode_page(mode);                        /* posts MODE_SCREEN[mode]; for 81 payload =
                                                    profile[2] ? 4 : (profile[1]==1 && tod_sub in {2,3}) ? tod_sub : 1 */
    idle_timeout(dev_mode == 2 ? 60 : 30);
}
```
`0x4201a5ac` is the same but ends with `idle_timeout(10)` (used after a brushing session).

### 6.3 Targets [C]

Vertical ring (mode pages only):

```c
/* swipe UP  = get_next_screen 0x4201a9bc :  80 -> 79 -> 77 -> 76 -> 78 -> (app_profile() ? 81 : 80);  81 -> 80
   swipe DOWN= get_last_screen 0x4201ac14 :  78 -> 76 -> 77 -> 79 -> 80 -> (app_profile() ? 81 : 78);  81 -> 78
   in mode indices: up 5,1,2,3,4,(0),5...   down 4,3,2,1,5,(0),4... */
static uint8_t mode_page_for_mode(void) { return MODE_SCREEN[mode]; }   /* the "tail" of both functions */

uint8_t get_next_screen(void) {                 /* 0x4201a9bc; 0xFF = none */
    uint8_t s = now_ui;
    if (show_mode) {                            /* demo ring: 80/81 -> 79 -> 77 -> 76 -> 78 -> 96 -> 100 -> 84 -> 80 */
        if (s == 80 || s == 81) return 79;
        switch (s) { case 79: return 77; case 77: return 76; case 76: return 78; case 78: return 96;
                     case 96: return 100; case 83: case 100: return 84; case 84: return 80; }
        return 0xFF;
    }
    if (screen_mode == 1 && (s == 83 || s == 84 || s == 96 || s == 100)) return saved_mode_page;
    if (!((state == 0 || state == 1) && (s==83 || s==84 || s==85 || s==96 || s==100 || s==104))) {
        switch (s) {
        case 92: return 92;                     /* info: stays, page index changes in page_commit */
        case 71: return 72;  case 72: return 73;  case 73: break;      /* -> tail */
        case 80: return 79;  case 79: return 77;  case 77: return 76;  case 76: return 78;
        case 78: return app_profile() ? 81 : 80;
        case 81: return 80;
        default:
            if (!(hist_count /*RTC 0x5000101c*/ != 0 && state == 3) || s == 98) {
                if (s == 98) { zone_flag = 0; ...; idle_timeout(1); return 0xFF; }
                if (s != 101 && s != 102 && s != 82) return 0xFF;
                if (!(session_active && paused)) return 0xFF;
                if (s != 102) {
                    if (elapsed_s >= 120) return (s == 82) ? 83 : 100;   /* quit after >= 2 min -> score */
                    zone_flag = 0;
                }
            } else {                                                    /* state 3 with history */
                if (zone_flag) return (s == 83 || s == 100) ? 98 : 0xFF;
            }
        }
    }
    return mode_page_for_mode();
}
/* get_last_screen 0x4201ac14 is identical except for:
     71 -> 0xFF, 72 -> 71, 73 -> 72
     78 -> 76, 76 -> 77, 77 -> 79, 79 -> 80, 80 -> app_profile() ? 81 : 78, 81 -> 78
     paused quit: s == 102 -> 100
     show mode: 78 -> 76, 76 -> 77, 77 -> 79, 79 -> 80, 80/81/82 -> 84, 84 -> 100, 83/100 -> 96, 96 -> 78
   (a dead "spa bubble" branch on flag 0x3fca4fd6, which is always 0, is omitted) */
```

Horizontal ring, recomputed inside every `screen_switch(new)` (0x4201ffdc):

```c
if (new in 76..81) { screen_mode = 0; saved_mode_page = new; }
left_target  = left_of(saved_mode_page);      /* 0x4201aec0 */
right_target = right_of(saved_mode_page);     /* 0x4201ae50 */

uint8_t left_of(uint8_t saved) {              /* swipe LEFT (msg 7) */
    if (show_mode || saved == 0xFF) return 0xFF;
    if (now_ui in 76..81) return 100;                       /* score page (83 if FUN_4201c71c()==8: never) */
    if (now_ui == 83 || now_ui == 84 || now_ui == 100) return 96;
    if (now_ui == 96) return saved;
    return 0xFF;
}
uint8_t right_of(uint8_t saved) {             /* swipe RIGHT (msg 9) */
    if (show_mode || saved == 0xFF) return 0xFF;
    if (now_ui in 76..81) return 96;                        /* clock / weather page */
    if (now_ui == 96 || now_ui == 84) return 100;
    if (now_ui == 83 || now_ui == 100) return saved;
    return 0xFF;
}
```

So:  `LEFT:  mode -> 100 -> 96 -> mode`,  `RIGHT: mode -> 96 -> 100 -> mode`.
From a side page, swipe up/down or a short button press returns to `saved_mode_page`.
The pages cannot be disabled individually; the only configuration effects are `app_profile()`
(screen 81 in the ring) and `show_mode` (no side ring, demo ring instead).

### 6.4 `page_update(dir)` = 0x420208f8 ("update_now_ui_index") [C] (disassembly-verified)

```c
void page_update(uint8_t dir) {               /* 1 up, 2 down, 3 left, 4 right */
    uint8_t old = now_ui; bool old_is_mode = (old >= 76 && old <= 81);
    uint8_t last = get_last_screen(), next = get_next_screen();
    switch (dir) {
    case 1: if (screen_mode == 0) ui_now = next; else { ui_now = saved_mode_page; screen_mode = 0; } break;
    case 2: if (screen_mode == 0) ui_now = last; else { ui_now = saved_mode_page; screen_mode = 0; } break;
    case 3: if (old_is_mode || screen_mode == 1) { screen_mode = 1; ui_now = left_target;  } break;
    case 4: if (old_is_mode || screen_mode == 1) { screen_mode = 1; ui_now = right_target; } break;
    }
    if (old == 96 && ui_now != 96) clock_hide_all();               /* 0x4201f47c */
    side_dwell_flags = 0;                                          /* 0x3fca4fed, usage statistics only: */
    if (screen_mode == 1) side_dwell_flags = ui_now == 96 ? 1 : (ui_now == 83 || ui_now == 100) ? 4 : ui_now == 84 ? 0x10 : 0;
    page_commit(ui_now, dir);                                      /* 0x4201af48 */
    mode_page_update();                                            /* 0x420207c8 */
    if (screen_mode == 1 && (ui_now == 83 || ui_now == 100)) {
        show_score(score);                 /* "swipe right or left": posts 100 with P[0] = score (0x4201a774) */
    } else if (ui_now != 100)              /* "set_ui_screen_acttive(now_show_screan)" */
        screen_switch(ui_now);
}

void page_commit(uint8_t new_scr, uint8_t dir) {                   /* 0x4201af48 */
    idle_timer_refresh();
    if (new_scr == 0xFF) return;
    if (dir == 1 || dir == 2) {
        if (new_scr in {71,72,73}) dwell_s = 0;
        if (now_ui == 73 && new_scr in 76..81) idle_timeout(30);
        else if ((state == 3 || state == 4) && now_ui in {83,84,100}) { dwell_s = 0; if (state == 3) state = 4; }
        else if (now_ui in {101,102,82}) { user_quit = 1 /*0x3fca4dc8*/; motor_stop(); }   /* 0x42019528: ends the session */
        else if (now_ui == 92) {
            info_page = (dir == 1) ? (info_page ? info_page - 1 : 3) : (info_page < 3 ? info_page + 1 : 0);
            ui_post(92, &info_page, 1);
        }
    }
    idle_timer_refresh();
    now_ui = new_scr;
    if (new_scr in 76..81) {
        mode = profile[7] = SCREEN_MODE[new_scr - 76];
        if (state == 1) idle_timeout(30); else if (state == 3 || state == 4) idle_timeout(10);
    }
    if (show_mode) led(3, 1, 4);
}
```

`prepare_target(dir)` (0x42020e14) returns false (and the swipe is ignored) when the target is
0xFF; otherwise it only pre-renders the target into the buffer (no visible effect).

---------------------------------------------------------------------------------------------

## 7. Timers

### 7.1 Blink / animation tick, every 50 ms (0x42021e9c) [C] (disassembly-verified)

Runs regardless of the current screen unless noted. `phase` toggles every tick (0x3fca4fd1), so
"on phase" = every 100 ms.

```c
void blink_tick(void) {
    phase ^= 1;
    /* boot animation, screen 70 ([179] #226) */
    if (aging) { invalidate([179]); show([172..178]); update_aging_digits(); if (phase) v179++; if (v179 == 19) v179 = 0; }
    else if (v179 < 19 && ui_now == 70) {
        invalidate([179]); if (phase) v179++;
        if (v179 == 19) { if (*(u8*)0x3fca4d8c == 1) v179 = 0; else boot_anim_done(); }   /* 0x4201a574(70) */
    }
    /* charging strip [147] #735: every 2nd tick, only when power_state != 2 */
    if ((++c100 & 1) == 0 && power_state != 2) { v147 = (v147 <= 20) ? v147 + 1 : 0; invalidate([147]); }
    /* every 5th tick (250 ms) */
    if (++c250 % 5 == 0) {
        battery_layout();                         /* 0x42021918, does nothing when power_state == 2 */
        if (mode == 5) {                          /* brushing background [117] #577 animates in mode 5 */
            if (v117 == 14) v117 = 0; else if (v117 <= 13 && phase) v117++;
        }
    }
    if (ui_now == 88) { show([137]); invalidate([137]); }
    if (ui_now == 95 && v97 <= 18) { if (phase) v97++; invalidate([97]); if (v97 == 19) after_95(); }  /* 0x4201a574(95) */
    if (ui_now == 84 && !show_mode) { v118 = min(v118, 12) + 1; invalidate([118]); }     /* every tick: 50 ms/frame, 0..13 then holds 13 */
    if (ui_now == 100 && v3 <= 18) { if ((c250 & 1) == 0) v3++; invalidate([3]); }       /* 100 ms/frame, 0..19 then holds */
    else if (ui_now == 103) { v1 = (v1 < 5) ? v1 + 1 : 0; invalidate([1]); }             /* every tick: 50 ms/frame, loops 0..5 */
    status_icon();                                /* 0x42020f94, §9 */
    if (now_ui == 96) {                           /* clock page */
        if (clock_mode == 1) {                    /* "loading" animation [38] #850, 100 ms/frame, 5..0 */
            if (t96 < 100) t96 += 100; else { t96 = 0; f96 = f96 ? f96 - 1 : 5; hide([38]); v38 = f96; show([38]); }
        }
        clock_digits();                           /* 0x4201f6dc */
    }
    zone_blink();                                 /* 0x42021db8: only on screen 99 */
}
```
`vN` = variant byte of element [N]. (Stock detail: `ui_now == 100` with `v3 > 18` skips the 103
check; irrelevant because the screens are exclusive.)

Corrections to the earlier analysis: screen 103 runs at **50 ms per frame and loops** (not
100 ms one-shot); screen 84 runs at 50 ms per frame; in mode 5 the brushing background cycles
frames 0..14 at 500 ms per frame.

### 7.2 1 Hz sequencer (0x4201b1e4) [C]

```c
void sequencer_1hz(void) {
    side_dwell_stats();                                   /* 0x4201fc54: usage counters only */
    if (show_mode) {
        if (++c3 >= 3) { c3 = 0; ui_post(12, 0, 0); }     /* demo: next page every 3 s */
        if (batt_pct < 11) { show_mode = 0; show_main(); }
    }
    if (power_state != 2 || asleep || *(u8*)0x3fca4e90) return;
    if (now_ui == 88) return;                             /* (flag 0x3fca4fd9 is never set) */
    if (dwell_s < 30) dwell_s++;
    switch (now_ui) {
    case 71: if (dwell_s == 2) show(72); break;
    case 72: if (dwell_s == 2) { show(73); idle_timeout(10); } break;
    case 73: break;
    case 87: if (dwell_s >= 5) { touch_irq(false); now_ui = 82; } break;   /* "strength screen to brushing screen";
                                                                              no message – the per-second 82 refresh takes over */
    default:
        if ((state == 0 || state == 1) && (now_ui == 84 || now_ui == 85 || now_ui == 104)) {
            if (dwell_s == 6) show_main();                                  /* wake screen -> mode page */
        } else if (state == 3 && (now_ui == 83 || now_ui == 84 || now_ui == 100)) {
            if (dwell_s == 3) { if (zone_flag) show(98); }
            else if (dwell_s == 10) {
                if (hist_count != 0) { show_history(1); state = 4; }        /* 84 */
                else { show_main(); idle_timeout(10); }
            }
        } else if (state == 4 && (now_ui == 83 || now_ui == 84 || now_ui == 100)) {
            if (dwell_s == 10) { show_main(); idle_timeout(10); }
        } else if (now_ui == 98) {
            if (dwell_s == 20) { zone_flag = 0; ...; idle_timeout(1); }
        } else if (now_ui == 103 && dwell_s == 1) {
            copy zone_t[] to the UI copies; show_score(score);              /* 100 */
        }
    }
    if (ota_wd < 100) ota_wd++;                    /* 0x3fc9ab99, reset to 0 by every OTA progress message */
    if (ota_wd == 30) { show(90); delay(1000); esp_restart(); }
}
```
`dwell_s` is zeroed by most `show_*` wrappers (those for 70 excepted), and incremented before
the comparisons, so "dwell_s == N" fires between N-1 and N seconds after the screen was posted.

### 7.3 Idle timeout -> display off [C] 0x42014278, 0x420142d0, main loop 0x4201d0ec

`idle_timeout(T)` = 0x42014278: `timeout_s = T; t0 = uptime_s; armed = 1`.
`idle_timer_refresh()` = 0x42014298: `t0 = uptime_s` (called at every completed gesture and in
`page_commit`). Checked once per second, **only** when: `power_state == 2`, no BLE-app activity
flag (0x42011340), not show mode, no OTA, `0x3fca4d8d != 0xD7`, `sys_config[0x70] == 0` (when
it is non-zero the code evaluates `is_mode_page(0)`, which is false, so the device never
idles out — 0x4201d441), and no brushing session (`session_active == 0`, so not while paused
either).

```c
bool idle_expired(void) {                        /* 0x420142d0 */
    if (!armed || asleep) return false;
    uint32_t dt = uptime_s - t0;
    if (dt < timeout_s) return false;
    if (batt_pct <= 10 && power_state == 2 && dev_mode != 2) {
        if (dt == timeout_s) { show(94); /* low battery */ if (batt_pct && buzz_pending) triple_buzz(); }
        if (dt < timeout_s + 3) return false;    /* low-battery screen stays 3 s */
    }
    asleep = 1; return true;                     /* -> go to sleep, §8.2 */
}
```

Values passed to `idle_timeout()` (seconds):

| T | Where | Situation |
|---|---|---|
| 60 | 0x4201cc40 | cold boot (then 7 if sys_config[0x6f]=='7', 8 on the very first boot after reset) |
| 60 | 0x4201a538 | boot animation finished -> mode page |
| 30 (60 in factory mode) | 0x4201a3fc | every `show_main()` |
| 30 | 0x4201bd70 / 0x4201bee8 | every wake |
| 30 / 10 | 0x4201af48 | mode page selected by swipe: 30 if state==1, 10 if state 3/4, unchanged otherwise |
| 30 | 0x4201a94c | info screen 92 opened |
| 30 | 0x4202865c | lock toggled |
| 30 | 0x42017a0c | charger plugged / unplugged (1 if unplugged with battery 0) |
| 10 | 0x4201a1a0 | pairing guide page 73 |
| 10 | 0x4201a5ac, 0x4201c2b8 | mode page after a brushing session; session end on the charger |
| 10 | 0x4201b1e4, 0x4201b0d8 | score/history -> mode page |
| 10 | main loop | update prompt 97 shown |
| 5 | 0x4201b0d8 | short press on info 92 / low battery 94 |
| 60 | 0x4201b0d8 | short press on 71/72/73/93 |
| 1 | 0x4201c790, 0x4201b1e4 | button with battery 0 (screen 94); after screen 98 |
| 0 | 0x42029218 | wake with battery 0 and no charger (screen 94, sleep at once) |
| 60 | wifi_event_handler, 0x4200bbac, 0x4200fb68 | network / BLE provisioning activity |
| 5, 10 | factory paths | — |

On the charger the idle check is not evaluated; instead only the backlight times out (§8.3).

---------------------------------------------------------------------------------------------

## 8. Display on / off

Backlight primitives [C] 0x4201dcc8, 0x4201d9a4, 0x4200d2f0, 0x4200d49c, 0x4200d160:

* LEDC low-speed timer 0, 13 bit, 5000 Hz; backlight = "LEDC5" = channel 4 on GPIO21, configured
  with initial duty 8191 (= off, active low).
* `led(4, mode, 4)` = 0x4201dcc8(4, mode, 4) -> immediate: mode 0 = **ON, raw duty 0**;
  mode 1..3 = **OFF, raw duty 8191**. The mode is cached (0x3fc9abb5, init 0xFF): a call with
  the cached value does nothing. The call is ignored completely while an LED pattern is running
  (0x3fc9aba8 != 4).
* LED pattern engine (0x4201e03c, 10 ms tick) addresses the backlight as LED index 5 with a
  brightness value `b` in 0..6000 and writes raw duty `8191 - b`. Fade-in curve for index 5
  (0x4201db20): per 10 ms `b += 1` while `b < 20`, then `b += 199`, clamp 6000 (about 200 ms
  barely lit + 310 ms ramp). Fade-out: `b -= 120` per 10 ms (6000 / 50 steps).
  * pattern 0 (table 0x3fc9ae44), wake on battery: t=0 LEDs 1,2,3 and backlight off;
    **t=10..500 ms backlight fade-in to b=6000 (raw 2191)**; LED chase 700..2300 ms.
  * pattern 2 (0x3fc9ac5e), charger plugged: 0..100 ms all off; LED chase; **t=1000..1500 ms
    backlight fade-in**.
  * pattern 3 (0x3fc9abbc), going to sleep: **0..500 ms LEDs and backlight fade out**.
* There is no brightness setting. The only levels ever written are raw 8191, the ramp, raw 2191
  and raw 0.

### 8.1 Turning the display on

| Wake reason | Path | Sequence | First screen |
|---|---|---|---|
| Cold boot / wake from deep sleep (reset) | brush_app 0x4201cc40 | `idle_timeout(60)`; `lcd_init()` (0x42029120); `ui_task_init()`; history day check 0x4201bc88(0,0); first screen; `led(4,0,4)` = backlight full on at once | `sys_config[0x34]` in {0, 11} (never set up / just reset): **70** boot animation. Otherwise `wake_screen()` |
| Button short press while asleep, on battery | 0x4201c790 -> `wake()` 0x4201bd70 | see below | `wake_screen()`; the press does nothing else |
| Raise-to-wake: IMU interrupt (main event bit 2) | 0x4201cc40 -> 0x4201cf9d -> `wake()` | only if `sys_config[8] == 1`, power_state == 2, and gyro-wake count (RTC 0x50001022, NVS `storage/wakeupcount`) < 5; the count is incremented per motion wake (max 20) and cleared by a button wake | `wake_screen()` |
| BLE wake notification (main bit 0x4000) | -> `wake()` if asleep and on battery | | `wake_screen()` |
| Charger plugged while asleep | 0x42017a0c(0) -> `wake()` | then `led(4,1,4)`, LED pattern 2 (backlight fades in at 1.0 s) | **93** charging |
| Charger plugged while awake | 0x42017a0c(0) | stops a running session; `idle_timeout(30)`; `led(4,1,4)`; pattern 2 | **93** charging |
| Charger removed while asleep, or button press while asleep on the charger | `wake_for_charge()` 0x4201bee8 | reset hw, 100 ms, `lcd_init()`, msg 0, msg 120, `led(4,0,4)`, `idle_timeout(30)` | **120** battery level |
| Charger removed while awake | 0x42017a0c(1) | `idle_timeout(30)`; LED pattern 0 (backlight fade-in). If the charge backlight timeout had not yet hit: `show_main()`; else `wake_screen()` 4 s later | mode page / `wake_screen()` |

`wake()` = 0x4201bd70 ("motorwakeup") [C]:
1. debounce counter 0x3fc9f308 must be >= 4, else return; `state = 1`.
2. `hw_reinit()` 0x4200df88 (includes LEDC re-init -> backlight raw 8191), `vTaskDelay(100)`,
   IMU init, `lcd_init()`, touch IRQ on, touch state reset.
3. if `power_state == 2 && batt_pct != 0`: `led(1,0,0)` = LED pattern 0 (backlight fade-in to
   raw 2191); else `led(4,0,4)` (backlight raw 0).
4. `idle_timeout(30)`; `wake_screen()`; `ui_post(0)` (the UI task runs `lcd_init()` a second
   time and enables drawing); `gpio_set_level(21, 0)`.
5. BLE advertising restart etc.; if NVS `IntoShow` != 0 enter show mode.

After a pattern-0 wake the backlight stays at raw 2191 until something calls `led(4,0,4)`
with pattern idle — that is the short press that starts brushing (0x4201c885). [C]
(Also 0x420183d0: 4.0 s after the power state becomes "battery" `led(4,0,4)` is called once.)

`wake_screen()` = 0x42029218 [C]:

```c
if (dev_mode == 2 && aging_count) show(aging_count < 12 ? 117 : 118);
else if (state < 2 && batt_pct == 0 && gpio_get_level(9) == 1) { show(94); idle_timeout(0); }
else if (advert_active())            show(104);              /* 0x4201d794: BLE-configured date window */
else if ((id = greeting_id()) == 4 || (id != 99 && sys_config[0x76] == 1)) show(85, id);   /* 0x420291a4 */
else                                 show_history(flag);     /* 84 */
```
`greeting_id()`: 4 if today == birthday (month/day at 0x3fca33e4+2/+3); else the month number
if today is 1 Jan (1), 14 Feb (2), 20 Mar (3), 1 May (5), 5 Oct (10); else 99; 99 if the RTC
year byte > 200 (clock not set).

The wake screen is replaced by the mode page after 6 s (§7.2), by a swipe up/down
(`get_next_screen` tail), or brushing starts on a short press.

### 8.2 Turning the display off (idle sleep) [C] main loop, 0x4201b900, 0x4201b764

When `idle_expired()` returns true (and no update prompt is pending):
1. `led(1, 1, 3)`: LED pattern 3 = LEDs and backlight fade out over 500 ms.
2. task "sleep_brush_task": touch IC to sleep, `vTaskDelay(500)`, save `user_config`
   (0x420185b4), then `brush_work_ooer_to_sleep` (0x4201b764):
   * restore the remembered mode: if `!locked && saved_mode(0x3fc9ab9c) != 0xFF &&
     sys_config[6] == 0` then `mode = saved_mode` (saved_mode is the mode at the last brushing
     start / lock toggle; note the config was saved just before with the displayed mode);
   * IMU to wake-on-motion;
   * **panel command 0x10 (SLPIN)**, `led(4,1,4)` (raw 8191), `gpio_set_level(21, 1)`;
   * LEDs off, touch IRQ removed, `ui_post(1)` + `fb->enabled = 0`;
   * start the BLE-timeout timer (30 s or 120 s) that later leads to deep sleep.
   No DISPOFF (0x28) is sent.

### 8.3 On the charger [C] 0x42018250, 0x4201829c

A 1 Hz counter (0x3fc9ab89, reset to 0 at plug-in) counts while `power_state != 2`; when it
reaches **30** the backlight is switched off (`led(4,0,4); led(4,1,4)`), the panel keeps its
content. Any button event then switches the backlight on again (`led(4,0,4)`) and restarts the
30 s. Gestures are not processed on the charger. The charging screen content is refreshed every
250 ms from `batt_pct` (§9, screen 93).

---------------------------------------------------------------------------------------------

## 9. Screens 70..120

Common helpers:

* **status icon** `status_icon()` 0x42020f94 [C], evaluated on the blink tick whenever its
  inputs change and otherwise every 500 ms (it also invalidates the four icon elements, so mode
  pages are recomposed at least every 500 ms). Elements at (28,0) 22x22, black transparent:
  [180] `#842` = lock icon, [167] `#732`, [166] `#733`, [165] `#709`.
  ```c
  if (ui_now == 82 && ui_running == 1) { hide([180]); return; }
  hide all four; pick:
    bound = (sys_config[0x0c] == 2); w = wifi_state /*0x3fca2ad9*/; c = cloud_state /*0x3fca3870*/;
    other = !bound || w == 2 ? [167]
          : (w == 1 && c == 2) ? [165]
          : (w == 1 && c == 1 && weak_rssi /* < -83 dBm, cleared at >= -79 */) ? [166] : none;
    if (locked && (other == none || first_half_of_each_second)) show([180]); else if (other) show(other);
  ```
  ([I]: `#732` = "not paired / no network", `#733` = weak Wi-Fi, `#709` = connected.)
  A custom firmware without the Oclean cloud can show only the lock icon.
* **dev overlay** [170] `#710` at (0,50): visible only when `dev_mode == 2` (0x42020644) — keep
  hidden.
* **mode_page_update()** 0x420207c8 [C]: `[163].var = [160].var = [158].var = [156].var =
  [149].var = lang`; recompute `sub_mode` (0x42020674: `profile[2] ? 4 : (profile[1]==1 &&
  tod_sub in {2,3}) ? tod_sub : 1`); then on screen 81 exactly one name element is visible with
  `var = lang`: sub_mode <2 -> [154] `#399`, 2 -> [152] `#609`, 3 -> [153] `#592`, 4 -> [151] `#626`.
* Countdown digits (screens 82/99/101/102): `rem` = 0x4201bb34 = `mode == 5 ? elapsed_s :
  max(total_s - elapsed_s, 0)`; `[116].var = rem/60`, `[114].var = (rem%60)/10`,
  `[113].var = rem%10` (digits `#173+d`); [115] `#183` colon and [112] `#184` fixed.
  (In mode 5, when the elapsed counter at +0x72 hits 120 a voice/music cue 6 is played.)

| Scr | Elements (list order) | Trigger(s) | Dynamic rules | Leaves to |
|---|---|---|---|---|
| **70** boot | [179] `#226+f` full screen; factory overlays [178],[177],[176..172] | cold boot when `sys_config[0x34]` in {0,11} (0x4201a148); factory aging | f = 0..19, +1 per 100 ms. Overlays hidden unless `aging` (then: battery % digits [176],[175] with "%" [174], aging count [173],[172], loop) | at f == 19: if first-boot flag set: stays; else if bound or guide already seen (`sys_config[0x34] != 1`) or factory: `show_main()`, idle 60 s; else **71** |
| **71/72/73** pairing guide | [171] `#23` / [169] `#24` / [168] `#25` (+[170]) | after 70 on an unpaired device (0x4201a168) | static | 71 -2 s-> 72 -2 s-> 73 (idle 10 s). Swipe up: 71->72->73->mode page; down: 73->72->71. Short press: mode page (idle 60 s) |
| **76..81** mode pages | art, [180],[167],[166],[165], name, [162] `#574` footer (0,149), [170] | `show_main()`; swipes; after sessions | name variant = lang; status icon; 81: see mode_page_update and the two lists below | swipe up/down = other mode; left/right = 100 / 96; short press = start brushing; hold 2 s = lock; hold 5 s = 92; idle -> sleep |
| **82** brushing | [117] `#577+p`, [116],[115],[114],[113],[112], [111],[110],[109],[108] quadrants, [107] `#225`, [170], [180] | start/resume (0x4201bb18); re-posted **every second** while running and `now_ui != 87` | P[0]==1 (always, in this build): hide [111],[110],[109],[108],[107]; if `mode != 5`: `p = min(14, ((total_s - rem) & 0xFF) / (total_s / 15))`; in mode 5 p is animated (§7.1). Countdown digits. The P[0]==0 "paused" layout (quadrants + pause icon) is only used when FUN_4201c71c() != 12, i.e. never | short press -> pause (**101**); time reached / stop -> **103**; in mode 5: 87 first |
| **83** score (alt) | quadrants, badges, digits | only if FUN_4201c71c()==8 | — | unreachable |
| **84** history | [118] `#874+f` (0,10 80x133), [170] | `wake_screen()`; 10 s after score when `hist_count != 0`; after 95; demo ring | f = 0..13 at 50 ms, then holds 13 (show mode: fixed 13). No numbers are drawn (the digit elements [119..129] exist but are in no screen list) | state 0/1: mode page after 6 s; state 4: mode page after 10 s; swipe up/down: mode page; short press: starts brushing (state 0/1) or mode page (state 3/4) |
| **85** greeting | [104] `#11+k` full screen, [103],[102],[101],[100],[99] date, [98] `#450+..` (0,107) | `wake_screen()` | by id: 1 (1 Jan): k=2, [98].var = lang; 2 (14 Feb): k=3, lang+17; 3 (20 Mar): k=5, lang+51; 5 (1 May): k=1, lang+34; 10 (5 Oct): k=4, lang+68; 4 (birthday): k=0, lang+85 and the date "M-D" is shown, see below; other ids hide the date | like 84 (6 s) |
| **87** intensity | [106] `#18+(level-1)` (0,0 80x130), [105] `#416+lang` (0,120; rows >= 160 cut), [180] | brushing start in mode 5; `strength_step`; lock restore | level 1..5 | after 5 s without a step: `now_ui = 82`, touch IRQ off, next 1 Hz refresh shows 82 |
| **88** update progress | [137] `#160+pct/10`, [136] `#280+lang` | OTA progress (0x4201a1ec) | — | 89 / 90; watchdog: 30 s without progress -> 90, restart |
| **89** update OK | [133] `#170`, [132] `#246+lang` | 0x4201a20c | — | restart |
| **90** update failed | [135] `#171`, [134] `#263+lang` | 0x4201a228 | — | restart |
| **91** lock popup | [131] `#841` (0,0 80x80), [130] `#824+lang` (0,81) | §3.3 | — | 1 s, then the saved screen |
| **92** info | [77] `#643+page`, version [76],[75],[74],[73],[72],[71],[70],[69], [170] | button held 5 s on a mode page | page 0: show version `V a.b.c.d` at y=108: [76] `#696` "V", digits `#697+d` from the version string chars 0,2,4,6 ([75],[73],[71],[69]), dots `#707` [74],[72],[70]; pages 1..3: hide all eight | swipe up = previous page (0->3), down = next page; short press = mode page (idle 5 s); idle 30 s |
| **93** charging | [147] `#735+f` strip (0,111), [148] `#0+lv` (20,34), digits [146..138], [170] | charger plugged (0x42017a0c); re-shown after reconnect | f = 0..21 at 100 ms (only while power_state != 2). Every 250 ms from `batt_pct`: `lv` = 0 (<16), 1 (16..30), 2 (31..44), 3 (46..60), 4 (61..80), 5 (81..99), 6 (100) — 45 leaves lv unchanged; digit layout: 100: [146]=1,[145]=0,[144]=0,[143]"%" (x=12,26,40,54); 10..99: [142]=tens,[141]=ones,[140]"%" (x=19,33,47); 0..9: [139]=digit,[138]"%" (x=28,42). Strip hidden at 100 % (only by the 93 message handler) | backlight off after 30 s; unplug -> mode page / wake screen |
| **94** low battery | [89] `#7` (20,41) | button with battery 0; idle expiry with battery <= 10 %; wake with battery 0 | static | sleep (3 s / 1 s / at once) |
| **95** | [97] `#552+f` (0,0 80x10) | sequencer (dead branch: 0x4201c168 always returns 0) | f 0..19 at 100 ms | 84 |
| **96** clock / weather | see below | swipe right from a mode page, left from 100 | see below | swipe; short press = mode page |
| **97** update prompt | [88] `#661`, [87] `#662+lang`, version [86],[85]..[79], [78] `#679+lang` | idle expiry with a pending Wi-Fi OTA and battery > 20 % (once) | digits from the new-version string (0x3fca3ac8+200, +202, +204, +206) | short press = start update; hold 3 s = decline -> mode page; idle 10 s |
| **98** | [18] `#909+lang`, [17] `#908` | 3 s after score if `zone_flag` | — | unreachable (flag never set) |
| **99** zone-guided brushing | [16] `#926`, zone overlays, countdown | start when `zone_flag` | current zone blinks (250 ms) | unreachable |
| **100** score | [16] `#926` (0,20), zone overlays [15..4], [93],[96],[95],[94],[90],[92],[91], [3] `#888+f` (0,10 80x150) | 0..1 s after 103; swipe left from a mode page / right from 96 | P[0] = score. **100**: hide every badge/digit/zone element, show [3], f = 0..19 at 100 ms then hold. **90..99**: hide [3]; badge [93] `#206`; digits [96]=tens,[95]=ones (`#185+d`, key 0x4646). **< 90**: hide [3]; badge [90] `#207`; digits [92]=tens,[91]=ones (`#195+d`, key 0xC7F9). For < 100 the 12 zone overlays: hidden if zone time == 0, variant 1 if 1..4 s, variant 0 if >= 5 s | after a session: 10 s -> 84 (or mode page); as side page: swipes / short press |
| **101** paused | [16], zone overlays, countdown [116..112], [2] `#225` (21,45), [170], [180] | short press during brushing (0x4201c73c) | zone overlays from `zone_t[]` (same rule as 100); countdown | short press = resume (82) or stop when time is up; swipe up/down = quit: score 100 if elapsed >= 120 s else mode page |
| **102** | as 101 | pause when `zone_flag` | — | unreachable |
| **103** complete | [1] `#850+f` (0,40 80x80) | session end (0x4201c2b8) when: ended while running; or ended paused/quit with elapsed >= 120 s | f loops 0..5 at 50 ms | within 1 s -> 100 |
| **104** custom picture | [0] `#998` (bank A 0x75E000 / B 0x76E000) | `wake_screen()` when a BLE-configured "advert" window is active | Stock defect [C] 0x403785c9: for picture 998 each 160-byte row is read to the *address of the row-buffer pointer* (0x3fca5100) instead of the buffer, overwriting the UI globals behind it (frame-buffer object, current screen, language). The screen cannot work in this build — do not port | like 84 (6 s) |
| **105..116** | force-sensor calibration (0x42026f5c) | factory | — | — |
| **117/118/119** | factory aging results ([48]/[49]/[47] + digits) | factory | — | — |
| **120** battery | [148], digits [146..138], [170] | `wake_for_charge()` | as 93 without the strip; the layout is set once from `batt_pct` by the message handler (lv is not written for 100 %) | idle 30 s |

Element lists not present in `stock_ui_tables.h`:
```c
/* screen 81 when profile[0]==0 && profile[2]!=1 (hdr 0x3fc9fb5c) – art #103 with name #399 */
static const uint8_t STOCK_SCR_81_ALT[] = {150, 180, 167, 166, 165, 154, 162, 170, 0xff};
/* screen 81 otherwise = STOCK_SCR_81 (hdr 0x3fc9fb98). If profile[0] is neither 0 nor 1 and
   profile[2] != 1 no list is selected (previous screen stays). */
```

Zone element mapping for screens 100/101 (0x4201a8c0 + 0x420224a8) [C]:
```c
/* zone_t index -> element (picture) ; overlays 927..937 are drawn at y=20, 939..949 at y=64 */
static const uint8_t ZONE_ELEM[12] = { 15 /*#927*/, 14 /*#929*/, 9 /*#939*/, 8 /*#941*/, 10 /*#937*/, 11 /*#935*/,
                                        5 /*#947*/,  4 /*#949*/, 13 /*#931*/, 12 /*#933*/, 7 /*#943*/, 6 /*#945*/ };
/* variant: 0 if t >= 5 s, 1 if 1..4 s (pictures 928,930,...); hidden if t == 0 */
```
(The score message path 0x4201b170 uses the same order; the persisted copy keeps only the low
byte of each value.)

Screen 85 birthday date (0x4201fde0) [C]: digits `#173+d`, separator [101] `#575`; x positions:
```c
/* m<10,d<10 : [103]=m @19, [101] @33, [100]=d @47;            hide [102],[99]
   m<10,d>=10: [103]=m @12, [101] @26, [100]=d/10 @40, [99]=d%10 @54;  hide [102]
   m>=10,d<10: [103]=m/10 @12, [102]=m%10 @26, [101] @40, [100]=d @54; hide [99]
   both >=10 : [103] @4, [102] @19, [101] @33, [100] @47, [99] @61 */
```

Screen 96 (0x4201fb24, 0x4201f900, 0x4201f654, 0x4201f6dc) [C]. All elements of the list are
hidden on entry (0x4201f47c) and then shown per `clock_mode` (0xFF is treated as 3):

```c
static const uint8_t CLK_GLYPH[14] = {20,21,22,23,24,25,26,27,28,29, 0, 1, 2, 30};
/* variant added to base #843: digits 0..9 -> #863..#872, [10] -> #843, [11] -> #844, [12] -> #845, [13] -> #873 */

mode 3 (default: no weather ever received): show [39] #849 (0,39 80x80) and [19] #848 (0,119 80x20). No clock.
mode 0/1/2: clock HH:MM at y=0, redrawn when the minute changes (and on entry):
      [46] H tens @x=13, [45] H ones @25, [44] glyph[13] (colon, 5 px) @37, [43] M tens @42, [42] M ones @54
      banner: weather.flag ? [40] #847 : [41] #846   (0,27 80x20)
  mode 0 (weather valid): icon by weather.code: 0 [32] #861, 1 [33] #860, 2 [34] #859, 3 [36] #857,
          4 [37] #856, 5 [31] #862, 6 [35] #858 (all at 0,39 80x80; codes > 6 give a NULL element in stock)
          temperature line at y=119 "t2 ~ t1" built from glyphs: optional minus glyph[12], digits,
          glyph[11] (degree), glyph[10] (separator), centred: start x = (80 - (width+17))/2,
          digit pitch 11, sign 8, see 0x4201f900 for the exact x arithmetic
  mode 1 (loading): [38] #850+f (0,39), f cycles 5..0 at 100 ms; bottom line "-- ~ --":
          [25] glyph[12] @17, [24] glyph[12] @25, [26] glyph[10] @33, [23] glyph[12] @41, [22] glyph[12] @49
  mode 2 (no data): [39] #849; weather zeroed; same "-- ~ --" line
```
In show mode a fixed demo record is used (0x3fc9b384: flag 0, 18, 10, code 2).
For the custom firmware (no weather source) mode 3 or mode 2 are the only reproducible
variants; mode 2 is the one that shows the time.

Brushing start on a short press (0x4201c790, UI-relevant part) [C]:
`saved_mode = mode; led(4,0,4); state = 2; session_active = 1; LEDs 2,3 off; ...;`
then `mode == 5 ? show(87, strength-1) (strength forced to 3 if not 1..5) : (touch_irq(false),
show(82))`.

Session end `0x4201c2b8` (UI part) [C]: `score = compute()`; `state = 3`; on battery: show 103
or the mode page per the rule in the table; history totals are updated (only if elapsed > 14 s
and score != 0); on the charger: `idle_timeout(10)` only.

---------------------------------------------------------------------------------------------

## 10. Settings and persistence

| Setting | Changed on device? | RAM | Persisted where | Src |
|---|---|---|---|---|
| Brushing mode | swipe up/down on a mode page | 0x3fca4c85+6 | NVS `storage/user_config` (blob 100 B, 0x50 used) **byte 19**; written by 0x420185b4 when going to sleep (and on some key paths) | [C] |
| Intensity 1..5 (mode 5) | swipe up/down on screen 87 | RTC slow 0x50001019 | RTC memory only (survives deep sleep, not power loss); default 3 | [C] |
| Touch lock | button 2 s | 0x3fca5d9c | `user_config` byte 0x34 (and saved popup screen byte 0x35) | [C] |
| Language | no (BLE cmd 0x42010e88) | 0x3fc9a69e+0x67 / 0x3fca5114 | NVS `storage/sys_config` byte 5, magic 0xAA in byte 9 | [C] |
| Auto mode by time of day | no (BLE) | sys_config[6] | `user_config` byte 0x27 | [C] |
| Raise-to-wake | no (BLE) | sys_config[8] (default 1) | `user_config` byte 0x29 | [C] |
| Greeting pages | no (BLE) | sys_config[0x76] (default 1) | `user_config` byte 0x3f | [C] |
| Birthday month/day | no (BLE) | 0x3fca33e4+2,+3 | `user_config` bytes 17, 18 | [C] |
| App brush profile flags | no (BLE) | profile[0], [1..5] | `user_config` byte 4, bytes 0x2b..0x2f | [C] |
| Last score, zone seconds (low bytes) | — | 0x3fc9aba3, 0x3fca4dcc..0x3fca4de2 | `user_config` byte 0x3e; bytes 0x36..0x3d and 0x40..0x43 | [C] |
| History day totals | — | RTC 0x5000101a..0x50001021 | NVS `storage/shuanhuan` (blob 0x32 B): BE u16 seconds, BE u16 score sum, BE u16 count, u8 day | [C] 0x4201bc88 |
| Gyro wake count | — | RTC 0x50001022 | NVS `storage/wakeupcount` | [C] |
| Shop demo mode | BLE on; button press / 8 s hold off | 0x3fca4dca | NVS `storage/IntoShow` (1 byte) | [C] |
| Pairing/guide state, bound flag | — | sys_config[0x34], [0x0c] | `sys_config` bytes 0, 4 | [C] |
| Weather, custom picture bank | BLE | 0x3fca4ff4, 0x3fc9a719 | not examined | — |
| Panel id | BLE / UART | — | `storage/screen_config` (see earlier panel analysis) | [C] |

The page index is **not** persisted: after every wake the UI starts on the wake screen and then
on the page of the current `mode`; side pages and the info page index are RAM only.

---------------------------------------------------------------------------------------------

## 11. Data the custom firmware has to feed

| Input | Stock source | Used for |
|---|---|---|
| battery % | 0x3fca4b6c+0x0e | 93/120 digits and level, low-battery rules (0 and <= 10), 70 aging |
| power state (battery / charging / full) | 0x3fca4b6c+8 (2/1/3) | gesture enable, charging strip, idle logic, wake path |
| session active / running-paused | 0x3fca4d5b / 0x3fc9aba5 (200/0) | swipe semantics, 82 vs 101, idle gating |
| planned / elapsed seconds | 0x3fca4c90+0x6e / +0x70 (u16) | countdown, progress frame, >= 120 s rule |
| mode | 0x3fca4c85+6 | mode page, mode-5 behaviour |
| intensity level | RTC 0x50001019 | 87 |
| per-zone seconds (12) | 0x3fca4cda (u16[12]) | 100 / 101 overlays |
| score 0..100 | 0x3fc9aba3 | 100 |
| history count today | RTC 0x5000101c | whether 84 follows the score |
| RTC time (y,m,d,h,min) | 0x4201d89c | 96 clock, greeting, auto mode |
| language | 0x3fca5114 | every `+lang` picture |
| lock flag | 0x3fca5d9c | swipe blocking, icon |
| firmware version string "a.b.c.d" | *(0x3fca3404+8) | 92 page 0 |
| OTA percent / result | 0x4201a1ec(p), 0x4201a20c, 0x4201a228 | 88/89/90 |
| touch samples / button events | §3 | navigation |

---------------------------------------------------------------------------------------------

## 12. Corrections to `prior_ui_findings.txt`

* "Stock has no backlight control" — wrong: LEDC channel 4 / GPIO21 is driven as described in
  §8 (0x4200d2f0 case 5, 0x4200d49c case 5, pattern tables). `gpio_set_level(21, x)` calls at
  wake/sleep exist as well.
* Screen 103: 50 ms per frame, looping, for up to 1 s (not 100 ms).
* Screen 82 "paused" layout is not used in this build; pause shows 101. Screens 99, 102, 98, 83,
  95 are unreachable as shipped.
* Screen 82 progress uses `(elapsed & 0xFF) / (total/15)`; in mode 5 it is an animation and the
  digits count up.
* A picture overhanging the right edge (x+w > 80) is not drawn at all; only bottom overhang is
  cut row by row.
* Variants 100/101/102 of the drawer are never produced by the UI code; sub-mode names are
  separate elements with `variant = lang`.
* The idle timeout "not recovered" is the table in §7.3; the first screen after wake is §8.1.
* Screen 96 is a clock + weather page whose default state shows no clock.
* The low-battery screen at idle expiry applies to battery <= 10 % and lasts 3 s.

---------------------------------------------------------------------------------------------

## 13. Open questions

1. **Gesture orientation.** Which physical finger movement yields `dL > 0` ("up") / `dS > 0`
   ("right") depends on how the touch IC coordinates map to the handle; the touch driver
   (0x42025f2c config, 0x420263fc read) was not analysed here. The sample rate of the IC (needed
   for the `cnt >= 6` rule) is also unknown.
2. **Meaning of pictures**: the six mode names, `#184` (glyph right of the countdown), `#574`
   footer, the status icons `#732/#733/#709`, `#849/#848` on page 96, the info pages
   `#643..#646`, and which language each index 0..16 is. Needs the picture dump.
3. **`zone_flag` (0x3fca4e7a)**: no instruction writing a non-zero value was found in the
   decompiled application range, so screens 98/99/102 are treated as dead. A write through an
   unrelated base pointer cannot be excluded 100 %.
4. **Effective brightness of the two "on" levels** (raw 2191 vs raw 0) on the real panel, and
   whether the difference is visible; whether the stock unit really idles at raw 2191 after a
   motion wake (follows from the code path, not observed).
5. **Charger-detect pin**: the code that drives the charging UI polls `gpio_get_level(9)`
   (0 = charger present: 0x4201840c, 0x42029218) and the GPIO8 ISR path raises the motion-wake
   event (0x40377c98 case 8 -> main bit 2). CONTEXT.md lists these two pins the other way
   round ("likely"). To be reconciled by the power/input specs.
6. `0x42011340()` (0x3fca308e) and `sys_config[0x70]` gate the idle check and the short-press
   dismiss; they are set by BLE handlers that were not traced (both are 0 by default).
7. Paused-session timeout: the idle check is skipped while `session_active`, so the automatic
   end of a paused session must come from the brushing logic (not analysed here).
8. Temperature-line x arithmetic of page 96 mode 0 was read but not re-derived into a table
   (needs weather data to matter).
9. The 12-byte touch report layout and the touch state machine values (`touch_ic_state == 5`
   as a precondition for starting a session) belong to the input spec.
10. Deep-sleep wake causes: after the BLE timeout the device deep-sleeps and the next wake is a
    cold boot (§8.1 first row). Whether the boot path treats a button wake differently from a
    motion or charger wake (app_main / 0x4200d77c) was not analysed here; the UI part of the
    boot path is the same in all cases.
11. `sys_config[0x34]` progression 11 -> 0 -> 1 -> 2 across boots was read from 0x4201cc40 but
    the exact value at the time 0x4201a538 tests it (`!= 1` -> skip the pairing guide) depends
    on the order of that code and should be re-checked if the pairing guide is ported.

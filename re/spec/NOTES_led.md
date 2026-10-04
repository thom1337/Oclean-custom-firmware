# NOTES — LED patterns, backlight, battery gauge, charge pins (module L)

Spec: `re/spec/led_battery_charge.md`. Stock code read for this port: 0x4201d9a4, 0x4201dadc,
0x4201db20 (disassembly), 0x4201dcc8, 0x4201dd74, 0x4201ddf8, 0x4201e03c, 0x4200d160..0x4200d6b0,
0x42017744..0x42018250, 0x4201840c, 0x42017a0c, 0x4200d77c, 0x4200dbbc, 0x4200df88, the GPIO ISR
0x40377c98. Tables and `.data` initial values were checked against `re/seg0_3c110020.bin` and
`re/seg1_3fc99e00.bin` (five script tables, both `led_configs[]`, LEDC timer / channel configs,
soc and charge-time tables, default record, every gauge / LED `.data` variable): all match the spec.

## 1. Files and entry points

| file | content |
|---|---|
| `main/oem_led.c` (core) | `oem_led_set`, `oem_led_all`, `oem_led_abort_script`, `oem_led_tick`, `oem_led_level`, `oem_led_init`, `oem_led_reinit_charge_light`, `oem_led_park`, `oem_led_backlight_lit` (not stock); script tables, fade engine, steady tick (breathe, blink, empty-battery rule), Wi-Fi light ramp, pseudo LED 5 |
| `main/hw_led.c` (ESP-IDF) | LEDC timer 0 / channels 0..4, the three stock HAL calls (`hal_led_set`, `hal_led_process`, `hal_led_get`), `hal_led_max[]`, pin release / park (`hal_led_*`, declared in `oem_hal.h`) |
| `main/oem_gauge.c` (core) | `oem_gauge_boot`, `oem_gauge_tick`, `oem_gauge_save`, `oem_charge_thermal_check`, `oem_charge_thermal_cut` (diagnostics), hooks `oem_gauge_settle_reset`, `oem_gauge_rearm`, `oem_gauge_session_done`, `oem_gauge_sleep_enter/exit`, `oem_gauge_report_reset`, `oem_gauge_reset_record` |
| `main/hw_battery.c` (ESP-IDF, replaced) | `oem_batt_mv_now`, `oem_batt_adc_init` |
| `main/hw_charge.c` (ESP-IDF, replaced) | `oem_charger_present`, `oem_charge_allow`, `oem_charge_pins_init`, `oem_wlc_off`, `oem_charger_alive_take`, `oem_charge_blocked` and `oem_charger_alive_count` (diagnostics), GPIO9 / GPIO2 interrupts |
| `re/tools/uisim/sim_led.c`, `sim_gauge.c`, `fake_idf/` | host tests (section 5) |

Header additions (own blocks only): `oem_state.h` — `gauge_inited`, `plug_cnt`, `slew_cnt`,
`full_cnt`, `gauge_period`, `batt_fault`, `batt_raw_mv`; `oem_hal.h` — the `hal_led_*` driver
interface; `oem_api.h` — the functions above that were not listed, and `OEM_BATT_MV_MIN_VALID`.
No bytes of `oem_rtc_t.reserved` are used: stock reloads its RTC copies of mV / percent from NVS on
every boot before using them, so they are ordinary statics here.

Not stock: `oem_led_backlight_lit()` (the backlight level the driver holds is not 0, whether a
state or a script set it; the UI task asks it before it draws, NOTES_ui 3.10), and the charge
diagnostics that `/api/status` shows in `diag` (web UI: Brush tab):

| key | source |
|---|---|
| `batt_raw_mv` | `g_oem.batt_raw_mv`: the last `oem_batt_mv_now()` of the gauge tick (once a second while not brushing), before the charge compensation and the filter; 0 = no reading |
| `charge_blocked` | `oem_charge_blocked()`: the last `oem_charge_allow()` was "block" (GPIO26 driven high); null in safe mode, where the pin keeps whatever the previous boot left held |
| `thermal_cut` | `oem_charge_thermal_cut()`: the thermal cut-off is latched (stock flag 0x3fc9ab8a == 1). The pin can be released all the same (last point of section 3) |
| `charger_present` | `oem_charger_present()`; null in safe mode, where the charger pins are not set up |
| `alive_edges` | `oem_charger_alive_count()`: falling edges on GPIO2 since boot |

`oem_charge_blocked()`, `oem_charge_thermal_cut()` and `oem_charger_alive_count()` return copies
and touch no pin, so any task may call them. `oem_charge_blocked()` only means something once
`oem_charge_allow()` has run in this boot, which it never does in safe mode: `/api/status`
sends null there, and the web UI shows "unknown".

For the integrator: `main/CMakeLists.txt` needs `oem_led.c`, `hw_led.c`, `oem_gauge.c` added
(`hw_battery.c`, `hw_charge.c` are already listed). The old entry points `hw_battery_init`,
`hw_battery_mv`, `hw_battery_pct`, `hw_charge_init`, `hw_charge_tick`, `hw_charge_enabled` are
gone; `main/hardware.c` (lines 45, 49, 90, 93, 94) still calls them. `main/hw_io.c` still
configures LEDC channels itself and has an old `hw_charger_present` (wrong polarity) and
`hw_led_set`: its LED / charger part must go when `hw_led.c` is linked, or the two fight over
the channels.

## 2. How the other modules are expected to use it

Boot, in this order: `oem_charge_pins_init()`, `oem_batt_adc_init()` (both also run on first
use), `oem_led_init()` once, then `oem_gauge_boot()` before anything reads `g_oem.batt_pct`
(it also sets `power_state = OEM_PWR_BATTERY`, as stock 0x4201831c).

Main loop: every 10 ms `oem_led_tick()` then the app's charger poll; every second, while not
brushing, `oem_gauge_tick()`. The gauge tick calls `oem_charge_thermal_check()` itself (as
stock); nobody else needs to. Stock also calls the gauge tick every 10 ms while brushing on the
dock before the first measurement (0x4201840c); the start-up sequence here is time based, so
that works too.

The gauge writes `g_oem.power_state` in three places, as stock does: 2 in `oem_gauge_boot`, 2 at
the first measurement (0x420178b8; the charger poll then re-detects the dock), 3 when full.
It makes two kinds of LED calls: `(2, ON, 4)` when full, and `(0,OFF,4) (1,OFF,4) (2,BREATHE,2)`
on the third tick after `plug_cnt` was set to 0.

Gauge side of the charge state machine 0x42017a0c (app module), line by line:

| stock | port |
|---|---|
| attach: `FUN_420179e0()`; `0x3fca4b5b = 0`; `wlc_set(0)`; `0x3fca4b68 = 0`; `0x3fc9ab8c = 100` | `oem_gauge_settle_reset(); g_oem.slew_cnt = 0; oem_wlc_off(); g_oem.full_cnt = 0; g_oem.plug_cnt = 100;` |
| attach while asleep: `0x3fc9ab8c = 0` after the wake | `g_oem.plug_cnt = 0;` |
| attach: `FUN_420179f4(period)`; `charge_enable(1)` | `oem_gauge_rearm(g_oem.gauge_period); oem_charge_allow(true);` |
| attach: `0x3fca4b5c = 1`, `comp_mv = 50` | nothing (write-only flag; the compensation is a constant) |
| removal: `0x3fc9ab8c = 100`; `wlc_set(0)` | `g_oem.plug_cnt = 100; oem_wlc_off();` |
| removal, normal path: `FUN_420179f4(2)`; `FUN_420179e0()` | `oem_gauge_rearm(2); oem_gauge_settle_reset();` |
| charger poll: `if (!g_inited) return` | `g_oem.gauge_inited` |
| charger poll: GPIO2 edge clears the un-plug counter | `if (oem_charger_alive_take()) unplug_cnt = 0;` at the top of the poll |
| charger poll: 400 ticks on the charger | `oem_led_level(2, 0)` |

Other call sites: wake 0x4201bd70 — `oem_gauge_sleep_exit(); oem_gauge_rearm(2);`; session end
(event 0x80000, 0x4201ce6f) — `oem_gauge_rearm(2)`; session scoring 0x4201c2b8 —
`oem_gauge_session_done(done_s)`; `sleep_brush_task` 0x4201b764 — `oem_gauge_sleep_enter()`;
BLE connected — `oem_gauge_report_reset()`; before deep sleep / OTA / reset — `oem_gauge_save()`.

Power module: `oem_led_park()` is the LED part of 0x4200dbbc / 0x4200dda4 (GPIO17..20 plain
outputs at off, GPIO21 open-drain high, all held; LEDC is detached, its timer keeps running as
in stock). `oem_led_init()` is the boot / wake form: it un-holds GPIO17..21, presets the off
levels and runs `led_init` (what 0x4200d77c and 0x4200df88 do around `led_init`), so calling it
after your own un-hold is harmless. `oem_led_reinit_charge_light()` is the "idle on the charger"
form (GPIO19 only) and also re-applies the charge light, so a following
`oem_led_set(2, ...)` is a harmless repeat. GPIO26 / GPIO45 in the parking and wake code:
`oem_charge_allow(true)` and plain `gpio_set_level(45, 0)`.

`oem_led_init()` does not touch the pattern state (stock `led_init` does not either). Stock
relies on every LED being in state OFF before it parks the pins (sleep_brush_task sets them);
keep that order, otherwise a state byte says ON while the LED is dark and the next ON is skipped
as "no change".

## 3. Deviations from the spec / stock, and why

1. **Battery reading that is not a battery voltage** (`oem_batt_mv_now()` below
   `OEM_BATT_MV_MIN_VALID` = 2000 mV; the driver returns 0 on an ADC or calibration failure).
   Stock treats 0 mV as a voltage: 0 % at boot, the filter falls to 0 % within seconds, the 0 % is
   saved, and the brush then shows the low-battery screen and sleeps on every wake until a charge
   raises the value — which it cannot if the ADC stays broken. On the charger the 50 mV
   compensation even wraps to 65486 mV (instant 100 %). Here such a sample is "no reading":
   * while running it is skipped: filter, percentage and record keep their values;
   * at boot / first measurement the percentage is the saved one if there is one and it is not
     the 0 % lock, else a neutral 50 %; nothing is written to NVS; the start-up still completes
     (`gauge_inited`), so charger detection works;
   * the first usable sample later is taken as the first measurement (record rules, lock
     included);
   * `g_oem.batt_fault` is 1 meanwhile and the log says so.

   Why this is safe: the 0 % lock and the low-battery screens are user-interface rules, not cell
   protection — the firmware never switches the load off by voltage except by going to sleep, and
   the cell has its own protection circuit; a brush that cannot measure its battery behaves like
   one at 50 % and browns out if the cell is really empty. Below 2000 mV the CPU could not be
   running from the cell, so no real "empty" reading is lost.
   **The boot gate in the power module (≤ 3299 mV → deep sleep) must apply the same rule**, or a
   failed ADC sends the brush back to sleep at every boot: skip the gate when
   `oem_batt_mv_now() < OEM_BATT_MV_MIN_VALID`.
2. **Table overrun** (spec 5.3): index clamped, everything from 4100 mV up is 100 %.
3. **Record with a percentage above 100** (not 0xff) is taken as "no record"; stock would show it.
4. **Start-up timing**: stock keys "block charging at second 2, measure at second 3" on the main
   task's seconds counter, which `oem_gauge_tick(void)` does not get. It runs on a clock started
   by the first gauge tick (block from 1.5 s, measure from 2.5 s): the same ticks at 1 Hz, and
   correct when ticked every 10 ms.
5. **`oem_gauge_boot`** uses the two-read sample for its first estimate (stock: one read).
6. **ADC sample**: the 10 ms between the two reads is two FreeRTOS ticks here (10..20 ms); the
   10 ms stock waits after the second read has no effect and is left out. The gauge tick
   therefore blocks the main task 10..20 ms once per second (stock: 20 ms).
7. **ADC calibration**: curve fitting (`adc_cali`) instead of stock's legacy line fitting; expect
   a few mV of difference against the stock thresholds 3299 / 3445 / 3999 / 4109 mV. The NTC
   channel (GPIO10) and the unused ADC2 characterisation are not set up.
8. **GPIO26 "block"**: stock enables the output and then sets it high, which drives the pin low
   for a few microseconds the first time. The level is latched high before the output is
   enabled, so the pin is never low.
9. **Thermal check without a temperature** (`oem_imu_temp` false): nothing is changed. The
   custom "cut charging when the reading is lost" logic of the old `hw_charge.c` is gone (not
   stock).
10. **`oem_led_set` with anim outside 0..4**: ignored. Stock would start a "script" with a stale
    table pointer; no caller does it.
11. **LEDC writes**: the driver caches each channel's duty, writes only on change and answers
    "get level" from the cache (stock re-reads the register 10 ms later: same value). LEDC errors
    are logged, not fatal (stock: `ESP_ERROR_CHECK`). `hal_led_set(4, level)` clamps level to 8191.
12. **GPIO ISR**: the GPIO2 edge is latched for `oem_charger_alive_take()` instead of clearing
    the app's counter from the ISR; the stock ISR also sets its wake flag 0x3fc9f301 for GPIO9
    low, which is the power module's variable and is not written here.
13. Not ported: `wlc_set(x != 0)` (phone-app command that inhibits charging), 0x4201821c
    (factory ageing), the per-second stock printf lines, the BLE battery notification 0x4200e6c8
    (the BLE / MQTT glue can watch `g_oem.batt_pct`).

Stock behaviour that looks odd but is reproduced as coded (all covered by the host tests):

* Backlight: duty 0 after a direct ON, duty 2191 after a scripted fade-in; the sleep script
  started from the direct ON leaves level 2071 (duty 6120) until the sleep code sets OFF.
* BLINK only works on LED4; on LED1..3 it stays dark. BREATHE on LED1..3 plateaus at 4000.
* At the end of every script the Wi-Fi light counter is zeroed: after the chase script with
  Wi-Fi connected LED1 drops from 4000 and ramps up again over 1 s.
* After un-plugging, `per_cnt` is re-armed against the *charging* period while the period goes
  back to 64, so the first gauge update comes 43..60 s later (and is skipped by `settle`); the
  spec's "every 2 s" starts after that. Likewise a change of the charging period mid-count can
  delay one update.
* The drop limiter grants (seconds since the last grant) / 100, without a cap: after a long
  time without a drop the percentage follows the voltage at once. `credit16` (sleep time) is
  never cleared, so after long sleeps every update may drop 5 or more points.
* Between boot and the first measurement (about 4 s) the percentage is the raw voltage
  estimate, even when the record will override it (e.g. 30 % shown, then the 0 % lock).
* The thermal state is not re-applied when the charge state machine releases GPIO26.

## 4. Requests / open points for other modules

`oem_app.c` and `hw_power.c` as they are in the tree today already call the hooks the way
section 2 describes (attach / removal, poll, wake, sleep, session end; `oem_led_park`,
`oem_led_init`, the `OEM_BATT_MV_MIN_VALID` rule in the boot gate). What is left:

* **Glue**: call `oem_led_init()` once at boot (nobody does yet; `hw_power.c` only calls it on
  wake).
* **Glue**: `hw_emulated()` must be defined; keep `g_oem.wifi_status` current. The Wi-Fi light
  condition is stock's `g_oem.sys[0x0c] == 2` ("provisioned"): if the custom Wi-Fi setup does not
  set that byte, LED1 never shows Wi-Fi state. Either mirror "an SSID is configured" into
  `sys[0x0c]`, or tell me to switch the condition to a cached `hal_wifi_has_ssid()` (not called
  from the 10 ms tick as it is, because its cost is unknown).
  Other tasks (web, MQTT) should read `g_oem.batt_pct` / `batt_mv` / `batt_fault`, not call
  `oem_batt_mv_now()` (it sleeps 10..20 ms).
* **Anyone using GPIO2 or GPIO9 interrupts**: `hw_charge.c` installs the per-pin handlers for
  both (and calls `gpio_install_isr_service(0)`, tolerating "already installed"). A second
  `gpio_isr_handler_add` on those pins would replace them.
* No prototype was added to the "Requests" block.

## 5. Host tests

```
cc -std=gnu11 -Wall -Wextra -Imain -Ire/tools/uisim/fake_idf re/tools/uisim/sim_led.c main/hw_led.c -o /tmp/sim_led && /tmp/sim_led
cc -std=gnu11 -Wall -Wextra -Imain re/tools/uisim/sim_gauge.c -o /tmp/sim_gauge && /tmp/sim_gauge
```

`sim_led` runs `oem_led.c` on the real `hw_led.c` against a fake LEDC / GPIO
(`re/tools/uisim/fake_idf`) and prints raw duty versus time: 71 checks, 0 failed. Covered: timer
and channel configuration, initial duties, boot pin preset; static states; scripts 0, 1, 1B, 2, 3
(durations 232 / 232 / 232 / 262 / 52 ticks, every fade start / end time and step size, backlight
soft start, raw 2191 after the wake and the charger script, raw 8191 after the sleep script,
level 2071 left when the sleep script starts from the static backlight); breathe on LED3 (period
415 ticks, 0.80 s rise, 243 ticks at 4000, 14 dark) and on LED4 (1.63 s rise, 75 ticks full);
blink (330 ms, one interval in eight 250 ms; dark on LED1..3); Wi-Fi light in the three modes;
requests dropped during a script, abort; the empty-battery rule; charge-light re-init after
parking; no LEDC write without a duty change (0 writes in 10 s steady state).

`sim_gauge` runs `oem_gauge.c` with virtual time and fakes for ADC, charge pin, NVS, LED, UI,
IMU: 92 checks, 0 failed (also clean under `-fsanitize=address,undefined` and `-O2`). Covered:
percent at every 100 mV step and between, the 3445 / 4109 mV clamps, the latch; the charge period
table; start-up sequence at 1 Hz and at 10 ms ticks; discharge (one-shot 5, one point per 100 s,
never up, free drop after a session, fast fall below 10 %); the 0 % lock and its three releases;
charging at constant voltage (60 s per point, 99 → 100 after three confirmations, FULL after six
ticks, the LED call once) and a whole simulated charge from 3.5 V (monotonic, slew per band,
FULL after 176 min with the cell at twice the table speed); record save / restore / default /
garbage, NVS write count; unusable readings; thermal cut-off with hysteresis; `plug_cnt`; un-plug
timing; sleep credits. Expected values were worked out from the stock code before running. The
last test covers the diagnostics: `batt_raw_mv` against the filtered value, and the thermal
latch staying set when a re-attach releases the pin.

Not testable on the host: the ESP-IDF calls themselves (`hw_battery.c`, `hw_charge.c`, the real
LEDC), checked only with `re/tools/esp_syntax.sh` (clean for all five files).

## 6. Unverified on hardware — watch on first boot

1. **Charging**: the old firmware drove GPIO26 high (= blocked). After this change the pin is
   released at boot; confirm the dock actually charges (voltage rising in the log, or
   `batt_raw_mv` in `/api/status`, section 1) and that `oem_charger_present()` (GPIO9 low)
   follows the dock.
2. **LED polarity / position**: LED1, 2, 4 lit with the pin low and LED3 with the pin high is
   inferred from the stock configuration. If an LED is on when it should be off, it is one of
   these. Colours are unknown.
3. **Backlight**: full after `(4, ON, 4)`, visibly dimmer (73 %) after a wake or charger script —
   stock does the same; the jump when the app calls `(4, ON, 4)` four seconds after boot / un-plug
   may be visible.
4. **Battery voltage**: compare the logged mV with a meter once; if curve fitting reads more than
   ~20 mV off, the 0 % point (3445 mV) and "full" (4109 mV) shift accordingly. A log line
   `no ADC calibration` means the gauge runs on the fallback of deviation 1.
5. **GPIO2**: nothing is known about what pulses it. If the brush reports "removed" while on
   the dock, check that `oem_charger_alive_take()` is wired into the poll. `alive_edges` in
   `/api/status` counts its falling edges.
6. **LEDC under power management** (charge light while idle on the dock): `CONFIG_PM_ENABLE` is
   on now, but no light sleep is ever entered (tickless idle is off) and the APB clock, which
   `LEDC_AUTO_CLK` selects for 5 kHz at 13 bit, stays at 80 MHz (NOTES_power section 5), so the
   PWM keeps running as before. If automatic light sleep is enabled later, or the 40 MHz stage
   becomes reachable, check that the breathing charge light survives it (spec open question 7).
7. **Thermal cut-off** depends on `oem_imu_temp()`; it has never been exercised on the device.
   `thermal_cut` and `charge_blocked` in `/api/status` show the latch and what the pin was last
   set to.

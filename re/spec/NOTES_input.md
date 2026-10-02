# NOTES: input module (touch, gestures, button, force sensor)

Spec: `input.md`, `brushing.md` section 4. Stock code read for it: the bus `0x4201e844..0x4201ec30`
and `0x4200cb84 / cbd0 / cbec / cc30 / cc4c / cc58`; touch `0x4201b430`, `0x4201b458`, `0x4201b4f8`,
`0x42025e20..0x420269b0`; gestures `0x42025ae4`, `0x42025afc`, `0x42025ca4`, `0x420214b0..0x42021508`;
button `0x40377c98..0x40377e74`, `0x42019c74..0x42019cf8`, `0x4201b680`; force sensor
`0x420269e0..0x42027bd0`, `0x403787cc`, and the Awinic library `0x42071e64..0x42072e72`,
`0x421036bc..0x42103890` (not in `re/work/decomp`; decompiled separately and read in disassembly).

## 1. What is implemented

| File | Content |
|---|---|
| `main/hw_bus.c`, `hw_bus.h` | Bit-bang I2C on SCL13 / SDA14: `hw_bus_init` (`0x4200cb84`), `hw_bus_write8` (`0x4200cbd0`), `hw_bus_write16` (`0x4200cbec`), `hw_bus_read` (`0x4200cc30`). Same pin operations and order as stock, including the poll-until-high after every release (clock stretching) and the 4-attempt address loop. Main task only. |
| `main/hw_touch.c` | IQS7222D. `oem_touch_init` (bus + RDY driven low, `0x4200da4c`), `oem_touch_irq` (`0x4200cc58`), `oem_touch_set_state` (`0x4201b430`), `oem_touch_state`, `oem_touch_step` (`0x4201b458`: probe, the 67 init writes, ack-reset, run, reseed, sleep sequence), `oem_touch_tick_10ms` (3 s reseed watchdog `0x42025e20`). |
| `main/oem_gesture.c` | Core, host-buildable. `oem_gesture_sample` (`0x42025ca4`), `oem_gesture_end` (`0x42025afc`), `oem_gesture_touching`. Posts UI messages 7 / 8 / 9 / 10 / 11. |
| `main/hw_button.c` | GPIO3: ISR + timers at 2 / 3 / 5 / 8 s + the 100 us "wdtcheck" timer, `oem_button_init` (`0x42019cf8` + the GPIO3 part of `0x4200d77c`). Codes go to `oem_button_push()` + `OEM_EV_BUTTON`; the 8 s hold also posts `OEM_UIEV_FACTORY`. |
| `main/hw_pressure.c` | AW8686X. `oem_pressure_init` (`0x42027bd0`: id probe, register setup, calibration from NVS, temperature coefficient), `oem_pressure_start` / `_stop` (`0x42027b4c` / `0x42027ba0`), `oem_pressure_sample` (`0x420277d4`), `oem_pressure_available`. |
| `main/oem_force.c` | Core, host-buildable. Port of the Awinic algorithm library the stock driver calls (input filter, baseline tracking, press state), the calibration-record parser and the temperature coefficient. |
| `main/oem_input.h` | Module-private header (gesture and force functions shared by the files above and the host tests). No ESP-IDF content. |
| `re/tools/xt_emu.py` | Small Xtensa interpreter: runs functions of the stock image on the host (decoding with the toolchain's objdump). Used by the two differential tests below. |
| `re/tools/uisim/sim_gesture.c` | Host test of the decoder. |
| `re/tools/uisim/sim_force.c`, `check_force.py` | Force algorithm: self test, and comparison with the stock machine code. |
| `re/tools/uisim/sim_input_drv.c`, `check_input_drv.py` | Touch and pressure drivers on a logging fake bus, compared with the stock driver code. |
| `re/tools/uisim/sim_input_hw.c` | Bit-bang bus against a pin-level I2C slave model; button against a virtual clock. |
| `re/tools/uisim/fake_idf_input/` | Host stand-ins for the IDF headers these drivers include. |

Shared headers: `oem_state.h` got five diagnostic fields in the input block (`force_raw`,
`force_base`, `force_coef`, `touch_x`, `touch_y`; nothing in the core reads them). `oem_api.h`,
`oem_hal.h`, the timer list and the RTC struct are unchanged (`oem_pressure_available()` was
already declared in the Requests block by the app module; `hw_pressure.c` defines it and
replaces the weak fallback in `oem_app.c`).

**Build**: add `hw_bus.c hw_touch.c hw_button.c hw_pressure.c oem_gesture.c oem_force.c` to
`main/CMakeLists.txt`; `hw_i2c.c` and the button part of `hw_io.c` are superseded. All six pass
`re/tools/esp_syntax.sh` without warnings; the two core files also build with the host compiler.

## 2. How the force value is produced (was open in brushing.md)

Established completely; `oem_pressure_available()` returns true when the chip answers.

1. **Chip setup** (`0x42027894`, `0x42026d08`, `0x42026ae8`): id register 0x00 must read 0x61,
   0x62 or 0x64 (3 attempts). Then `05=A5, 2F=10`, 20 ms, `05=A5`, 22 register writes from rodata
   `0x3c11c9ab`, the analog front-end sequence (two waits of 1 ms and 5 ms, three
   read-modify-writes on 0x31 / 0x31 / 0xC0) and `17=81, 16=20, 1B=06`. 53 writes in all; the
   names of the registers are not known, the bytes are the stock ones.
2. **One sample every 20 ms** (`0x420277d4`): `16=44`, `16=20`, wait 2 ms, read 2 bytes from
   0xB0 (little endian), subtract 0x2000 -> signed ADC value.
3. **Algorithm** (Awinic library, one channel, parameters hard-coded in `0x42027894`):
   - input filter: output one sample late; a sample that differs from both neighbours by more
     than 16 counts is replaced by their mean; steps smaller than the calibrated noise are
     low-passed (60 % old value);
   - baseline: mean of samples 16..20 after init, then tracked while nothing presses (the step
     depends on distance, slope and recent history), frozen while pressed, reset to the signal
     after 60000 samples (20 min) of continuous deviation, and lowered to the signal when a
     press ends below it;
   - `force = (filtered sample - baseline) * coef / 100`.
4. **Calibration** (`coef`, `noise`): 16-byte record at the start of the NVS blob
   `storage / aw8686x_config` written by the factory test (`0x42026c08`): `':' A0 06 | noise u16 |
   coef u16 | 5A 5A | byte sum u32 | 00 | CR LF`. The factory computes `coef = 20000 / (ADC
   counts under the test load)`, so one unit is 1/200 of that load (grams if the load was 200 g
   [inferred]). Without a valid record stock uses noise 9, coef 23; so does the port.
5. **Temperature** (`0x403787cc`, at every init): `k = 1 + (IMU temperature now - byte 0 of NVS
   blob rec_temperature) / 100`, limited to 0.75..1.25 (1 if no base was recorded).
6. **Result**: `g_oem.pressure = trunc(max(0, force) * k)` as int16 (a 16-bit overflow reads
   as 0, like stock's `max(0, value)` in `0x42018530`).

This image always runs with its hardware-version flag (`0x3fc9abba`) at 1; only that branch is
ported. The factory test / calibration procedure (`0x42026f5c`, `0x42027368`) is not ported.

## 3. Tests

Run from the repository root (commands are also at the top of each file).

| Test | Result |
|---|---|
| `sim_gesture` | 70 checks pass: four directions (decoder and raw coordinates), payloads, 5 / 6 samples, the 70 ms timer restart, travel 19 / 20 on both axes and the one-sample lag of the x extreme, 14 cross-axis combinations (ties, the 49 limit), long touch at sample 80 (once, with / without travel, swipe after it), gates (`sys[0x69]`, screen 94, `reset_flag`, charger: event dropped and state kept), session cases (running, paused, touch that began while running, `stop_delay` 30 / 31). |
| `sim_force` | self test passes (record parser, temperature coefficient, a press / release / drift scenario). |
| `check_force.py` | The library is executed from the stock image in the interpreter and compared with `oem_force.c` after every sample, **all 236 state bytes**: 19 sample streams (brushing with ripple, taps, dips, drift, random walks, full-scale steps, long press / long dip with short timeouts, calibrations 23 / 41 / 50 / 100 / 220..600, and six streams with non-stock parameters to reach branches that are dead with the stock constants). 236 000 samples in the full run plus 60 000 per seed in `--quick` mode (two seeds): no difference. The one difference found on the way (a divisor in a branch only reachable with non-stock parameters) is fixed. gcov on the port under these streams: the only branches not taken are ones that cannot be (they depend on the fields nothing ever writes, section 6, or are excluded by an earlier test). |
| `check_input_drv.py` | The stock touch state machine, AW8686X init, temperature function and sample function are executed in the interpreter with their bus calls hooked; `hw_touch.c` / `hw_pressure.c` run on a logging bus. Compared line by line: touch scenario (cold start, id probe, init skipped while RDY is high, 67 init writes, ack-reset, run with and without finger, re-init on status bits 1 and 3, reseed, sleep sequence, re-init with the 0x55 channel byte, every other state value) = 407 transfers; pressure with 4 calibration / temperature combinations x 260 samples (833 transfers each, pressure value after every sample), wrong id, id on the third read. All equal. Plus port-only checks (pin setup, reseed watchdog, start / stop / re-init, dropped samples). |
| `sim_input_hw` | 38 checks pass: bus wire patterns for write8 / write16 / read (ACK / NACK placement, byte order, repeated START, pins released after STOP), clock stretching, the absent-slave pattern (4 STARTs, 57 SCL edges), back-to-back transfers; button timing (79 / 80 / 1499 / 1500 / 1999 / 2000 ms, holds to 9 s, bounce, timer restart, release just before a timer, the stuck-timer restart). |

Not testable here: real pin timing, the chips' behaviour, interrupt latency.

## 4. Integration (glue / other modules)

Call order at boot (the app module already calls the rest where stock does):

```
oem_power_boot()            // releases the pad holds, installs the ISR service
oem_touch_init()            // AS EARLY AS POSSIBLE after it: bus pins, RDY driven low
oem_button_init()           // any time before oem_app_boot
oem_pressure_init()         // optional, after the IMU is up (it reads the temperature);
                            // oem_pressure_start() does it itself if it was not called
oem_app_boot()              // -> oem_touch_irq(true), oem_pressure_start(), init_ok = 5, oem_touch_set_state(0)
```

- If `oem_touch_init()` is never called, `oem_touch_irq()` / `oem_touch_set_state()` do the pin
  setup on first use (the RDY-low period at boot is then lost). There is no such fallback for
  the button: without `oem_button_init()` there is no button.
- Glue: `OEM_EV_TOUCH_RDY` -> `oem_touch_step()`, `OEM_EV_PRESSURE` -> `oem_pressure_sample()`,
  `OEM_UIEV_GESTURE` -> `oem_gesture_end()` (the app / UI modules do this already);
  `hal_event_post` / `hal_ui_event_post` are called from the GPIO ISRs and from esp_timer
  callbacks; `oem_button_push()` is called from both as well (the app's implementation is a
  lock-free queue, fine).
- `oem_touch_tick_10ms()` must be called on every 10 ms tick while awake (it divides by 100
  itself and applies the stock conditions; the app's extra condition is harmless).
- `oem_pressure_start()` = stock init + timer start: after `oem_pressure_stop()` (screen off)
  the next start sets the chip up again and takes a new baseline, as stock does on every wake.
  The brush head must be unloaded for the first 0.4 s after that, as on stock.
- Sleep parking of GPIO12 / 13 / 14 and the hold release on wake are the power module's
  (`0x4200dbbc`, `0x4200dda4`, `0x4200df88`); nothing of that is in these drivers.
- NVS: `hal_nvs_get("aw8686x_config")` and `hal_nvs_get("rec_temperature")` (stock namespace
  `storage`, 30-byte blobs). If the stock NVS did not survive the switch to the custom firmware,
  the defaults apply (log line at init says which).
- Web UI (suggestion): show `g_oem.pressure`, `force_raw`, `force_base`, `force_coef`,
  `touch_x`, `touch_y` and `oem_touch_state()` on a diagnostics page; they answer most of the
  first-boot questions below without a serial port.

## 5. Deviations from stock, and why

1. **Bus timing**: stock has no delays. The same GPIO calls are used, plus `esp_rom_delay_us(2)`
   after every SCL edge, after the START's SDA edge and after STOP, so that the clock phases stay
   within I2C fast-mode limits whatever the CPU clock is (stock's CPU clock and therefore its bus
   speed are not known). Expected speed about 60..100 kHz. The transfer functions return whether
   the address was acknowledged (stock returns nothing).
2. **Touch, state 5, read not acknowledged**: stock would take the idle-bus 0xFFFF for a
   "reset" status and run the full init and re-ATI; here the report is skipped and a warning is
   logged every 50th time. A real reset still shows in the next answered read.
3. **Button**: (a) the stuck-timer check restarts with `esp_system_abort()` (panic restart)
   instead of `esp_restart()` inside the interrupt; (b) stock increments `init_ok` on every
   button edge during boot (five edges would enable input before the main task is up) — not
   ported; (c) `key_press_last` (`0x3fc9f301`) is not provided, the power module samples GPIO3
   itself; (d) no printing from the ISR.
4. **Pressure**: (a) the algorithm state is static (stock allocates 236 bytes at every init and
   never frees them); (b) a sample whose read is not acknowledged is dropped, and after 50 in a
   row (1 s) `oem_pressure_available()` turns false and sampling stops until the next init —
   stock would feed 0xFFFF into the baseline tracker; (c) `g_oem.pressure` is written at every
   sample (stock copies it into that variable from the 30 ms session tick, `0x42018530`; the LED
   and motor reactions of that function are the app's); (d) the waits (20 / 1 / 5 / 2 ms) are
   busy waits (100 Hz tick); (e) only record length 6 is accepted (stock reads `len` unchecked);
   (f) if the IMU temperature cannot be read the coefficient is 1; (g) `force_raw` is the
   unfiltered ADC value (stock's BLE debug variable holds the filtered one).
5. **Gesture**: write-only stock variables (touch counter, `x > 130` counter), the
   production-test "266_touch" counter and two calls to empty functions are left out. The
   "on battery only" test of the stock UI task is repeated inside `oem_gesture_end()`.
6. **Emulation** (`hw_emulated()`): no bus traffic, no RDY interrupt; `oem_touch_state()` is 5;
   pressure unavailable; the button works on the GPIO.

## 6. Notes on the stock code

- The spec says GPIO13 / 14 are plain inputs after `brush_gpio_cfg`; `0x4200cb84` makes them
  push-pull outputs at level 1. The first START turns them into inputs. Ported as found.
- A status read of the touch IC ends with STOP before the `DB=0D` / `FF=00` writes (input.md open
  question 6); kept.
- In the Awinic library three fields are read but never written (state offsets 0x4a, 0x4e,
  0xa0 / 0xa2). With them at 0 the "peak" used for the release threshold is derived from the
  baseline alone, and one baseline candidate formula never runs. Ported as it behaves.
- The library keeps its own press / touch state (press above `max(20, noise * coef / 100)`,
  release below 10); the brush logic does not use it, only the force.

## 7. Unverified on hardware: what to watch on first boot

1. **AW8686X id**: log line `pressure: AW8686X id 0x6x`. The old driver's "id 0x00" was most
   likely an unacknowledged address (its read function returned early and left the 0). If the
   new log says `not found (id 0xff, address not acknowledged)`, the chip does not answer at
   all: check the bus on a scope (pull-ups, speed), not the register code.
2. **Force scale**: the init log says `factory calibration` or `no stored calibration, default`.
   At rest `pressure` must be 0 and `force_raw` well inside -8192..8191; light brushing should
   give some tens to a few hundred, hard pressing more than 400. If the numbers are far off with
   the default coefficient: `coef = 20000 / (force_raw - force_base under a 200 g load)`
   [inferred from the factory code].
3. **Touch comes up**: `touch: IQS7222D found`, `init sent`, then `oem_touch_state()` = 5 within
   a second of boot and after every wake. If it stays at 0x21 (33): RDY edges arrive but RDY is
   high again when the main task looks (window too short / main task too slow). If it stays 0:
   no RDY edges at all.
4. **Swipe directions**: stock's mapping is kept: decoder x (= 255 - register 0x14) rising =
   "swipe up" (message 10: next mode page / strength up), falling = "down" (8); y (= 255 -
   register 0x15) rising = "right" (9), falling = "left" (7). Which way that is on the handle is
   not known; if up / down or left / right are swapped, the fix is the sign in
   `oem_gesture_end()`.
5. **Gesture timing**: 6 reports are needed; with the 12 ms report rate that is about 70 ms.
   Frequent `status read not acknowledged` warnings mean reports are being lost.
6. **RDY low at boot**: stock holds GPIO12 low from pin setup until the main task is up
   (several 100 ms). If `oem_touch_init()` is called late the low time is shorter; whether the
   chip cares is unknown.
7. **Button**: a restart at the moment of a button press would be the stuck-timer check firing
   on contact bounce (press, bounce high, press again within the same interrupt burst). Stock
   has the same check; if it happens, drop the check in `on_press()`.
8. **CPU time**: a pressure sample holds the main task for about 3 ms every 20 ms (2 ms of it
   the conversion wait). If that hurts, split it into two events (trigger, then read 2 ms later
   from a one-shot timer).
9. **Init duration**: the 67 touch init writes take about 50 ms at the expected bus speed, with
   the window held open. If the chip closes the window earlier the init is cut short and the
   status read after it will show a reset (re-init loop, visible in the log).

## 8. Open points

- Register names of the AW8686X and bit names of the IQS7222D are unknown / inferred; only the
  byte sequences are known to be right.
- Physical unit of the force value (grams?) and whether this unit has a factory record in NVS.
- The sleep sequence of the touch IC runs only if an RDY edge arrives after
  `oem_touch_set_state(7)` and before the interrupt is removed (stock waits 500 ms); same as
  stock.

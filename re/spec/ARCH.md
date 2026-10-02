# Porting the OEM behaviour: architecture and rules for implementers

The custom firmware re-implements the stock (OEM) application of the Oclean X Ultra 20 so the
brush behaves like stock (screen pages, touch gestures, brushing modes, LEDs, charging display,
auto on/off) while keeping the custom Wi-Fi / web UI / MQTT / BLE additions. Nobody can watch the
device during development and flashing is costly, so the code must be right from the specs:
`re/spec/{ui_flow,input,brushing,led_battery_charge,power}.md` (each fact tagged CONFIRMED or
INFERRED with stock addresses). The decompiled stock code is in `re/work/decomp/f2/<addr>.c`
(grep `re/work/decomp/all2.c`, index in `re/work/decomp/summary.txt`); exact disassembly:
`python3 re/tools/fd.py ADDR [+LEN]`. When a spec is unclear or two specs disagree, read the
stock code and follow it; record what you found in your NOTES file.

## Structure

The stock app is two tasks around plain globals: the main task ("brush_app", event-group
driven: 10 ms tick, button, touch RDY, charger, session start/end, sleep) and the UI task
(message queue, 50 ms blink tick, renders screens). The port keeps that shape.

```
 web / MQTT / BLE  --(oem_remote_*, hal_lock)-->  oem core (platform independent, main/oem_*.c)
 ISRs / timers     --(hal_event_post bits)----->    oem_app.c   main-loop body, button, charger, idle, wake, sequencer
                                                    oem_brush.c profiles, session clock, zone cue, pressure, score
                                                    oem_ui.c    UI messages, pages, blink tick, screen composition
                                                    oem_led.c   LED scripts / states      oem_gauge.c battery gauge
                                                    oem_gesture.c touch gesture decoder    oem_wave.c  motor waveform
                                                  drivers (ESP-IDF, main/hw_*.c)
                                                    hw_led.c hw_battery.c hw_charge.c hw_bus.c hw_touch.c hw_button.c
                                                    hw_pressure.c hw_motor.c hw_imu.c hw_power.c hw_display.c
                                                  glue (oem_glue.c): tasks, event groups, timers, HAL (oem_hal.h)
```

Contracts (read all three before writing code):

* `main/oem_state.h` — `g_oem`, the shared state (stock globals by spec name, with owner), and the
  RTC-retained struct.
* `main/oem_hal.h` — what the platform provides to core files (lock, time, events, timers, NVS
  blobs, LCD, picture partition). Core files (`oem_*.c`) include only `oem_hal.h`, `oem_state.h`,
  `oem_api.h`, `ui_render.h`/`stock_ui_tables.h` (UI only) and the C library — never ESP-IDF
  headers — so they also build on the host.
* `main/oem_api.h` — every function one module calls on another, with the stock address it ports.
  Implement exactly the prototypes of your block. If you need something from another module that
  is not listed, add a prototype to the "Requests" block at the end and describe it in your NOTES
  file. You may append fields / HAL functions / timers in the blocks reserved for your module in
  the three headers; do not edit other parts of them.
* `main/hardware.h` — pin map and `hw_emulated()` (true under QEMU: skip bus traffic that would
  hang there, return benign values).

Threading: every core function runs with the core lock held (the glue takes it), in the main
task unless `oem_api.h` says UI task. Drivers post events from ISRs / esp_timer callbacks with
`hal_event_post()`; they must not call core functions from an ISR. `hal_delay()` may be used where
stock calls `vTaskDelay` inside a handler.

Facts that override older notes: charger present = GPIO9 LOW; motion interrupt = GPIO8 (active
high); GPIO26 driven high BLOCKS charging (input / high-Z allows it); GPIO45 is always written 0;
the touch IC (IQS7222D, 0x44) and the force sensor (AW8686X, 0x6A) share the bit-bang I2C bus on
SCL13 / SDA14; backlight = LEDC channel 4 / GPIO21, active low. ESP-IDF is v5.1.1; FreeRTOS tick
is 100 Hz in this project (stock: 1000 Hz), so never rely on `vTaskDelay` below 10 ms — use
`esp_rom_delay_us` for short waits and esp_timer for periodic work.

## Rules

* Port the stock behaviour faithfully, including its timing constants. Do not "improve" it,
  except: skip factory / production / aging / shop-demo / OEM-cloud paths unless a normal user
  can reach them; and where the spec flags a stock bug (out-of-bounds table read, memory
  corruption), do the safe thing and say so in a comment.
* Never invent a pin, register value or timing. If the spec does not have it and the stock code
  does not settle it, leave the feature out and list it in NOTES.
* Write code that reads like the rest of `main/` (C11, 4 spaces, `static` helpers, short comments
  that say why, stock address in a comment where a function ports one). Logging: drivers use
  `ESP_LOGx`; core files use `hal_log()`.
* Stay inside your files. Do not edit files owned by another module, the build files, or
  anything under `re/spec/`. Do not run `idf.py build` (other people are working in the same
  tree); check ESP-side files with `re/tools/esp_syntax.sh main/<file>.c` and core files with the
  host compiler (`gcc -std=gnu11 -Wall -Wextra -fsyntax-only -Imain main/<file>.c`).
* Test core logic on the host: put a small test program under `re/tools/uisim/` (named for your
  module), with fakes for whatever your module calls, and run it. Report what you tested and
  what you could not test.
* Do not touch the device or the network. Do not commit.
* Finish with `re/spec/NOTES_<module>.md`: what you implemented (file list, entry points), every
  deviation from the spec and why, requests to other modules, and what remains unverified on
  hardware (things the owner should watch for on first boot).

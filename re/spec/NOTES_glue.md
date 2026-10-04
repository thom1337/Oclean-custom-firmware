# NOTES — platform glue (`main/oem_glue.c`)

The last piece of the port: the HAL of `oem_hal.h` on FreeRTOS / ESP-IDF, the two tasks of the
stock layout, the boot sequence (`brush_app_start`), `hw_emulated()`, and the safe-mode guards
around it. Built with `idf.py build` (0x14b8d0 bytes, 57 % of the 3 MB slot free; 0x12a2f0 bytes
since Bluetooth is no longer built, section 5, which leaves 22 % of the brush's own 0x180000
slot free) and run under QEMU (section 9). Nothing here ran on the brush. Section 12 lists what
a review of the first version found and how each point was fixed.

## 1. Files and entry points

| File | Content |
|---|---|
| `main/oem_glue.c` | every `hal_*` function of `oem_hal.h` that no driver provides, `hw_emulated()`, `brush_app_start()`, `oem_glue_early_init()`, `brush_app_running()`, `oem_glue_imu_temp()`, `oem_glue_set_tz()`, the tasks `brush_app` and `UI_TASK` |
| `main/oem_glue.h` | `oem_glue_early_init()`, `brush_app_running()`, `oem_glue_imu_temp()`, `oem_glue_set_tz()` |
| `re/tools/uisim/qemu_inject.c` | test only, not part of the build: scripted button / remote input for a QEMU run (section 9.3) |
| `re/tools/uisim/sim_glue.c` | host test: what the glue's Wi-Fi rules do to the brush logic (section 9.5) |

`re/tools/undefined_syms.sh` prints nothing; `re/tools/esp_syntax.sh main/oem_glue.c` is clean.
No core file (`oem_*.c`) and no driver (`hw_*.c`) was changed. Changes in other files: section 8.

## 2. Threading as built

| Task | Runs | Priority | Core | Stack | Least stack left in QEMU |
|---|---|---|---|---|---|
| `brush_app` | `oem_app_boot()` once, then `oem_app_handle(bits)` per wake-up | 3 | 1 | 8192 | 5396 |
| `UI_TASK` | `oem_ui_handle(bits)`, then pushes the frame | 2 | 1 | 6144 | 3648 |
| `brush_music` (hw_motor.c) | motor stream | 10 (raised from 3) | 0 | 4096 | - |
| esp_timer task (IDF) | HAL timer callbacks: post a bit | 22 | 0 | 3584 | - |
| FreeRTOS timer service task | sets event bits posted from interrupts | 22 (raised from 1) | 0 | 2048 | - |

Stock: `brush_app` and `UI_TASK` both 3072 bytes, priority 3, no affinity, 1 ms tick; the motor
task priority 3 on core 0.

* **Stacks** are generous on purpose (the main task does NVS writes and logging; the web log hook
  alone formats into 256 bytes of stack). QEMU exercised no SPI / bit-bang / flash-heavy path, so
  the real marks will be lower; they are in every `glue: ui: screen ...` log line.
* **UI priority 2, not 3.** Composing and pushing a frame keeps a core busy for about 10 ms, up
  to 20 times a second (screens 70, 84, 103). On stock's 1 ms tick equal priorities share a core
  in 1 ms slices; on the 10 ms tick of this project the main task would wait out the whole frame
  (late 10 ms ticks, missed touch windows). One step lower the UI task can never hold up the main
  task. The core lock is a mutex with priority inheritance, so the main task does not wait behind
  a preempted UI task either.
* **Both on core 1.** Core 0 carries Wi-Fi, BLE, lwIP, the esp_timer task and the motor stream
  (37.5 ms of samples buffered); nothing the brush logic does can starve those, and the bit-bang
  bus sees fewer interrupts. Pinned explicitly because an un-pinned task gets pinned by IDF to
  whatever core it happens to run on at its first float operation (the force scaling does one).
  Tasks above priority 3 without affinity (httpd, MQTT: 5) can still land on core 1 and delay the
  main task for as long as they compute; none of them computes for long.
* **Motor stream at priority 10.** `hw_motor.c` creates `brush_music` as stock does (priority 3,
  core 0); `brush_app_start()` raises it right after `oem_motor_init()`
  (`vTaskPrioritySet(xTaskGetHandle("brush_music"), 10)`), as NOTES_motor section 6 foresees.
  The DMA holds 37.5 ms. At priority 3 the task sits below what this firmware adds on core 0:
  the web server and the MQTT client (both 5, no affinity) can compute for longer than that
  (Home Assistant discovery: 37 messages in a row; a TLS handshake), and each buffer the stream
  misses goes out as silence. 10 is above those two and below lwIP (18), the default event loop
  (20), the NimBLE host (21), the esp_timer task (22) and Wi-Fi (23). The task blocks in the I2S
  write nearly all the time (about 1 % of a core), so nothing below it is held up. The priority
  is in the `glue: brush app running (...)` log line.
* **Core lock**: one recursive mutex, created by `oem_glue_early_init()` (first thing in
  `app_main`). The two tasks hold it around every call into the core. Others that take it through
  `hal_lock()`: httpd task (web_server.c, metrics.c), MQTT task (commands, state on connect), the
  `mqtt_state` task (periodic state), the default event loop task (wifi_mgr.c), the NimBLE host
  task (ble_server.c). They can wait for as long as a main-task handler runs: about 0.5 s during a
  wake (pin restore, 100 ms, IMU, panel init), 0.4 s during the lock buzz. None of them talks to
  a bus while holding it: they copy state, queue commands and UI messages, which takes
  microseconds (the one exception is the gauge record, an NVS write, before a restart and at the
  end of an update).
* **IMU temperature for the snapshot.** `metrics_get_brush_state()` used to call
  `oem_imu_temp()` under the core lock in whatever task asked: three register reads, each with
  1 ms of busy-wait, on an SPI device that only one task may use. The MQTT task did that 37
  times in a row at every connect (once per discovery entity), the NimBLE host task (priority
  21, core 0) once per GATT write. Now the main task, which owns the IMU, reads the temperature
  itself: every 5 s, after a handler pass that carried the 10 ms tick, with the lock it already
  holds (`imu_temp_poll()`; 3 ms on core 1). `oem_glue_imu_temp()` returns that value (NAN: no
  reading, emulation, safe mode) and the snapshot copies it. A reading can be 5 s old.
* **The esp_timer task must never take the core lock**: it would hold back every timer of the
  system for that long, and `hw_button.c` restarts the brush if the 100 us timer of a press has
  not run by the next press. The only offender was the periodic MQTT publish; it now runs in a
  task (section 8).
* **Events**: two FreeRTOS event groups (main: 24 bits, UI: 0x3FF), waited on with clear-on-exit.
  A post from a task sets the bits directly (esp_timer callbacks are dispatched from the
  esp_timer task in this configuration). A post from an interrupt uses
  `xEventGroupSetBitsFromISR`, which only queues the request for the FreeRTOS timer service task.
  The project configures that task at priority 1, below everything else: the bits would be set
  only when core 0 has nothing else to do. Stock relays its GPIO interrupts through a
  priority-30 task (`key_int`, 0x4201b560). `brush_app_start()` therefore raises the timer
  service task to the priority of the esp_timer task (22); nothing else in the image uses
  FreeRTOS software timers. If its queue (10 entries) is ever full, the bits are kept and posted
  by the next 10 ms tick, with a warning in the log.
* **Timers**: one esp_timer per line of `HAL_TIMER_LIST`, task dispatch, `skip_unhandled_events`
  (a periodic timer that fell behind fires once: the event bit cannot count anyway).
  `hal_timer_start` = stop, then start. `TICK10` and `BLINK50` are started before the tasks exist
  and are never stopped by the glue.
* **`hal_delay(ms)`**: `vTaskDelay(ceil(ms / 10) + 1)` with the lock held. The extra tick makes the
  wait never shorter than asked (a delay of n ticks lasts n-1..n periods); `hal_delay(100)` is
  100..110 ms where stock's is 99..100 ms.
* **Main-loop check**: the 10 ms timer callback counts ticks since the main task last finished a
  pass; at 3000 (30 s) it calls `esp_system_abort()`. Reason: the firmware update handlers take
  the core lock, so a main task that hangs with the lock leaves a brush that cannot be updated
  and has no reset button. A restart brings the web UI back, and if the hang returns within the
  first minute of every boot the boot guard ends in safe mode. No handler blocks longer than
  about 0.5 s; flash erases yield every 20 ms.
* **Frame push outside the lock**: only the UI task composes into the frame buffer, so the 7 ms
  transfer needs no core lock. The main task can clear the buffer during a transfer in one case
  (`oem_ui_page_back()`: short press on a side page); that frame may tear, the redraw follows at
  once. Stock has the same race.

## 3. HAL functions

| Function | Implementation |
|---|---|
| `hal_lock` / `hal_unlock` | recursive mutex (above) |
| `hal_ms`, `hal_uptime_s` | `esp_timer_get_time()` |
| `hal_delay` | above |
| `hal_time` | as stock 0x4201d89c: `time()`, `localtime_r()`, year - 100; an unset clock (1970, or 1969 west of Greenwich) gives year 226 / 225 = "not set". Local time: the zone is the configured one (section 3.1) |
| `hal_log` | `ESP_LOGI("oem", ...)`, lines up to 191 characters |
| `hal_event_post`, `hal_ui_event_post` | above; no-ops while the groups do not exist (safe mode) |
| `hal_timer_start` / `_stop` | above; no-ops while the timers do not exist |
| `hal_nvs_get` | namespace `storage`, blob; returns the stored length, copies `min(len, stored)`. A blob longer than the buffer is read whole into a heap buffer first (NVS cannot read part of a blob) |
| `hal_nvs_set` | `nvs_set_blob` + `nvs_commit`; a failure is logged |
| `hal_rtc` | `hw_power_rtc()`; the initial values and the magic check are in `hw_power.c`, nothing is repeated here |
| `hal_lcd_init` / `_sleep` / `_blit` | `hw_display_wake()` / `_sleep()` / `_blit()` |
| `hal_res_read` | `ui_res_read()` |
| `hal_restart` | `boot_guard_clean_exit()`, `esp_restart()` |
| `hal_wifi_has_ssid` | live: (the configuration passed to `brush_app_start()` names an SSID, or `wifi_mgr_has_creds()`: credentials the web UI applied since) and not `wifi_mgr_setup_ap_up()` (section 5). Under emulation `wifi_mgr.c` is never started and the boot configuration alone decides |
| `hal_ble_connected` | `ble_server_connected()`; always false in a build without Bluetooth, which is the default (section 5) |
| `hal_net_sleep` / `hal_net_wake` | section 5 |
| `g_oem.fw_version` | "a.b.c.d" from the IDF app version if it has that form, else "0.0.0.0" (a git-describe string would show four arbitrary digits on info page 0) |

`hal_anymotion_allowed` and the `hal_led_*` functions are the drivers' (hw_power.c, hw_led.c).

### 3.1 Time zone

Stock is given local time by the phone app. Here SNTP sets the clock in UTC, and the core needs
local time for the day totals of history screen 84 and the daily-goal LED chase (they reset at
midnight), the "morning" window of the auto mode (03:01..12:00), the greeting dates and the
session records.

* `app_config_t.tz` (config_store.c: NVS `oclean/tz`, 47 characters, default `UTC0`) holds a
  POSIX TZ string such as `CET-1CEST,M3.5.0,M10.5.0/3`. The C library has no zone database: a
  name like `Europe/Berlin` is not one.
* `oem_glue_set_tz()` = `setenv("TZ")` + `tzset()`. `brush_app_start()` calls it before the main
  task exists, so the first `hal_time()` in `oem_app_boot()` is already local (after a wake from
  deep sleep the clock is right at once: the RTC keeps it). `h_config_post` calls it when the
  value changes; `localtime_r()` reads `TZ` under the C library's lock, so that is safe while the
  brush logic runs.
* newlib 4.1 silently keeps the previous zone when it cannot parse the start of the string, so
  `oem_glue_set_tz()` checks that part itself (a name of 3..10 letters or a `<...>` name, then
  an offset) and refuses anything else: the web server then keeps the old value and says so in
  its reply, the boot falls back to `UTC0`. Errors further back in the string end in newlib's
  defaults (US change dates, or no summer time). What the library made of the string is logged:
  `glue: time zone <+01>-1<+02>,M3.5.0,M10.5.0/3: local time is UTC+1:00` (the offset on the
  clock's date, i.e. 1 January 1970 while the clock is not set). All of this was measured, not
  assumed: section 9.2.
* Web UI (Settings, "Clock"): a text field and a button that fills it from the browser. The
  string is computed in `app.js` (`browserTz()`): the two offsets and this year's two change
  instants are read from `Date`, each change is taken as "n-th or last such weekday of the
  month". Run under node for all 436 zones of the build host's tzdata: every string passes the
  check of `oem_glue_set_tz()` and fits (44 characters at most); 425 carry exactly the rule
  tzdata itself gives for the zone (names aside), 11 are right for this year but not for every
  year: Israel, Palestine, Egypt, Greenland, Chile (changes tied to another weekday or to
  24:00), Morocco (Ramadan), Alberta (rules about to change), and Dublin, which is merely
  written the other way round. A page that finds the default `UTC0` on the brush fills the
  field in at once; nothing is stored until "Save".

## 4. Boot (`brush_app_start`)

```
oem_glue_early_init()                  // app_main, right after weblog_init: lock, hw_emulated()
... boot_guard_check, nvs_flash_init, config_load (app_main)
brush_app_start(cfg):
  oem_glue_set_tz(cfg->tz)             // UTC0 if the stored string is not a POSIX TZ string
  hw_power_set_pre_sleep_hook(stop_radios)
  cause = oem_power_boot()             // may not return (empty battery, refused motion wake)
  oem_touch_init()                     // directly after it: RDY is held low from here on
  oem_charge_pins_init(); oem_batt_adc_init(); oem_led_init()
  hw_display_init(); ui_res_init()
  oem_button_init(); oem_motor_init()
  motor stream task ("brush_music") -> priority 10
  event groups, boot semaphore, frame buffer (25600 bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), timers
  oem_ui_init(fb)
  timer service task -> priority 22
  hal_timer_start(TICK10, 10, periodic); hal_timer_start(BLINK50, 50, periodic)
  create UI_TASK, create brush_app     // its first action: oem_app_boot(cause) under the lock
  wait (at most 3 s) until oem_app_boot() has returned
  return !hw_emulated()                // false under QEMU: app_main starts no radios
```

* `oem_pressure_init()` is not called here: `oem_pressure_start()` (second half of the boot, after
  the IMU init) does it, which is where stock runs it.
* The wait at the end means that what `app_main` starts afterwards finds `g_oem` loaded;
  `wifi_mgr_start()` writes `g_oem.sys[0x0c]`, which `brush_sys_config_load()` would otherwise
  overwrite if it ran later. In QEMU `oem_app_boot()` takes 20..40 ms; on the brush add the panel
  init (0.15..0.3 s).
* If an allocation or a task creation fails, the brush logic is not started and
  `brush_app_running()` stays false: the firmware then behaves as in safe mode (web UI and update
  work).

## 5. Radios around sleep

* **Bluetooth is not built** (`CONFIG_BT_ENABLED=n` in `sdkconfig.defaults`): nobody uses the
  phone app with this firmware, and the controller, enabled from boot until deep sleep, never
  slept. `ble_server.c` then compiles to three stubs: `ble_server_start()` only logs,
  `ble_server_stop_adv()` does nothing, and `ble_server_connected()`, with it
  `hal_ble_connected()`, is always false, so no phone can stretch the 30 s before deep sleep to
  120 s. `OEM_EV_BLE_WAKE` keeps its other source, the first Wi-Fi connection (next point).
  With `CONFIG_BT_NIMBLE_ENABLED` the file is the GATT server it was; what these notes say
  about BLE and the NimBLE host task applies to such a build only.
* **Wi-Fi connected** (stock handler 0x4200bbac, here in `wifi_mgr.c`): idle time 60 s unless
  brushing, wake script `oem_led_set(1, 0, 0)`, `OEM_EV_BLE_WAKE`. Stock gets there once per
  wake, because it stops Wi-Fi at the first disconnect and starts it again only at the next wake
  (power.md 6.2). `wifi_mgr.c` reconnects for ever, so these effects are applied to the **first**
  `STA_CONNECTED` since boot only (every wake from deep sleep is a boot), and once more after new
  credentials were applied. A link that comes back later only sets `g_oem.wifi_status`. Before
  this, every reconnect restarted the idle timer and woke the screen: a link coming back more
  often than about once a minute kept the brush out of deep sleep, and on the dock a reconnect
  after the 30 s backlight time-out lit the screen for good.
  The wake script is played off the charger only (`power_state == OEM_PWR_BATTERY`). It fades
  the backlight in, and on the dock nothing switches that off again once the 30 s after docking
  have passed (`delay_to_close_screen()` acts at second 30 only); on a full brush it also
  leaves the charge light dark. Stock cannot connect that late (it gives up after a few
  attempts), this driver can: a network that was down at boot, credentials saved while the
  brush is docked. Stock's own wake function (0x4201bd70) plays the script on battery only,
  too. In the first three seconds of a boot on the dock the charger is not detected yet and the
  script still starts; the charger handling then aborts it, as in stock.
* **Screen-off stage** (`hal_net_sleep`, called 27 s after the screen went off): nothing is
  stopped. Stock stops Wi-Fi there and reconnects on a wake; here Wi-Fi carries the web UI and
  MQTT, which are what the brush stays up for in that stage, and with a network configured and no
  phone connected deep sleep follows 3 s later anyway. The station keeps the modem sleep it has
  from `esp_wifi_init()` on: `WIFI_PS_MIN_MODEM`, the driver's default, which nothing in `main/`
  changes. Stock sets `WIFI_PS_MAX_MODEM` (power.md 6.2); that is deliberately not copied: there
  the station wakes per listen interval and may sleep through DTIM beacons, and a web server
  has to hear the ARP requests of whoever wants to reach it (comment in `wifi_mgr_start()`).
  Without Bluetooth there is no coexistence to take into account. `hal_net_wake` has nothing to
  restore.
* **Setup AP** (`oclean-setup`: no credentials, or five failed attempts, about 50 s after the
  wake). While it is up `hal_wifi_has_ssid()` answers "no", which gives the 120 s window of an
  unconfigured brush instead of 30 s. These restart that window (`oem_net_activity()`,
  which only acts in the screen-off stage on battery): the AP coming up after failed attempts
  (by then the 30 s window is usually already running and would end the AP 10..20 s later; the
  host simulation shows that the changed `hal_wifi_has_ssid()` alone does not help there), a
  station joining it (`WIFI_EVENT_AP_STACONNECTED`), a load of the page (`GET /`) or of the
  Files tab (`/api/parts`), and a partition copy while it runs (`/api/res`, at most every 5 s). The
  requests an open page repeats by itself (`/api/status`, `/api/log`) do not count: a forgotten
  browser tab must not keep the brush awake. Cost: a brush that cannot reach its network (a
  trip) stays up for about 170 s per wake instead of 67 s. And whatever fetches `/` more often
  than the window lasts (a monitor polling the page every 20 s, say) keeps the brush awake, as
  a client that keeps sending commands always could.
* **Deep sleep** (`hw_power_set_pre_sleep_hook`): `ble_server_stop_adv()` (new in ble_server.c;
  with Bluetooth built: `ble_gap_adv_stop()` and a flag that keeps the GAP callback from
  advertising again; without: nothing) and `esp_wifi_stop()`. The hook runs in the main task
  with the core lock held, so it calls nothing that waits for another of our tasks: no
  `esp_mqtt_client_stop`, no `nimble_port_stop` (both join a task that may be blocked on the
  core lock). `ble_gap_adv_stop()` waits only for the controller (2 s at most). The hook does
  nothing before `brush_app_start()` has returned (boot-time re-sleep: no radio exists yet) and
  under emulation.
* Not done: MQTT is not told. The broker publishes the last will ("offline") when the keep-alive
  runs out. A BLE connection is not closed; it ends with the sleep.
* A deep sleep that `hw_power.c` refuses (charger present, QEMU) leaves the radios untouched: on
  the dock the brush stays reachable.

## 6. Safe mode

`app_main` does not call `brush_app_start()` in safe mode, but it starts Wi-Fi, the web server
and MQTT, whose handlers use `hal_lock()` and `g_oem`.

* The core lock exists from `oem_glue_early_init()` on. Event posts and timer calls are no-ops.
* `brush_app_running()` is false. `g_oem` then only holds its initialiser (0 %, "on battery"),
  no driver is initialised, nobody runs the main loop. Guarded call sites:

| File | What is skipped when the brush logic does not run |
|---|---|
| web_server.c | `oem_net_activity()` (page load, config, OTA, log level, Files tab, partition copy); `/api/brush` answers 500; `oem_gauge_save()` on reboot and after an update; the OTA screens 88 / 89 / 90; **the "battery below 20 %" and "brushing" refusals of `/api/ota`**; `oem_charger_present()` for `charger_present` in `/api/status` (its first call sets up the charger pins) |
| metrics.c | nothing to skip any more: the snapshot copies `oem_glue_imu_temp()`, which stays NAN when the main task does not run (it used to call `oem_imu_temp()`, which would set up the IMU SPI bus) |
| mqtt_ha.c | remote commands |
| wifi_mgr.c | `oem_idle_timeout(60)`, `oem_led_set(1, 0, 0)` on the first STA connected; `oem_net_activity()` for the setup AP |
| ble_server.c | `oem_net_activity()`, `oem_remote_strength()`, `oem_gauge_report_reset()` (BLE is not started in safe mode, and not built at all by default) |

  Two of these were real faults, not just tidiness: with `g_oem` at its initial values `/api/ota`
  refused every update in safe mode ("battery below 20 %"), which is the one thing safe mode is
  for; and `/api/reboot` would have saved the gauge's power-on state, replacing the stored
  battery record by "no record".
* The time zone is not applied at boot in safe mode (`brush_app_start()` does it, and nothing
  reads the brush's clock there); a zone saved in the web UI is still stored and applied.
  `wifi_mgr.c` writes `g_oem.wifi_status` and `g_oem.sys[0x0c]` in safe mode too: plain memory.
* Safe mode cannot be run in QEMU (it starts Wi-Fi). This section is checked by reading only.

## 7. Emulation detection

`hw_emulated()`: the factory MAC in eFuse (`esp_efuse_mac_get_default`) reads all zero. QEMU's
ESP32-S3 has blank eFuses; a real chip always has a MAC (this brush's starts with the Oclean OUI e8:06:90:…). A read
error counts as "real hardware". The result is cached; `oem_glue_early_init()` evaluates it
before any other task or interrupt handler exists and logs it:

```
I (151) glue: emulated: yes (eFuse MAC 00:00:00:00:00:00)
```

On the brush the line must read `emulated: no (eFuse MAC <your eFuse MAC>)`.

## 8. Changes outside the glue

| File | Change | Why |
|---|---|---|
| `app_main.c` | calls `oem_glue_early_init()` after `weblog_init()` | lock for safe mode; emulation line in the log |
| `web_server.c` | guards of section 6; `net_activity()` helper, also called by `GET /`; `tz` in `GET` / `POST /api/config` (applied live through `oem_glue_set_tz()`, the reply carries the zone in effect) | safe mode; setup AP (section 5); time zone (section 3.1) |
| `metrics.c`, `metrics.h` | the snapshot copies `oem_glue_imu_temp()` instead of calling `oem_imu_temp()`; `metrics_fw_version()` | section 2: no bus traffic under the core lock in network tasks. (The first version had moved the read inside the core lock: it ran outside it before, from the httpd / MQTT / BLE tasks, on an SPI device that the main task also uses) |
| `mqtt_ha.c` | command guard; the periodic state publish moved from the esp_timer callback to a task `mqtt_state` (6144 bytes, priority 2) that the timer wakes; a mutex keeps a client restart from destroying the client under a publish; `device_obj()` takes `metrics_fw_version()` instead of a snapshot per discovery entity | section 2: the esp_timer task must not wait for the core lock, and the publish also writes to a socket; discovery no longer takes the core lock at all |
| `wifi_mgr.c`, `wifi_mgr.h` | safe-mode guard; stock "connected" effects on the first `STA_CONNECTED` only (`s_greeted`, cleared where new credentials are applied), the wake script off the charger only; `wifi_mgr_has_creds()`, `wifi_mgr_setup_ap_up()` (`s_ap_up` now volatile); `oem_net_activity()` when the setup AP comes up after failed attempts and on `WIFI_EVENT_AP_STACONNECTED`; `wifi_mgr_apply_sta()` sets `g_oem.sys[0x0c] = 2` | section 5; live `hal_wifi_has_ssid()`; the Wi-Fi light and the "not bound" icon follow credentials entered in the web UI without a restart |
| `ble_server.c`, `ble_server.h` | `ble_server_stop_adv()`; guards | pre-sleep hook; safe mode |
| `config_store.c`, `config_store.h` | `app_config_t.tz`, NVS key `tz`, default `APP_CONFIG_TZ_DEFAULT` = `UTC0` | time zone |
| `www/index.html`, `www/app.js`, `www/style.css` | Settings: "Clock" with the time-zone field and "Use this browser's zone" (`browserTz()`) | time zone |

`hw_motor.c` was not changed: the priority of its task is set from `brush_app_start()`.

The MQTT task, the BLE stop, the Wi-Fi event handling, the setup-AP rules and all guards could
not be run (no radios in QEMU): they compile, and were checked by reading. What the reconnect
and setup-AP rules do to the brush logic was run in the host simulation (section 9.5).

## 9. QEMU evidence

Image: `re/tools/uisim/mkqemu.py build FLASH.bin --res res_fake.bin --stub adc_hw_calibration`,
machine `esp32s3`, UART to a file. Time stamps are the guest's tick time in ms; the guest ran at
about real time.

**The emulator itself is not stable**: with its default multi-threaded TCG it dies with a host
SIGSEGV in generated code (`code_gen_buffer`, thread `mttcg_cpu_thread_fn`; seen with host gdb)
in roughly one boot of four, always in the first 0.4 s. The UART log then ends at one of two
places: after the "emulated" line (what follows in `app_main` is the boot guard, `nvs_flash_init`
and the otadata read, which maps flash), or after the partition list of `ui_res_init` (what
follows is `esp_partition_mmap` of the picture partition). Both are before the glue has created
anything, and with `-accel tcg,thread=single` 16 of 16 boots survived, against 7 of 12 without.
A run that dies this way (exit code 139) is simply started again; all runs below are with the
default (multi-threaded) emulator.

All logs in this section are from the image with the review fixes of section 12 (the runs of
the first version were repeated; the screens and times came out the same within 0.1 s). They
predate `CONFIG_PM_ENABLE` and the build without Bluetooth.

### 9.1 First boot, blank NVS (150 s)

```
I (151) glue: emulated: yes (eFuse MAC 00:00:00:00:00:00)
W (171) config: no saved config, using defaults
I (181) glue: time zone UTC0: local time is UTC+0:00
I (181) hw_power: not a deep sleep reset, gyro_wakeup_count 0
I (181) hw_charge: charger pins ready: charger absent, charging allowed
W (181) hw_lcd: emulated: no panel
I (201) ui_res: OEM picture partition 'res' @0x620000 size 0x800000 (mapped): pictures present
W (201) hw_motor: emulated: no I2S, the motor task only keeps time
I (201) oem: oem app boot, wake cause 0
I (221) glue: brush app running (wake cause 0, tasks on core 1, prio 3 / 2, motor stream prio 10); radios off (emulated)
I (221) app: oclean custom firmware up (radios off)
I (221) glue: ui: screen 70, frame 1 (stack left: main 5624, ui 5284)
I (2041) glue: ui: screen 71, frame 39 (stack left: main 5624, ui 3860)
I (3201) glue: ui: screen 72, frame 40 (stack left: main 5624, ui 3860)
I (5201) glue: ui: screen 73, frame 41 (stack left: main 5624, ui 3860)
I (15761) oem: screen off: wifi 0 ble 0
I (15961) oem: pm: batt 90 asleep 1 reset 0 force 0
I (15961) hw_power: pins: screen-off stage
I (15971) hw_power: brush_resume_sleep
I (42201) glue: screen off for 27 s: radios stay up until deep sleep
I (135761) oem: pm: batt 90 asleep 1 reset 0 force 1
W (135951) hw_power: emulated: deep sleep2 skipped
```

Boot animation 70 (38 frames in 1.8 s = 20 per second), pairing guide 71 / 72 / 73 from the
1 Hz sequencer, idle time-out 10 s after page 73 (the sequencer sets it), fade-out, screen-off
sequence 0.55 s later, second stage after 200 ms, `hal_net_sleep` after 27 one-second blocks,
BLE window 120 s (no SSID), deep sleep refused under emulation. The display line is the
emulation one: there is no panel in QEMU, `hw_display_blit` returns at once, so "frame" counts
frames composed and handed to the driver. (The 60 s idle time after boot is not seen on this
path: the pairing guide replaces it with 10 s. Section 9.2 shows the 30 s of the mode page.)

### 9.2 Second boot of the same flash (60 s), and a boot with a prepared NVS (110 s)

Second boot: the settings written by the first one are read back (first screen 84, not 70).

```
I (191) oem: oem app boot, wake cause 0
I (211) glue: ui: screen 84, frame 1 (stack left: main 5620, ui 5296)
I (2691) glue: ui: screen 84, frame 51 (stack left: main 5620, ui 3856)
I (6201) glue: ui: screen 80, frame 122 (stack left: main 5620, ui 3856)
I (31191) glue: ui: screen 80, frame 172 (stack left: main 5620, ui 3856)
I (36751) oem: screen off: wifi 0 ble 0
I (36951) hw_power: pins: screen-off stage
```

(Screen 84 draws 20 frames a second, the mode page 2: 50 frames in 25 s.)

NVS made with `nvs_partition_gen.py`: `oclean/wifi_ssid`, `oclean/tz` =
`<+01>-1<+02>,M3.5.0,M10.5.0/3` (what the web UI computes for central Europe),
`storage/sys_config` (boot stage 2, bound), `storage/brush_battery` = 70 % as a 30-byte blob
(stock: 20; exercises the partial read of `hal_nvs_get`):

```
I (163) config: config loaded (mqtt=off host=:1883)
I (173) glue: time zone <+01>-1<+02>,M3.5.0,M10.5.0/3: local time is UTC+1:00
I (193) oem: oem app boot, wake cause 0
I (233) glue: brush app running (wake cause 0, tasks on core 1, prio 3 / 2, motor stream prio 10); radios off (emulated)
I (233) glue: ui: screen 84, frame 1 (stack left: main 5624, ui 5188)
I (2693) glue: ui: screen 84, frame 51 (stack left: main 5624, ui 3860)
I (6233) glue: ui: screen 80, frame 122 (stack left: main 5624, ui 3860)
I (31193) glue: ui: screen 80, frame 172 (stack left: main 5624, ui 3860)
I (36783) oem: screen off: wifi 1 ble 0
I (36983) oem: pm: batt 70 asleep 1 reset 0 force 0
I (36983) hw_power: pins: screen-off stage
I (36993) hw_power: brush_resume_sleep
I (63233) glue: screen off for 27 s: radios stay up until deep sleep
I (66783) oem: pm: batt 70 asleep 1 reset 0 force 1
W (66973) hw_power: emulated: deep sleep2 skipped
I (97473) oem: pm: batt 70 asleep 1 reset 0 force 1
W (97663) hw_power: emulated: deep sleep2 skipped
```

This is the host simulation's `boot_idle_sleep` scenario (NOTES_app 7: mode page at 6.0,
sequence at 36.55, stage 2 at 36.75, Wi-Fi off 27 s later, deep sleep at 66.7), shifted by the
0.19 s at which `oem_app_boot` runs here: 6.23, 36.78, 36.98, 63.23 and 66.97 s, each within
0.1 s of the simulation (ticks merge during the boot, and a `hal_delay` is a tick longer here).
The refused deep sleep is tried again after another window (97.7 s), as `ble_timeout()` does.

Time zone, two short boots with other values of `oclean/tz` (the clock is not set in QEMU, so
the offset shown is that of 1 January 1970):

```
I (178) glue: time zone <-0330>3:30<-0230>,M3.2.0,M11.1.0: local time is UTC-3:30
W (174) glue: time zone 'Europe/Berlin' not set: not a POSIX TZ string
I (174) glue: time zone UTC0: local time is UTC+0:00
```

The first is Newfoundland as the web UI computes it, the other two lines are one boot with a
zone name in NVS: refused, the brush runs on UTC.

What this C library does with a TZ string was measured with a scratch image (a few lines added
to the scripted-input copy, not kept): `setenv` + `tzset`, then the offsets `localtime_r` gives
for 15 January and 15 July 2026 and for the second before and at 01:00 UTC on 29 March and
25 October 2026.

| TZ | January / July | Changes | Remark |
|---|---|---|---|
| `CET-1CEST,M3.5.0,M10.5.0/3` and `<+01>-1<+02>,M3.5.0,M10.5.0/3` | +60 / +120 | 29 Mar +60 > +120, 25 Oct +120 > +60 | both forms of the name work, the changes are exact |
| `<+00>0<+01>,M3.5.0/1,M10.5.0` | 0 / +60 | 29 Mar 0 > +60, 25 Oct +60 > 0 | |
| `<-0330>3:30<-0230>,M3.2.0,M11.1.0` | -210 / -150 | | |
| `<+1030>-10:30<+11>-11,M10.1.0,M4.1.0` | +660 / +630 | | southern hemisphere, half-hour summer time |
| `<-04>4<-03>,M9.1.0/0,M4.1.0/0` | -180 / -240 | | change at 00:00 |
| `<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45` | +825 / +765 | | |
| `IST-2IDT,M3.4.4/26,M10.5.0` | +120 / +180 | | an hour above 24 is accepted |
| `<+0530>-5:30` | +330 / +330 | | |
| `Europe/Berlin`, `garbage`, empty | as before | as before | nothing changes: the zone set before stays in force |
| `CET-1CEST,garbage` and `CET-1CEST` | +60 / +120 | none on those two days | US change dates |
| `CET-1CEST,M13.1.0,M10.5.0` | +60 / +60 | none | no summer time |

### 9.3 Scripted input

QEMU cannot press the button, so a second image was built from a copy of the same sources plus
`re/tools/uisim/qemu_inject.c` (use: see its header). It plays a script: button codes are
delivered as the button driver does it (`oem_button_push` + `hal_event_post`) **from an
interrupt** (the FreeRTOS tick hook on CPU 0), remote commands (`oem_remote_*`,
`oem_net_activity`) and UI messages from a task of its own, as the web server, MQTT and the
gesture decoder do. Prepared NVS as in 9.2. Shortened log (`inject:` lines are the script):

```
W (10151) inject: inject: short press on the mode page: start a session
I (10161) oem: short press: asleep 0 power 2 touch 5 screen 80
I (10161) oem: session: mode 5, 300 s, gear 1
I (10201) glue: ui: screen 87, frame 130 (stack left: main 5412, ui 3856)
W (14151) inject: inject: remote strength 4 (live in mode 5)
I (14151) oem: motor gear 24: 195 Hz duty 36 type 0x50
I (18251) glue: ui: screen 82, frame 149 (stack left: main 5412, ui 3856)
W (20151) inject: inject: short press: pause
I (20291) glue: ui: screen 101, frame 151 (stack left: main 5412, ui 3856)
W (23151) inject: inject: short press: resume
I (23221) glue: ui: screen 82, frame 158 (stack left: main 5412, ui 3856)
W (40151) inject: inject: remote stop
I (40171) oem: session end: 27 of 300 s, score 9
I (40291) glue: ui: screen 103, frame 176 ...        then 100 at 40551, 84 at 50561
W (45151) inject: inject: remote mode 3
I (60551) glue: ui: screen 76, frame 423 ...         (the mode-3 page)
W (64151) inject: inject: 2 s hold on the mode page: touch lock on
I (64161) oem: motor gear 53: 220 Hz duty 10 type 0x1f
I (64581) glue: ui: screen 91, frame 431 ...         popup, back to 76 at 65601
W (68151) inject: inject: swipe up while locked: popup          (91, back to 76 at 69201)
W (72151) inject: inject: 2 s hold: touch lock off
W (76151) inject: inject: swipe up: next mode page
I (76151) glue: ui: screen 78, frame 454 ...
I (86931) oem: screen off: wifi 1 ble 0
I (116931) oem: pm: batt 70 asleep 1 reset 0 force 1
W (117131) hw_power: emulated: deep sleep2 skipped
W (125151) inject: inject: remote start (screen off by now): wake + session
I (125151) hw_power: pins: awake
I (125271) oem: session: mode 3, 120 s, gear 50
W (140151) inject: inject: remote stop
I (140151) oem: session end: 14 of 120 s, score 11 (not counted)            then 103, 100, 84, 76
I (170881) oem: screen off: wifi 1 ble 0
W (185151) inject: inject: client activity in the screen-off stage: window restarts
W (190151) inject: inject: short press in the screen-off stage: wake
I (190161) oem: short press: asleep 1 power 2 touch 5 screen 76
I (190161) hw_power: pins: awake
I (190281) glue: ui: screen 84, frame 718 ...        then 76 at 195441
I (225991) oem: screen off: wifi 1 ble 0
W (256171) hw_power: emulated: deep sleep2 skipped
W (262151) inject: inject: 8 s hold: factory reset (button code 4 + UI event), the brush restarts
```

QEMU then leaves (`-no-reboot`, exit code 0): the restart of `hal_restart()`. The next boot of
that flash is the shipping state the reset leaves behind (boot stage 11): boot animation, 8 s,
button ignored (the script's presses have no effect), "deep sleep1" right in the second stage:

```
I (229) glue: ui: screen 70, frame 1 (stack left: main 5616, ui 5212)
I (8779) oem: screen off: wifi 1 ble 0
I (8979) oem: pm: batt 70 asleep 1 reset 1 force 0
W (9159) hw_power: emulated: deep sleep1 skipped
I (9669) hw_power: pins: screen-off stage
```

What this shows of the glue: an event posted from an interrupt on core 0 reaches the main task
on core 1 within 10 ms (through the raised timer service task); a UI event
posted from an interrupt reaches the UI task (factory reset); `hal_lock` from a third task;
`hal_timer_start` / `_stop` for FAST30, BLE_TIMEOUT, SLEEP200 and the TICK10 restart on wake;
`hal_delay` in the handlers (pause 100 ms, lock buzz 400 ms, deep sleep 100 + 50 ms: 170..200 ms
between the `pm` and the `deep sleep` line); `hal_net_wake`; `hal_nvs_set` + `hal_restart`; the
30 s window with an SSID stored. The 30 s check of the main loop never fired in any run.

### 9.4 Not shown by QEMU

Real pictures on a panel, LEDC, I2S, ADC, the bit-bang bus, the IMU; GPIO interrupts (nothing
drives the emulated pins in this setup: the interrupt path was exercised from the tick interrupt
instead); Wi-Fi, BLE, MQTT, the web server; the pre-sleep hook (the sleep is refused before it);
a real deep sleep and its wake causes; the boot-time re-sleep; safe mode; the main-loop check
firing; the motor stream under load (its task only keeps time here); the IMU temperature (the
emulated read returns "no reading" at once); frequency scaling (under emulation `pm_init()`
skips `esp_pm_configure`, NOTES_power section 5).

### 9.5 Host simulation of the Wi-Fi rules

`re/tools/uisim/sim_glue.c` (build line in its header) replays what `wifi_mgr.c` and
`hal_wifi_has_ssid()` do to the core, against the real `oem_app.c` / `oem_brush.c` in the
harness of `sim_app.c`. 8 scenarios, 33 checks, all pass (also with
`-fsanitize=address,undefined`); `sim_app` itself still passes its 30 scenarios.

| Scenario | Result (times in s after boot) |
|---|---|
| sta_reconnects | first `STA_CONNECTED` at 3, then a disconnect and a reconnect every 20 s: screen off at 36.55 and deep sleep at 66.7 as without any Wi-Fi event; no reconnect wakes the screen |
| sta_reconnects_old | counter-check, the first version's handler in the same situation: the screen never goes off, no deep sleep within 400 s |
| dock_late_connect | on the dock from 10, backlight off at 40, `STA_CONNECTED` at 100: no LED script, the backlight stays off |
| dock_late_connect_old | counter-check, first version: the wake script starts at 100 and nothing switches the backlight off in the next 300 s (the LED fake of the harness does not model levels; that script 0 fades the backlight in is NOTES_led section 3) |
| dock_boot_connect | boot on the dock, `STA_CONNECTED` at 2, before the charger is detected (3.0): the wake script starts and is aborted by the charger handling at 3.01, as in stock; backlight off at 33.0 |
| setup_ap | AP up at 50 (screen-off stage, 30 s window running): 120 s window from 50.0; a station joining at 120 and a page load at 150 restart it; deep sleep at 270.15; the screen stays off |
| setup_ap_unreported | counter-check, `hal_wifi_has_ssid()` false from 50 on but nothing reported: deep sleep at 66.7, on the window that was running |
| setup_ap_before_screen_off | AP up at 20, screen still on: the report does nothing; screen off at 36.55 with the 120 s window, deep sleep at 156.7 |

This tests the rules, not `wifi_mgr.c`: which Wi-Fi event leads to which call was checked by
reading.

## 10. Deviations from the brief / from stock

1. UI task priority 2 and both tasks pinned to core 1 (brief: both 3 or lower; stock: 3, no
   affinity). Section 2.
2. The FreeRTOS timer service task is raised to priority 22 at run time, so that
   `xEventGroupSetBitsFromISR` is prompt. The alternative is
   `CONFIG_FREERTOS_TIMER_TASK_PRIORITY`, but `sdkconfig` is not under version control.
3. `hal_delay` adds one tick (never shorter than asked).
4. `brush_app_start()` waits for `oem_app_boot()` (at most 3 s) before it returns.
5. The main-loop check (30 s without a pass: abort). Not in stock, not in the brief.
6. `oem_touch_init()` comes right after `oem_power_boot()`, before the LED and display init
   (NOTES_input: as early as possible); `oem_pressure_init()` is left to `oem_pressure_start()`.
7. `hal_net_sleep` / `hal_net_wake` change nothing (section 5).
8. Frame log: a line per screen change and after 50, 150, 350, 750 ... frames of one screen,
   instead of every 50th frame: the charging screen draws 12 frames a second while its backlight
   is on (nothing is drawn behind a dark one, NOTES_ui 3.10) and would otherwise push everything
   else out of the 16 KB web log.
9. The periodic MQTT publish runs in its own task (section 8).
10. `oem_glue_early_init()` sets the log level of the IDF tag `gpio` to WARN: the driver logs every
    `gpio_config()` at INFO, about 30 lines for one screen-off and wake, in a web log of 16 KB.
    The level switch of the web UI (it sets `*`) brings them back.
11. The motor stream task runs at priority 10, not stock's 3 (section 2).
12. The main task reads the IMU temperature every 5 s for the web / MQTT / BLE snapshot (3 ms of
    busy-wait, also during a session and in the screen-off stage). Stock has no such reader.
13. The stock "Wi-Fi connected" effects run once per boot and once per new set of credentials,
    not on every `STA_CONNECTED`, and the wake script among them only off the charger (stock:
    also on the charger). Section 5.
14. While the setup AP is up the brush counts as having no network (120 s window), and the AP
    coming up, a station joining it and a page load count as client activity (section 5).
15. `hal_time()` is local time from a configured POSIX TZ string (section 3.1); stock gets local
    time from the phone app.
16. Bluetooth is not part of the default build, and Wi-Fi stays on `WIFI_PS_MIN_MODEM` where
    stock sets `WIFI_PS_MAX_MODEM` (section 5).

## 11. Open points, and what to watch on the first boot

1. `glue: emulated: no (eFuse MAC <your eFuse MAC>)` must be the first line of the glue. "yes"
   on the brush would mean no drivers and no radios (it cannot happen with a programmed MAC).
2. `glue: brush app running (wake cause N, ...); radios allowed`, then `glue: ui: screen N,
   frame M (stack left: main X, ui Y)`. X and Y are the numbers to check: if either gets below
   about 1000, raise `MAIN_STACK` / `UI_STACK`.
3. `glue: events from an interrupt posted late`: the timer service queue was full; should never
   appear.
4. A restart with the panic reason `glue: the brush_app task made no pass for 30 s` is the
   main-loop check: the main task hung or was starved. Tell which screen / action preceded it.
5. `E gpio: gpio_install_isr_service(...): GPIO isr service already installed` appears two or
   three times at boot (hw_charge.c, hw_touch.c, hw_button.c call it after hw_power.c did). The
   drivers handle the return code; the line is IDF's and harmless.
6. **Time zone: set it once.** A brush updated from an earlier build has no `tz` and runs on
   `UTC0` until the Settings page is opened and saved (the page fills the field from the
   browser). The log line `glue: time zone ...: local time is UTC+h:mm` at boot shows what is
   in effect; until then the day totals reset at UTC midnight and the morning window is shifted.
   The string holds the change rules, not a zone name: it stays right as long as the zone's
   rules do. Zones whose changes are not "n-th weekday of a month" (section 3.1) need the
   proper string typed in, e.g. `IST-2IDT,M3.4.4/26,M10.5.0`.
7. **No sign on the brush when Wi-Fi connects on the dock.** The wake script that stock plays
   at "Wi-Fi connected" is skipped on the charger (section 5), and the Wi-Fi light is off there
   by stock's own rule. Credentials saved while the brush is docked therefore show only in the
   web UI (the setup AP disappears when the brush has its address). Off the charger the first
   connection, and the first one after new credentials, plays the script and wakes the screen.
8. **Setup AP on battery.** The AP lives for 120 s after it came up, after a station joined and
   after each load of the page. Somebody who needs longer than that between loading the page
   and pressing "Save" loses the AP (the brush goes to deep sleep; a button press brings it
   back, about 50 s later with stored credentials, at once without). A brush away from its
   network pays about 100 s of Wi-Fi per wake for the AP (section 5).
9. The MQTT broker learns of a deep sleep only through the last will.
10. Info page 0 shows "V 0.0.0.0" until the project has a version of the form a.b.c.d
    (`PROJECT_VER` or `version.txt`).
11. Without the OEM picture partition the screens are black (the glue logs a warning);
    `ui_res.c` still speaks of a "built-in screen" that the port does not have.
12. Wake-up takes longer than on stock: the panel is initialised twice (main task, then UI
    message 0), each 0.15..0.3 s (depending on the panel table) with the core lock held, where
    stock's two tasks overlap.
13. `g_oem.clock_mode`, `cloud_state` and `wifi_weak` are not maintained by the glue: side page
    96 shows the stock "no data" pictures. `clock_mode = 2` after the first SNTP sync would show
    a clock (in local time now, see point 6).
14. `hw_motor: N DMA buffers went out empty since the last start` in the log (at a gear change)
    would mean the stream still gets gaps although its task now runs at priority 10: note what
    the brush was doing (Wi-Fi connect, MQTT, update).
15. The IMU temperature in the web UI / MQTT (`imu_temp`) is the main task's reading, up to 5 s
    old; `null` means the IMU did not answer.
16. `glue: brush app running (... motor stream prio 10)`: "prio -1" would mean that `hw_motor.c`
    could not create its task (it logs `no motor task`).
17. `ble_srv: Bluetooth is not part of this build` is the stub of `ble_server_start()` (section
    5), not a fault.
18. On the dock the `glue: ui: screen 93, frame N` lines stop when the backlight goes off, 30 s
    after docking: nothing is drawn behind a dark backlight (NOTES_ui 3.10), so the frame count
    stands still. A button press brings the light and the frames back for another 30 s.

## 12. Review of the first version: findings and fixes

Six findings, all confirmed and fixed. None of the fixes touches a core file (`oem_*.c`) or a
driver (`hw_*.c`).

| Finding | Fix | Checked by |
|---|---|---|
| **Motor stream starved** (medium). `brush_music` (priority 3, core 0) below the MQTT task (5): Home Assistant discovery took a brush-state snapshot per entity, 37 in a row, each with 3 ms of IMU busy-wait under the core lock; the stream could miss its 37.5 ms. | Stream task at priority 10 (`brush_app_start`); `device_obj()` reads only the version (`metrics_fw_version()`); the snapshot copies an IMU temperature that the main task reads every 5 s (`oem_glue_imu_temp()`). Sections 2, 8. | build; QEMU log line `motor stream prio 10`; reading. The stream itself needs the brush (open point 14). |
| **`hal_wifi_has_ssid()` a boot-time snapshot** (low), and **the "bound" byte not updated** (low): credentials entered in the web UI counted from the next boot only (120 s instead of 30 s window, Wi-Fi light dark, "not bound" icon). | `hal_wifi_has_ssid()` also asks `wifi_mgr_has_creds()`; `wifi_mgr_apply_sta()` sets `g_oem.sys[0x0c] = 2`. Section 3. | build; reading. |
| **`hal_time()` in UTC** (low): day totals, daily-goal chase, morning window, greeting dates and records shifted by the local offset. | Time zone as a POSIX TZ string in the configuration, applied at boot and on change; web UI field filled from the browser. Section 3.1. | QEMU (section 9.2: zone applied before `oem_app_boot`, a zone name refused, the library's parsing measured); node (`browserTz()` against tzdata, section 3.1). |
| **Every Wi-Fi reconnect replayed the stock "connected" effects** (high): screen stuck on at the dock, no deep sleep on a link that keeps coming back. | Effects on the first `STA_CONNECTED` since boot / since new credentials only; the wake script off the charger only. Section 5. | host simulation (9.5: with and without the fix); reading. |
| **Setup AP unusable on battery** (medium): after a password change the AP was up for 10..20 s per wake before deep sleep. | While the AP is up the brush counts as unconfigured (120 s window); the AP coming up, a station joining and a page load restart the window. Section 5. | host simulation (9.5); reading. |

Three things go beyond what the review proposed:

* The wake script at "Wi-Fi connected" is not played on the charger. Limiting the effects to
  the first connection ends the reported case (a reconnect on the dock), but a first connection
  can also come after the backlight time-out there, with the same result: a network that was
  down at boot, or new credentials saved while the brush is docked. Price: no sign on the brush
  for that (open point 7).
* The window is also restarted when the setup AP comes up. The proposal was the changed
  `hal_wifi_has_ssid()`, the station-joined event and the page load; but the AP comes up about
  50 s after the wake, when the 30 s window is already running, and the new answer of
  `hal_wifi_has_ssid()` is only asked for when a window starts. A phone would have had to join
  within the remaining 10..20 s (scenario `setup_ap_unreported`).
* `oem_glue_set_tz()` checks the start of the string and the web server refuses what fails
  the check, because the C library accepts a zone name such as `Europe/Berlin` without
  complaint and without effect.

One thing is narrower than a reader might expect: `s_greeted` is cleared where the new
credentials are applied (`retry_cb`, one second after the settings were saved), not in
`wifi_mgr_apply_sta()` itself, so that a reconnect of the old link in that second cannot use up
the one greeting.

# NOTES: power + IMU module (agent P)

Spec: `power.md` §3, §4.1, §5, §6.1 and `led_battery_charge.md` §7. Checked against the stock
code `0x4200c034`, `0x4200d77c`, `0x4200dbbc`, `0x4200dda4`, `0x4200df88`, `0x4200ccc0`,
`0x4200cd54`, `0x4200cfb8`, `0x4200cfd4`, `0x40377c98`, `0x42013fd0`..`0x42014240`,
`0x4201b6dc`..`0x4201b740`, `0x4201e0c4`..`0x4201e74c`.

## 1. What is implemented

| File | Content |
|---|---|
| `main/hw_power.c` | `oem_power_boot`, `oem_power_prep_screen_off`, `oem_power_restore_after_screen_off`, `oem_power_deep_sleep`, `oem_power_stay_alive`, `oem_power_resume_sleep`, `oem_power_cpu_lock`, `oem_motion_irq`; `hal_anymotion_allowed`; `hw_power_rtc`; `hw_power_set_pre_sleep_hook`; `hw_power_apb_held`, `hw_power_cpu_locks` (section 5) |
| `main/hw_power.h` | `hw_power_set_pre_sleep_hook(void (*fn)(void))`, `oem_rtc_t *hw_power_rtc(void)`, `bool hw_power_apb_held(void)`, `int hw_power_cpu_locks(void)` |
| `main/hw_imu.c` | `oem_imu_normal`, `oem_imu_amd`, `oem_imu_power_down`, `oem_imu_temp` (QMI8658 on SPI3; the bus is set up on first use) |

Header additions (my blocks only):
* `oem_state.h`: `g_oem.motion_gate` (stock 0x3fc9f307). **Written by the app module**, read by
  the GPIO8 ISR: non-zero = do not post `OEM_EV_MOTION`. Stock writers: event 0x100000 handler
  `= 0`; event 0x02 handler `= hal_anymotion_allowed()` and `= 1` after a wake; `motorwakeup`
  `= 1`. If the app never writes it, it stays 0 and the ISR posts on every motion edge while it
  is installed (the handler's own `wake_gate` test still applies).
* `oem_hal.h`: `bool hal_anymotion_allowed(void)` — stock `allow_anymotion_check_flag`
  (0x3fca2885). It is **implemented in `hw_power.c`**, not in the glue; a host build of a core
  file that calls it needs a fake.
* `oem_api.h` Requests block: nothing added.

Other modules' functions used: `oem_charger_present()`, `oem_batt_mv_now()` and
`OEM_BATT_MV_MIN_VALID` (boot checks, deep-sleep guard), `oem_charge_allow(true)` (GPIO26 row
of the pin tables), `oem_led_park()` (LED rows of the pin tables), `oem_led_init()` (inside the
wake restore, where stock calls `led_init`), `boot_guard_clean_exit()`, `hal_event_post()`.

Not tested on a host (nothing platform independent here). Checked with
`re/tools/esp_syntax.sh main/hw_power.c main/hw_imu.c` (clean), and `hw_power.c` once more with
`-DCONFIG_PM_ENABLE=1` and with `-DCONFIG_PM_ENABLE=1 -DCONFIG_FREERTOS_USE_TICKLESS_IDLE=1`
(clean). Register sequences and pin tables were re-read against the spec and the decompilation
row by row.

## 2. What the integrator has to do

Build: add `hw_power.c` and `hw_imu.c` to `main/CMakeLists.txt`; remove `hw_i2c.c` (it also
initialises SPI3; both in one image = the second `spi_bus_initialize` fails). `esp_pm.h` was
found with the current include paths; add `esp_pm` to `REQUIRES` if the build says otherwise.

Glue: `oem_rtc_t *hal_rtc(void) { return hw_power_rtc(); }`.

### 2.1 Boot (every reset, including a wake from deep sleep, which is a full reboot)

```
boot_guard_check();
nvs_flash_init();                         // before oem_power_boot (NVS copy of gyro_wakeup_count)
hw_power_set_pre_sleep_hook(stop_radios); // any time before the radios start
cause = oem_power_boot();                 // may not return (boot-time re-sleep)
oem_led_init();                           // LEDC; before or after oem_power_boot, both work
hw_display_init(); bus / touch / pressure init   // AFTER oem_power_boot
button / motor / battery / charge init           // any order
... glue, oem_app_boot(cause)
```

`oem_power_boot()` does, in this order: wake cause; `gpio_hold_dis` on 13, 14, 21, 42, 41, 37,
17, 18, 19, 20, 12, 3; ISR service; GPIO3 input + pull-up (direction and pull only); GPIO8
input; GPIO10 input; `oem_imu_normal()`; the re-sleep checks; GPIO37 = 0; PM locks. Through
`oem_charger_present()` it also triggers `oem_charge_pins_init()` (GPIO45, GPIO26, GPIO9,
GPIO2), which is where stock does that too. It needs the scheduler (it is called from
`app_main`) and nothing from the glue.

Who releases the sleep holds after a deep-sleep wake:

| Pins | Released by |
|---|---|
| 13, 14, 21, 42, 41, 37, 17..20, 12, 3 | `oem_power_boot()` |
| 4, 5, 6, 7 (IMU SPI) | `hw_imu.c`, when the bus is set up (first `oem_imu_*` call) |
| 38, 39, 40 (LCD SPI) | display driver (`hw_display_init` already does it) |
| 26 | `oem_charge_allow()` (un-hold, set, hold again) |
| 45 | stays held low (stock never releases it) |

Drivers that talk to their hardware while initialising must come after `oem_power_boot()`:
display (before it GPIO37 is still high / floating and 41, 42 may still be held) and bus /
touch / pressure (12, 13, 14 may still be held). LED, charge, battery and button drivers may
come before or after.

Safe mode (`boot_guard_mode() == BOOT_SAFE`): `oem_power_boot()` does not look at it. If the
integrator calls it in safe mode, the empty-battery check can still put the brush to sleep.

### 2.2 Screen-off stage and wake from it

Sleep (stock event 0x100000 handler, 0x4201d2a8), the calls of this module in **bold**:

```
(brush_pm_control(0))
**oem_power_prep_screen_off();**         // includes oem_led_park() and oem_charge_allow(true)
if (power_state != OEM_PWR_BATTERY) oem_led_reinit_charge_light();
**oem_power_resume_sleep();**
g_oem.motion_gate = 0;
**oem_motion_irq(true);**
GPIO45 = 0 (oem_wlc_off or nothing: the parking already wrote it)
```

Wake (stock `motorwakeup` 0x4201bd70):

```
**oem_power_stay_alive();**              // also clears g_oem.asleep, as stock
g_oem.motion_gate = 1;
**oem_power_restore_after_screen_off();**   // includes oem_led_init() and oem_charge_allow(true)
hal_delay(100);
**oem_imu_normal();**
hal_lcd_init();                          // display: reset + init table (GPIO42 / 41 are free again, GPIO37 is low)
pressure start; oem_touch_irq(true); oem_touch_set_state(0); oem_touch_set_state(0x21);
oem_led_set(...); ... ; **oem_motion_irq(false);** ...
```

Re-initialisation that must follow `oem_power_restore_after_screen_off()`:

| What | Why | Call |
|---|---|---|
| LEDC (GPIO17..21) | the parking made them plain held outputs | **done inside the restore** (`oem_led_init()`, stock position of `led_init`). Calling it again afterwards is harmless. |
| LCD panel | RST / DC were held low, GPIO37 was high | `hal_lcd_init()` (stock `lcd_init` 0x42029120), 100 ms later as stock |
| Touch RDY interrupt | GPIO12 was "disabled, low, held"; the restore makes it input + pull-up, falling edge | `oem_touch_irq(true)` and the touch state calls |
| Pressure sensor / bus | the restore only un-holds GPIO13 / 14 (the tables never park them) | pressure start |
| IMU | was in any-motion mode | `oem_imu_normal()` |
| LCD SPI, IMU SPI | untouched by the screen-off parking | nothing |

### 2.3 Deep sleep

`brush_pm_control` (app) keeps its own steps: `oem_imu_power_down()` when motion wake is off,
100 ms, `oem_gauge_save()`, 50 ms, then **`oem_power_deep_sleep(motion_wake)`**, then 500 ms.
Inside `oem_power_deep_sleep`, in this order:

1. refuse (log, return) under QEMU, when the charger is present, or when the EXT1 wake cannot
   be armed; EXT1 = GPIO3 | GPIO9 "any low", plus EXT0 = GPIO8 high when `motion_wake`;
2. the pre-sleep hook (stop Wi-Fi / BLE; stock stops BLE advertising at this point);
3. USB D+ pull-up off (first line of stock `brush_pm_control`);
4. NVS copy of `gyro_wakeup_count` updated if it differs;
5. pin parking 0x4200dda4;
6. timer wake disabled, RTC peripherals kept on, `boot_guard_clean_exit()`,
   `esp_deep_sleep_start()`.

**The function returns when the sleep was refused**; the caller must cope (the brush simply
stays in the screen-off stage).

The pre-sleep hook runs in the main task with the core lock held. It must not wait for a task
that can be blocked on `hal_lock` (a web / MQTT / BLE handler in the middle of an
`oem_remote_*` call), or the brush hangs awake.

## 3. Deviations from the spec / stock, and why

1. **Deep sleep is refused** on the charger (pin level, not `power_state`), under QEMU and
   when EXT1 cannot be armed. For that the wake sources are armed before the pin parking
   (stock: after). Reason: no way to recover a brush that sleeps without a button wake.
2. **Battery check at boot**: a reading below `OEM_BATT_MV_MIN_VALID` (2000 mV, defined by
   the gauge module; includes 0 = ADC failure) counts as "no reading" and the boot continues.
   Stock would go to sleep on 0. The reading is `oem_batt_mv_now()` (two conversions
   averaged, `adc_cali` curve fit) where stock uses one conversion with the legacy line fit,
   so 3299 mV is a few mV off the stock threshold.
3. **`key_press_last`** (0x3fc9f301, set by stock's GPIO3 / GPIO9 ISR when the line is low):
   replaced by sampling GPIO3 and `oem_charger_present()` at the moment of the motion-wake
   check. A press that begins and ends inside the first ~50 ms of the boot is missed.
4. **Any-motion window**: stock sets a flag and restarts a 5 s esp_timer from the ISR; here
   the ISR stores the tick count and `hal_anymotion_allowed()` compares. Same result, no
   esp_timer call from interrupt context.
5. **`gyro_wakeup_count` in NVS** ("wakeupcount", 10 bytes, byte 0): stock reads it on every
   boot and writes it on every change. Here it is read only when the RTC copy did not survive
   (reset that is not a deep-sleep wake), written on the changes made in this module (button
   wake: 0; accepted motion wake: +1) and brought up to date before every deep sleep. So the
   app module may change `hal_rtc()->gyro_wakeup_count` without writing NVS itself.
6. **Boot GPIO setup** is split by owner. `oem_power_boot()` only releases the holds and
   configures GPIO8, GPIO10, GPIO37 (and GPIO3 as readable input). Consequences: the LED pins
   are not driven between the hold release and `oem_led_init()` (stock: LEDC runs before the
   checks) — they float, which is the dark state; GPIO37 goes low before the display bus
   exists (stock: after), the panel is reset by `hal_lcd_init()` later in both cases.
7. **GPIO8 interrupt**: stock sets any-edge and adds the handler at boot, the main task
   removes it again. Here the pin is a plain input at boot and `oem_motion_irq(true)` sets
   the edge type and adds the handler. `oem_motion_irq(false)` at boot is harmless.
8. **Pin tables**: row 6 (remove the I2C0 driver, reset GPIO36 / 35) is left out — the port has
   no I2C0 driver and never touches those pins. Rows 1-5 and 17 are done by `oem_led_park()`,
   so the backlight pin is parked at the start instead of the end.
9. **PM locks**: created and taken before `esp_pm_configure` (stock: configure first), so
   automatic light sleep can never start; `light_sleep_enable` is only set when tickless idle
   is configured (IDF rejects it otherwise). An unbalanced `oem_power_cpu_lock(false)` is
   ignored with a warning (stock aborts through `ESP_ERROR_CHECK`).
10. **IMU**: read framing `reg | 0x80` in one transfer (the one that works on the device; open
    question 6 of the spec). As in stock every register is read in its own transfer with its
    own address and 1 ms before it (busy-wait). `oem_imu_temp()` reads WHO_AM_I first and
    returns false when it is not 0x05 (stock returns whatever it reads). The stock gate
    "sensor type == 4" is dropped (it is a constant).
11. **Not ported** (stock `app_main`, outside this module's list): brown-out detector off
    (`RTC_CNTL_BROWN_OUT_REG = 0`); "TimeRTC" save / restore; `load_daily_totals`;
    `load_sys_config`; the diagnostic counter 0x50001000. The brown-out detector therefore
    stays as the project configures it (enabled, level 7). If the brush resets when the motor
    starts on a weak battery, that line is the difference to stock.
12. The 10 ms wait after "charger present" in the boot check is left out (`oem_batt_mv_now()`
    already takes longer).

## 4. Found in the stock code (not in the spec)

* The two "loops" of the boot check leave on their first pass (both `deep_sleep1` calls do not
  return), so the check is: charger -> boot; battery <= 3299 -> sleep; not a motion wake ->
  boot; 10 ms; button or charger -> boot; window open and count <= 4 -> count++ (below 20),
  boot; else sleep.
* The boot-time re-sleep does **not** park the pins (no 0x4200dda4): it is
  `qmi_amd_mode(); deep_sleep1();` with the holds already released. Ported like that.
* IMU SPI init 0x4200ccc0 starts with `gpio_hold_dis` on 7, 6, 5, 4 (needed after 0x4200cd54).
* `qmi8658_config_reg` passes 0 for the low-pass and self-test arguments, and
  `0x4201e598` is called with (1, 1, 1) (disassembly 0x4201e6dc, 0x4201e3c4): the spec's
  register bytes are right.
* `gpio_force_unhold_all()` (IDF 5.1.1): sets DG_PAD_FORCE_UNHOLD for the digital pads and
  clears only the "force hold all" bit for the RTC pads; the per-pin holds of GPIO12 and
  17..21 stay. FORCE_UNHOLD is not cleared again until the next reset, so after one
  screen-off stage the per-pin holds on digital pads (26, 37..42, 45, 48) have no effect.
* `gpio_config()` of this IDF routes the pad back to the GPIO output register
  (`SIG_GPIO_OUT_IDX`) and the GPIO function, so the levels written to pins that belong to
  LEDC / SPI do reach the pad (spec open question 4).
* RTC data: the bootloader reloads `.rtc.data` on every reset that is not a deep-sleep wake.
  The comment in `oem_state.h` ("survive ... software resets") does not hold for an
  `RTC_DATA_ATTR` variable — nor for stock, which is why stock mirrors these values in NVS.
  After `esp_restart()` `hal_rtc()` holds the initial values (ota_oneshot 1, strength 3,
  hist_day 0xff, the rest 0) and the count from NVS.

## 5. CONFIG_PM_ENABLE

Set in `sdkconfig.defaults`, with tickless idle left off: `pm_init()` creates the three locks and
calls `esp_pm_configure` with stock's 160 / 40 MHz, and `oem_power_stay_alive / resume_sleep /
cpu_lock` take and release real locks. (Without it they keep their bookkeeping and the D+
pull-up write and do nothing else; the CPU stays at 160 MHz.)

What that gives in this build:

* IDF holds 160 MHz for every core that is not idle, so tasks and interrupt handlers run as
  before, the bit-bang bus included.
* With both cores idle the CPU drops to 80 MHz for as long as an APB lock is held; the APB clock
  is 80 MHz at both speeds. Our APB lock is held while awake (also on the charger) and dropped
  in the screen-off stage, as in stock.
* Stock's 40 MHz in the screen-off stage is not reached: the motor's I2S channel is enabled
  from boot on, and the IDF std driver holds an APB lock of its own for as long as a channel is
  enabled (stock's legacy driver only inside `i2s_write`). `hw_motor.c` was deliberately left
  as it is. Wi-Fi holds one more APB lock while its modem is awake.
* The CPU lock holds 160 MHz while brushing. The web OTA takes no PM lock (stock holds the CPU
  lock during its downloads): the task that receives and writes the image runs at 160 MHz like
  every other.
* Light sleep cannot start: without tickless idle IDF has no automatic light sleep, and the
  NO_LIGHT_SLEEP lock is never released.
* Under emulation `pm_init()` creates and takes the locks but skips `esp_pm_configure` (QEMU
  does not model the clock tree). Safe mode never calls `pm_init()` and runs at a fixed
  160 MHz.

With the APB clock at 80 MHz throughout, LEDC keeps its timing (charge light during the
screen-off stage on the charger, led spec open question 7).

`hw_power_apb_held()` and `hw_power_cpu_locks()` return the bookkeeping, and `/api/status`
shows it in `diag`: `pm_apb_lock` (false in the screen-off stage and in safe mode) and
`pm_cpu_locks` (1 while brushing, else 0). They say what this module has asked for, not which
frequency the chip runs at.

## 6. Unverified on hardware — watch on the first boots

1. Log at boot: `hw_power: not a deep sleep reset, gyro_wakeup_count N`, then
   `hw_imu: QMI8658 rev ...` and `ctrl1..8: 60 06 76 .. 00 .. 03 c0` (CTRL1, 2, 3, 5, 7, 8).
   `qmi8658_init fail` = the IMU does not answer; motion wake will not work.
2. `battery empty (... mV <= 3299 mV) and no charger: back to deep sleep` with a charged
   battery means the ADC reads low: the brush then only starts on the charger. Putting it on
   the charger always gets it out (GPIO9 low skips the check).
3. First deep sleep (idle, screen off, then 30 s / 120 s): last log line
   `deep sleep2 (wake: button, charger, motion)`. Then check each wake: button
   (`EXT1 GPIO 3 (button)`), lifting the brush (`EXT0 GPIO8 (motion)`), charger
   (`EXT1 GPIO 9 (charger)`).
4. If it comes straight back with `EXT1 GPIO 3` or `GPIO 9` without anyone touching it: the
   pins have no pull-up in deep sleep (spec open question 5; stock enables none). Remedy:
   `rtc_gpio_pullup_en()` on GPIO3 / GPIO9 before sleeping.
5. If it comes straight back with `EXT0 GPIO8`: INT1 was still high when the sleep started
   (spec open question 2). The counter limits this to 5 wakes, then
   `motion wake over the limit` and `deep sleep1`; a button press re-enables motion wake.
6. `charger present: deep sleep refused` means the app asked for deep sleep on the charger
   (it should not); the brush stays awake.
7. Current in deep sleep and whether anything stays lit: the levels of the digital pads
   (GPIO37, 41, 42, 48, LCD and IMU SPI pins) during deep sleep depend on holds that IDF does
   not keep without `gpio_deep_sleep_hold_en()`; stock does not call it and neither does the
   port (spec open question 3).
8. After a boot-time re-sleep (empty battery, motion wake refused) the pins are not parked,
   as in stock: GPIO37 and the LED pins float during that sleep.
9. `oem_imu_temp()` after `oem_imu_power_down()` / in any-motion mode: whether the
   temperature register still updates is unknown (stock only uses it on the charger, where
   the IMU is in normal mode).
10. The CTRL9 handshake polls STATUSINT up to 100 times (2 ms each) for "done" and again for
    "acknowledged". With an IMU that does not answer one of the two waits runs out, so
    `oem_imu_amd()` busy-waits about 0.4 s in the main task (two commands).
11. Frequency scaling (section 5): once at boot `esp_pm_configure` logs
    `pm: Frequency switching config: CPU_MAX: 160, APB_MAX: 80, APB_MIN: 40, Light sleep: DISABLED`.
    `hw_power: esp_pm_configure: N` instead means the call failed; the CPU then stays at
    160 MHz. `pm_apb_lock` in `/api/status` must be true while awake, also on the dock.

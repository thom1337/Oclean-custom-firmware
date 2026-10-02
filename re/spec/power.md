# Power management of the stock Oclean X Ultra 20 firmware (auto on / auto off, sleep, wake, IMU)

Source: stock image `blufixx_9_V2` (ESP-IDF v5.1.1). Every item carries the address it was read
from. **CONFIRMED** = read from the disassembly/decompilation or from the data segments.
**INFERRED** = a conclusion drawn from confirmed facts (reasoning given). Stock tick is 1000 Hz, so
`vTaskDelay(n)` = n ms.

Stock source-file names seen in assert strings: `ocleanbrush_app/system/src/brush_pm_control.c`,
`ocleanbrush_app/brush_task.c`, `ocleanhal/platform_devices.c`, `ulp_riscv_blufi_example_main.c`.

---

## 0. Corrections to the earlier leads (read this first)

| # | Earlier lead (CONTEXT.md / HARDWARE_MAP.md) | What the code does | Evidence |
|---|---|---|---|
| 1 | GPIO8 = charger detect (active high), GPIO9 = IMU motion INT | **GPIO8 = QMI8658 INT1 = any-motion interrupt, active HIGH (EXT0 wake, level 1). GPIO9 = charger / external power present, active LOW (EXT1 wake together with the button).** | EXT0 wake branch in `app_main` calls the "count_wakeup_gyro" logger 0x4200ca64 and sets the "woken by motion" flag (0x4200c0ca..0x4200c0ea); GPIO8 ISR branch arms the `anymotion_timeout` timer (0x40377d0c); `0x4201840c` calls `0x42017a0c(0)` ("USB_IN_ACTION") when `gpio_get_level(9)==0`; QMI8658 any-motion is routed to INT1 (section 5). CONFIRMED. The custom firmware's `HW_BTN_CHARGE_DET 8` / `HW_BTN_GYRO_WAKE 9` and `hw_charger_present()` (`GPIO8 == 1`) are therefore wrong: charger present is `gpio_get_level(9) == 0`. (The input-subsystem spec `input.md` reached the same conclusion independently.) |
| 2 | "deep sleep1" enables EXT1, "deep sleep2" is the ship mode without EXT1 | Both enable EXT1 (GPIO3\|GPIO9). **"deep sleep2" additionally enables EXT0 on GPIO8 (motion wake); "deep sleep1" does not** (no motion wake). The normal idle sleep is "deep sleep2". | 0x420141cc / 0x420141f8 / 0x42014220 / 0x42014240. CONFIRMED |
| 3 | `brush_resume_sleep` "masks the GPIO9 interrupt" (`0x60038018 &= ~bit9`) | 0x60038018 is `USB_SERIAL_JTAG_CONF0_REG`; bit 9 is `USB_SERIAL_JTAG_DP_PULLUP`. The code turns off the USB D+ pull-up (GPIO20 is an LED on this board). | 0x42014134, 0x4201c5e1; IDF `usb_serial_jtag_reg.h`. CONFIRMED |
| 4 | "acquire/release ls+cpu locks around active work", auto light sleep | The `ESP_PM_NO_LIGHT_SLEEP` lock is acquired once at boot and never released, so **automatic light sleep is never entered**. The only low-power states are "screen off, CPU still running (DFS)" and deep sleep. | 0x42014000 (only two references to the lock handle 0x3fca41ac in the whole image). CONFIRMED |
| 5 | gyro_wakeup_count "cap ~19, >=5 re-sleep" | limit test is `count > 4`; increments while `count < 20`; persisted in NVS as well as in RTC memory. | 0x4201b6f0, 0x4201b714, 0x4201b740. CONFIRMED |
| 6 | TIMER wake = housekeeping | Timer wake is explicitly disabled before every deep sleep; the TIMER branch in `app_main` only prints. | `esp_sleep_disable_wakeup_source(4)` at 0x42014200 / 0x42014248. CONFIRMED |

---

## 1. Names, flags and globals used below

Function names in quotes are stock names recovered from assert/log strings; the others are mine.

| Address | Name | Role |
|---|---|---|
| 0x4200c034 | `app_main` | boot / wake dispatch |
| 0x4200d77c | "brush_gpio_cfg" | GPIO init, **contains the boot-time "go straight back to sleep" checks** |
| 0x42013fd0 | "enter_auto_light_sleep" | `esp_pm_configure` |
| 0x42014000 | "brush_pm_init" | PM locks + `sleeo_timer` |
| 0x420140f4 | "brush_stay_alive" | take APB lock, clear `asleep` |
| 0x42014134 | "brush_resume_sleep" | release APB lock |
| 0x42014184 / 0x420141a8 | "enable_light_sleep_cpulock" / "close_light_sleep_cpulock" | take / release CPU_FREQ_MAX lock |
| 0x420141cc + 0x420141f8 | wake config + "deep sleep2" | EXT1 + EXT0, deep sleep |
| 0x42014220 + 0x42014240 | wake config + "deep sleep1" | EXT1 only, deep sleep |
| 0x42014268 | sleep_monitor_off | `enabled = 0` |
| 0x42014278 | "open_sleep_monitoring"(n) | idle timeout := n seconds from now |
| 0x42014298 | sleep_monitor_kick | restart the countdown, same n |
| 0x420142d0 | sleep_monitor_check | 1 Hz check, returns 1 when it is time to sleep |
| 0x42014374 | "close_components_and_parts" | start `sleeo_timer` 200 ms |
| 0x4201b764 | "brush_work_ooer_to_sleep" | screen-off sequence |
| 0x4201b900 | `sleep_brush_task` | one-shot task that runs the screen-off sequence |
| 0x4201bd70 | "motorwakeup" | wake from the screen-off stage (battery) |
| 0x4201bee8 | "motorwakeup_for_charge" | same, on the charger |
| 0x4201c5d0 | `brush_pm_control(forcesleep)` | deep-sleep decision + entry |
| 0x4200dbbc | gpio_prep_screen_off | GPIO state for the screen-off stage |
| 0x4200dda4 | gpio_prep_deep_sleep | GPIO state right before deep sleep |
| 0x4200df88 | gpio_restore_after_screen_off | undo of 0x4200dbbc |
| 0x4201b92c / 0x4201b9b0 | "ble_timeout_in_sleep" / "ble_timeout_in_sleep_data" | restart the BLE window timer |
| 0x4201cc40 | "pxp_reporter_task" (task name `brush_app`) | main task and event loop |
| 0x4201e730 | qmi8658 normal init | section 5 |
| 0x4201e74c | qmi8658 any-motion mode ("QMI8658 AMD MODE3/4") | section 5 |
| 0x4201e6f0 | qmi8658 reset + power down | section 5 |

RAM flags (all CONFIRMED by the listed readers/writers):

| Address | Name used here | Meaning |
|---|---|---|
| 0x3fca41a4 | `asleep` (stock getter prints it as `motor_wakeup_flag`, 0x420143b0) | 1 = screen-off stage entered (a wake-up is needed), 0 = awake |
| 0x3fc9a71a / 0x3fc9a71b | monitor `enabled` / `timeout_s` | .data initial values 1 / 30 |
| 0x3fca41a0 | monitor `start_s` | seconds since boot (`xTaskGetTickCount()/1000`, 0x4201b5e8) |
| 0x3fca4198 | monitor busy marker | 1 while the low-battery step runs, a button press turns it into 2 = abort |
| 0x3fca41b4 | `stay_alive` | 1 = APB lock held |
| 0x3fca4b6c+8 (0x3fca4b74) | `chg` | 2 = on battery, 1 = charging, 3 = full on charger |
| 0x3fca4b6c+0xe (0x3fca4b7a) | `battery_cap` | percent |
| 0x3fc9a69e | `cfg[]` | config struct; `cfg[8]` = motion wake enabled (default 1, BLE cmd `02 23 xx`, 0x4200eed8, table entry 0x3c117cd0), `cfg[0x34]` = boot stage (0 first boot, 1/2 normal, 11 = factory reset pending) |
| 0x3fc9f301 | `key_press_last` | set by the GPIO3/GPIO9 ISR when the line is low |
| 0x3fc9f307 | motion event gate | 0 = GPIO8 ISR may post the motion event |
| 0x3fc9f308 | `wake_gate` | 5 after the screen-off sequence ran; wake functions only act when >= 4 and reset it to 0 |
| 0x3fc9f309 | `pxp_main_task_init_ok` | 5 once the main task is up; ISRs ignore edges before that |
| 0x3fca2884 | woke_by_ext0 | set in `app_main` on EXT0 (motion) wake |
| 0x3fca2885 | `allow_anymotion_check_flag` | 1 for 5 s after a motion edge / motion wake |
| 0x3fca4ea0 | `button_long_reset_flag` | 1 on the first boot after a factory reset |
| 0x3fca4e9b | `quit_Production_test_flag` | 2 = factory production mode active |
| 0x3fca4dca | guitai / "IntoShow" flag | 1 = shop-counter demo mode |
| 0x3fc9aba4 | BLE-wake allowed | set to 1 by the screen-off sequence |
| 0x3fc9a1e0 | Wi-Fi off counter | .data initial 200 (inert), set to 0 at screen-off |

Main event group 0x3fca4eac (setter 0x40377f10, wait mask 0x7fffea, clear-on-exit, 0x4201cdc4). Bits
relevant to power:

| Bit | Posted by | Handler in the main loop |
|---|---|---|
| 0x02 | GPIO8 ISR (`0x40377f24`) | motion wake, section 4.2 |
| 0x08 | `bletimeout` timer callback 0x4201b590 ("BLETIMEOUT_NOTIF") | forced deep sleep |
| 0x20 | GPIO9 ISR when low | charger inserted callback `0x42017a0c(0)` |
| 0x40 | 10 ms periodic timer 0x4201b5a0 | 10 ms tick; every 100th tick runs the 1 Hz block |
| 0x80 | button events | see `input.md` |
| 0x4000 | BLE connect (blufi event 3, 0x4200b830), Wi-Fi STA_CONNECTED (0x4200bbac), a few cloud paths ("BRUSH_BLE_WAKE_UP_NOTIF") | wake the screen if `asleep` |
| 0x80000 | brushing ended (`0x4201b608(0)`) | end-of-session handling |
| 0x100000 | `sleeo_timer` callback 0x42013fc8 ("OCLEAN_BRUSH_SLEEP_SEND") | second half of the screen-off transition |

---

## 2. Awake -> asleep

Stock has two stages:

1. **Screen-off stage** ("soft sleep"): display, LEDs, touch/pressure sensor off, IMU in low-power
   any-motion mode, CPU still running with the APB lock released, BLE still advertising /
   connected, Wi-Fi left on for 27 more seconds. Any button press, motion, BLE connection or
   charger insertion brings the screen back without a reboot.
2. **Deep sleep**: entered when the BLE window timer expires (30 s or 120 s after screen-off).
   Wake = full reboot.

Deep sleep is only ever entered when `chg == 2` (on battery): first test in `brush_pm_control`
(0x4201c5db). On the charger the brush never sleeps (section 2.6).

### 2.1 The idle timer ("sleep monitoring")

```c
void open_sleep_monitoring(uint8_t n)   /* 0x42014278 */
{ timeout_s = n; start_s = now_s(); enabled = 1; }
void sleep_monitor_off(void)            /* 0x42014268 */ { enabled = 0; }
void sleep_monitor_kick(void)           /* 0x42014298 */ { start_s = now_s(); }

/* 0x420142d0, called once per second from the main loop (see 2.2) */
bool sleep_monitor_check(void)
{
    uint32_t el = now_s() - start_s;
    if (!enabled || asleep) return false;
    if (el < timeout_s)     return false;
    busy = 1;
    if (battery_cap <= 10 && chg == 2 && quit_Production_test_flag != 2) {
        if (el == timeout_s) {                 /* exactly at expiry */
            show_low_battery_screen();         /* 0x4201a260, screen code 0x5e */
            if (battery_cap != 0 && brushed_since_wake /*0x3fca4197*/)
                buzz3();                       /* 0x4201babc: 3 x (motor pattern 0x21, 400 ms, stop, 400 ms) */
            brushed_since_wake = 0;
            if (busy == 2) return false;       /* button pressed meanwhile (0x4201b680) */
        }
        if (el < timeout_s + 3) { busy = 0; return false; }   /* show the warning for 3 s */
    }
    asleep = 1; busy = 0;
    return true;
}
```
CONFIRMED (0x420142d0..0x42014371).

### 2.2 When the check runs and what blocks it

Main loop, 1 Hz block (10 ms tick counter 0x3fca4df1 > 99), code at 0x4201d1e2..0x4201d4cd:

```c
if (chg != 2) { delay_to_close_screen(); }          /* 0x42018250, section 2.6 */
else if (remote_brush_flag /*0x3fca308e*/ == 1) ;   /* BLE cmd 0xa4 param 1 */
else if (guitai_mode /*0x3fca4dca*/) puts("get_guitai_show_mode");
else if (ota_started /*0x3fca4194*/) ;
else if (ui_mode /*0x3fca4d8d*/ == 0xD7) ;          /* demo / production UI mode */
else if (cfg[0x70] != 0 && !weak_stub_0x4210225c()) ;/* stub returns 0 for the value passed: sleep blocked while cfg[0x70] != 0 (never set by code; default 0) */
else if (brushing /*0x3fca4d5b*/) ;
else if (sleep_monitor_check()) {
    /* 0x4201d417: an OTA-prompt detour exists here (0x42011978) - only relevant with the OEM cloud */
    led_set(1, off, mode 3);                         /* 0x4201dcc8(1,1,3) */
    xTaskCreatePinnedToCore(sleep_brush_task /*0x4201b900*/, "sleep_brush_task",
                            4096, NULL, 3, &h /*0x3fca4e04*/, tskNO_AFFINITY);
}
```
CONFIRMED. While `asleep == 1` the 1 Hz block additionally calls `0x4200bae0` (Wi-Fi off counter,
section 6.2) instead of the UI sequencer `0x4201b1e4`.

While brushing the monitor is off (`sleep_monitor_off()` at brush start, 0x4201c790) and the
CPU_FREQ_MAX lock is held (0x42014184); the lock is released at session end (0x4201ce36).

### 2.3 Idle timeout values (argument of `open_sleep_monitoring`)

All CONFIRMED as call sites; the "situation" column is the caller's purpose (INFERRED from its
strings / screen codes where noted; screen codes are the UI spec's).

| Seconds | Call site | Situation |
|---|---|---|
| 60 | 0x4201cc43 | main task start (every cold boot / deep-sleep wake) |
| 30 (60 if production mode) | 0x4201a3fc (`uipxp_task_show_main_screen`) | every time the main screen is (re)shown - this is the normal "idle after wake" value |
| 30 | 0x4201bd70, 0x4201bee8 | wake from the screen-off stage |
| 30 | 0x42017a0c | charger removed while awake (battery > 0) / charger inserted |
| 1 | 0x42017a0c | charger removed with `battery_cap == 0` |
| 30, or 10 when wake-state 0x3fca4e8f is 3/4 (after a session) | 0x4201af48 | screen changed by a touch gesture; every gesture also calls `sleep_monitor_kick()` (0x4201af48, 0x42025afc) |
| 10 | 0x4201a5ac | main screen shown right after a brushing session |
| 10 | 0x4201c2b8 | session ended while on the charger |
| 10 | 0x4201b1e4 | end of the post-session result screens (after 10 s on the last one) -> main screen |
| 1 | 0x4201b1e4, 0x4201a9bc, 0x4201ac14 | screen 0x62 after 20 s / dismissed |
| 10 / 5 / 60 | 0x4201b0d8 | button pressed on a non-main screen -> back to main: 10 (screens 0x53,0x54,0x64), 5 (0x5c,0x5e), 60 (others) |
| 60 | 0x4200bbac | Wi-Fi STA connected while not brushing |
| 60 | 0x4201a538 | event 0x10000 (binding done) |
| 30 | 0x4201a94c, 0x4201cab8 | brush-info screen (5 s button hold) |
| 10 | 0x4201a1a0, 0x42019b48, 0x42019b7c | screen 0x49; leaving "cooker"/aging mode |
| 10 | 0x4201d46b | OTA prompt screen shown instead of sleeping (first time only) |
| 8 | 0x4201ccd0 | first boot after factory reset (`cfg[0x34] == 11`) |
| 7 | 0x4201cca9 | boot with `cfg[0x6f] == 0x37` (set by the BLE factory-reset command) |
| 5 | 0x420107b8, 0x4201d0dd, 0x42026f5c | BLE/UI helper; production mode every 240 s; pressure calibration |
| 1 | 0x4201c790 | button pressed with `battery_cap == 0` (low-battery screen, then sleep) |
| 0 | 0x42029218 | wake screen chooser finds `battery_cap == 0` and no charger (`GPIO9 == 1`): low-battery screen, sleep at once (+3 s warning) |

Typical sequences (INFERRED by chaining the confirmed pieces):
* Wake (button/motion/charger removal): greeting/history screen (codes 0x54/0x55/0x68) for 6 s
  (`0x4201b1e4`: seconds-on-screen counter 0x3fca4de5 == 6) -> main screen with 30 s -> screen off
  about 36 s after the wake if nothing is touched.
* After a session with a score: result screens with the monitor off (about 1 s + 10 s + 10 s,
  `0x4201b1e4`), then main screen with 10 s. After a short session: main screen with 10 s.

### 2.4 Screen-off sequence

`sleep_brush_task` (0x4201b900), CONFIRMED:

```c
touch_state(7);                    /* 0x4201b430(7): touch IC to sleep, see input.md */
vTaskDelay(500);
save_user_config();                /* 0x420185b4, NVS */
if (cloud_flag /*0x3fca3870*/ == 1) http_queue(0x0e);   /* 0x42011880, OEM cloud job */
brush_work_ooer_to_sleep();        /* 0x4201b764 */
esp_timer_start_once(sleeo_timer, 200000);   /* 0x42014374 -> event 0x100000 after 200 ms */
vTaskDelete(self);
```

`brush_work_ooer_to_sleep` (0x4201b764), CONFIRMED, in this order:

1. `wake_gate (0x3fc9f308) = 5`.
2. Read the clock into 0x3fca4eb8 and write the 8-byte NVS blob "TimeRTC" (namespace "storage").
3. `puts("brush_work_ooer_to_sleep")`; `0x42011918()` ("updata_Advertising_configuration": queues
   OEM cloud job 0x13 when the cloud flag 0x3fca3870 == 1).
4. Mode bookkeeping (0x3fca4c85[6..7]) - UI, not power.
5. `puts("QMI8658 AMD MODE2..")`; if `chg == 2`:
   * `gyro_wakeup_count > 4` -> `qmi_amd_mode()` then `qmi_power_down()` (motion wake disabled);
   * else `qmi_amd_mode()` (0x4201e74c).
6. LCD command 0x10 (SLPIN) (`0x42028918`).
7. `led_set(4, off)` (backlight LEDC channel) and `gpio_set_level(21, 1)`.
8. Stop the pressure-sensor timer (`0x42027ba0`).
9. `led_set(0..3, off, 4)`; `led_set(4, off)`.
10. Remove the GPIO12 (touch/pressure INT) ISR (`0x4200cc58(0)`).
11. `touch_state(7)`; `u16 0x3fca4e08 = 1`; UI message (`0x4202149c`).
12. `0x3fc9aba4 = 1`; `0x3fca4e0c = 0`; battery bookkeeping `0x42017d54`; Wi-Fi off counter
    `0x3fc9a1e0 = 0`.
13. `printf("wifi status:%d ble status:%d")`.
14. If `chg == 2`: `esp_timer_stop(bletimeout)`, then start it once:
    * no Wi-Fi SSID stored (`esp_wifi_get_config` ssid[0] == 0, 0x4200bb40): **120 s** (0x7270e00 us);
    * SSID stored and BLE not connected (`0x4204eb58()` == 0): **30 s** (0x1c9c380 us);
    * SSID stored and BLE connected: **120 s**.
    (`0x4204eb58` returns byte +12 of the blufi state at 0x3fcaa1ac, the same byte
    `esp_blufi_send_encap` tests before logging "ble connection is broken": "connected" is
    INFERRED from that; the three durations and the branch structure are CONFIRMED.)
15. `0x3fca4d8d = 0`; `0x3fca4dca = 0`.

200 ms later the main loop handles event 0x100000 (0x4201d2a8), CONFIRMED:

```c
flag_0x3fca4ea2 = 1; flag_0x3fca4dfa = 1;
if (asleep) {
    brush_pm_control(0);              /* deep-sleeps right here in the special cases of 2.5 */
    gpio_prep_screen_off();           /* 0x4200dbbc, table in 3.2 */
    if (chg != 2) {                   /* on the charger: keep the charge LED alive */
        gpio_hold_dis(19); led_init(); led_set(2, chg == 3 ? on : state 3, 4);
    }
    brush_resume_sleep();             /* USB D+ pull-up off; release APB lock; stay_alive = 0 */
    if (wifi_connected) http_queue(5);/* 0x4200bb1c */
    motion_gate (0x3fc9f307) = 0;
}
gpio_isr_handler_add(8, isr);         /* 0x4200d75c(1): motion INT armed only now */
gpio_set_level(45, 0);
```

### 2.5 `brush_pm_control(forcesleep)` (0x4201c5d0), CONFIRMED

```c
void brush_pm_control(bool forcesleep)
{
    if (chg != 2) return;
    USB_SERIAL_JTAG_CONF0 &= ~BIT(9);                 /* D+ pull-up off */
    printf("battery_cap %d get_motor_wakeup_flag %d button_long_reset_flag %d "
           "quit_Production_test_flag %d forcesleep %d\r\n", ...);

    bool special = battery_cap == 0 || button_long_reset_flag || quit_Production_test_flag == 2;
    bool ext1_only;

    if (special && asleep && !forcesleep) {           /* path X: no BLE window at all */
        flag_0x3fc9f306 = 0;  vTaskDelay(100);
        save_battery_record();                        /* 0x42017990, NVS "brush_battery" */
        qmi_amd_mode();  qmi_power_down();            /* 0x4201e74c, 0x4201e6f0 */
        vTaskDelay(50);
        gpio_prep_deep_sleep();                       /* 0x4200dda4 */
        deep_sleep1();                                /* EXT1 only */
        vTaskDelay(500);  return;
    }
    if (!forcesleep) return;

    /* path F: forced (BLE window expired) */
    if (cfg[8] == 0)                       { qmi_power_down(); ext1_only = true; }
    else if (gyro_wakeup_to_up_limit())    { qmi_power_down(); ext1_only = true; }   /* count > 4 */
    else                                     ext1_only = false;                      /* IMU stays in AMD mode from 2.4 step 5 */
    flag_0x3fc9f306 = 0;  vTaskDelay(100);
    save_battery_record();
    vTaskDelay(50);
    gpio_prep_deep_sleep();
    puts("long_time_nousing_sl1");
    if (ext1_only) deep_sleep1(); else deep_sleep2();
    vTaskDelay(500);
}
```

Callers: event 0x100000 -> `brush_pm_control(0)` (0x4201d2c7); event 0x08 "BLETIMEOUT_NOTIF" ->
`esp_ble_gap_stop_advertising()` (0x4204e5a0) then `brush_pm_control(1)` (0x4201d344..0x4201d349).
So in the normal case the string "long_time_nousing_sl1" is simply the log line of the forced
deep sleep after the BLE window.

What keeps the BLE window open (restarts the `bletimeout` one-shot with the same 30 s / 120 s
rule, only while `asleep && chg == 2`): a write to the command characteristic, unless the
response buffer 0x3fca2be6 (still holding the previous command's reply at that point) starts with
`03 02`; a 2-byte CCC descriptor write (both `ble_timeout_in_sleep`, called from 0x4200e2c8);
brush-record transfer (`ble_timeout_in_sleep_data` 0x4201b9b0 via 0x420114fc). `motorwakeup` stops
the timer. CONFIRMED (the `03 02` exception is read literally from the code; its intent - not
letting a periodic status poll keep the brush awake - is INFERRED).

### 2.6 On the charger (`chg` = 1 or 3)

* No idle timer, no screen-off sequence, no deep sleep: the 1 Hz block only runs
  `delay_to_close_screen` (0x42018250): counter 0x3fc9ab89 (reset to 0 on charger insertion,
  0x42017a0c) increments each second; **at 30 the backlight is switched off**
  (`led_set(4,on); led_set(4,off)`), unless `ui_mode == 0xD7`. CONFIRMED.
* A button press while the counter is >= 30 resets it and turns the backlight on again
  (0x4201829c, 0x4201cab8). Other button functions are ignored while awake on the charger
  (`input.md`).
* Charger removal is detected by polling every 10 ms (0x4201840c): GPIO9 high for 151 consecutive
  ticks (about 1.5 s; a GPIO2 falling edge or GPIO9 low resets the counter) -> `0x42017a0c(1)`:
  `chg = 2`, `open_sleep_monitoring(30)` (1 if battery empty), main screen if the backlight was
  still on, otherwise the wake screen is shown a few seconds later (0x4201d38d). CONFIRMED.
* Charger insertion: GPIO9 falling edge ISR -> event 0x20 -> `0x42017a0c(0)` ("USB_IN_ACTION"):
  if `asleep` -> `motorwakeup()`; stop brushing; `chg = 1`; charging screen. CONFIRMED.

---

## 3. Deep-sleep entry

### 3.1 Wake sources and the two variants (CONFIRMED)

```c
/* 0x42014220 */ static void wake_cfg_1(void) {
    printf("Enabling EXT1 wakeup on pins GPIO%d, GPIO%d\n", 9, 3);
    esp_sleep_enable_ext1_wakeup(0x208 /* BIT(3)|BIT(9) */, 0 /* mode 0 */);
}
/* 0x420141cc */ static void wake_cfg_2(void) {
    printf("Enabling EXT1 wakeup on pins GPIO%d, GPIO%d\n", 9, 3);
    esp_sleep_enable_ext1_wakeup(0x208, 0);
    esp_sleep_enable_ext0_wakeup(8 /* GPIO8 */, 1 /* high */);
}
/* 0x42014240 "deep sleep1" */            /* 0x420141f8 "deep sleep2" */
wake_cfg_1();                             wake_cfg_2();
esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER /*4*/);      /* both */
esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH /*0*/, ESP_PD_OPTION_ON /*1*/);
puts("deep sleep1\r");                    puts("deep sleep2\r");
esp_deep_sleep_start();
```

* EXT1 mode value 0 is named `ESP_EXT1_WAKEUP_ALL_LOW` in the v5.1.1 header; on the ESP32-S3 the
  hardware wakes when **any** selected pin is low (later IDF versions name the same value
  `ESP_EXT1_WAKEUP_ANY_LOW`). INFERRED from IDF documentation; consistent with the code, which
  reads `esp_sleep_get_ext1_wakeup_status()` and treats bit 3 alone as "button".
* No timer wake, no ULP program (the "ULP-RISC-V" strings are leftovers of the example the
  project was started from; `app_main` only prints them).
* No `rtc_gpio_pullup_en` / `gpio_deep_sleep_hold_en` / `esp_sleep_config_gpio_isolate` calls
  exist in the application code (searched the whole decompilation). GPIO3's pull-up is configured
  only through `gpio_config` (IO_MUX pull, digital domain). INFERRED: the board has external
  pull-ups on GPIO3 and GPIO9, otherwise EXT1 would fire immediately.
* Because "any low" includes GPIO9, deep sleep with the charger attached would wake at once;
  stock never deep-sleeps on the charger (2.5 first line).

| Variant | Wake sources | Used when |
|---|---|---|
| "deep sleep2" 0x420141f8 | button (GPIO3 low), charger (GPIO9 low), **motion (GPIO8 high)** | normal idle sleep with `cfg[8] != 0` and `gyro_wakeup_count <= 4` |
| "deep sleep1" 0x42014240 | button, charger | motion wake disabled (`cfg[8] == 0`), motion wake limit reached, battery empty, factory-reset/shipping, production mode, and the boot-time re-sleeps of section 4.1 |

### 3.2 GPIO state before sleeping (CONFIRMED, in call order)

`gpio_prep_screen_off` 0x4200dbbc (screen-off stage) and `gpio_prep_deep_sleep` 0x4200dda4
(immediately before `esp_deep_sleep_start`). "cfg" = `gpio_config` (mode / pulls / intr),
"hold" = `gpio_hold_en`.

| Step | Pin | screen-off stage 0x4200dbbc | deep sleep 0x4200dda4 |
|---|---|---|---|
| 1 | 17,18,19,20 | `gpio_hold_dis` each | same |
| 2 | GPIO17 (LED ch0) | cfg OUTPUT, level 1, hold | same |
| 3 | GPIO18 (LED ch1) | cfg OUTPUT, level 1, hold | same |
| 4 | GPIO19 (LED ch2) | cfg OUTPUT, level 0, hold | same |
| 5 | GPIO20 (LED ch3) | cfg OUTPUT, level `(0x3fc9abba == 1)` = 1, hold | same |
| 6 | HW I2C0 | - | `i2c_driver_delete(0)`; `gpio_reset_pin(36)`; `gpio_reset_pin(35)` (0x4200cb6c) |
| 7 | GPIO2 | cfg INPUT, no pulls, NEGEDGE | same |
| 8 | GPIO45 (WLC_EN) | cfg OUTPUT, level 0 | same |
| 9 | GPIO12 (touch INT) | cfg mode DISABLE (0), intr field 2; level 0; hold | same |
| 10 | GPIO26 (CHARGE_EN) | `0x4200d650(1)`: hold_dis, `gpio_set_direction(26, INPUT)`, hold | same |
| 11 | GPIO7,6,5,4 (IMU SPI CS,SCLK,MOSI,MISO) | - | `0x4200cd54`: hold_dis; each cfg OUTPUT + pull-up, level 1, hold |
| 12 | GPIO48 (motor amp) | cfg OUTPUT, level 0 (no hold here) | same |
| 13 | GPIO37 | cfg OUTPUT, **level 1**, hold | same |
| 14 | GPIO38,40,39 (LCD CS,MOSI,SCLK) | `gpio_sleep_set_direction(pin, GPIO_MODE_DISABLE)` (0x4200cfb8) | `0x4200cfd4`: each cfg INPUT, level 1, hold |
| 15 | GPIO42 (LCD RST) | cfg OUTPUT, level 0, `vTaskDelay(2)`, hold | same |
| 16 | GPIO41 (LCD DC) | cfg OUTPUT, level 0, hold | same |
| 17 | GPIO21 (backlight) | cfg OUTPUT_OD (mode 6), level 1, hold | same |
| 18 | all | `gpio_force_unhold_all()` (0x4037b534: clears DG_PAD_FORCE_HOLD, sets FORCE_UNHOLD and CLR_DG_PAD_AUTOHOLD in RTC_CNTL_DIG_ISO_REG, then `rtc_gpio_force_hold_dis_all`) | not called |

Notes:
* GPIO37 is driven 0 at boot / wake (0x4200db27, 0x4200df88) and 1 for sleep, so it is an
  active-low enable of something that is powered while awake (INFERRED; which rail is not
  determinable from this code). It is not the IMU supply: the IMU has to keep running during
  "deep sleep2" while GPIO37 is 1, and at boot the IMU is initialised before GPIO37 is driven 0
  (INFERRED).
* The byte 0x3fc9f306 is cleared right before both deep-sleep paths; no reader was found in the
  application code (CONFIRMED by search), so it can be ignored.
* GPIO45 additionally gets `gpio_hold_en(45)` + `gpio_sleep_sel_en(45)` at boot (0x4200d9f2).
* GPIO3, GPIO8, GPIO9 are left as configured at boot (inputs; 3 with IO_MUX pull-up, ANYEDGE).
* The SPI drivers are not removed before the pins are reconfigured (the code calls `gpio_config`
  on pins still attached to SPI2/SPI3); whether the "level 1" written with `gpio_set_level`
  actually reaches the pad for those pins is not determinable statically (open question 4).
* Whether the digital pads (GPIO26..48) keep their level during deep sleep with only
  `gpio_hold_en` is an IDF/hardware question (open question 3). The RTC pads (GPIO12, 17..21) are
  held.

Order of everything for the normal case (path F): BLE advertising stop -> D+ pull-up off -> (IMU
power-down only if motion wake is disabled) -> 100 ms -> NVS battery record -> 50 ms -> table
above -> wake sources -> `esp_deep_sleep_start()`. Wi-Fi was already stopped by the 27 s counter
(6.2); the BT controller is not explicitly disabled. CONFIRMED.

### 3.3 RTC-retained variables (segment at 0x50001000, initial values from the image)

| Address | Size | Init | Name / meaning | Evidence |
|---|---|---|---|---|
| 0x50001000 | u8 | 0 | `gyro_wakeup_wakeup_count`: diagnostic, ++ each time a motion-wake boot passes the check in `brush_gpio_cfg`, cleared when boot continues | 0x4200db4c, 0x4200da41 |
| 0x50001001 | u8 | 1 | OTA-prompt allowed flag | 0x42011968, 0x42011978 |
| 0x50001004 | 4 x float, 0x50001014 s8 | 0 | cloud/advert data | 0x42011a60 |
| 0x50001015 | u8 | 0 | charger bookkeeping flag | 0x42017a0c, 0x4201d3a0 |
| 0x50001016 | u16 | 0xffff | battery voltage record (filtered mV) | 0x42017f08, 0x42017990 |
| 0x50001018 | u8 | 0xff | `brush_battery_record` (battery %) | 0x42017f08, printed at 0x4200db72 |
| 0x50001019 | u8 | 3 | last strength level (INFERRED from its use with the strength screen 0x57) | 0x4201c790, 0x4201b50c |
| 0x5000101a | u8 | 0xff | `pre_day` (day of the daily brushing totals) | 0x4201bc88 |
| 0x5000101c / 1e / 20 | u16 each | 0 | today's session count / score sum / seconds | 0x4201bc88, 0x4201c100 |
| **0x50001022** | u8 | 0 | **`gyro_wakeup_count`** | 0x4201b6dc..0x4201b740 |
| 0x50001024 | 16 x u32 | 0 | music/decoder table | 0x4201ef64 |

`gyro_wakeup_count` is also stored in NVS: blob "wakeupcount" (10 bytes, byte 0 = count) in
namespace "storage", written on every change (0x42023d34) and read back at each boot
(0x4201b6dc -> 0x42023d68). The battery record is mirrored in NVS blob "brush_battery" (20 bytes,
first 3 bytes = %, mV high, mV low; 0x4202490c / 0x42024940). CONFIRMED.

---

## 4. Wake path

### 4.1 `app_main` (0x4200c034) and `brush_gpio_cfg` (0x4200d77c), CONFIRMED

```c
void app_main(void)
{
    evt = xEventGroupCreate();                    /* 0x4201c5c0 -> 0x3fca4eac */
    pxp_main_task_init_ok = 0;
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG /*0x600080e8*/, 0);     /* brown-out detector off */
    nvs_flash_init();  /* erase + retry on NO_FREE_PAGES / NEW_VERSION_FOUND */
    track_wakeup_load();                          /* table 0x3fc9a1e4 [12] = 0x4200c974, NVS "TrckWkup" */
    load_daily_totals();                          /* 0x4201c100 -> RTC 0x5000101a..20 */
    load_sys_config();                            /* 0x42018cf8, NVS "sys_config" -> cfg[0x34] ... */

    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1:                   /* 3: button or charger */
        pin = ffs(esp_sleep_get_ext1_wakeup_status()) - 1;
        printf("111Wake up from GPIO %d\n", pin);
        if (pin == 3) { gyro_wakeup_count = 0 (+NVS);   /* 0x4201b740 "key_wakeup_clear_gyro_count" */
                        track_wakeup_button(); }        /* 0x4200cacc */
        save_time_to_nvs("TimeRTC");
        break;
    case ESP_SLEEP_WAKEUP_EXT0:                   /* 2: motion */
        puts("222Wake up from GPIO8");  track_wakeup_gyro();   /* 0x4200ca64 */
        woke_by_ext0 = 1;
        save_time_to_nvs("TimeRTC");
        break;
    case ESP_SLEEP_WAKEUP_TIMER:                  /* 4: never armed; only a puts */
        break;
    default:                                      /* power-on / reset / esp_restart */
        if (cfg[0x34] != 11) restore_time_from_nvs("TimeRTC");   /* 0x42024ff4, 0x4201d8f8 */
        puts("Not a deep sleep reset");
    }
    esp_timer_create(anymotion_timeout_cb /*0x4200b734: allow_anymotion_check_flag = 0*/);
    enter_auto_light_sleep();                     /* esp_pm_configure, section 6.1 */
    if (woke_by_ext0) anymotion_arm();            /* 0x40377c64: allow flag = 1, one-shot 5 000 000 us */
    brush_gpio_cfg();                             /* may not return, see below */
    hw_i2c0_init();                               /* 0x4200cb34: SDA36 SCL35 100 kHz */
    create decoder/music tasks (0x4201f18c); key_int + brush_app tasks (0x4201c56c);
    brush_pm_init();                              /* 0x42014000 */
    create http task (0x420117dc); load Wi-Fi list (0x42024418);
    Wi-Fi init (section 6.2); BT controller + blufi + GATT init;
}
```

Inside `brush_gpio_cfg`, after the LED/ADC/button/IMU-SPI setup and `qmi_normal_init()`
(0x4201e730), GPIO9 is configured (input, no pulls, ANYEDGE, ISR) and the count is loaded from NVS.
Then (0x4200da21..0x4200dbb6):

```c
for (;;) {
    if (gpio_get_level(9) == 0) {                     /* charger present */
        printf("get_battery_adc_origin %d\r\n", batt_mv()); vTaskDelay(10);
        break;                                        /* continue booting */
    }
    if (batt_mv() <= 3299 /* 0xce3 */) {              /* empty and no charger */
        qmi_amd_mode();  deep_sleep1();               /* never returns */
        continue;
    }
    if (!woke_by_ext0) break;                         /* button / charger / cold boot: continue */

    /* motion wake, battery OK, no charger */
    gyro_wakeup_wakeup_count++;                       /* RTC 0x50001000 */
    for (;;) {
        vTaskDelay(10);
        printf("key_press_last = %d brush_battery_record %d allow_anymotion_check_flag %d,"
               "gyro_wakeup_wakeup_count %d\r\n", ...);
        if (key_press_last)            goto go_on;    /* button went low since boot */
        if (gpio_get_level(9) == 0)    goto go_on;    /* charger */
        if (allow_anymotion_check_flag && !gyro_wakeup_to_up_limit()) {
            gyro_wakeup_count++ (+NVS);               /* 0x4201b714, only while < 20 */
            goto go_on;
        }
        qmi_amd_mode();  deep_sleep1();               /* straight back to sleep, no screen */
    }
}
go_on:
gyro_wakeup_wakeup_count = 0;
... rest of the GPIO init (GPIO13/14, GPIO12, I2S, GPIO48 = 0, LCD SPI, GPIO42 = 1, GPIO41 = 1, GPIO37 = 0)
```

`batt_mv()` = 0x4200cc94 = `esp_adc_cal_raw_to_voltage(adc1_get_raw(CH0)) * 2`.

Per wake cause:

| Cause | Counter | Goes straight back to sleep when | Otherwise |
|---|---|---|---|
| Button (EXT1 bit 3) | `gyro_wakeup_count = 0` | battery <= 3299 mV and no charger -> "deep sleep1" | full boot, screen on |
| Charger (EXT1 bit 9) | unchanged | never (GPIO9 low skips the battery test) | full boot, screen on, then `chg = 1` via 0x4201840c |
| Motion (EXT0) | `gyro_wakeup_count++` if allowed | battery <= 3299 mV; or count already > 4; or the 5 s `allow_anymotion_check_flag` has expired -> "deep sleep1" (EXT1 only, so no further motion wakes) | full boot, **screen on** |
| Cold boot / reset | unchanged | battery <= 3299 mV and no charger | full boot; time restored from NVS |

So yes: **a motion wake turns the screen on** ("auto turn on" when the brush is picked up), up to
5 consecutive times. `gyro_wakeup_count` is reset only by a button wake from deep sleep
(0x4200c114), a button press in the screen-off stage (0x4201c790 -> 0x4201b740) or the start of a
brushing session (0x4201c790). Once it exceeds 4 the next sleep powers the IMU down and uses
"deep sleep1", i.e. motion wake stays off until the button is pressed. CONFIRMED. Purpose
(INFERRED): stop a brush that is being carried around from waking forever.

### 4.2 Motion / BLE / button wake in the screen-off stage (no reboot)

* GPIO8 ISR (any edge, 0x40377d0c): if `init_ok >= 5`: `anymotion_arm()` (allow flag = 1 for 5 s);
  if `wake_gate >= 4` and `motion_gate == 0` post event 0x02. The ISR is only installed between
  the event-0x100000 handler and the next wake; it is removed while awake
  (`0x4200d75c(0)` at 0x4201cd7f and in both wake functions). CONFIRMED.
* Event 0x02 (0x4201ce17 / 0x4201cf9d): only if `cfg[8] == 1 && chg == 2`:
  `motion_gate = allow_anymotion_check_flag`; if `wake_gate >= 4` and `gyro_wakeup_count <= 4`:
  `gyro_wakeup_count++`, `motorwakeup()`, statistics counter 0x3fca4dfb++, `motion_gate = 1`.
* Event 0x4000 (BLE connected, Wi-Fi connected, some cloud messages): if `asleep == 1`,
  `0x3fc9aba4 != 0` and `chg == 2` -> `motorwakeup()` (0x4201ce84). So a phone connecting during
  the BLE window turns the screen on.
* Button short press while `asleep` (0x4201c790): `gyro_wakeup_count = 0`, `motorwakeup()` -
  the first press only wakes; see `input.md`.
* Charger inserted while `asleep`: "WAKE UP AND BRUAH", `motorwakeup()` (0x42017a0c).

`motorwakeup` (0x4201bd70), CONFIRMED order:

```c
if (wake_gate < 4) { wake_gate++; return; }
wake_gate = 0;
brush_stay_alive();                       /* APB lock, asleep = 0 */
0x3fca4e8f = 1;                           /* wake-state: woke from screen-off */
esp_timer_stop(Raise_the_bright_screen_timer);
motion_gate = 1;
gpio_restore_after_screen_off();          /* 0x4200df88 */
vTaskDelay(100);
qmi_normal_init();                        /* 0x4201e730 */
lcd_init();                               /* 0x42029120 */
pressure_sensor_start(20);                /* 0x42027bd0, 0x42027b4c(0x14) */
gpio12_irq(1);  motor_stop();  touch_state(0);  touch_state(0x21);
led_set(chg == 2 && battery_cap != 0 ? 1 : 4, on);
restart the 10 ms periodic timer (esp_timer_stop + start_periodic 10000 us);
esp_timer_stop(bletimeout);
open_sleep_monitoring(30);
daily-totals rollover (0x4201bc88(0,0,0));
show wake screen (0x42029218);  0x4202148c();
gpio_set_level(21, 0);                    /* backlight */
remove GPIO8 ISR;
if (!ble_connected) esp_blufi_adv_start();   /* 0x4204e55c */
esp_wifi_reinit();                        /* 0x4200bb5c, section 6.2 */
motion_gate = 1;  0x3fca4e0b = 0;
if (NVS "IntoShow" != 0) open guitai mode (0x4201a970);
```

`gpio_restore_after_screen_off` (0x4200df88): `gpio_hold_dis` 13,14,21,42,17,18,19,20,37,41,12;
GPIO17 = 1, 18 = 1, 19 = 0, 20 = 1 (each re-configured OUTPUT); LEDC timer + 5 channels
re-configured (0x4200d2a8); GPIO2 input NEGEDGE; GPIO45 output 0; GPIO12 input + pull-up NEGEDGE;
GPIO26 input + hold (`0x4200d650(1)`); GPIO48 output 0; GPIO37 output 0; GPIO41 output 1.
CONFIRMED.

### 4.3 After the boot: what is shown

Main task (0x4201cc40), CONFIRMED order: `open_sleep_monitoring(60)`; battery init (`chg = 2`,
0x4201831c); LCD init; UI task init; daily totals; then
* `cfg[0x34]` is 0 or 11 (first boot / after factory reset): screen code 0x46 (0x4201a148; the
  first-use screen, name per the UI spec);
* else the wake-screen chooser 0x42029218: battery 0 % and no charger -> low-battery screen 0x5e
  and `open_sleep_monitoring(0)`; otherwise a date-dependent greeting (0x55), the brushing-history
  screen (0x54) or screen 0x68.

Then backlight on (`led_set(4, on)`), 500 ms, `qmi_normal_init()`, timers, button init, GPIO12 IRQ,
pressure sensor, GPIO8 ISR removed, `pxp_main_task_init_ok = 5`, counter-mode check, event loop.
The same boot path is used for every wake cause; the wake cause itself does not select the
screen. After 6 s the 1 Hz sequencer replaces the greeting by the main screen (30 s timeout).

---

## 5. QMI8658 (SPI3: MISO4 MOSI5 SCLK6 CS7, 10 MHz, mode 0)

Register write = one SPI transfer `[reg, value]` (0x4200cdf4 via 0x4201e0c4). Register read =
0x4200ce60 (it inserts `vTaskDelay(1)` before every byte). The custom firmware's `imu_wr(reg,val)`
/ `imu_rd(reg|0x80, ...)` are the equivalents. Interrupt pin: **INT1 -> GPIO8**, push-pull, active
high (CTRL1 bit 3 enables the INT1 output; CTRL8 bit 6 routes motion events to INT1); bit meanings
are from the QST datasheet / reference driver, the bytes are CONFIRMED. The stock driver is the
QST reference driver (`qmi8658_config_motion`, `qmi8658_enable_amd`, `qmi8658_config_reg`).

Driver state at 0x3fca4f1c: [+4] enabled sensors, [+8] acc range, [+0xc] acc ODR, [+0x10] gyro
range, [+0x14] gyro ODR, [+0x18] `ctrl8_value`, [+0x1c] acc LSB/g, [+0x1e] gyro LSB/dps.

### 5.1 Common block M: `config_motion` (0x4201e508), CONFIRMED

```
ctrl8_value &= ~0x02;  W 0x09 = ctrl8_value          (0xC0 after init)
W 0x0B = 0x06   CAL1_L  any-motion threshold X   (6/32 g)
W 0x0C = 0x06   CAL1_H  any-motion threshold Y
W 0x0D = 0x06   CAL2_L  any-motion threshold Z
W 0x0E = 0x09   CAL2_H  no-motion threshold X    (9/32 g)
W 0x0F = 0x09   CAL3_L  no-motion threshold Y
W 0x10 = 0x09   CAL3_H  no-motion threshold Z
W 0x11 = 0xF7   CAL4_L  MOTION_MODE_CTRL (any-motion X|Y|Z, OR logic; no-motion X|Y|Z, AND logic)
W 0x12 = 0x01   CAL4_H  "first command"
CTRL9(0x0E)
W 0x0B = 0x03   any-motion window (samples)
W 0x0C = 0x01   no-motion window
W 0x0D = 0x2C   significant-motion wait window  = 0x012C
W 0x0E = 0x01
W 0x0F = 0x64   significant-motion confirm window = 0x0064
W 0x10 = 0x00
W 0x12 = 0x02   CAL4_H  "second command"
CTRL9(0x0E)
```
`CTRL9(cmd)` (0x4201e1bc): `W 0x0A = cmd`; read 0x2D (STATUSINT) and repeat (1 ms apart, at most
100 extra reads) until bit 7 = 1; `W 0x0A = 0x00`; read 0x2D until bit 7 = 0 (same limit).
(Threshold units and window meanings: datasheet, INFERRED; bytes CONFIRMED.)

### 5.2 Normal operation: `qmi_normal_init` (0x4201e730 -> 0x4201e6a8), CONFIRMED

Runs only if the sensor-type byte 0x3fc9aee6 == 4 (its .data value).

```
R 0x00 (WHO_AM_I) until it reads 0x05: up to 5 reads, and the whole attempt twice (leftover of the
       two I2C addresses 0x6a/0x6b at 0x3c11a94f); if never: puts("qmi8658_init fail"), stop
ctrl8_value = 0xC0
W 0x02 = 0x60        CTRL1: address auto-increment, big-endian, INT1/INT2 outputs disabled
R 0x01 x1            revision
R 0x49 x3            firmware id
R 0x51 x6            UUID
W 0x08 = 0x00        CTRL7: all sensors off
W 0x09 = 0xC0        CTRL8: CTRL9 handshake via STATUSINT.bit7, activity INT -> INT1, any-motion OFF
block M (5.1)
W 0x08 = 0x00 ; delay 1 ms
W 0x03 = 0x06        CTRL2: accel +-2 g (16384 LSB/g), ODR code 6
R 0x06 ; W 0x06 = (value & 0xF0)          CTRL5: accel LPF off
W 0x04 = 0x76        CTRL3: gyro +-2048 dps (16 LSB/dps), ODR code 6
R 0x06 ; W 0x06 = (value & 0x0F)          CTRL5: gyro LPF off
W 0x08 = 0x03 ; delay 1 ms                CTRL7: accel + gyro on
R 0x02 x8            dump CTRL1..CTRL8 to the log
```
Called at boot in `brush_gpio_cfg`, again 500 ms into the main task (0x4201ccb5), and in both
wake functions.

### 5.3 Sleep: any-motion mode `qmi_amd_mode` (0x4201e74c -> 0x4201e6dc -> 0x4201e598(1,1,1)), CONFIRMED

```
puts("QMI8658 AMD MODE3..");  (only continues if sensor type == 4)  puts("QMI8658 AMD MODE4..");
block M (5.1)
ctrl8_value = (ctrl8_value & ~0x02) | 0x40 ; W 0x09 = ctrl8_value (0xC0) ; delay 2 ms
W 0x08 = 0x00 ; delay 1 ms
W 0x08 = 0x00 ; delay 1 ms                (config_reg starts by disabling again)
W 0x03 = 0x2E        CTRL2: accel +-8 g, ODR code 0xE (low-power ODR, 11 Hz class)
R 0x06 ; W 0x06 = (value & 0xF0)
(gyro not configured: enabled sensors = accel only)
R 0x02 ; W 0x02 = (value | 0x08)          CTRL1: enable INT1 output  (0x60 -> 0x68)
ctrl8_value |= 0x02 ; W 0x09 = ctrl8_value (0xC2) ; delay 1 ms       any-motion ON
W 0x08 = 0x01 ; delay 1 ms                CTRL7: accel only
```
Log markers "QMI8658 AMD MODE a.." .. "c7.." are printed between the steps.

### 5.4 Power-down `qmi_power_down` (0x4201e6f0), CONFIRMED

```
W 0x60 = 0xB0 ; delay 10 ms               soft reset
W 0x02 = 0x60 ; W 0x03 = 0x0F ; W 0x04 = 0x0F ; W 0x08 = 0x00
R 0x02 x8 (log) ; delay 10 ms
```
After this INT1 is disabled, so GPIO8 is not driven (INFERRED from CTRL1 = 0x60).

### 5.5 Use of the IMU while awake

* Any-motion detection and the INT1 output are off (CTRL8 = 0xC0, CTRL1 = 0x60) and the GPIO8 ISR
  is removed: there is **no lift / raise detection while the screen is on**. CONFIRMED.
* An esp_timer named "Raise_the_bright_screen_timer" (handle 0x3fca4ea4, callback 0x4201b5cc ->
  event 0x02) is created (0x4201bb98) but never started (only `esp_timer_stop` references).
  CONFIRMED dead code.
* The `brush_toolbox` task (0x42014d0c) reads accel (0x420145e0) and gyro (0x420146b0) samples
  through a sensor-ops table (0x3c11928c..): status register 0x2E bits 0..1, 12 data bytes from
  0x35; axis mapping in 0x4201e770/0x4201e79c: out_x = -raw_y, out_y = raw_x, out_z = -raw_z; gyro
  zero-offset calibration ("gyro off ...") and a +-3 LSB dead band. This feeds brushing-zone /
  posture tracking - see the brushing spec. CONFIRMED that the data path exists; algorithm not
  analysed here.
* Die temperature: register 0x33, 2 bytes, value / 256 degC (0x4201e240), used by the charge
  thermal protection.

### 5.6 "guitai" / "IntoShow"

"guitai" (Chinese for shop counter; the cloud message is logged as "MESSAGE_GET_COUNTER_MODE",
0x42011a60) is a **retail display mode**, not a stand sensor. Flag 0x3fca4dca, persisted as the
1-byte NVS blob "IntoShow". Enabled by BLE command category 2 / id 0xA0 with a non-zero byte
(0x4201014c, table entry 0x3c117d50) or by the cloud message 0x15; read back with category 3 /
id 0xA0 (0x420101d0). While set: UI mode 0xD7, the idle check is skipped ("get_guitai_show_mode"),
so the brush never sleeps on battery; it leaves the mode by itself when the battery is below 11 %
(0x4201b1e4) or on a button press ("close guitai", 0x4201c790). CONFIRMED.

---

## 6. PM locks, Wi-Fi and BLE around sleep

### 6.1 `esp_pm_configure` and the locks (CONFIRMED)

* `esp_pm_configure(&(esp_pm_config_t){ .max_freq_mhz = 160, .min_freq_mhz = 40,
  .light_sleep_enable = true })` - constants at 0x3c118b2c, call at 0x42013fd0, once at boot.
* `brush_pm_init` (0x42014000) creates `l_apb` (ESP_PM_APB_FREQ_MAX, handle 0x3fca41b0), `l_ls`
  (ESP_PM_NO_LIGHT_SLEEP, 0x3fca41ac), `l_cpu` (ESP_PM_CPU_FREQ_MAX, 0x3fca41a8), then acquires
  `l_apb` and `l_ls`, and creates the one-shot `sleeo_timer` ("periodicsleep", 0x3fca419c).
* `l_ls` is never released -> no automatic light sleep at any time.
* `l_apb`: released by `brush_resume_sleep` (screen-off stage), re-acquired by `brush_stay_alive`
  (wake). Effect: in the screen-off stage the CPU may scale down to 40 MHz when idle.
* `l_cpu`: acquired at brushing start (0x4201c790) and during OTA/HTTP downloads (0x42013b40,
  0x42013e24); released at session end (0x4201ce36) and after OTA.

### 6.2 Wi-Fi (CONFIRMED)

* Boot (`app_main`): netif + default STA, `esp_wifi_init`, `esp_wifi_set_mode(WIFI_MODE_STA)`;
  `esp_wifi_start()` **only if an SSID is stored** (`esp_wifi_get_config` ssid[0] != 0, 0x4200bb40)
  and `cfg[0x34] != 11`; `esp_wifi_set_ps(WIFI_PS_MAX_MODEM /*2*/)`.
* `WIFI_EVENT_STA_START` -> `esp_wifi_connect()` if the "want connection" flag 0x3fc9a1e1 is set
  (.data initial 1). On a failed connect the stored AP list is cycled (round counter 0x3fca2ada,
  give up when it exceeds 2) and then `esp_wifi_stop()`; after a successful connection the
  counter is set to 0xff, so a later disconnect stops Wi-Fi at once (0x4200bbac).
* So stock connects on every wake when provisioned, and uses the link for the OEM cloud (time,
  OTA check, record upload, counter-mode message).
* Screen-off stage: counter 0x3fc9a1e0 is zeroed in the screen-off sequence and incremented once
  per second while `asleep`; when it reaches **27**: `esp_wifi_disconnect()` (if connected) and
  `esp_wifi_stop()` (0x4200bae0).
* Wake from the screen-off stage: `esp_wifi_reinit` (0x4200bb5c): flag 0x3fc9a1e1 = 1; if not
  connected and an SSID is stored -> `esp_wifi_start()`; if still connected -> queue the cloud jobs
  (0x0b, 4, 5, 0x15).

### 6.3 BLE (CONFIRMED)

* Boot: `esp_bt_controller_mem_release(CLASSIC)`, controller init/enable (BLE), bluedroid + blufi
  + the GATT service; advertising starts on the blufi INIT_FINISH event (0x4200b830 ->
  0x4204e55c, device name "Oclean X Ultra 20").
* BLE connect: advertising stopped, event 0x4000 posted. BLE disconnect: advertising restarted.
* Screen-off stage: advertising / the connection stay up for the BLE window (30 s / 120 s,
  2.4 step 14), extended by GATT activity.
* Window expiry: `esp_ble_gap_stop_advertising()` then deep sleep. Wake from screen-off:
  advertising restarted if not connected.
* First boot after factory reset: advertising is stopped right away (0x4201ccd3).

---

## 7. Modes to stay clear of

| Mode | Trigger | Effect on power | Evidence |
|---|---|---|---|
| Factory reset -> shipping sleep | BLE command `09 ED EF` (0x420111d0) or the button long-press reset (`0x4201c6b8`): sets `cfg[0x34] = 11` (NVS "sys_config" byte 0) and reboots | next boot: idle timeout 8 s, advertising off, `button_long_reset_flag = 1` -> at screen-off `brush_pm_control(0)` takes path X: IMU powered down, "deep sleep1" immediately (no BLE window, no motion wake). `cfg[0x34]` is rewritten to 0, so the following wake shows the welcome screen | 0x4201ccc1..0x4201ccf9, 0x4201c5d0. CONFIRMED |
| Battery empty | `battery_cap == 0` | same path X; additionally the boot-time 3299 mV test (4.1) | CONFIRMED |
| Production mode | `quit_Production_test_flag == 2`. The flag is byte 0 of the NVS blob "production_test" (0x42027c98; a stored 0 is turned into 1 = normal retail state); 2 is written by the UART command "OPEN_PRODUCT_MODE" (0x42027d2c) | main-screen timeout 60 s, path X at screen-off, low-battery step skipped | CONFIRMED |
| "laohua" (aging / burn-in) | production mode + flag 0x3fca4e91 == 1 (3 s button hold in production mode, "cooker mode" 0x42019bc0) | runs the motor in cycles (counters against 27000 and 3000 ten-ms ticks, up to 12 rounds); "quit laohua" when the battery voltage word is <= 3200 (0xc80); buttons ignored | 0x4201cef9..0x4201d04c. CONFIRMED (cycle meaning INFERRED) |
| Counter ("guitai") mode | 5.6 | never sleeps on battery | CONFIRMED |
| `cfg[0x70] != 0` | only from a stored config blob | idle sleep blocked | 0x4201d402. CONFIRMED |

A re-implementation avoids all of them simply by not implementing these flags; the only one a
user can hit through the phone app is the factory reset.

---

## 8. Implementation checklist for the custom firmware

1. Fix the pin roles: charger present = `GPIO9 == 0`; motion INT = GPIO8 (active high).
2. Idle timer with the values of 2.3 (30 s on the main screen, 10 s after a session, 60 s at boot,
   low-battery +3 s step), checked at 1 Hz, blocked while brushing / on the charger.
3. Screen-off stage (2.4): IMU to any-motion mode (5.3) unless the count is over the limit, LCD
   SLPIN, backlight/LEDs off, GPIO table 3.2 (left column), GPIO8 ISR on, start the 30 s / 120 s
   window. Wake from this stage by button, motion (if `cfg[8]`), BLE connect, charger (4.2).
4. Deep sleep (2.5, 3.1, 3.2 right column): "deep sleep2" normally; "deep sleep1" plus IMU
   power-down (5.4) when motion wake is off or the count is > 4. Never on the charger.
5. Boot (4.1): wake-cause dispatch, 3299 mV test, motion-wake counter (limit 4, cap 20, reset by
   button), `allow_anymotion_check_flag` 5 s window.
6. `gyro_wakeup_count` in RTC memory (stock also mirrors it to NVS "wakeupcount" byte 0).
7. On the charger: no sleep; backlight off after 30 s; button turns it on again.

Stock stops Wi-Fi 27 s after screen-off and has no web server/MQTT; how long the custom firmware
keeps its radios up before deep sleep is a product decision outside this spec.

---

## 9. Open questions

1. **GPIO8/GPIO9 electrical confirmation.** The role assignment (GPIO8 = IMU INT1 active high,
   GPIO9 = charger active low) is CONFIRMED from code in five independent places, but nobody has
   measured the lines. One on-device log of `gpio_get_level(8)` / `gpio_get_level(9)` on and off
   the charger would close it.
2. **Any-motion INT1 behaviour** (pulse vs. level, how long GPIO8 stays high after motion): stock
   never reads STATUS1 (0x2F) to clear it and relies on EXT0 level-high wake plus any-edge GPIO
   interrupts. Not determinable from the firmware.
3. **Digital pad state during deep sleep.** Stock uses only per-pin `gpio_hold_en` (IDF says this
   does not hold digital pads GPIO26..48 through deep sleep on the S3 without
   `gpio_deep_sleep_hold_en`, which stock does not call), and `gpio_force_unhold_all()` was called
   earlier in the screen-off stage. So the real level of GPIO37/41/42/45/48/26/38..40 during stock
   deep sleep is unknown; replicating the same calls in the same order reproduces whatever stock
   gets.
4. **SPI pins before sleep.** `gpio_config` + `gpio_set_level(…,1)` are applied to pins still owned
   by the SPI drivers (IMU 4..7 before deep sleep, LCD 38..40); whether the pads actually reach
   level 1 depends on the GPIO-matrix routing left by the driver.
5. **Pull-ups in deep sleep** on GPIO3 / GPIO9: no RTC pull-up is enabled by stock; external
   resistors are INFERRED, not seen.
6. **QMI8658 SPI read framing in stock**: 0x4200ce60 sends the register byte without bit 7 and
   reads the data in a second transaction. This does not match the datasheet read protocol that the
   custom firmware uses successfully (`reg | 0x80`, single transaction). Not resolved; it does not
   affect the register values above.
7. **GPIO37 function** (driven 0 awake, 1 asleep) - which rail it switches is not visible in code.
8. **ODR code meanings** (accel/gyro code 6, low-power code 0xE) and the threshold/window units
   are datasheet interpretations; the register bytes are exact.
9. `cfg[0x70]` and the weak stub 0x4210225c: looks like a removed feature; the only observable
   effect is "sleep blocked while non-zero".
10. Wake-screen selection details (0x42029218 / 0x420291a4 / 0x4201d794) and the exact
    post-session screen timings belong to the UI spec; only the idle-timer interactions were
    traced here.

# Oclean X Ultra 20 (ESP32-S3) — Custom Firmware Hardware Map & Reimplementation Plan

> **Superseded first-pass map — do not trust in isolation.** This was the initial
> reverse-engineering pass. A later, deeper pass (the specs in `re/spec/` and the port in
> `main/`, now confirmed on the running device) corrected several items here. Known errors
> below: **GPIO8 is the IMU any-motion interrupt and GPIO9 is charger-present (active low)** —
> the opposite of what some sections imply; **GPIO26 blocks charging when driven high and
> allows it when left as input** (not "active-high enables"); **GPIO45 is always driven 0**;
> the **backlight is LEDC channel 4 on GPIO21 (active low)**, not GPIO12; the touch controller
> is an **Azoteq IQS7222D at 0x44 on the bit-bang bus SCL13/SDA14** and the **AW8686X force
> sensor shares that bus**; the **QMI8658 IMU is on SPI3** (MISO4 MOSI5 SCLK6 CS7). For the
> authoritative, hardware-confirmed map see the Hardware table in the top-level `README.md`
> and the `re/spec/*.md` specs. The pin inventory and the display/motor/ADC sections below are
> still useful; the role/polarity notes are not.
>
> Reverse-engineered from stock `ota.bin` (project `blufixx_9_V2`, derived from `ulp_riscv_blufi_example`, ESP-IDF v5.1.1). Every value below is taken directly from the per-subsystem extraction; confidence is carried through from the evidence. Anything below "confirmed" is also listed in Section 3 (UART-confirm) because a wrong pin can damage hardware.

---

## 1. Hardware Map

### 1.1 GPIO map (ESP32-S3)

| GPIO | Function | Direction / mode | Pulls / level / intr | Subsystem | Confidence |
|---|---|---|---|---|---|
| **1** | VBAT sense = ADC1_CH0 | INPUT | no pull, no intr | Battery | confirmed |
| **2** | Aux interrupt in — likely QMI8658 gyro INT (or 2nd button / VBUS) | INPUT | NEGEDGE, ISR added (handler 0x40377d88) | Buttons / IMU | **guess** (role) / confirmed (pin+mode) |
| **3** | Primary power/key button | INPUT | pull-up ON, ANYEDGE, ISR added, **active-LOW** | Buttons / PM | confirmed |
| **8** | Charger/dock-present detect | INPUT | no pull, ANYEDGE, ISR added; EXT0 wake level=HIGH | Buttons / PM | **likely** (role) / confirmed (pin+mode) |
| **9** | QMI8658 motion (any-motion) INT / 2nd wake line | INPUT | ANYEDGE, no pull, **no runtime ISR**; EXT1 wake (active-low) | PM / IMU | **likely** (role) / confirmed (pin+mode) |
| **10** | NTC thermistor = ADC1_CH9 | INPUT | no intr | Battery | confirmed |
| **12** | OUTPUT, init level 0 | OUTPUT | pull-up=1, set 0 | **Backlight candidate** (weak) | confirmed (pin) / unknown (role) |
| **13** | Ambiguous — OUTPUT at device-init 0x4200cb84, re-touched in gpio_cfg | OUTPUT? | — | unknown | **guess** |
| **14** | Ambiguous — same as GPIO13 | OUTPUT? | — | unknown | **guess** |
| **17** | Indicator LED — LEDC ch0 | OUTPUT | pull-up=1, init 1; LEDC inv=1 | Motor/LED | confirmed |
| **18** | Indicator LED — LEDC ch1 | OUTPUT | pull-down=1, init 1; LEDC inv=1 | Motor/LED | confirmed |
| **19** | Indicator LED — LEDC ch2 | OUTPUT | no pull, init 0 | Motor/LED | confirmed |
| **20** | Indicator LED — LEDC ch3 | OUTPUT | init; software-inverted (active-low) | Motor/LED | confirmed |
| **21** | Indicator LED — LEDC ch4 | OUTPUT_OD (mode 6) **in sleep variants only** | reset in boot, driven in sleep fns | Motor/LED | confirmed (ch map) / likely (OD role) |
| **26** | CHARGE_EN (master charge enable + over-temp cutoff) | OUTPUT (push-pull) | active-HIGH enables | Battery / PM | confirmed |
| **33** | I2S BCK (motor audio) | OUTPUT | board default | Motor | **likely** |
| **34** | I2S DATA_OUT → amp → motor coil | OUTPUT | board default | Motor | **likely** |
| **35** | I2C0 SCL | OUTPUT/OD | external pull-up | Pressure/I2C | confirmed |
| **36** | I2C0 SDA | OUTPUT/OD | external pull-up | Pressure/I2C | confirmed |
| **37** | OUTPUT, init 0 (last pin configured) | OUTPUT | set 0 | unknown rail | confirmed (pin) / unknown (role) |
| **38** | LCD CS (hardware CS) | OUTPUT | — | Display | confirmed |
| **39** | LCD SCLK | OUTPUT | — | Display | confirmed |
| **40** | LCD MOSI/SDA | OUTPUT | — | Display | confirmed |
| **41** | LCD D/C (low=cmd, high=data) | OUTPUT | init 1 | Display | confirmed |
| **42** | LCD RST (active low) | OUTPUT | init 1 | Display | confirmed |
| **45** | WLC_EN (wireless-charge enable); also power/latch rail (gpio_hold_en) | OUTPUT | cascades into CHARGE_EN | Battery / PM | **likely** |
| **47** | I2S WS (motor audio) | OUTPUT | board default | Motor | **likely** |
| **48** | OUTPUT, init 0; setter called from many power/charge sites | OUTPUT | set 0 | unknown (power) | confirmed (pin) / unknown (role) |
| **5/6/7** | OUTPUT+pull-up — likely LEDs/indicator (sibling init fn 0x4200cd54) | OUTPUT | pull-up=1 | unknown | **likely** |
| MCLK / MISO / I2S DATA_IN | **−1 (unused)** | — | — | Motor/Display | confirmed |

Full `brush_gpio_cfg` union mask (confirmed): `0x00012620041E770E` = GPIOs {1,2,3,8,9,10,12,13,14,17,18,19,20,26,37,41,42,45,48}.

### 1.2 ADC channels (ADC_UNIT_1, legacy driver)

| Signal | Unit / channel | GPIO | Atten | Width | Notes | Confidence |
|---|---|---|---|---|---|---|
| VBAT | ADC1_CH0 | GPIO1 | DB_11 (val 3; =DB_12 in 5.x) | 12-bit | `mV = raw_to_voltage(raw,&chars) * 2` (1:1 divider); 2 samples averaged | confirmed |
| NTC | ADC1_CH9 | GPIO10 | DB_11 | 12-bit | **Configured but never read at runtime** — temp comes from IMU reg 0x33 instead | configured confirmed / "unused" **likely** |

Calibration: `esp_adc_cal_check_efuse()` gated before `esp_adc_cal_characterize(UNIT_1, DB_11, WIDTH_12, default_vref=0, &chars@0x3fca2b70)`. A second characterize (unit=1, chars@0x3fca2b4c) exists but is unused by the battery path.

### 1.3 Buses & peripherals

| Peripheral | Config | Confidence |
|---|---|---|
| **SPI2_HOST (LCD)** | MOSI=40, SCLK=39, MISO=−1, CS=38, DC=41, RST=42; 30 MHz; mode 0; queue 7; DMA auto; max_xfer 1600 | confirmed (mode 0 = likely) |
| **I2C0** | MASTER, SDA=36, SCL=35, 100 kHz, internal pull-ups DISABLED, clk_flags=0 | confirmed |
| **I2S0 (motor)** | BCK=33, WS=47, DATA_OUT=34, MCLK=−1, DATA_IN=−1 | **likely** (board defaults) |
| **LEDC** | TIMER_0, LOW_SPEED, 5000 Hz, 13-bit; 5 ch → GPIO17–21; ch0/ch1 output_invert=1, ch3/ch4 SW-inverted | confirmed (drives **LEDs**, not motor) |

### 1.4 Named chips

| Role | Part | Address / bus | Confidence |
|---|---|---|---|
| Force/pressure sensor | **Awinic AW8686X** | I2C0 @ 7-bit **0x6A**, WHOAMI reg 0x00 = 0x61/0x62/0x64 | confirmed |
| IMU (motion wake) | **QST QMI8658** 6-axis | I2C0 (AMD any-motion engine) | confirmed |
| Display controller | **ST7735/ST7735S** (4 variants) + one non-ST7735 "3022" variant | SPI2 | confirmed (3022 part = guess) |
| Motor | voice-coil/audio transducer driven via I2S + external amp | I2S0 | confirmed (mechanism) |
| 2nd I2C device | unknown @ 7-bit **0x44** (not pressure; Azoteq IQS cap-touch strings present) | I2C0 | **likely** |

---

## 2. Per-Subsystem Reimplementation Plan (ESP-IDF v5.1.1)

The existing project already carries WiFi/web/MQTT + full BLE protocol knowledge — reuse it. Rebuild only the HAL/driver layer below.

### 2.1 Motor — **NOT LEDC. It is an audio transducer via I2S.**

This is the single most important correction: the brush motor is a voice-coil actuator driven by the ESP-ADF audio pipeline. "Music through the motor" is literal — the coil *is* the speaker. The 5 LEDC channels are indicator LEDs.

**Chain to replicate:** FreeRTOS music task (`brush_music_app`) owns an ESP-ADF `audio_pipeline`: raw/fatfs stream → `mp3_decoder` → `i2s_stream_writer` → I2S0 → external amp → motor coil.

- **Command path:** producer API packs `{mode/gear, time}` into globals, then `xTaskNotify(music_task, bit, eSetBits)`. Bit **15 (0x8000)** = start/play; bit **16 (0x10000)** = stop. Task waits on `xTaskNotifyWait`. Reproduce with one task + a 32-bit notify bitmask.
- **Two modes:** `PLAYWAVE` (loop fixed PCM/tone for normal brushing) and `PLAYMUSIC` (decode selected MP3 by `music_index`).
- **Speed/pitch** = `i2s_stream_set_clk(el, sample_rate, bits, ch)` at runtime. **Intensity** = PCM amplitude scaling.
- **Gear mapping** is to waveform + sample rate, NOT to an LEDC duty. Default gear→value table gear1=51,2=52,3=1,4=24,5=32 (confidence: guess — per-mode params copied from 21-byte DROM default tables at 0x3c1196ee/0x3c119716/0x3c11973e).
- **Minimal custom FW:** skip MP3; push a sine/square PCM buffer at ~250–330 Hz fundamental into `i2s_stream_writer`; gear = amplitude + frequency preset. New code may use `i2s_std` instead of the legacy driver the image links.

**I2S pins (verify before driving the coil):** BCK=33, WS=47, DATA_OUT=34, MCLK/DIN=none. These are stock `esp32_s3_box_lite` defaults and may be board-overridden; there is **likely an amp-enable GPIO not yet located**.

**LEDC (indicator LEDs — don't break them):** `ledc_timer_config`: TIMER_0, LOW_SPEED, 5000 Hz, 13-bit, AUTO. 5× `ledc_channel_config`, all timer 0: CH0=17, CH1=18, CH2=19, CH3=20, CH4=21. CH0/CH1 `output_invert=1`; CH3/CH4 software-inverted (write `8191-duty`, active-low). One setter clamps CH0/1/2 duty to ≤4000. Per-LED state flags at DRAM 0x3fc9abb4..b9.

> Note: `dac_data`/`dac_coef`/`offset_mv` strings belong to the **sensor** calibration routine (0x42026b60), not the motor — do not conflate with motor duty.

### 2.2 Battery / NTC (legacy ADC)

The firmware uses the deprecated legacy ADC (`adc1_config_width`, `adc1_config_channel_atten`, `adc1_get_raw`) + deprecated `esp_adc_cal`. Both still compile in 5.1.1 for a 1:1 port; the modern path is `esp_adc/adc_oneshot.h` + `adc_cali` curve-fitting (S3 supports it).

**Voltage + percent algorithm to replicate:**
1. `raw = adc1_get_raw(ADC1_CHANNEL_0)`.
2. `mV = esp_adc_cal_raw_to_voltage(raw, &chars) * 2` — **keep the ×2** (1:1 external divider). Characterize with `(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 0, &chars)` after `check_efuse()` OK.
3. Sample twice ~10 ticks apart, average `(s1+s2)/2`.
4. `idx = clamp(mV/100 − 29, 5, 11)`; linearly interpolate between a voltage-breakpoint table and a percent table at `[idx−1]..[idx]`. **Exact table literals NOT decoded** — recover from stack-fill in fn 0x42017ca0 if an exact curve is needed; index math + interpolation are confirmed.
5. Thermal protection: `temp ≥ 72.0 °C → CHARGE_EN=0`; `temp < 67.0 °C → CHARGE_EN=1` (hysteresis), using the **IMU temperature** (reg 0x33, scaled ÷256), not the NTC.

**GPIO:** CHARGE_EN=GPIO26 (push-pull, active-high). WLC_EN=GPIO45 (cascades into CHARGE_EN, **likely**). No charge-status/USB-detect input GPIO found — USB/charge state is a firmware flag `[0x3fca4b6c+8]==2`.

### 2.3 Buttons / touch

**There is NO cap-touch peripheral** — do not use `touch_pad_*`. The `266_touch` counter belongs to the pressure-sensor gesture code.

- Pins: GPIO3 (primary, pull-up, ANYEDGE, active-low), GPIO9 (secondary/wake, ANYEDGE, no pull), GPIO8 (detect, ANYEDGE, treated active-high), GPIO2 (NEGEDGE, likely gyro INT). GPIO1/10 input no-intr.
- Pattern: `gpio_install_isr_service(0)`; `gpio_config` each; `gpio_isr_handler_add(pin, key_isr, (void*)pin)` for 3/8/2; ISR does `xQueueSendFromISR(key_queue,...)`.
- Task **`key_int`**, fn 0x4201b560, **prio 30 (highest)**, stack 2048, queue 0x3fca4df4: `xQueueReceive(portMAX_DELAY)` → debounce/duration FSM reading `gpio_get_level`.
- Press timers (confirmed): esp_timers at **2s / 3s / 5s / 8s**. Short (<2s) = toggle brushing (`handle_button_short_press`, with OTA-lock check). Escalating holds → suspend/resume-by-profile, enter-OTA, reboot (`BTN_REBOOT_PRESS`), factory reset (`button_long_reset_flag=true`). Exact level→seconds mapping **not fully traced**.
- Debounce guard: per-pin counter must reach ≥5 ticks before acting.

### 2.4 Display — ST7735/ST7735S 80×160 RGB565 on SPI2

Legacy `spi_master`. 4 variants selected at boot from a 2-byte panel-id at DRAM 0x3fca5e3d → `(3,4)=han2, (3,5)=huaersheng_0305, (4,5)=huaersheng, (5,6)=huaersheng_3022`, else default `huaersheng_0305`. **For a clean reimpl, pick 0305 (default) unless you read a different id.**

**Bus/device:** `buscfg{mosi=40,miso=−1,sclk=39,quadwp=−1,quadhd=−1,max_transfer_sz=1600}`; `spi_bus_initialize(SPI2_HOST,&buscfg,SPI_DMA_CH_AUTO)`; `devcfg{mode=0,clock_speed_hz=30e6,spics_io_num=38,queue_size=7}`.

**Byte I/O:** DC (GPIO41) toggled manually before each transfer, one byte via `spi_device_polling_transmit` (length=nbytes*8). cmd={DC=0; tx}, data={DC=1; tx}.

**Reset + init:** DC=1; RST=1; RST=0 (10ms); RST=1 (10ms); delay 120ms; run selected table; DISPON in table; FILL(0,0,80,160)=black. MADCTL(0x36)=0xC8, COLMOD(0x3A)=0x05. Column offset **+24** (runtime set_window) / +26 (han2 hardcoded) — confirm on panel. Window = CASET(0x2A)+RASET(0x2B)+RAMWR(0x2C), 16-bit pixels big-endian.

**Default table — huaersheng_0305 (recommended):**
```
0x11; delay 120ms
0xB1: 05 3A 3A   0xB2: 05 3A 3A   0xB3: 05 3A 3A 05 3A 3A   0xB4: 03
0xC0: 64 04 84   0xC1: C5   0xC2: 0D 00   0xC3: 8D 2A   0xC4: 8D EE   0xC5: 0E
0xE0: 15 0B 02 00 08 00 00 00 00 05 11 35 10 12 05 3F
0xE1: 0E 0E 03 00 06 00 00 00 00 06 12 37 10 10 06 3F
0x3A: 05   0x36: C8   0x29 (DISPON)
```
(Other three tables — huaersheng, han2, huaersheng_3022 — are fully recovered in the extraction and available if the panel-id differs. The 3022 variant is a **non-ST7735 controller (part # guessed)** that dims via cmd 0x51; identify the real controller before using it.)

**Low-level primitives (confirmed addrs):** write_cmd=0x4202875c, write_data=0x42028778, spi_xfer=0x4200d04c, reset=0x42028734, set_window=0x42028794, fill=0x42028848.

**Backlight pin: UNKNOWN** — not provable from the binary. GPIO12 is the weak candidate. **Scope on hardware.**

### 2.5 Pressure sensor (AW8686X) + I2C

```c
i2c_config_t conf = {
  .mode = I2C_MODE_MASTER, .sda_io_num = 36, .scl_io_num = 35,
  .sda_pullup_en = GPIO_PULLUP_DISABLE, .scl_pullup_en = GPIO_PULLUP_DISABLE,
  .master.clk_speed = 100000, .clk_flags = 0,
};
i2c_param_config(I2C_NUM_0, &conf);
i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
```
Deinit: `i2c_driver_delete(0); gpio_reset_pin(36); gpio_reset_pin(35)`.

**AW8686X:** 7-bit addr **0x6A** (write 0xD4 / read 0xD5; firmware shifts the 7-bit addr). 8-bit register pointer + N data bytes (standard write-then-read). Probe WHOAMI reg 0x00, accept **0x61/0x62** (0x62 checked in pressure path), 0x64 variant branch exists; retried ≤4×. Early init writes: reg 0x1B=0x00, reg 0x16=0x40 (**likely**). Full FTC/DAC/flash-cal + SW algorithm layer exists; **for basic pressure, read raw force registers and skip FTC.**

**Sampling layer:** periodic esp_timer `pressure_sensor_timer` polls the sensor → over-pressure detector; `0x5A5A` magic sentinel in parsed buffer; exposed via `brush_notify_task_start_sensor`/`stop_sensor`.

Modern driver equivalent (i2c_master.h): bus `{port=0,sda=36,scl=35,clk=DEFAULT,enable_internal_pullup=false}`, device `{scl_speed_hz=100000, device_address=0x6A}`.

> A second unrelated device at 0x44 and Azoteq IQS cap-touch strings share the bus — keep them distinct from AW8686X.

### 2.6 Power / sleep — **no ULP program despite the blufi heritage**

**PM init:** 3 `esp_pm_lock_create` → `l_apb`(APB_FREQ_MAX), `l_ls`(NO_LIGHT_SLEEP), `l_cpu`(CPU_FREQ_MAX). Hold APB; acquire/release ls+cpu around active work. `enter_auto_light_sleep` → `esp_pm_configure(&{max,min,light_sleep_enable=true})`.

**ULP: NOT used.** No RISC-V segment in the ELF; the "initializing it" branch only logs — no `ulp_riscv_load_binary`/`run`. Motion sensing during sleep is the **QMI8658's own hardware any-motion INT**, not ULP polling. Do **not** port a ULP program.

**RTC-retained state** (`RTC_DATA_ATTR`, RTC_SLOW_MEM ~0x50001000): `gyro_wakeup_count` @0x50001022 (debounce ≥5 → treat as spurious & re-sleep; cap ~19; cleared on key press), `count_motor_work`/`count_wakeup_gyro`/`count_wakeup_button`, battery record.

**Wake sources:** EXT1 mask **0x208 = GPIO3|GPIO9**, mode 0 = `ESP_EXT1_WAKEUP_ANY_LOW`; EXT0-style single pin **GPIO8 level=1** (charger); TIMER; cold/default.

**Wake dispatch** in app_main: `esp_sleep_get_wakeup_cause()` → EXT1 (`get_ext1_wakeup_status` → GPIO3=button-on / GPIO9=gyro: ++count, re-sleep if over limit), EXT0=charger UI, TIMER=housekeeping, default=full init.

**Two deep-sleep paths:** "deep sleep1" enables EXT1 (button+motion wake); "deep sleep2" = ship/force-sleep, EXT1 NOT enabled (only GPIO8/charger + reset wake). `brush_pm_control(forcesleep)` gates on `battery_cap`, `get_motor_wakeup_flag()`, `button_long_reset_flag`, `quit_Production_test_flag`, `forcesleep`. Mask the GPIO9 interrupt before sleeping (firmware clears bit 9 in `brush_resume_sleep`) to avoid races.

### 2.7 BLE GATT app + task layout

**Tasks** (`xTaskCreatePinnedToCore`, all `tskNO_AFFINITY`):

| Task | fn | stack | prio |
|---|---|---|---|
| key_int | 0x4201b560 | 2048 | **30** |
| brush_app (main) | 0x4201cc40 | 3072 | 3 |
| brush_toolbox | 0x42014d0c | 3072 | 3 |
| brush_decoder_app | 0x4201eca4 | 3072 | 3 |
| brush_music_app | 0x4201f24c | 3072 | 3 |
| sleep_brush_task | 0x4201b900 | 4096 | 3 |
| show_lock_ui_task | 0x42028544 | 4096 | 3 |
| uart_rx_task | 0x42027f9c | 3072 | 3 |
| http_test_task (OTA/upload) | 0x420135e0 | 9216 | 1 |

**GATT dispatch** (stay app-compatible): write payload = `byte[0]=category, byte[1]=sub-opcode, rest=payload`. Route on category:
- cat 0x01 → single handler 0x42010c64
- cat 0x02 → Table C (DROM 0x3c117c28)
- cat 0x03 → Table B (0x3c117bb0, 15 entries) — status/sessions
- cat 0x07 → Table A (0x3c117a40, 46 entries) — large config/diagnostic set
- cat 0x09 → raw `0x09 0xED 0xEF` = **factory reset**

Tables = arrays of `{u32 id, u32 handler}` (8-byte entries), linear-searched, `addx8 + l32i+4 + callx8(arg=&payload[1])`. Confirmed opcodes: 0x0201→0x42010cf0, **0x0206→0x420108cc (set brush mode: low-5-bit field + high-3-bit field, level≤13)**, 0x0209, 0x020C, 0x020F, 0x0303 (status)→0x4200ef8c, **0x0307→0x42010468 (dump brush records, framed with category-echo + 'O'(0x4F)/'K'(0x4B))**.

Services: custom brush service (`BAT_GATTS_TABLE_DEMO`) + standard DIS. 128-bit UUIDs (service `8082caa8...`, chars `bb85/86/89/90`) are assembled at runtime — declare them statically.

### 2.8 Storage (two tiers)

1. **NVS** namespace **`storage`** (NVS_READWRITE). Keys (confirmed cluster): `wakeupcount, shuanhuan, sensor_config, screen_config, aw8686x_config, binding_wifi, rec_temperature, production_test, hardware_verson, wifi_list, mag_cli, mag_zone, mag_clilev, gesture_cli, user_config, motor_data, time_config, sys_config, brush_battery, voice_version, ui_version, http_domain`. Treat each as an opaque packed blob.
2. **Raw flash partition `brushdata`** (type=data) for session records via `esp_partition_read/erase/write` (NOT NVS). Record format: `[2-byte big-endian header][payload ≤182 bytes (0xB6)]`. RAM mirror `brush_data_store_if` @0x3fca4dbc: `total_num`, `total_byte`. Caps: ≤33 records per enumeration pass, **hard cap 121** before forced flush; partition reader allocates 91×64=5824-byte buffer.

**Commit flow:** BLE 0x0307 streams records (20-byte MTU slices) framed with 'O'/'K'; on app ACK → "brush data commit confirm ok" → erase partition + zero counters. Cloud path POSTs to `http(s)://<http_domain>/OTA/v4/V1Brush/UploadBrushRecord` (host from NVS `http_domain`). **For custom FW you can implement only the BLE 0x0307 upload + clear and ignore cloud.**

> Exact `esp_partition_find_first` type/subtype and on-flash record sub-fields beyond the 2-byte header + 182-byte cap are **not pinned** — verify against a live flash dump.

---

## 3. MUST Confirm Over UART / On Hardware Before Flashing

Getting any of these wrong can brick or physically damage the device. Dump and verify before the first flash.

**Hard blockers (do before anything):**
1. **Real partition table** — `esptool.py read_flash 0x8000 0xC00 ptable.bin` → `gen_esp32part.py`. Exact offsets/sizes are NOT in the image. Must contain: `nvs`, `otadata` (0x2000), `phy_init`, `ota_0`, `ota_1`, and a data partition labeled `storage` (+ `brushdata`). Keep `otadata` consistent or the bootloader rolls back. Flash ≤16MB.
2. **Back up stock flash fully** (`read_flash 0 0x1000000`) — there is no stock image to restore otherwise (per prior Oclean notes, no reflash precedent).

**Pin-level unknowns that can damage HW or mis-drive the coil:**
3. **I2S motor pins** (BCK=33, WS=47, DATA_OUT=34) — confidence *likely*, board defaults. Driving the wrong pin into the coil amp is a physical risk. Confirm against schematic/scope.
4. **Motor amp-enable GPIO** — strongly suspected to exist, **not located**. Find it before driving the motor.
5. **Backlight pin** — UNKNOWN (GPIO12 weak candidate). Scope it.
6. **WLC_EN = GPIO45** (*likely*) and its cascade into CHARGE_EN — confirm before toggling charge rails.
7. **GPIO37, GPIO48 roles** — confirmed OUTPUT, unknown function (power rails?). Confirm before driving.
8. **GPIO13/GPIO14 final direction** — ambiguous (OUTPUT at device-init vs re-touched in gpio_cfg). Verify.
9. **GPIO2 vs GPIO8 roles** (gyro INT vs charger/dock vs VBUS) — *guess/likely*. Confirm wake behavior.
10. **GPIO21 OUTPUT_OD** in sleep variants — confirm it's a shared/open-drain line before driving.
11. **VBAT divider ratio** — the ×2 is inferred; verify the resistor divider so battery % isn't wildly off.

**Data not recovered (affects correctness, not safety):**
12. Battery voltage→percent **table literals** (fn 0x42017ca0) — only index math recovered.
13. Motor **gear→waveform/sample-rate tables** (21-byte DROM defaults) — reverse if exact feel matters.
14. `brushdata` record sub-fields beyond the 2-byte header.
15. 3022 display controller part number (guess) — only needed if panel-id ≠ 0305.
16. The two `esp_sleep` helper calls in the deep-sleep sequence (arg 4 wake-source-disable + RTC pd/config) were not byte-exact decoded — confirm against IDF v5.1.1 `esp_sleep.c`.

---

## 4. Risk & Safe Bring-Up Sequencing

**Recoverability story.** As long as you (a) have the full stock-flash backup, (b) keep `ota_0`/`ota_1` + correct `otadata`, and (c) never erase the bootloader (0x0) or partition table (0x8000), every bad flash is recoverable over UART with esptool — the ESP32-S3 ROM download mode (hold BOOT/GPIO0 at reset) cannot be locked out by an app image. **The device is brick-safe for firmware bugs.** The non-recoverable risks are *physical*: driving the motor coil through the wrong pin/level, or toggling a charge rail incorrectly. So the sequence front-loads read-only and low-energy work and defers the motor.

**Recommended order (each stage gated on the previous passing over UART console):**

1. **Boot + console only.** Minimal `app_main`: read `esp_sleep_get_wakeup_cause`, init NVS, bring up UART logging. Confirm it boots in the right OTA slot and doesn't bootloop. No peripherals driven.
2. **Battery read (read-only, safe first).** ADC1_CH0 → mV → %. Pure input; cannot damage anything. Validates the ×2 divider and your ADC cal. Log raw + mV + %.
3. **I2C bus + sensor probes (read-only).** Bring up I2C0 (36/35, 100 kHz, no internal pull-ups — **confirm external pulls exist first**). Probe AW8686X WHOAMI @0x6A and QMI8658 WHOAMI. No actuation. Also reads IMU temp (reg 0x33) needed for charge protection.
4. **Buttons + GPIO inputs.** Configure 3/8/9/2 as inputs with ISRs; log edges. Confirm active-low on GPIO3 and which pin is charger-detect vs gyro INT. Still no outputs driven.
5. **LEDs (LEDC) — first safe output.** Low-current indicator LEDs on 17–21. Confirms your output path, inversion logic, and the LEDC config without touching the motor.
6. **Charge control (careful).** Only after battery + IMU-temp read work: drive CHARGE_EN (26) and, once confirmed, WLC_EN (45). Implement the 72 °C/67 °C cutoff *before* enabling charging unattended.
7. **Display.** SPI2 + 0305 init table + screen fill. No physical risk; mostly a correctness exercise (offsets, MADCTL). Backlight pin still TBD — leave it until identified.
8. **Motor — LAST, and at low amplitude.** Only after (3)/(4) confirm pins and the amp-enable GPIO is found. Bring up I2S + a single low-amplitude PLAYWAVE tone (small PCM amplitude, ~250 Hz) and listen/feel. Ramp amplitude slowly. This is the only stage with an irreversible-damage path, so it goes after everything else is trusted.
9. **Deep sleep / wake.** Configure EXT1 (0x208 ANY_LOW) + EXT0 (GPIO8 high), RTC-retained counters, re-sleep debounce. Test button wake and motion wake; verify no wake-on-sleep races (mask GPIO9 before sleeping).
10. **BLE app + storage.** Reuse the existing BLE stack; wire the category/opcode dispatch tables and the `brushdata` partition + NVS keys last, since they're the most decoded and least risky.

**Confidence summary:** GPIO modes, SPI/I2C/LEDC/ADC configs, task table, NVS keys, and GATT dispatch are read directly from code (high confidence). The *motor pins*, *amp-enable*, *backlight*, *WLC_EN*, *GPIO37/48/13/14 roles*, and *partition offsets* are the genuine unknowns — all routed to Section 3 and deferred in the bring-up order above. The biggest single correction vs the task premise: **the motor is I2S audio, not LEDC PWM.**
# NOTES: motor module (oem_wave.c, hw_motor.c)

Spec: `brushing.md` section 1 (and 2.5). Stock code read for it: `0x40377f70` (generator,
disassembly), `0x4201f24c` (music task), `0x4201f074`, `0x4201e7c8`, `0x42018edc`, `0x4201e834`,
`0x4201f134`, `0x4201f178`, `0x4201f18c`, `0x4200d0c4`, `0x4200d130`, `0x4200d148`, `0x4200eaa8`,
`0x4201eca4` (start of the decoder task), and `i2s_legacy.c` / `i2s_std.c` / `i2s_hal.c` /
`i2s_ll.h` of IDF 5.1.1.

## 1. What is implemented

| File | Content |
|---|---|
| `main/oem_wave.c` | Core, host-buildable. Wave table, swell steps, the 54-entry gear table (all three checked byte for byte against rodata). Generator `oem_wave_period()` = stock `0x40377f70`. `oem_motor_gear()` (`0x42018edc` + `0x4201e7c8`), `oem_motor_off()` (`0x4201e834`), `oem_motor_playing()` (`0x4201f178`), `oem_motor_app_gear_set()` (table `0x3fca33e8`). `oem_wave_task_step(bits)` = loop body of the music task `0x4201f24c`. |
| `main/oem_wave.h` | Module-private header shared by the two files and the host test (generator state, event bits, the four `hw_motor_*` hooks). No ESP-IDF content. Other modules need only `oem_api.h`. |
| `main/hw_motor.c` | ESP-IDF. `oem_motor_init()` (I2S0, GPIO48, event group, task), `oem_motor_amp()` (`0x4200d148`), the task, and the hooks: `hw_motor_post` (`0x4201ec90`), `hw_motor_write` (`0x4200d130`), `hw_motor_restart` (stock's `i2s_set_clk`), `hw_motor_start_allowed` (GPIO9 gate). Replaces the old sine driver; `hw_motor_init / hw_motor_set / hw_motor_running` are gone. |
| `re/tools/uisim/sim_wave.c` | Host test (section 4). |
| `re/tools/uisim/sim_wave_ref.py` | Second, independent model of the stock generator (written from the disassembly, tables and float literals read from the stock image) that `sim_wave -d` is compared against. |

Shared headers: one line added to the Requests block of `oem_api.h` (section 6). Nothing added to
`oem_state.h`, `oem_hal.h`, the timer list or the RTC struct.

Behaviour, as in stock:

- **Samples**: one period of `n = 24000 / freq` samples per generator call; table wave (100 points,
  peak 98) or sine for type 0x51; `sample = trunc((amp + extra) * shape / 50)`, `extra = 6` for
  type 0x50, anything above 44 plays as 50; full scale 16383. Same operation order and float
  precision as the stock routine (the Xtensa build of `oem_wave.c` emits the same sequence:
  `float.s`, `__divsf3`, `mul.s`, `mul.s`, `__divsf3`, `trunc.s`; for the sine `__muldf3`,
  `__divdf3`, `sin`, `__truncdfsf2`).
- **Chunk**: the period is sent 9 times, then the modulation step runs once (swell 0x21, pulse
  0x1f, triangle 0x20), so amplitude changes every 9 periods. Modulation state is never reset.
- **Start** (`oem_motor_gear`): parameters handed over, bit 0x8000. The task restarts the I2S
  clock, queues **5 chunks = 45 periods with the amp still off**, then enables the amp, then goes
  on chunk by chunk. 45 periods are 231 ms at 195 Hz and 281 ms for the idle hum; the DMA holds
  up to 37.5 ms of that, so the amp opens roughly 0.2 s to 0.25 s after the request.
- **Gear change while playing**: same path. Clock restart, 5 chunks of the new wave, amp
  asserted again. The new wave reaches the wire after what the DMA still holds (under 40 ms);
  the task is busy with the 45 periods for about 0.2 s, and only then is the amp re-asserted
  (this is what turns the amp back on after the zone cue while the idle hum plays).
- **Stop** (`oem_motor_off`): amp off at once in the caller, request cleared, bit 0x10000; the
  task switches the amp off again and stops producing. The I2S clocks keep running and send zeros.
- **Over-pressure**: `g_oem.motor_state == 2` (and `dev_mode != 2`) halves the duty when the gear
  is issued.
- **Charger gate**: a start is executed only while GPIO9 reads high; otherwise it is dropped
  (`oem_motor_playing()` still says true, as in stock). hw_motor.c logs a warning, stock is silent.
- **`hw_emulated()`**: no I2S channel; the task runs the same logic and waits the duration of the
  samples instead of writing them. GPIO48 is still driven.

## 2. I2S: what goes on the wire, and why

Stock uses the legacy driver with `channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT`,
`I2S_COMM_FORMAT_STAND_MSB`, then `i2s_set_clk(0, 24000, 16, I2S_CHANNEL_MONO)`. In IDF 5.1.1
`i2s_config_transfer()` turns that into `slot_mode = MONO, slot_mask = I2S_STD_SLOT_RIGHT,
ws_width = 16, ws_pol = false, bit_shift = false, left_align = false`, and `i2s_set_clk()` only
rewrites the mask when it is BOTH, so RIGHT stays. The legacy driver then calls the same
`i2s_hal_std_set_tx_slot()` the std driver calls. With that slot config the HAL sets TDM channel
mask 0x02 and mono-copy off: **the sample goes out in the right slot, the left slot carries zero**.

`hw_motor.c` passes exactly these values to `i2s_channel_init_std_mode()`, so the registers and
the wire are the same as stock: BCK 768 kHz, WS 24 kHz, 16-bit slots, MSB first without the
Philips one-bit delay, data in the right slot only. Clock: PLL, MCLK multiple 256 (MCLK not
routed), as the legacy defaults. DMA: 3 buffers of 300 samples, `auto_clear` (stock
`tx_desc_auto_clear`). The S3 macro `I2S_STD_MSB_SLOT_DEFAULT_CONFIG` used by the old custom code
selects BOTH slots (and `left_align = true`, which should not matter at 16/16 bits); if the amplifier mixes
L and R, that was twice the stock drive for the same sample values. Differences that remain:
stock opens the port as TX+RX (no data-in pin; no effect on the pins), and its interrupt is
level 2 / IRAM.

`i2s_set_clk` on every start is reproduced with `i2s_channel_disable` /
`i2s_channel_reconfig_std_clock` / `i2s_channel_enable`: both drivers stop TX, write the same
clock again and restart the DMA at the first buffer, so a gear change while playing gives the
same short irregularity (buffers replayed out of order for up to 37.5 ms) as stock. One small
difference: the std driver also empties its free-buffer queue on enable; the legacy one does not.

## 3. Things found in the stock code that the spec does not say (all ported as they are)

- **Triangle range** is `base-16 .. base`, not `base-15 .. base` (the direction flips after the
  step below `base-15`). In the pulse type a wobble section that ends while the amplitude is
  rising leaves the direction "up", so the next section can start one step **above** the duty
  (`base+1`).
- **Negative amplitude**: gear 53 (duty 10, pulse) and any halved pulse / triangle gear run the
  triangle below zero (down to -6, or -11 halved). The wave is then inverted and small; nothing
  overflows.
- **Type 0x50 above duty 38**: `duty + 6 > 44` plays as 50, so gears 27..32 (duty 39..44) are all
  full scale (16383), while gear 26 (duty 38) is 44/50.
- **The wave is not exactly DC-free**: the 100-point table is read at `n` points with
  `idx = max(0, (100k+100)/n - 1)`, which for `n > 100` takes one more point from the positive
  half. Mean over a period: +0.15 .. +0.17 % of the peak (+28.5 LSB at full scale, 0.09 % of the
  int16 range); exactly 0 for the sine and for `n = 94` (gear 50). These are stock's numbers
  (confirmed by the independent model), not a porting error.
- **GPIO48 at boot**: the decoder task `0x4201eca4` (not ported) configures GPIO48 and sets it
  **high** when it starts; the main task's `motor_off()` at boot lowers it again. Spec 1.2 says
  every change after boot goes through `amp_enable`; this one does not. The port never raises
  the pin at boot.
- **Legacy driver detail**: with `tx_desc_auto_clear` IDF 5.1.1 zeroes every buffer after it was
  sent (not only on underrun), the same as the std driver's `auto_clear`.
- `0x42101f60` / `0x42101f68`, called around start and stop in the music task, are empty functions.
- **Stock races** between the main task and the music task: (a) the generator reads and writes
  `amp` / `dir` while `motor_wave()` may be rewriting them; a clamp or modulation write-back can
  overwrite a freshly requested amplitude (for example leave 50 in place after a change from
  gear 32 to a weaker gear); (b) the task handles the bits in the fixed order start, stop, next,
  so "off, then gear" that arrive in one pass end with the wave stopped although one is requested.
  See deviations 1 and 2.

## 4. Host test

```
cc -std=gnu11 -Wall -Wextra -ffp-contract=off -I main re/tools/uisim/sim_wave.c main/oem_wave.c -lm -o sim_wave
./sim_wave            # checks, exit code 0 when all pass
./sim_wave -d | python3 re/tools/uisim/sim_wave_ref.py     # sample-exact comparison with the second model
```

Result: all checks pass; the comparison covers 43200 chunks (54 gears x full / halved duty x 400
chunks, brushing second advancing through the pulse sections) with 0 mismatching samples.

First chunk of each gear (`amp` = effective amplitude after +6 / clamp; `peak formula` =
`amp * 16383 / 50` truncated; `step` = largest sample-to-sample step inside a period, `wrap` =
step from the last sample of a period to the first of the next):

| gear | use | Hz | n | Hz real | type | duty | amp | peak formula | peak got | mean LSB | mean / peak | step | wrap |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 51 | strength 1 | 220 | 109 | 220.18 | 0x50 | 10 | 16 | 5242 | 5242 | +8.35 | +0.16 % | 962 | 909 |
| 52 | strength 2 | 220 | 109 | 220.18 | 0x50 | 14 | 20 | 6553 | 6553 | +10.44 | +0.16 % | 1203 | 1136 |
| 1 | strength 3 | 195 | 123 | 195.12 | 0x50 | 18 | 24 | 7863 | 7863 | +13.71 | +0.17 % | 1444 | 1364 |
| 24 | strength 4 | 195 | 123 | 195.12 | 0x50 | 36 | 42 | 13761 | 13761 | +23.97 | +0.17 % | 2527 | 2387 |
| 32 | strength 5 | 195 | 123 | 195.12 | 0x50 | 44 | 50 | 16383 | 16383 | +28.54 | +0.17 % | 3009 | 2841 |
| 54 | mode 1 | 230 | 104 | 230.77 | 0x1f pulse | 38 | 38 | 12451 | 12451 | +20.76 | +0.17 % | 2286 | 2159 |
| 47 | mode 2 | 210 | 114 | 210.53 | 0x50 | 48 | 50 | 16383 | 16383 | +24.92 | +0.15 % | 3009 | 2841 |
| 50 | mode 3 | 255 | 94 | 255.32 | 0x22 | 20 | 20 | 6553 | 6553 | 0.00 | 0.00 % | 1806 | 1136 |
| 48 | mode 4 | 225 | 106 | 226.42 | 0x21 swell | 15 | 15 | 4914 | 4914 | +8.04 | +0.16 % | 902 | 852 |
| 49 | idle hum | 160 | 150 | 160.00 | 0x51 sine | 2 | 2 | 655 | 655 | 0.00 | 0.00 % | 28 | 27 |
| 10 | mode 5 base | 195 | 123 | 195.12 | 0x50 | 25 | 31 | 10157 | 10157 | +17.72 | +0.17 % | 1865 | 1762 |
| 33 | low battery | 225 | 106 | 226.42 | 0x1f pulse | 24 | 24 | 7863 | 7863 | +12.87 | +0.16 % | 1444 | 1364 |
| 53 | lock buzz | 220 | 109 | 220.18 | 0x1f pulse | 10 | 10 | 3276 | 3276 | +5.23 | +0.16 % | 601 | 568 |
| 44 | tick | 401 | 59 | 406.78 | 0x50 | 10 | 16 | 5242 | 5242 | +8.19 | +0.16 % | 1871 | 909 |

Checks per gear: period length `24000 / Hz` and the frequency measured from zero crossings of
the written stream; peak equal to the formula (sine: within the half-sample crest loss);
max = -min; mean within 0.5 % of the peak; the step where periods and chunks join never larger
than the largest step inside a period.

Modulated types over time (amplitude the chunks were generated with):

| gear | type | run | amp range | peak range | worst join / in-period step |
|---|---|---|---|---|---|
| 54 | pulse, duty 38 | seconds 0..4.5 | 38 | 12451 | 2159 / 2286 |
| 54 | pulse, duty 38 | seconds 5..125 | 22..38 | 7208..12451 | 2159 / 2286 |
| 48 | swell, duty 15 | 150 s | 15..35 | 4914..11468 | 1989 / 2106 |
| 37 | triangle, duty 25 | 30 s | 9..25 | 2948..8191 | 1420 / 1504 |
| 40 | triangle, duty 45 | 30 s | 50 (clamped) | 16383 | 2841 / 3009 |
| 53 | pulse, duty 10 | seconds 5..35 | -6..11 | 0..3604 | 625 / 662 |

Sweep: all 54 gears, full and halved duty, 3000 chunks each: largest sample 16383, lowest
amplitude -11, largest |mean| / peak 0.24 %.

Playback logic with fake hardware: 45 periods (5535 samples at 195 Hz) written before the amp
goes on, one clock restart; gear change while playing restarts the clock, writes 45 periods of
the new gear (no old-gear sample after the change) and asserts the amp again; over-pressure
halving and its factory-mode exception; gear ids 0 and 55 fall back to gear 2; idle hum 150
samples, peak 655; stop drops the amp in the caller and the task goes idle with nothing more
written; closed charger gate writes nothing and leaves the amp off; "gear, off" in one pass ends
stopped and "off, gear" in one pass ends running; app gear table by step index, entry never set
(0 Hz) and step 4 ignored, index outside 0..3 ignored.

Not testable on the host: everything in `hw_motor.c` (only syntax-checked with the cross
compiler through `re/tools/esp_syntax.sh`; no build, no device): the std driver set-up, the
restart sequence, task timing against the 37.5 ms DMA depth, the GPIO48 hold.

## 5. Deviations from stock

1. **Parameter hand-over.** Stock shares `freq / base / type / amp / dir` as plain globals between
   the two tasks (race (a) above). The port passes a request word plus a counter; the generator
   applies it at the start of its next chunk (`amp = duty, dir = 1`, as `motor_wave()` does). Same
   result as stock whenever stock's race does not hit; never a mixed parameter set.
2. **Stale stop.** A stop bit that arrives in the same pass as a start that was executed, while
   the request is "wave" again (so `off` came before `gear`), is dropped instead of killing the
   wave just started. Stock would leave the motor dead until the next gear call. By the button
   this cannot happen (the handlers have 100 / 50 ms delays); with `oem_remote_*` back to back it
   can. If strict stock order is wanted: remove the `started` condition in `oem_wave_task_step()`.
3. **Frequency limits.** A request whose period does not fit 2400 samples (below 10 Hz) or with
   0 Hz is ignored and logged. Stock divides by zero at 0 Hz (an app gear entry that was never
   set) and mallocs `10 n` bytes otherwise. No stock gear is affected (lowest: 130 Hz).
4. **App gear table bounds.** `oem_motor_app_gear_set()` ignores an index outside 0..3; the stock
   BLE handler writes past the 4-entry table.
5. **Amp pin at init.** `oem_motor_init()` switches the amp off through the hold sequence
   (`gpio_hold_dis`, level 0, `gpio_hold_en`) instead of a bare `gpio_set_level(48, 0)`, so a pad
   left held high by a reset during brushing is released. It does not raise the pin at boot as
   the stock decoder task does.
6. **Amp calls serialised.** `oem_motor_amp()` wraps its three GPIO calls in a critical section,
   because the main task and the motor task both call it. It also refuses to switch the amp on if
   the I2S channel could not be created (not in emulation).
7. **Write timeout** 200 ms instead of 100 s; on a failed write the task waits the duration of
   the samples, so a dead channel cannot make it spin.
8. **Task**: stack 4096 instead of 3072; priority 3 and core 0 as stock; no task-watchdog
   registration.
9. **Not ported**: voice clips (PLAYMUSIC, MP3 decoder task, `clip_state`, `play_prev`), the
   factory override of the charger gate (`0x3fca308f`), the stock `printf`s. hw_motor.c adds a
   warning when a start is dropped by the gate and an underrun counter (logged at the next start:
   "N DMA buffers went out empty").

## 6. Requests to other modules / integration

- `oem_api.h` labels `oem_motor_gear(gear_id, use_app_table)` as `0x4201e7c8`; with these
  arguments it is stock `0x42018edc` (gear lookup), which calls `0x4201e7c8` (duty halving) and
  `0x4201f074`. All three are in `oem_motor_gear()`.
- **Request (in `oem_api.h`)**: `uint8_t oem_brush_step_index(void)` = `ses+0x20`, from the brush
  engine. Stock `motor_gear()` indexes the app gear table with it when `use_app_table` is set.
  Note the stock quirk: `step_advance()` calls `motor_gear()` before incrementing the index, the
  pressure state machine calls it after, so the same step uses entry `i` first and `i+1` on later
  re-issues. Return the live value; do not correct it here.
- **Glue**: call `oem_motor_init()` once at boot before the first `oem_motor_gear / off` (calls
  made earlier only move the amp pin). Add `oem_wave.c` to `main/CMakeLists.txt`. `hardware.c`
  still calls the removed `hw_motor_init()` / `hw_motor_set()`.
- **Threading**: `oem_motor_gear / off / playing / app_gear_set` expect the core lock (single
  caller at a time), from the main or the UI task. `oem_motor_amp()` may be called from any task.
  The motor task never takes the core lock, so `hal_delay()` with the lock held (low-battery
  buzz, lock buzz) does not stall the stream. It reads `g_oem.elapsed_s` without the lock (one
  16-bit read, as stock).
- **Power module**: GPIO9 must be an enabled input before the first start; if it reads 0 the
  motor never starts (warning "start dropped: GPIO9 low"). `hw_power.c` writing GPIO48 = 0 in
  its sleep paths does not conflict with this module.
- **Task priorities**: the DMA holds 37.5 ms and the tick is 10 ms here (stock: 1 ms, all app
  tasks at priority 3). If the main or UI task is CPU-bound for tens of milliseconds at a
  priority of 3 or more on core 0, the stream gets gaps. The underrun counter shows it; raise
  the priority in `oem_motor_init()` then. The task is cheap: one period of float math per chunk
  (40..56 ms); the idle hum's sine is double precision in software, a few milliseconds at most
  (estimate, not measured).
- **BLE / web**: `oem_motor_app_gear_set()` stores whatever it is given, as stock does. A sender
  that can be fed arbitrary values should refuse nonsense frequencies before calling it.

## 7. Unverified on hardware: watch on first boot

1. Log `hw_motor: motor ready (...)`. An `i2s setup:` error means the motor stays off.
2. **Drive strength.** Samples are stock's and go out in the right slot only. If the motor is
   clearly weaker than with the previous custom build at a comparable amplitude, the amp mixes
   L+R and this is the stock level (the old build doubled it). If it does not move at all, the
   slot assumption is wrong for this amp: compare with stock before changing `slot_mask`.
3. **Start delay**: about 0.2 s between the request and the amp opening is expected (pre-roll).
4. `start dropped: GPIO9 low` while off the charger: GPIO9 is not configured or the charger
   polarity is not what ARCH.md says.
5. `N DMA buffers went out empty`: scheduling gaps (see task priorities).
6. Gear changes while running (contact detected, over-pressure, strength swipe) should be felt
   as an immediate change with at most a brief irregularity, as on stock.
7. GPIO48 after a reset in the middle of brushing: the amp must be off once
   `oem_motor_init()` has run.
8. The pulse mode (mode 1) depends on `g_oem.elapsed_s` advancing once per second; if the brush
   engine does not update it, mode 1 stays steady.

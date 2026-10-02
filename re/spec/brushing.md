# Stock Oclean X Ultra 20: brushing engine and motor drive

Source: stock application image (project `blufixx_9_V2`, IDF v5.1.1), decompilation in `$S/decomp`, raw
segments in `$HOME/oclean-custom-firmware/re/`. Helper scripts: `$S/spec/work_brushing/`
(`gen_tables.py` regenerates every C array below from the rodata segment; `lf.py` maps a line of
`all2.c` to its function).

Legend: **[C]** = CONFIRMED, read from code or data at the address given. **[I]** = INFERRED, with the
reasoning. Addresses `0x42…`/`0x403…` are code, `0x3c…` rodata, `0x3fc9…` initialised RAM, `0x3fca…`
bss, `0x5000…` RTC memory. Stock tick is 1 kHz, so `vTaskDelay(n)` = n ms.

Short version of what differs from the current custom firmware (`main/hw_motor.c`):

| Item | Custom today | Stock |
|---|---|---|
| Sample rate | 16 kHz | **24 kHz**, 16-bit, mono, MSB (left-justified) format [C] |
| Waveform | sine | **100-point flattened wave table**, peak 98; a true sine is used only for the idle hum [C] |
| Frequency | 240..320 Hz | **195 Hz** for gears 3..5, **220 Hz** for gears 1..2 (strength mode); 210..255 Hz in the fixed modes [C] |
| Amplitude | 0.35..1.0 of 32000 | `(duty [+6]) / 50 * 16383`, i.e. at most half of full scale [C] |
| Gears 1..5 | always available | only in mode 5; the other modes have a fixed motor setting [C] |
| Session | on/off | timed program, 30 s zone cue, pause with 30 s timeout, auto stop, score, record [C] |
| Pressure | none | anti-splash idle until the bristles touch, amplitude halved above 400 [C] |

---

## 1. Motor drive

### 1.1 I2S setup (boot) [C]

`0x4200d0c4`, called once from the hardware init `0x4200d77c` (decomp line 234). Legacy driver:

```c
i2s_config_t cfg = {                       /* stack struct built at 0x4200d0c4 */
    .mode                 = 0x0d,          /* I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX */
    .sample_rate          = 44100,         /* 0xac44; replaced by i2s_set_clk below */
    .bits_per_sample      = 16,
    .channel_format       = 3,             /* I2S_CHANNEL_FMT_ONLY_RIGHT */
    .communication_format = 2,             /* I2S_COMM_FORMAT_STAND_MSB (no 1-bit shift) */
    .intr_alloc_flags     = 0x404,         /* ESP_INTR_FLAG_LEVEL2 | ESP_INTR_FLAG_IRAM */
    .dma_buf_count        = 3,
    .dma_buf_len          = 300,
    .use_apll             = 1,             /* no APLL on the S3; ignored */
    .tx_desc_auto_clear   = 1,             /* underrun sends zeros */
    /* all remaining fields 0 (26 bytes memset) */
};
i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
i2s_set_pin(I2S_NUM_0, &pins);             /* 0x420769dc */
i2s_set_clk(I2S_NUM_0, 24000, 16, I2S_CHANNEL_MONO);
```

```c
/* rodata 0x3c116cfc */ i2s_pin_config_t = { .mck_io_num = -1, .bck_io_num = 33, .ws_io_num = 47, .data_out_num = 34, .data_in_num = -1 };
```

- The pin table is read from rodata, so BCK 33 / WS 47 / DOUT 34, no MCLK, no DIN are confirmed [C].
- `0x420769dc` is called with `(0, &pins)` right after `i2s_driver_install`; it is not in `names.txt`.
  It is `i2s_set_pin` [I: only legacy call that takes a port and a 20-byte pin struct].
- Equivalent with the new driver [I, from `i2s_legacy.c:i2s_config_transfer` in IDF 5.1.1]:
  `I2S_STD_CLK_DEFAULT_CONFIG(24000)`, 16-bit data and slot width, `slot_mode = I2S_SLOT_MODE_MONO`,
  `slot_mask = I2S_STD_SLOT_RIGHT`, `ws_width = 16`, `ws_pol = false`, `bit_shift = false`,
  `left_align = false`, `big_endian = false`, `bit_order_lsb = false`. On the S3,
  `I2S_STD_MSB_SLOT_DEFAULT_CONFIG(16, MONO)` (what `hw_motor.c` uses) sets `slot_mask = I2S_STD_SLOT_BOTH`,
  which makes the HAL copy each sample into both slots (`i2s_hal_std_set_tx_slot`, `is_copy_mono`). Stock
  enables the right slot only (TDM channel mask 0x02, mono-copy off) [C from `i2s_hal.c` / `i2s_ll.h` 5.1.1].
  What the amp does with that difference is open (section 9, item 3).
- DMA holds 3 x 300 mono samples = 37.5 ms at 24 kHz.
- GPIO48 is configured right after: `gpio_config({pin_bit_mask = 1ULL<<48, mode = GPIO_MODE_OUTPUT,
  no pulls, no intr})`, `gpio_set_level(48, 0)` [C: `0x4200d77c` lines 235..242].

### 1.2 Amp enable GPIO48 [C]

`0x4200d148 amp_enable(level)`:

```c
gpio_hold_dis(48); gpio_set_level(48, level); gpio_hold_en(48);
```

Every change of GPIO48 after boot goes through this function (9 call sites). 1 = amp on.
No other pin is touched by the motor code.

### 1.3 Tasks, event bits and the playback state [C]

`0x4201f18c` (from `app_main`) creates event group `0x3fca4fc8` and two tasks, both stack 3072, priority 3:
`brush_decoder_app` `0x4201eca4` (no core affinity) and `brush_music_app` `0x4201f24c` (core 0).

Event group `0x3fca4fc8` (setter `0x4201ec90`):

| Bit | Meaning | Set by |
|---|---|---|
| 0x8000 | start / new parameters | `0x4201f074` (wave), `0x4201f0b4` and `0x4201f108` (voice clip) |
| 0x10000 | stop | `0x4201f134` |
| 0x20000 | produce the next chunk | `0x4201f154`, posted by the music task to itself |

State bytes:

| Address | Name used here | Values |
|---|---|---|
| 0x3fca4fc3 | `play_req` | 0 none, 1 PLAYWAVE, 2 PLAYMUSIC |
| 0x3fca4fc2 | `play_prev` | copy of `play_req` after a start |
| 0x3fca4f6e | `running` | 1 while the task re-posts 0x20000 |
| 0x3fc9aee9 | `clip_state` | 2 idle (boot value), 0 clip playing, 1 clip finished |
| 0x3fca4fc4 / 0x3fca4fc5 | clip index requested / playing | 0..6 |
| 0x3fc9aeec (u16) | `freq` | Hz, boot value 250 |
| 0x3fc9aeeb | `base` | duty, boot value 20 |
| 0x3fc9aeea | `type` | waveform type, boot value 0x50 |
| 0x3fca4f6c (s16) | `amp` | current amplitude, 0..50 |
| 0x3fc9aee8 | `dir` | 1 = triangle going down, boot value 1 |

Music task loop: `xEventGroupWaitBits(eg, 0x38000, clear, any, 1000 ms)`, then in this order:

**Bit 0x8000** (only acted on if `gpio_get_level(9) != 0` or the factory override `0x3fca308f != 0`;
otherwise the request is dropped silently, see 5.3):

```
if (clip_state == 2 || running == 0) {
    if (play_req == 2) {                       /* voice clip */
        amp_enable(0); i2s_zero_dma_buffer(0);
        start_clip(clip index);                /* 0x4201f1d0, section 7 */
        clip_playing = index; clip_state = 0;
    } else {                                   /* PLAYWAVE */
        i2s_set_clk(0, 24000, 16, I2S_CHANNEL_MONO);
    }
    running = 1; post 0x20000;
    if (play_req == 1) { wave_chunk() x 5; }   /* 45 periods queued before the amp opens */
    amp_enable(1);
}
play_prev = play_req;
```

**Bit 0x10000**: `amp_enable(0); audio_pipeline_stop(pipeline); running = 0; clip_state = 2;`

**Bit 0x20000**:

```
if (play_req == 2) {
    if (clip_state == 1) {                     /* decoder task reported end of clip */
        if (clip_playing != 0) {               /* clips 1..6: go back to the motor wave */
            clip_state = 2; i2s_set_clk(0, 24000, 16, MONO); play_req = 1;
            amp_enable(0); wave_chunk() x 5; amp_enable(1);
            wave_chunk();
        } else {                               /* clip 0: stand-alone, stop afterwards */
            amp_enable(0); audio_pipeline_stop(); vTaskDelay(50);
            running = 0; clip_state = 2; play_req = 0;
        }
    }
} else if (play_req == 1) wave_chunk();
if (running) post 0x20000;
```

So normal brushing is **PLAYWAVE: a synthesised waveform**, generated one chunk at a time by `0x40377f70`
(IRAM) and pushed with blocking `i2s_write(0, buf, len, &written, 100000)` (`0x4200d130`). PLAYMUSIC is
only used for voice clips (section 7).

Entry points used by the rest of the firmware:

```c
/* 0x4201f074 */ void motor_wave(uint16_t freq_hz, uint8_t duty, uint8_t type) {
    freq = freq_hz; base = duty; type_ = type; play_req = 1; amp = duty; dir = 1; post(0x8000);
}
/* 0x4201e834 */ void motor_off(void) { amp_enable(0); play_req = 0; play_prev = 0; post(0x10000); }
/* 0x4201e7c8 */ void motor_gear_raw(uint32_t p, uint8_t type) {   /* p = 4 bytes of a gear entry */
    uint16_t hz = (p & 0xff) * 10 + ((p >> 8) & 0xff) / 10;
    uint8_t duty = (p >> 16) & 0xff;
    if (motor_state == 2 && factory_mode != 2) duty >>= 1;          /* over-pressure, section 4 */
    motor_wave(hz, duty, type);
}
/* 0x42018edc */ void motor_gear(uint8_t gear, uint8_t use_app_table) {
    if ((uint8_t)(gear - 1) > 53) gear = 2;
    if (!use_app_table) motor_gear_raw(first 4 bytes of oc_gear[gear-1], oc_gear[gear-1].type);
    else if (step_index < 4) motor_gear_raw(app_gear[step_index] ...);  /* 0x3fca33e8, section 2.5 */
}
```

A gear change while the wave is already running takes the same 0x8000 path (`clip_state` is 2), so it
re-runs `i2s_set_clk`, queues 5 chunks and re-asserts the amp. There is no amplitude ramp; the only
soft start is the anti-splash idle hum of section 4.1.

### 1.4 Waveform generator `0x40377f70` [C, verified against the disassembly]

One call = one "chunk":

1. `n = 24000 / freq` (integer). One period of `n` samples is computed.
2. Sample `k` (0..n-1):
   - type 0x51: `shape = (float)sin(3.1415926 * (2k) / n) * 16383.0f`, `extra = 0`
     (literals `0x400921fb53c8d4f1`, `0x467ffc00`).
   - any other type: `idx = max(0, (int16_t)((100*k + 100) / n) - 1)`,
     `shape = ((float)oc_wave100[idx] / 98.0f) * 16383.0f` (literal `0x42c40000`),
     `extra = (type == 0x50) ? 6 : 0`.
   - `if (amp + extra > 44) { amp = 50; extra = 0; clamped = true; }`
   - `sample = (int16_t)trunc(((float)(amp + extra) * shape) / 50.0f)` (literal `0x42480000`).
3. If `clamped`, 50 is written back to `amp` (0x3fca4f6c).
4. The modulation step for the type runs once (below) and may change `amp` for the next call.
5. The period is copied twice behind itself and the 3-period buffer (`6n` bytes) is written **three
   times**: 9 periods per call. (The buffer is `malloc(10n)` and freed each call.)

Consequences: actual pitch is `24000 / n` Hz (195 -> n=123 -> 195.1 Hz; 220 -> 109 -> 220.2 Hz;
230 -> 104 -> 230.8 Hz); peak is `(amp+extra)/50 * 16383`, at most 16383 (-6 dBFS); amplitude can only
change every 9 periods (about 40..46 ms).

```c
/* rodata 0x3c11ad93, 100 x int8, one full period, peak +-98 */
static const int8_t oc_wave100[100] = {
       0,   18,   33,   45,   55,   63,   70,   75,   79,   83,
      86,   88,   90,   92,   93,   94,   95,   96,   97,   97,
      97,   98,   98,   98,   98,   98,   98,   98,   98,   98,
      97,   97,   97,   96,   95,   94,   93,   92,   90,   88,
      86,   83,   79,   75,   69,   63,   54,   44,   32,   17,
       0,  -18,  -33,  -45,  -55,  -63,  -70,  -75,  -79,  -83,
     -86,  -88,  -90,  -92,  -93,  -94,  -95,  -96,  -97,  -97,
     -97,  -98,  -98,  -98,  -98,  -98,  -98,  -98,  -98,  -98,
     -97,  -97,  -97,  -96,  -95,  -94,  -93,  -92,  -90,  -88,
     -86,  -83,  -79,  -75,  -69,  -63,  -54,  -44,  -32,  -17,
};
```

Modulation step per type (state bytes: `pulse_on` 0x3fca4f56, `sw_hold` 0x3fca4f57, `sw_off` (s8)
0x3fca4f58, `sw_idx` 0x3fca4f59, `sw_state` 0x3fca4f5a; `elapsed_s` is the brushing second counter
0x3fca4c82). None of these is reset by `motor_wave()`.

```c
/* rodata 0x3c11abf3 */ static const uint8_t oc_swell_step[3] = { 1, 2, 1 };
```

```c
switch (type) {
case 0x21:                                  /* swell: base .. base+20 .. base */
    if (sw_state == 0)      { sw_off += oc_swell_step[sw_idx % 3]; sw_idx++;
                              if (sw_off > 19) { sw_state = 1; sw_off = 20; sw_idx = 0; } }
    else if (sw_state == 1) { if (++sw_hold > 29) { sw_state = 2; sw_hold = 0; } }
    else if (sw_state == 2) { sw_off -= oc_swell_step[sw_idx % 3]; sw_idx++;
                              if (sw_off < 1) { sw_state = 0; sw_off = 0; sw_idx = 0; } }
    amp = sw_off + base;
    break;
case 0x1f:                                  /* pulse: steady and wobble sections */
    if (elapsed_s < 5) { pulse_on = 0; break; }
    if (elapsed_s % 6 == 0) pulse_on ^= 1;  /* toggles on EVERY call during such a second */
    if (!pulse_on) { amp = base; break; }
    /* fall through */
case 0x20:                                  /* triangle between base-15 and base */
    if (dir) { amp--; if (amp < base - 15) dir = 0; }
    else     { amp++; if (amp >= base)     dir = 1; }
    break;
default:                                    /* 0x50, 0x51, 0x22, 0x00: constant amplitude */
    break;
}
```

Types seen in data: 0x50 table wave with +6, 0x51 sine, 0x1f pulse, 0x20 triangle, 0x21 swell, 0x22 and
0x00 plain table wave without the +6.

### 1.5 Gear table [C]

`motor_gear()` reads `f10`, `frac`, `duty`, `type`. Bytes `b3`, `b5`, `b6` are packed into the call
arguments but never read by `0x4201e7c8` / `0x4201f074` [C]; their meaning is unknown.
`freq = f10*10 + frac/10`.

```c
/* rodata 0x3c1194b8, 54 entries x 7 bytes. Only f10, frac, duty and type are read by the motor code. */
typedef struct { uint8_t f10, frac, duty, b3, type, b5, b6; } oc_gear_t;
static const oc_gear_t oc_gear[54] = {   /* index = gear id - 1 */
    /*  1 @0x3c1194b8 */ { 19, 50, 18, 40, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 18, type 0x50 */
    /*  2 @0x3c1194bf */ { 19, 50, 18, 90, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 18, type 0x50 */
    /*  3 @0x3c1194c6 */ { 19, 50, 19, 40, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 19, type 0x50 */
    /*  4 @0x3c1194cd */ { 19, 50, 19, 90, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 19, type 0x50 */
    /*  5 @0x3c1194d4 */ { 19, 50, 20, 40, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 20, type 0x50 */
    /*  6 @0x3c1194db */ { 19, 50, 20, 90, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 20, type 0x50 */
    /*  7 @0x3c1194e2 */ { 19, 50, 21, 40, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 21, type 0x50 */
    /*  8 @0x3c1194e9 */ { 19, 50, 21, 90, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 21, type 0x50 */
    /*  9 @0x3c1194f0 */ { 19, 50, 22,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 22, type 0x50 */
    /* 10 @0x3c1194f7 */ { 19, 50, 25,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 25, type 0x50 */
    /* 11 @0x3c1194fe */ { 19, 50, 24,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 24, type 0x50 */
    /* 12 @0x3c119505 */ { 19, 50, 25,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 25, type 0x50 */
    /* 13 @0x3c11950c */ { 19, 50, 26,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 26, type 0x50 */
    /* 14 @0x3c119513 */ { 19, 50, 27,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 27, type 0x50 */
    /* 15 @0x3c11951a */ { 19, 50, 28,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 28, type 0x50 */
    /* 16 @0x3c119521 */ { 19, 50, 29,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 29, type 0x50 */
    /* 17 @0x3c119528 */ { 19, 50, 29,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 29, type 0x50 */
    /* 18 @0x3c11952f */ { 19, 50, 30,  0, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 30, type 0x50 */
    /* 19 @0x3c119536 */ { 19, 50, 31,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 31, type 0x50 */
    /* 20 @0x3c11953d */ { 19, 50, 32,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 32, type 0x50 */
    /* 21 @0x3c119544 */ { 19, 50, 33,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 33, type 0x50 */
    /* 22 @0x3c11954b */ { 19, 50, 34,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 34, type 0x50 */
    /* 23 @0x3c119552 */ { 19, 50, 35,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 35, type 0x50 */
    /* 24 @0x3c119559 */ { 19, 50, 36,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 36, type 0x50 */
    /* 25 @0x3c119560 */ { 19, 50, 37,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 37, type 0x50 */
    /* 26 @0x3c119567 */ { 19, 50, 38,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 38, type 0x50 */
    /* 27 @0x3c11956e */ { 19, 50, 39,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 39, type 0x50 */
    /* 28 @0x3c119575 */ { 19, 50, 40,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 40, type 0x50 */
    /* 29 @0x3c11957c */ { 19, 50, 41,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 41, type 0x50 */
    /* 30 @0x3c119583 */ { 19, 50, 42,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 42, type 0x50 */
    /* 31 @0x3c11958a */ { 19, 50, 43,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 43, type 0x50 */
    /* 32 @0x3c119591 */ { 19, 50, 44,  6, 0x50, 10, 50 },  /* 195 Hz (n=123), duty 44, type 0x50 */
    /* 33 @0x3c119598 */ { 22, 50, 24, 20, 0x1f, 30,  0 },  /* 225 Hz (n=106), duty 24, type 0x1f */
    /* 34 @0x3c11959f */ { 22, 50, 28, 60, 0x1f, 30,  0 },  /* 225 Hz (n=106), duty 28, type 0x1f */
    /* 35 @0x3c1195a6 */ { 22, 50, 30, 60, 0x1f, 20,  0 },  /* 225 Hz (n=106), duty 30, type 0x1f */
    /* 36 @0x3c1195ad */ { 22, 50, 32, 60, 0x1f, 30,  0 },  /* 225 Hz (n=106), duty 32, type 0x1f */
    /* 37 @0x3c1195b4 */ { 13,  0, 25, 50, 0x20, 25, 10 },  /* 130 Hz (n=184), duty 25, type 0x20 */
    /* 38 @0x3c1195bb */ { 13,  0, 28, 50, 0x20, 25, 10 },  /* 130 Hz (n=184), duty 28, type 0x20 */
    /* 39 @0x3c1195c2 */ { 13,  0, 32, 50, 0x20, 25, 10 },  /* 130 Hz (n=184), duty 32, type 0x20 */
    /* 40 @0x3c1195c9 */ { 13,  0, 45,  0, 0x20, 15, 10 },  /* 130 Hz (n=184), duty 45, type 0x20 */
    /* 41 @0x3c1195d0 */ { 26,  0, 18,  0, 0x50, 25, 10 },  /* 260 Hz (n= 92), duty 18, type 0x50 */
    /* 42 @0x3c1195d7 */ { 23, 80, 13,  6, 0x50, 10, 50 },  /* 238 Hz (n=100), duty 13, type 0x50 */
    /* 43 @0x3c1195de */ { 18,  0, 14,  0, 0x50, 20, 10 },  /* 180 Hz (n=133), duty 14, type 0x50 */
    /* 44 @0x3c1195e5 */ { 40, 10, 10, 10, 0x50, 20, 10 },  /* 401 Hz (n= 59), duty 10, type 0x50 */
    /* 45 @0x3c1195ec */ { 35,  0, 42, 10, 0x50, 20, 10 },  /* 350 Hz (n= 68), duty 42, type 0x50 */
    /* 46 @0x3c1195f3 */ { 23, 50, 38,  0, 0x50, 10, 50 },  /* 235 Hz (n=102), duty 38, type 0x50 */
    /* 47 @0x3c1195fa */ { 21,  0, 48,  0, 0x50, 10, 50 },  /* 210 Hz (n=114), duty 48, type 0x50 */
    /* 48 @0x3c119601 */ { 22, 50, 15,  0, 0x21, 10, 50 },  /* 225 Hz (n=106), duty 15, type 0x21 */
    /* 49 @0x3c119608 */ { 16,  0,  2,  0, 0x51, 10, 50 },  /* 160 Hz (n=150), duty  2, type 0x51 */
    /* 50 @0x3c11960f */ { 25, 50, 20,  6, 0x22, 10, 50 },  /* 255 Hz (n= 94), duty 20, type 0x22 */
    /* 51 @0x3c119616 */ { 22,  0, 10, 40, 0x50, 10, 50 },  /* 220 Hz (n=109), duty 10, type 0x50 */
    /* 52 @0x3c11961d */ { 22,  0, 14, 40, 0x50, 10, 50 },  /* 220 Hz (n=109), duty 14, type 0x50 */
    /* 53 @0x3c119624 */ { 22,  0, 10, 20, 0x1f, 30,  0 },  /* 220 Hz (n=109), duty 10, type 0x1f */
    /* 54 @0x3c11962b */ { 23,  0, 38, 20, 0x1f, 30,  0 },  /* 230 Hz (n=104), duty 38, type 0x1f */
};
```

Gears referenced by code [C]:

| Gear | Where | Use |
|---|---|---|
| 51, 52, 1, 24, 32 | `0x42019368`, `0x420192e8` | strength 1..5 (mode 5) |
| 54, 47, 50, 48, 10 | profile tables (2.2) | modes 1, 2, 3, 4 and the mode-5 placeholder |
| 49 | `0x420193f8`, `0x4201977c`, `0x4201962c` | anti-splash idle hum (160 Hz sine, duty 2, peak 655) |
| 33 | `0x4201babc` | low-battery buzz: 3 x (400 ms on, 400 ms off) |
| 44 | brush task, event bit 0x800000 | 70 ms tick at 401 Hz |
| 53 | `0x420285bc` | 400 ms buzz then `motor_off()`, from the long-press path `0x4202865c(1)` unless screen 87 is up [C]; it accompanies the screen-lock toggle [I] |
| 18 | `0x420198fc` | factory mode only |
| 32 | `0x42019be8`, `0x42019ac8` | aging / production test |
| 47 | `0x42027d2c` `PCBA_WOKING_STATIC` | factory command |

Strength mapping (`0x42019368`, `0x420192e8`, reverse map `0x42019398`) [C]:

| Strength | Gear | Hz | duty | effective amp (+6, clamp) | peak |
|---|---|---|---|---|---|
| 1 | 51 | 220 | 10 | 16 | 5242 |
| 2 | 52 | 220 | 14 | 20 | 6553 |
| 3 | 1 | 195 | 18 | 24 | 7863 |
| 4 | 24 | 195 | 36 | 42 | 13761 |
| 5 | 32 | 195 | 44 | 50 | 16383 |

Strength is stored in RTC memory `0x50001019` (1..5; anything else becomes 3 at session start) [C].

---

## 2. Modes and profiles

### 2.1 Mode ids [C]

Config block `cfg` at `0x3fca4c85`: `cfg[6]` = current mode, `cfg[7]` = mode shown/saved.
The idle screen sets both (`0x4201af48`): screen 0x4f -> 1, 0x4d -> 2, 0x4c -> 3, 0x4e -> 4, 0x50 -> 5,
0x51 -> 0. The reverse map is `0x4201a3a0`: mode 0 -> screen 81, 1 -> 79, 2 -> 77, 3 -> 76, 4 -> 78, 5 -> 80.

| Mode | Screen | Steps (gear, seconds) | Motor | Total |
|---|---|---|---|---|
| 1 | 79 | (54, 120) | 230 Hz, duty 38, type 0x1f pulse | 120 s |
| 2 | 77 | (47, 180) | 210 Hz, duty 48 -> amp 50, steady | 180 s |
| 3 | 76 | (50, 120) | 255 Hz, duty 20, type 0x22 steady, no +6 | 120 s |
| 4 | 78 | (48, 150) | 225 Hz, duty 15, type 0x21 swell 15..35 | 150 s |
| 5 | 80 | (10, 150), (10, 150); gear replaced by strength | strength table 1.5 | 300 s (stops at 302 s) |
| 0 | 81 | from the phone app (2.4) | per step | per scheme |

- Mode names exist only as pictures (#382, #348, #771, #365, #715, #399 per the UI findings); no mode
  name string is in the app image [C: `strings_all.txt`]. Names are an open question.
- Mode 1 is the morning mode and mode 2 the evening mode [I]: with `sys[6] == 1` (`0x3fc9a69e[6]`,
  default 0) the wake path `0x4201a3fc` forces mode 1 from 03:01 to 12:00 and mode 2 otherwise [C].
- **Factory default mode is 5** (`0x420187b4`: `cfg[6] = 5`) [C]. Boot (`0x42019008`): a mode above 5
  becomes 5; mode 0 becomes 5 unless `cfg[0] == 1` or `cfg[2] == 1`; then `cfg[7] = cfg[6]`.
- Default strength is 3 [C].

### 2.2 Default profile tables [C]

`0x42019050 load_profile(mode)` copies five 21-byte rodata tables into 40-byte zero-padded records and
picks one: mode 1 -> A, 2 -> C, 3 -> E, 4 -> D, everything else -> B. Byte 0 is then overwritten with
`cfg[6]`.

```c
struct profile_record {       /* 40 bytes */
    uint8_t id;               /* brush_config_ID, written into the session record */
    uint8_t flags_steps;      /* bits 7..5 -> 0x3fca4d33, bits 4..0 = step count (clamped to 13) */
    struct { uint8_t a; uint8_t gear; uint8_t seconds; } step[];   /* 3 bytes each */
};
```

```c
/* table A (mode 1), rodata 0x3c1196ee, 21 bytes copied, rest of the 40-byte record zeroed */
{ 0x00, 0x01, 0x00, 0x36, 0x78, 0x00, 0x2e, 0x32, 0x00, 0x12, 0x32, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00 },
/* table B (modes 0/5/6 base), rodata 0x3c119716, 21 bytes copied, rest of the 40-byte record zeroed */
{ 0x00, 0x02, 0x00, 0x0a, 0x96, 0x00, 0x0a, 0x96, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00 },
/* table C (mode 2), rodata 0x3c11973e, 21 bytes copied, rest of the 40-byte record zeroed */
{ 0x00, 0x01, 0x00, 0x2f, 0xb4, 0x00, 0x21, 0x2d, 0x00, 0x12, 0x2d, 0x00, 0x21, 0x2d, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00 },
/* table D (mode 4), rodata 0x3c119766, 21 bytes copied, rest of the 40-byte record zeroed */
{ 0x00, 0x01, 0x00, 0x30, 0x96, 0x00, 0x30, 0x96, 0x00, 0x12, 0x2d, 0x00, 0x21, 0x2d, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00 },
/* table E (mode 3), rodata 0x3c11978e, 21 bytes copied, rest of the 40-byte record zeroed */
{ 0x00, 0x01, 0x00, 0x32, 0x78, 0x00, 0x30, 0x96, 0x00, 0x12, 0x2d, 0x00, 0x21, 0x2d, 0x00, 0x03, 0x1e, 0x00, 0x03, 0x1e, 0x00 },
```

Only `step count` steps are used; the bytes after them in rodata are leftovers. `a` is copied but not
read by any motor function [C]. Total time = sum of `seconds` of the used steps -> `ses+0x6e`.

Result in RAM: `0x3fca4d30` = `{use_app_gear_table, id, steps, flags, step[13]}`. `0x420194a8` then copies
`step[]` (39 bytes) to `ses+0x74`.

### 2.3 Mode 0 variants [C, `0x42019050`]

- `cfg[2] == 1`: one step, `id = 6`, `gear = cfg[5]`, `seconds = cfg[3]` (defaults 16 and 180).
- else `cfg[1] != 1` and `cfg[0] != 0`: 38 bytes from NVS blob `motor_data` offset 0.
- else `cfg[1] == 1` and `cfg[0] != 0`: 38 bytes from `motor_data` offset 0x3c between 03:01 and 12:00,
  otherwise offset 0x78.
- Mode above 5: forced to 5, and `motor_data` offset 0 if `cfg[0] != 0`.

`motor_data` is a 255-byte blob in NVS namespace `storage` (`0x420247ac` / `0x420247e8`).

### 2.4 Phone-app inputs (only for completeness) [C]

- BLE command 0x06 `handle_app_brush_scheme` (`0x420108cc`): `{id, flags|count, count x {a, gear, seconds}}`,
  gear forced to 18 when `id == 0`; stored at `motor_data` offset 0; sets `cfg[0] = 1`, `cfg[1] = 2`,
  `cfg[2] = 0`, mode 0. Refused while a session is active.
- BLE command 0x29 (`0x42010a3c`): morning and evening plans -> `motor_data` 0x3c / 0x78, `cfg[1] = 1`.
- BLE command 0x30 (`0x4200ff8c`): `cfg[2]`; 0 -> mode 5, 1 -> mode 0.
- BLE command 0x0c (`0x4200eb54`): anti-splash flag `sys[0x37]`. 0x0d (`0x4200ebbc`): zone-cue type `sys[2]`.

### 2.5 App gear table [C]

BLE command 0x08 (`0x4200eaa8`) sets `0x3fca4d30[0] = 1` and fills up to 4 entries of 7 bytes at
`0x3fca33e8` as `{f10, frac, duty, b3, 0, 0, 0}` (type 0). When the flag is set, `motor_gear()` ignores
its gear argument and uses entry `ses[0x20]` (the step counter), and does nothing if that is 4 or more.
Factory default: flag 0.

---

## 3. Session

### 3.1 Clocks [C]

| Source | Period | Event bit (group `0x3fca4eac`, setter `0x40377f10`) | Work |
|---|---|---|---|
| `pxp_periodic_1s_timer` `0x3fca4ea8`, cb `0x4201b5a0` | 10 ms | 0x40 | counts `0x3fca4df1` to 100, then the 1 Hz block |
| `pxp_fast_timer` `0x3fca4e1c`, cb `0x4201b678` | 30 ms (`0x4201c1d0`), runs only during a session | 0x200 | zone cue `0x4201962c`, pressure `0x42018530` |
| `pressure_sensor_timer` `0x3fca5c7c` | 20 ms [I: argument 0x14 scaled by 1000] | 0x400 | sample force `0x420277d4` |
| session start / end | | 0x40000 / 0x80000 (`0x4201b608`) | 0x80000 runs the end handler |

All of it runs in the `brush_app` task `0x4201cc40`, which waits on mask `0x7fffea`.

### 3.2 Session variables [C]

`ses` = `0x3fca4c90`.

| Address | Name | Meaning |
|---|---|---|
| 0x3fca4d5b | `active` | 1 from start until stop (also while paused) |
| 0x3fca4e03 | `unpaused` | 1 running, 0 paused |
| 0x3fc9aba5 | `stop_delay` | 200 = running; set to 0 on pause and counted up at 1 Hz |
| 0x3fc9ab97 | `motor_state` | 0 idle hum (waiting for contact), 1 normal, 2 over-pressure; boot value 1 |
| 0x3fca4c82 (u16) | `elapsed_s` | brushing seconds, frozen while paused |
| 0x3fca4c7a (u16) | `next_step_s` | `elapsed_s` at which the next step starts |
| 0x3fca4c74 (s16) | `pressure` | force value, section 4 |
| 0x3fca4c7e (u16), 0x3fca4c80, 0x3fca4c7c, 0x3fca4c7d | cue tick counter, cue active, cue count, `muted` | section 3.4 |
| ses+0x20 | `step_index` | number of steps started |
| ses+0x21 | `gear` | current gear id |
| ses+0x22..0x28 | start time | year-2000, month, day, hour, min, sec, weekday (`0x4201d89c`) |
| ses+0x2a, ses+0x2b | profile id, step count | |
| ses+0x4a (u16 x 12) | `zone_s[]` | seconds per zone, from the IMU zone tracker |
| ses+0x6e (u16) | `total_s` | planned seconds |
| ses+0x70 (u16) | `done_s` | elapsed seconds, clamped to `total_s` |
| ses+0x72 (u16) | `contact_s` | seconds with `pressure >= 50` |
| ses+0x74 | `step[13]` | `{a, gear, seconds}` |
| 0x3fc9aba3 | `score` | 0..100 |
| 0x3fca4b81 (240 bytes), 0x3fca4c72 | pressure log and its index | section 6 |

### 3.3 Start, step advance, auto stop [C]

Start = `0x420194a8` (via `0x4201ba38`, from the button handler, section 5):

```
cfg[6] = (factory_mode == 2) ? 5 : cfg[7];
load_profile(cfg[6]);
ses.id = profile.id; ses.step_index = 0; ses.steps = profile.steps;
ses.done_s = ses.contact_s = 0; cue_ticks = 0;
copy steps; next_step_s = step[0].seconds; ses.gear = step[0].gear;
motor_state = (sys[0x37] == 0) ? 1 : 0;        /* sys[0x37] = anti-splash, default 1 -> state 0 */
step_advance();
```

`0x420193f8 step_advance()`:

```
if (ses.step_index < ses.steps) {
    if (cfg[6] != 5) ses.gear = step[ses.step_index].gear;   /* mode 5 keeps the strength gear */
    if (motor_state == 0) motor_gear(49, 0);                 /* idle hum */
    else                  motor_gear(ses.gear, profile.use_app_gear_table);
    ses.step_index++;
} else {                                                     /* program finished */
    brush_state = 0x14; ses.done_s = ses.total_s; active = 0;
    motor_off(); post 0x80000;                               /* -> end handler, 3.6 */
}
```

1 Hz block, `0x42019560`:

```
if (active && stop_delay == 200) {
    elapsed_s = aging_mode ? 0 : elapsed_s + 1;
    if (elapsed_s == next_step_s) {
        if (ses.step_index < ses.steps) next_step_s += step[ses.step_index].seconds;
        if (ses.steps - 1 == ses.step_index) next_step_s += 2;
        step_advance();
    }
    ses.done_s = elapsed_s;
    if (pressure > 49) ses.contact_s++;
    if (ses.total_s < elapsed_s) ses.done_s = ses.total_s;
}
if (stop_delay < 60) { stop_delay++; if (stop_delay == 30) handle_motor_stop(); }
```

- The motor **stops by itself** when the last step ends [C]. A one-step mode stops exactly at its total.
  A profile with several steps runs 2 s past the total (mode 5: 302 s); `done_s` is still capped.
- Step boundaries re-issue the gear; nothing else marks them.
- In mode 5 after the first 150 s, the restore paths in section 4 use `step[1].gear` (gear 10) instead
  of the strength gear, because they index `step[step_index-1]` and only `step[0].gear` is patched [C].
  This looks like a stock bug; a re-implementation can keep the strength gear.

Also at 1 Hz while `active`: `score` is recomputed (`0x4201c260`); on even `done_s` one pressure-log byte
is stored; when `stop_delay == 200` the brushing screen is refreshed (`0x4201bb18`, UI screen 0x52 with
`total_s - done_s`), unless screen 0x57 is up.

### 3.4 Zone-change cue every 30 s [C, `0x4201962c`, disassembly checked]

Called every 30 ms while `active && unpaused` (and `0x3fca4f51 != 2`). `sys[2]` (`0x3fc9a6a0`) selects the
type; factory default 0 (`0x420187b4`), stored in NVS `user_config` byte 2.

**Type 0, motor stutter:**

```
if (elapsed_s % 30 == 0 && elapsed_s > 10 && !cue_active && cue_ticks > 50 && remaining() > 10) {
    muted = 1; amp_enable(0); cue_active = 1; cue_ticks = 0;
} else if (cue_ticks == 1) {
    if (cue_active) muted = 1;
} else if (cue_ticks > 1 && cue_active) {
    if (motor_state == 0) motor_gear(49, 0); else amp_enable(1);
    muted = 0; cue_active = 0; cue_ticks = 0;
    cue_count = (cue_count == 255) ? 60 : cue_count + 1;
}
cue_ticks++;
```

The amp is off for two fast ticks, **about 60 ms**, once at each multiple of 30 s. The wave keeps
streaming; only GPIO48 drops. There is no cue in the first 10 s or the last 10 s. `remaining()` =
`0x4201bb34`: `total_s - done_s`, except in mode 5 where it returns `done_s` (the display counts up).

**Type 1, voice:** if voice is enabled (`voice[0] == 1`, `0x3fc9f30a`), `elapsed_s % 30 == 0`,
`0 < elapsed_s < 300`, and not (mode 5 with `contact_s` in 118..122): play clip 3. It is requested on
every 30 ms tick of that second; a request while a clip is playing only updates `play_prev`.

Mode 5 extra (`0x4201bb34`): when `contact_s == 120` and not factory mode, clip 6 is requested.

### 3.5 Pause and resume [C, `0x4201c790`]

Short press while `active`:

- If `0x3fca4f51 == 2` or `0x3fca50e1 != 0` (set by a BLE path `0x4201c704`): `handle_motor_stop()`.
- If `done_s >= total_s`: `handle_motor_stop()`.
- If running: **pause**. `motor_state = 0`, clear the contact filter, `unpaused = 0`, `muted = 1`,
  `stop_delay = 0`, `motor_off()`, pause screen (`0x4201c73c`, UI 0x65 with the zone seconds), BLE state 2,
  `vTaskDelay(100)`.
- If paused: **resume**. Clear the contact filter, `motor_state = 0`, `vTaskDelay(50)`, `unpaused = 1`,
  `stop_delay = 200`, `motor_gear(49, 0)` (hum until contact is detected again), `muted = 0`, BLE state 1,
  brushing screen.

`elapsed_s` does not advance while paused. **Pause timeout is 30 s**: `stop_delay` reaches 30 and
`handle_motor_stop()` ends the session.

`0x42019528 handle_motor_stop()`: `active = 0; stop_delay = 200; post 0x80000; motor_off();` then
re-arms the GPIO12 interrupt (`0x4200cc58(1)`).

Other callers of `handle_motor_stop()` [C]: charger inserted (`0x42017a0c` USB_IN_ACTION), brush-task event
bit 4 (`0x4201b638`, OTA start), swiping away from screens 0x52/0x65/0x66 (`0x4201af48`), `0x4201d850`
(HTTP path), long press in factory mode.

### 3.6 End of session [C, `0x4201c2b8`, on bit 0x80000]

1. Stop the 30 ms timer; copy 19 u16 from `ses+0x40` to `0x3fca4e54` and `0x3fca4e2e`.
2. `score = compute_score()`.
3. If `done_s >= 15` **and** `score != 0`: build and store the record (section 6) and add to the daily
   totals (`0x4201bc88`: RTC `0x50001020 += done_s`, `0x5000101e += score`, `0x5000101c += 1`, saved to NVS).
   **15 s is the minimum session that counts.**
4. Next screen, on battery: screen 0x67 (103, "complete") when the session ended from the running
   screens; from the pause screen 0x65 it is 0x67 only if `done_s >= 120`, else the idle screen
   (`0x4201a5ac`). The 1 Hz sequencer posts the score screen one second after 0x67 (`0x4201b1e4` ->
   `0x4201a774(score)`: UI message 100 with one payload byte = score).
5. `vTaskDelay(100)`, re-arm GPIO12 interrupt, `sys[0x35] = 0` (session flag used by BLE handlers).
6. Back in the task: release the PM lock, BLE state 3, `led(2, 1)`, `led(3, 1)`.

---

## 4. Pressure handling

### 4.1 Value and thresholds [C]

`0x420277d4` (on event 0x400): raw force from the AW8686X library call `0x42103774`, negative -> 0,
multiplied by the float `0x3fc9fd78` (a temperature coefficient computed at init by `0x403787cc`, clamped
to 0.75..1.25), stored as s16 in `0x3fca5c80`. `0x42018530` copies `max(0, value)` to `pressure`
(`0x3fca4c74`) every 30 ms. Units are whatever the Awinic library returns [I: grams].

| Threshold | Meaning | Where |
|---|---|---|
| >= 50 | counts as contact for `contact_s` | `0x42019560` |
| 30..399 | "normal" window for the mode-0 angle prompt (clip 4) | `0x420156a0` |
| > 400 | over-pressure | `0x420198fc`, `0x42018530` |
| >= 600 | strong over-pressure: LED 3 mode 2 | `0x42018530` |
| > 600 | request voice clip 1 (if enabled) | `0x420198fc` |
| < 390 for 34 consecutive samples (about 1 s) | release | `0x42019830` |

### 4.2 Per 30 ms: `0x42018530` then `0x420198fc(pressure)` [C]

LEDs (`0x4201dcc8(led, mode, 4)` -> `0x4201d9a4`: led 2 = LEDC channel 2 / GPIO19, led 3 = LEDC channel 3 /
GPIO20; mode 0 writes duty 0x1fff (channel 2 clamps to 4000), modes 1..3 write duty 0; polarity and
blink are the LED subsystem's):

| pressure | motor_state | led 2 | led 3 |
|---|---|---|---|
| <= 400 | 1 | mode 0 | mode 1 |
| <= 400 | 0 | mode 1 | mode 1 |
| <= 400 | 2 | unchanged | unchanged |
| 401..599 | any | mode 1 | mode 0 |
| >= 600 | any | mode 1 | mode 2 |

While paused the task forces `motor_state = 0`, `led(2, 1)`, `led(3, 1)` every 30 ms.

Motor state machine (skipped while a voice clip plays, `clip_state == 0`):

```
state 0 (idle hum):
    push pressure into a 17-sample ring (0x3fca4d5e, index 0x3fca4d5d, "filled once" flag 0x3fca4d5c);
    if (filled && max - min > 29 && sum > 510) {          /* 0x42019860: bristles are on the teeth */
        clear ring; motor_state = 1;
        motor_gear(step[step_index-1].gear, use_app_gear_table);
    }
state 1 (normal):
    if (pressure > 400) { motor_state = 2; motor_gear(step[step_index-1].gear, ...); }
                                                          /* state 2 -> duty halved in 0x4201e7c8 */
state 2 (over-pressure):
    if (pressure > 600 && play_req != 2) voice_clip(1);
    if (34 consecutive samples < 390) { motor_state = 1; motor_gear(step[step_index-1].gear, ...); }
```

So over-pressure **halves the duty** (frequency unchanged; e.g. strength 4: duty 36 -> 18, amp 24) until the
force has been below 390 for about a second. No UI message is posted by this path [C]; the screen does
not change. Factory mode (`0x3fca4e9b == 2`) uses a 1000 Hz tone instead and is not needed.

Anti-splash: with `sys[0x37] == 1` (factory default) every start and every resume begins in state 0, the
160 Hz hum at duty 2, and switches to the real gear only when the contact filter fires (17 samples =
510 ms window). With `sys[0x37] == 0` a session starts directly in state 1.

---

## 5. Start and stop entry points

### 5.1 Button dispatcher `0x4201cab8(kind)` [C]

Registered with the key driver (`0x42019cf8`). `kind` 0 = short press. Ignored when: OTA is running
(`0x3fca4194 != 0`, log "get_ota_start_status = true"), `0x3fca4ea0 != 0` (long-press reset pending),
`0x3fca4dff != 0`, or the charge state `0x3fca4b74` is not 2 (2 = on battery) while awake. A press while
asleep on the charger runs `motorwakeup_for_charge` `0x4201bee8`. **Brushing cannot be started on the
charger.** Otherwise a short press calls `0x4201c790`.

### 5.2 Short press `0x4201c790` [C]

In order:

1. Aging mode (`0x3fca4e91 == 1`): return.
2. Battery percent `0x3fca4b7a == 0` or OTA running: log "low power.ota lock key"; if battery is 0 post the
   low-battery screen 0x5e (94) and set the idle timeout to 1 (`0x42014278(1)`); return. (At boot a reading below 10 % is stored as 0 when the saved
   battery flag is 0, `0x4201831c`.)
3. Shop-demo flag `0x3fca4dca` set (and not factory mode): leave demo ("close guitai"); return.
4. Asleep (`0x3fca41a4 == 1`): wake only (`0x4201b740`, `0x4201bd70`, `0x42019008`).
5. Certain non-idle screens (`0x4201b0d8`): go back to the idle screen; return.
6. Not `active`: start only if the touch controller state `0x3fca4ded == 5` (ready); else return.
7. `active`: pause / resume / stop as in 3.5.

Start sequence (step 6), in this order:

```
remember cfg[6]; led(4, 0); clear gyro wake count; clear contact ring;
sys[0x37] = (factory_mode != 2);  active = 1; muted = 0; unpaused = 1; cue_count = 0;
led(2, 1); led(3, 1); puts("WAKE UP AND BRUAH");
ses start time = now; elapsed_s = 0; clear pressure log (240 bytes);
session_init();                       /* 0x4201ba38 -> 0x420194a8: profile, motor start (3.3) */
BLE notify state 1; start the IMU/zone tracker;
if (cfg[6] != 5)  show brushing screen 0x52;
else { s = RTC strength (1..5 else 3); show strength screen 0x57 (87) with s-1;
       step[0].gear = ses.gear = strength_gear[s]; }          /* 0x42019368 */
vTaskDelay(30); take PM lock; post 0x40000; start the 30 ms timer; sys[0x35] = 1; stop_delay = 200;
```

Note the ordering: `sys[0x37]` is overwritten with 1 at every start in normal mode, so the BLE
anti-splash setting does not survive a start [C].

### 5.3 Gate inside the music task [C]

A start request (bit 0x8000) is executed only if `gpio_get_level(9) != 0` or `0x3fca308f != 0` (BLE
factory command 0x02, `0x4200f228`). The brush task treats GPIO9 low as "charger present":
`0x4201840c` calls `0x42017a0c(0)` ("USB_IN_ACTION") when the charge state is 2 and GPIO9 reads 0, and
`0x42017a0c(1)` after GPIO9 has read 1 for 151 ticks of 10 ms. This contradicts the shared hardware
notes (GPIO8 = charger, GPIO9 = IMU INT); see open questions.

### 5.4 Strength change during a session [C]

UI swipe up/down while `active` and not paused calls `0x4201b50c(up)`: reset the 5 s screen timer,
`s = strength_of(ses.gear) +/- 1` clamped to 1..5, store in RTC `0x50001019`, and **only if the current
screen is 0x57** apply it (`0x420192e8`): show screen 87 with `s-1`, `step[0].gear = ses.gear =
strength_gear[s]`, and if `motor_state != 0` call `motor_gear()` at once. After 5 s without a swipe the
1 Hz sequencer switches the screen id to 0x52. Screen 0x57 is only entered at the start of a mode-5
session (and by the lock-screen restore `0x420284c4`), so in practice strength is adjustable during the
first seconds of a mode-5 session, each swipe extending the window by 5 s.

### 5.5 Notification summary

Brush task group `0x3fca4eac`: 0x40 10 ms tick, 0x200 30 ms tick, 0x400 pressure sample, 0x40000 session
started (no handler), 0x80000 session ended, 0x4 stop request, 0x800000 70 ms tick buzz (setter not
found). Music group `0x3fca4fc8`: 0x8000 / 0x10000 / 0x20000 (1.3). No queues are involved; parameters
travel in the globals of 1.3.

---

## 6. Score and record

### 6.1 Score [C, `0x4201c260`, disassembly checked]

`0x4201c71c` hard-codes 12 zones, 5 s per zone, weight 84.

```c
uint8_t compute_score(const uint16_t zone_s[12]) {
    unsigned sum = 0;
    for (int i = 0; i < 12; i++) {
        unsigned v = (zone_s[i] < 5 ? zone_s[i] : 5) * 84 / 5;   /* 0,16,33,50,67,84 */
        sum += v < 125 ? v : 125;
    }
    unsigned s = sum / 10;
    return s < 100 ? s : 100;
}
```

The score is pure zone coverage: 12 zones with at least 5 s each give 100. Duration and pressure do not
enter. `zone_s[]` is filled once per second by `0x420149e4` from the IMU zone tracker's counters
(`0x3fca436e + {100,120,110,130,0,20,10,30,60,80,70,90}`, each divided by 5 [C]; the tracker's 200 ms gesture timer explains the factor [I]), and only while
`motor_state != 0` [C]. The zone classifier itself (brush_toolbox, QMI8658 plus
magnetometer calibration) is outside this spec; without it the score cannot be reproduced.

Screen inputs: score screen = UI message 100, payload `{score}`. Zone seconds are handed to the UI in
the globals `0x3fca4de2, 4de0, 4dda, 4dd8, 4dde, 4ddc, 4dd6, 4dd4, 4dd2, 4dd0, 4dce, 4dcc` in the order
`zone_s[0,1,2,3,5,4,6,7,8,9,10,11]` (`0x4201a8c0`, `0x4201b170`; note 4 and 5 swapped). The brushing
screen gets `{paused ^ 1, (total_s - done_s) & 0xff}` (`0x4201a82c`).

### 6.2 Record [C, `0x42019f20`]

`len = min(182, done_s/2 + 51)`. Buffer pre-filled with 0xff from byte 2.

| Byte | Content |
|---|---|
| 0..1 | `len`, big endian (byte 0 is always 0) |
| 2..7 | start time: year-2000, month, day, hour, minute, second |
| 8 | profile id (`ses+0x2a`) |
| 9..10 | `total_s`, big endian |
| 11..12 | `done_s`, big endian |
| 13, 14, 15 | constants 10, 20, 70 |
| 16..18 | 0 |
| 19 | `0x3fca4ebf` (RTC/time block byte 7) |
| 20 | low byte of `zone_s[0]` |
| 21..27 | 0xff |
| 28 | score |
| 29 | scheme type: 3, or 1 (morning) / 2 (evening) when `cfg[1] == 1` and mode 0 |
| 30 | `cfg[2]` |
| 31 | `0x3fca42a8` (count of clip-2 events) |
| 32..35 | low bytes of `zone_s[8..11]` (`ses+0x5a..0x60`) |
| 36..50 | 0xff |
| 51.. | pressure log: one byte per 2 s, `pressure / 4`, 0xff if `pressure >= 1000` |

Storage (`0x42024e5c`): the data partition (type 0x40, subtype 0), offset **0x7fb000**, a 0x16c0-byte
image that is read, erased (`esp_partition_erase_range(part, 0x7fb000, 0x5000)`), patched at the running
byte offset `0x3fca4dc2` and rewritten. At most 33 records; the count `0x3fca4dbd` and a wrap marker
`0x3fca4dbe == '!'` live in NVS `user_config` bytes 11 and 10. Records are uploaded and cleared by the
BLE/HTTP code (not covered).

Daily totals in RTC memory, see 3.6.

---

## 7. Voice clips through the motor

The board does use them [C]. They are **MP3**, decoded by the ESP-ADF pipeline built in
`brush_decoder_app` `0x4201eca4`: custom read callback `0x40377f3c` -> `mp3_decoder` (`0x42069344`, identified by the "MP3_DECODER" strings and the
log text "i2s_stream_writer/mp3_decoder" [I]) ->
`i2s_stream` writer; the I2S clock follows the stream info (`i2s_stream_set_clk`, log
"i2s_stream_set_clk : %d"). **Reproducing them needs an MP3 decoder** (stock links esp-adf's
`pvmp3`). Sample rate and channel count of the clips are unknown; the data is not in the app image.

Location [C]: data partition (type 0x40, subtype 0), base 0x9e0000 if the voice version string
(`0x3fc9f30e`, NVS) starts with 'B', else **0x800000** (`0x40378754`). From the base: 16 language blocks
back to back, each block = 7 clips, each clip = `u32 length (LE)` + MP3 bytes (`0x4201ef64` walks them
and stores block offsets in RTC `0x50001024[16]`). Language = `sys[0x67]` (`0x3fca33de`); values 0 and 1
use block 0, 2..16 use block `lang-1`, anything else block 0 ("language flag is invalid"). `0x4201efc8`
builds `clip_start[i]` at `0x3fca4f70`; the read callback streams `clip_start[i] .. clip_start[i+1]`
(which includes the next clip's 4 length bytes).

While a clip plays the wave generator is idle, so the head does not vibrate; afterwards the task
restores 24 kHz mono and resumes the wave (1.3), except for clip 0.

| Clip | Trigger [C] | Meaning [I] |
|---|---|---|
| 0 | BLE test flag `0x3fca33df == 1` (`0x420104b8`) | test / demo sound |
| 1 | `motor_state == 2` and pressure > 600; needs `voice[0] == 1 && voice[2] == 1` | too much pressure |
| 2 | IMU gesture `0x42014b94`, pressure <= 400; needs `voice[0] == 1 && voice[1] == 1`; also every 5 s in a production test | brushing motion warning |
| 3 | every 30 s when the cue type is 1 | change zone |
| 4 | mode 0 with `cfg[2] == 1`, pressure 30..399 for 67 ticks (`0x420156a0`) | brush angle hint |
| 5 | mode 0 with `cfg[2] == 1`, zone change detected (`0x42015544`) | zone hint |
| 6 | mode 5, `contact_s == 120` | two minutes done |

`0x4201f0b4(index)` refuses any clip while `muted` (pause or the stutter cue). `voice[0..2]`
(`0x3fc9f30a`) default to 1, 1, 1 and live in NVS `user_config` bytes 48..50.

A custom firmware without MP3 can drop all clips: the default zone cue is the motor stutter, and
over-pressure is still signalled by the halved amplitude and the LEDs.

---

## 8. Persistent settings touched by this subsystem [C]

NVS namespace `storage`, blob `user_config` (100 bytes, first 80 used; load `0x42018a74`, save
`0x420185b4`, defaults `0x420187b4`):

| Byte | Variable | Default |
|---|---|---|
| 2 | `sys[2]` zone-cue type | 0 (stutter) |
| 4 | `cfg[0]` app scheme present | 0 |
| 5 | `sys[0x37]` anti-splash | 1 |
| 7 | profile id | 0 |
| 10, 11 | record wrap marker, record count | 0, 0 |
| 12 | use app gear table | 0 |
| 19 | `cfg[6]` mode | 5 |
| 39 | `sys[6]` mode by time of day | 0 |
| 43..47 | `cfg[1..5]` | 2, 0, 180, 1, 16 |
| 48..50 | `voice[0..2]` | 1, 1, 1 |
| 62 | last score | 0 |

Strength: RTC memory `0x50001019` (lost on power loss -> 3). Blob `motor_data` (255 bytes): app schemes.

---

## 9. Open questions

1. **Mode names.** Only picture ids are known; which of modes 1..4 is "clean", "whiten", "massage" etc.
   needs the picture partition. The motor data says: 1 = 230 Hz pulsing (morning), 2 = 210 Hz strongest
   (evening), 3 = 255 Hz gentle steady, 4 = 225 Hz swell.
2. **GPIO9.** Code at `0x4201840c` / `0x42017a0c` / `0x4201f291` treats GPIO9 low as "charger present" and
   blocks motor starts while it is low. The shared notes call GPIO9 the IMU interrupt and GPIO8 the
   charger line. One of the two is wrong, or the line is shared. Until settled, do not copy the GPIO9
   gate into the custom firmware; use whatever charger detect is verified.
3. **I2S slot.** Stock sends mono data with `slot_mask = RIGHT`; the custom firmware currently sends the
   same sample in both slots and the motor does run that way. The amp part is not identified, so it is
   unknown whether it plays right only (then both configurations give the same drive) or the L/R mix
   (then stock's drive is half of what the same sample values give with both slots filled). To match
   stock exactly, use `slot_mask = I2S_STD_SLOT_RIGHT`; check by feel or with a scope on the coil.
4. **Gear bytes b3, b5, b6** are not read by the motor path. Unknown purpose (b5/b6 look like a pair:
   10/50, 30/0, 25/10, 20/10).
5. **Pressure units and scale.** Thresholds are in the AW8686X library's output units times a
   temperature coefficient; the library internals were not analysed here.
6. **LED polarity / blink** for led 2 (GPIO19) and led 3 (GPIO20) and what "mode 2" does: LED subsystem.
7. **Zone classifier** (inputs to the score and zone screens): not analysed; lives in brush_toolbox
   (`0x42014b94` .. `0x420156a0` area, 200 ms gesture timer).
8. **Setter of brush-task bit 0x800000** (70 ms / 401 Hz tick) was not found.
9. **Voice clip audio parameters** (sample rate, mono/stereo, bitrate) need a dump of the data partition
   at 0x800000.
10. The profile step byte `a` and profile flags (bits 7..5 of byte 1) are stored but not used by any
    function analysed.
11. `0x3fca4f51` (value 2 disables the zone cue and turns a press into stop) and `0x3fca4dff` (blocks the
    button) have no writer in the decompiled set other than a clear; their source is unknown.

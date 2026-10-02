# Stock firmware spec: USER INPUT (touch, button, other input lines)

Scope: the touch IC, the physical button, the gesture decoder, and how raw input becomes
UI/brush commands in the stock Oclean X Ultra 20 image.
Every item is tagged **CONFIRMED** (read from code/data at the address given) or **INFERRED**
(reasoning given). Addresses are stock VMAs. Helper scripts used to extract the tables are in
`spec/work_input/` (`dump.py`, `gen_iqs_init.py`).

## 0. Summary of what matters

| Topic | Result |
|---|---|
| Touch IC | **Azoteq IQS7222D** trackpad controller, I2C address **0x44**, on the **bit-bang I2C bus SCL=GPIO13 / SDA=GPIO14** (same bus as the AW8686X at 0x6A). RDY line = **GPIO12** (input, pull-up, falling edge). Hardware I2C0 on 35/36 is not used for it. |
| Other IQS types | The IQS620/624/625/269A/7211A/7222A/7222C/7228A branches are probe-and-print only; the driver forces type "7222D" before init. Dead code. |
| AW8686X | Brushing-pressure sensor only. It produces no UI input (no tap/press event). |
| Gestures that exist | swipe up / down / left / right and a "long touch" (which is a no-op in the UI). **There is no tap and no double-tap** in the stock firmware. |
| Button | GPIO3, active low. Everything is done **inside the GPIO ISR** plus four one-shot esp_timers (2 s, 3 s, 5 s, 8 s). The `key_int` task is dead (its queue is never written). |
| GPIO8 / GPIO9 | **Roles are the opposite of what CONTEXT.md assumes**: GPIO8 = IMU any-motion interrupt (active high, EXT0 wake), GPIO9 = charger present (active LOW, "USB_IN"). See section 6. |
| GPIO2 | Falling edge only clears the charger-removal debounce counter. |

---

## 1. Touch hardware: IQS7222D

### 1.1 Identification and bus

- **CONFIRMED** device address 0x44: every touch transfer passes `0x44` to the bus helpers
  (`0x42025e80`, `0x42025ebc`, `0x42025ee4`, `0x42025f08`, `0x420267b4`, `0x420267e4`,
  `0x42026814`, `0x420269b0`, `0x42026434`). The helpers shift it left by one (`0x4200cbd0`,
  `0x4200cbec`, `0x4200cc30`), so the wire bytes are 0x88 (write) / 0x89 (read).
- **CONFIRMED** bus = bit-bang on GPIO13/GPIO14: `0x4200cbd0 -> 0x4201ec30`, `0x4200cbec -> 0x4201ebdc`,
  `0x4200cc30 -> 0x4201eb54`, all built from `0x4201e89c` (START), `0x4201e8c0` (STOP), `0x4201e920`
  (send byte), `0x4201e9b4` (read ACK), `0x4201eae8` (read byte), `0x4201ea6c` (send ACK/NACK). These only
  touch `gpio 0xd` (clock) and `gpio 0xe` (data). The AW8686X helpers (`0x420269ec`, `0x42026a24`,
  `0x42026a3c`) call the same three functions with address 0x6A, so both chips share this bus.
- **CONFIRMED** RDY = GPIO12: `0x4200cc4c` returns `gpio_get_level(12)` and is the "window open" test
  at the top of the init function `0x42025f2c`; `0x4200cc58` installs the GPIO12 interrupt.
- **CONFIRMED** chip id read: `0x42026434` reads 8 bytes from register 0x00 and compares bytes
  0..2 with `16 04 00` to print "The IC type is IQS7222D" (product number 0x0416 = 1046).
- **CONFIRMED** the id result is irrelevant: `0x42026754` writes `g_3fc9c4e4 = 0x0c` (the "7222D"
  code, also the `.data` default at 0x3fc9c4e4) before calling the init, and the run-time reader
  `0x42026780` only handles type 0x0c. So only the IQS7222D path is live.
- **INFERRED** the chip is really fitted and working: the button handler refuses to start a brushing
  session unless the touch state machine is in state 5 (`0x4201c84c`..`0x4201c855`), and state 5 is only
  reachable through RDY falling edges on GPIO12. A board without the chip could never start brushing
  from the button.

### 1.2 Bit-bang I2C protocol (0x4201e844..0x4201ec90)

Open-drain is emulated: "release" = `gpio_set_direction(pin, GPIO_MODE_INPUT)`, "drive low" =
`gpio_set_direction(pin, GPIO_MODE_OUTPUT)` + `gpio_set_level(pin, 0)`. No internal pull-ups are
enabled on 13/14 (`0x4200d77c` configures both as plain inputs at the end of GPIO setup), so the
lines need external pull-ups (**INFERRED**). There are **no delays** anywhere in the bus code; the
speed is whatever the GPIO calls take (**CONFIRMED** no delay calls; resulting frequency unknown).

After every release the code polls the pin until it reads high, at most 1000 polls (counter at
0x3fca4f54). This gives clock-stretch tolerance on SCL.

| Primitive | Address | Sequence |
|---|---|---|
| START | 0x4201e89c (+0x4201e844) | release SDA, wait high; release SCL, wait high; SDA low; SCL low |
| STOP | 0x4201e8c0 | SDA low; release SCL, wait high; release SDA, wait high |
| send byte | 0x4201e920 | 8 bits MSB first: set SDA (release for 1, low for 0); release SCL, wait high; SCL low. After bit 8: release SDA |
| read ACK | 0x4201e9b4 | release SDA and SCL, wait SCL high; poll SDA up to 160 times; low = ACK (return 1), else return 0; SCL low |
| read byte | 0x4201eae8 | release SDA; 8x: release SCL, wait high, sample SDA, SCL low |
| send ACK | 0x4201ea6c(0) | SDA low; release SCL, wait high; SCL low |
| send NACK | 0x4201ea6c(1) | release SDA; release SCL, wait high; SCL low |

Transfers (all end with STOP; return value is always 0, errors are not propagated):

- `write8(addr7, reg, data, n)` = `0x4200cbd0` -> `0x4201ec30`: START, addr<<1, [retry START+addr
  until ACK, at most 4 attempts], reg, data[0..n-1], STOP.
- `write16(addr7, reg_hi, reg_lo, n, data)` = `0x4200cbec` -> `0x4201ebdc`: START, addr<<1 (same
  retry), reg_hi, reg_lo, data[0..n-1], STOP. Used for IQS registers 0x8000 and above through
  `0x42025e80(reg_hi, reg_lo, n, data)`.
- `read(addr7, reg, n, buf)` = `0x4200cc30` -> `0x4201eb54`: START, addr<<1 (retry), reg, repeated
  START, (addr<<1)|1, n bytes (ACK after each except NACK after the last), STOP.

### 1.3 RDY pin (GPIO12) handling

| When | What | Address | Status |
|---|---|---|---|
| Boot, `brush_gpio_cfg` | `gpio_config{pin 12, mode OUTPUT, pull_up 1, intr NEGEDGE}` then `gpio_set_level(12, 0)` - RDY is **driven low** from here until the main task enables the interrupt | 0x4200da4c..0x4200da6a | CONFIRMED |
| Enable | `0x4200cc58(1)`: `gpio_config{pin 12, mode INPUT, pull_up 1, pull_down 0, intr NEGEDGE}`; `gpio_isr_handler_add(12, 0x40377d88, (void*)12)` | 0x4200cc58 | CONFIRMED |
| Disable | `0x4200cc58(0)`: same `gpio_config`, then `gpio_isr_handler_remove(12)` | 0x4200cc58 | CONFIRMED |
| ISR | if `init_ok (0x3fc9f309) >= 5` and `gpio_get_level(12) == 0`: `xEventGroupSetBits(main_evt 0x3fca4eac, 0x100)` | 0x40377c9e..0x40377cb2, 0x40377f30 | CONFIRMED |
| Before light/deep sleep | `gpio_config{pin 12, mode DISABLE}`; `gpio_set_level(12,0)`; `gpio_hold_en(12)` | 0x4200dbbc, 0x4200dda4 | CONFIRMED |
| On wake | `gpio_hold_dis(12)` (also 13, 14) | 0x4200df88, 0x4200d77c | CONFIRMED |

**INFERRED**: driving RDY low for the first several hundred ms of boot acts as a reset/hold of
the IQS (on Azoteq parts the RDY pin doubles as a reset/force-comms input). The code never
explains it; replicate it as is.

The main task (`0x4201cc40`) handles bit 0x100 by calling the touch state machine `0x4201b458`
once per RDY falling edge.

Where the interrupt is enabled/disabled (all **CONFIRMED**, calls to `0x4200cc58`):

- enabled: main-task init (`0x4201cc40`, only if byte 0x3fca5e3f == 0; no write to that byte was
  found in the image, so always), both wake functions (`0x4201bd70`, `0x4201bee8`), motor stop `0x42019528`, brush pause
  (`0x4201ca5d -> 0x4201c835`), end of session `0x4201c2b8`, leaving aging mode.
- disabled: brush start when the mode is not 5 (`0x4201c9b4`), brush resume (`0x4201ca6d`),
  going to sleep (`0x4201b764`), the 1 Hz sequencer when the strength screen 87 times out back to
  the brushing screen (`0x4201b1e4`), factory reset (`0x4201c6b8`), the two HTTP download
  routines (`0x42013b40`, `0x42013e24`), entering aging mode (`0x4201cbe5`).

So touch is live on idle screens, while paused, and during a session in mode 5 while the
strength screen is up. It is off while the motor runs in the other modes.

### 1.4 Driver state machine (0x4201b458, one step per RDY falling edge)

State byte `0x3fca4ded`, set with `0x4201b430(state)`; `0x4201b430(0)` additionally sends
"force comms" (`0x42025ebc`: `write8(0x44, 0xFF, {0x00}, 1)`) and clears the RDY event counter
`0x3fca4dec` (which otherwise counts RDY events up to 100).

| State | Action on the next RDY edge | Next state |
|---|---|---|
| 0 | nothing | 0x20 |
| 0x20 | id probe `0x42026434` (read reg 0x00, 8 bytes; print only) | 0x21 |
| 0x21 | full init `0x42026754(0)` -> `0x42025f2c(0)`. If GPIO12 is high at entry the init is skipped and the state stays 0x21 | 1 on success |
| 1 | `0x420267b4`: `write8(0xD0, {0x05})` (ack reset + re-ATI) | 5 |
| 2, 3, 10, 11 | nothing | 5 |
| 5 (run) | `0x420269b0`: `read(0x10, 12 bytes)` into 0x3fca5a48; if `(u16 status & 0x000A) != 0` -> state 0x21 (re-init). Otherwise `0x42026780` -> `0x42026418` -> `0x420263fc`: `write8(0xDB,{0x0D})`, gesture sample `0x42025ca4(X, Y)`, `write8(0xFF,{0x00})` | 5 |
| 6 | `0x420267e4`: `write8(0xD0, {0x08})` (reseed) | 5 |
| 7 (sleep) | prints " ###### iqs7222d_channel_enable sleep ok", runs the sleep sequence `0x42026814` | 7 |
| 4, 8, 9, other | nothing | unchanged |

(Decoded from the disassembly at 0x4201b458..0x4201b4f1; **CONFIRMED**.)

Who sets the state (**CONFIRMED**):

| Call | Where | Meaning |
|---|---|---|
| `(0)` | main-task init 0x4201cc40 (after `init_ok = 5`) | cold start: force comms, then 0 -> 0x20 -> 0x21 -> 1 -> 5 |
| `(0)` then `(0x21)` | wake functions 0x4201bd70, 0x4201bee8 | force comms, then full re-init (no id probe) |
| `(0x21)` | brush pause, 0x4201ca45 | full re-init when the motor stops |
| `(6)` | end of session 0x4201c2b8; charger removed while awake 0x42017a0c; after a swipe up/down handled while brushing (UI task 0x42022cb8 area); 1 Hz watchdog 0x42025e20 | reseed |
| `(7)` | `sleep_brush_task` 0x4201b900 (500 ms before the sleep work) and again in 0x4201b764 | put the IC to sleep |

1 Hz reseed watchdog `0x42025e20` (called from `0x4201b4f8` only when state == 5, from the main
loop's once-per-second block when not brushing, or brushing but paused; **CONFIRMED**):

```
if (last_y_raw (0x3fca5a40) == 60000) {        // no finger
    if (++no_touch_secs (0x3fca5a17) >= 3) { no_touch_secs = 0; set_state(6); }
    touch_secs (0x3fca5a16) = 0;
} else {                                       // finger present
    if (++touch_secs >= 3) { touch_secs = 0; set_state(6); }
    no_touch_secs = 0;
}
```
So the chip is reseeded every 3 s, whatever the finger state.

### 1.5 Init sequence (0x42025f2c with argument 0, the only live call)

Precondition: `gpio_get_level(12) == 0`, otherwise the function returns 1 and nothing is sent.
No delays between writes. All multi-byte values are little-endian 16-bit words.
"W8" = `write8` (1-byte register), "W16" = `write16` (2-byte register, high byte first).
Bytes were extracted from the image with `work_input/gen_iqs_init.py`; **CONFIRMED**.

```c
/* step, kind, register, bytes                                              source */
 0 W8  DB    0D                                                           /* literal, 0x42025ee4 */
 1 W8  D0    00 00 02 00 FF FF 05 00 0C 00 05 00 28 00 0A 00 64 00 FF     /* .data 0x3fc9c485, 19 bytes */
 2 W16 8000  7F 05 62 70  7F 05 62 10  7F 05 62 20  7F 05 62 40
             7F 0C 62 80  7F 0C 62 20  7F 0C 62 04                        /* rodata 0x3c11c6ed, 28 bytes */
 3 W16 8700  83 2B 10 30 00 02                                            /* 0x3c11c6e7 */
 4 W16 9000  08 12 0F 00 08 28                                            /* 0x3c11c6e1 */
 5 W16 9100  08 12 0F 00 14 28                                            /* 0x3c11c6db */
 6 W16 9200  08 12 0F 00 14 28                                            /* 0x3c11c6d5 */
 7 W16 9300  08 12 0F 00 14 28                                            /* 0x3c11c6cf */
 8 W16 9400  0A 12 0F 00 08 28                                            /* 0x3c11c6c9 */
 9 W16 9500  0A 12 0F 00 08 28                                            /* 0x3c11c6c3 */
10 W16 9600  0A 12 0F 00 08 28                                            /* 0x3c11c6bd */
11 W16 9700  08 12 0F 00 08 28                                            /* 0x3c11c6b7 */
12 W16 9800  08 12 0F 00 14 28                                            /* .data 0x3fc9c4db */
13 W16 9900  08 12 0F 00 14 28                                            /* 0x3c11c6b1 */
14 W16 9A00  83 2B 10 30 00 02                                            /* 0x3c11c6e7 again (same pointer as step 3) */
15 W16 9B00  0A 12 0F 00 08 28                                            /* 0x3c11c6ab */
16 W16 9C00  0A 12 0F 00 08 28                                            /* 0x3c11c6a5 */
17 W16 9D00  0A 12 0F 00 08 28                                            /* 0x3c11c69f */
18 W16 A000  33 5D 45 64 E4 2F EA 69                                      /* .data 0x3fc9c4d3  (ch0)  */
19 W16 A100  13 5D 45 60 E1 39 E8 69                                      /* .data 0x3fc9c4cb  (ch1)  */
20 W16 A200  13 5D 45 60 E1 39 DE 69                                      /* .data 0x3fc9c4c3  (ch2)  */
21 W16 A300  13 5D 45 60 E1 35 EB 71                                      /* .data 0x3fc9c4bb  (ch3)  */
22 W16 A400  23 14 3D 80 E1 35 FE 69                                      /* 0x3c11c697 */
23 W16 A500  23 14 3D 3E E1 39 E4 61                                      /* 0x3c11c68f */
24 W16 A600  23 14 3D 3E E1 39 FB 69                                      /* 0x3c11c687 */
25 W16 A700  13 5D 45 64 E4 31 EE 71                                      /* .data 0x3fc9c4b3  (ch7)  */
26 W16 A800  13 5D 45 60 E1 3B F5 71                                      /* .data 0x3fc9c4ab  (ch8)  */
27 W16 A900  13 5D 45 60 E1 39 DB 69                                      /* .data 0x3fc9c4a3  (ch9)  */
28 W16 AA00  13 5D 45 60 E1 35 F0 69                                      /* .data 0x3fc9c49b  (ch10) */
29 W16 AB00  23 14 3D 3E E1 35 FA 61                                      /* 0x3c11c67f */
30 W16 AC00  23 14 3D 3E E1 3D FA 69                                      /* 0x3c11c677 */
31 W16 AD00  23 14 3D 3E E1 35 F0 61                                      /* 0x3c11c66f */
32 W16 AE00  42 D8 D8                                                     /* 0x3c11c66c, 3 bytes */
33..40 W16 B000..B007, 2 bytes each:
             23 0A | 3C 3C | 3C 3C | 1A 36 | FF 00 | FF 00 | 8F C7 | EE 06 /* 0x3c11c65c */
41..52 W16 B008..B013, 2 bytes each:
             52 04 | 74 04 | 96 04 | 40 05 | 62 05 | 84 05 | 00 00 x6      /* 0x3c11c644 */
53 W16 B014  1F 0F 4D 64 00 00 00 00                                      /* 0x3c11c63c, 8 bytes */
54..62 W16 C000..C008, 2 bytes each: 00 00 (all nine)                     /* 0x3c11c62a */
63 W8  DB    0C                                                           /* .data 0x3fc9c484 */
64 W8  DC    00                                                           /* .bss 0x3fca5c58, never written */
65 W8  DD    1E 00                                                        /* 0x3c11c628 */
66 W8  FF    00                                                           /* literal, 0x42025f08 */
```

Notes:

- Steps 33..52 and 54..62 are separate 2-byte transfers, one register each (loops at
  0x42026238, 0x42026269, 0x420262a8), not one burst.
- **Second and later inits differ in one byte per trackpad channel.** The eight `.data` channel
  arrays (ch0,1,2,3,7,8,9,10) start with byte[1] = 0x5D. The sleep function writes them with
  byte[1] = 0x54 and then leaves **0x55** in RAM (0x42026986..0x420269a6). After the first sleep
  every init therefore sends `xx 55 ...` instead of `xx 5D ...` for those eight channels. The
  other six channels (4,5,6,11,12,13) have byte[1] = 0x14 and never change.
- The alternative tables at rodata 0x3c11c462..0x3c11c4b1 (eight 9-byte entries) and the
  `0xB0` value for the first system byte are only used when the init argument is non-zero.
  The only caller passes 0 (0x4201b4bd). Dead data.
- After the init the state machine sends `W8 D0 05` on the next RDY edge (state 1).

Register meaning (**INFERRED** from the public IQS7222 register map as used by the Linux
`iqs7222` driver, which lists the IQS7222D groups as cycle 0x8000 x7, global 0x8700, button
0x9000 x14, channel 0xA000 x14, filter 0xAE00, trackpad 0xB000 (24 words), GPIO 0xC000, system
0xD0..; I could not check a datasheet here):

- 0xD0 low byte: bit0 ack-reset, bit2 re-ATI, bit3 reseed, bits5:4 power mode (0 normal, 2 ULP,
  3 auto), bits7:6 interface (0 streaming, 1 event, 2 stream-in-touch).
- 0xD0 block words: D1 = 0x0002 ATI error timeout, D2 = 0xFFFF, D3 = 5, D4 = 12 (normal-power
  report rate, ms), D5 = 5, D6 = 40 (low-power report rate, ms), D7 = 10, D8 = 100 (ULP report
  rate, ms), D9 low byte = 0xFF.
- 0xDA: event mask. 0xDB: comms setup, bit0 = "hold the window open across STOP" (0x0D = on,
  0x0C = off). Writing register 0xFF closes a held window, or requests a window when none is open.
- Channel setup byte[1] bit0 = channel enable. Enabled channels are 0,1,2,3,7,8,9,10.
- B004 = B005 = 0x00FF: trackpad X and Y resolution 255. This matches the `255 - value`
  inversion in the decoder (section 2).

### 1.6 Reading events (state 5)

`read(0x44, reg 0x10, 12 bytes)` into `u16 st[6]` at 0x3fca5a48 (**CONFIRMED** 0x420269b0):

| Word | Register | Use in stock code |
|---|---|---|
| st[0] | 0x10 system status | `st[0] & 0x000A` non-zero -> re-init (state 0x21). **INFERRED** bit1 = ATI error, bit3 = reset occurred |
| st[1] | 0x11 | unused |
| st[2] | 0x12 | unused |
| st[3] | 0x13 | unused |
| st[4] | 0x14 trackpad X | first argument of the decoder |
| st[5] | 0x15 trackpad Y | second argument of the decoder |

No other register is read at run time. Gesture flags of the IC are not used; gestures are
decoded in the ESP32 from X/Y only.

### 1.7 Sleep sequence (0x42026814, state 7)

**CONFIRMED** from the disassembly 0x42026814..0x420269ae. No delays.

```c
W8  DB   0D
W16 A000 33 54 45 64 E4 2F EA 69     /* ch0  with byte[1]=0x54 */
W16 A100 13 54 45 60 E1 39 E8 69     /* ch1  */
W16 A200 13 54 45 60 E1 39 DE 69     /* ch2  */
W16 A300 13 54 45 60 E1 35 EB 71     /* ch3  */
W16 A800 13 54 45 60 E1 3B F5 71     /* ch8  */
W16 A900 13 54 45 60 E1 39 DB 69     /* ch9  */
W16 AA00 13 54 45 60 E1 35 F0 69     /* ch10 */
W16 A700 13 54 45 64 E4 31 EE 71     /* ch7  */
W8  D0   20
R   10   12 bytes (result unused)
W8  DA   00 00
W8  13   00
W8  D0   A0
/* RAM only: byte[1] of the eight channel arrays = 0x55 */
W8  FF   00
```

It only runs if an RDY edge arrives while the state is 7. `sleep_brush_task` (0x4201b900) sets
state 7, waits 500 ms, then `0x4201b764` removes the GPIO12 interrupt and the GPIOs are parked
(section 1.3).

Wake: `0x4201bd70` (battery) / `0x4201bee8` (on charger): `gpio_hold_dis`, 100 ms delay, IMU, LCD
and pressure re-init, then `0x4200cc58(1)`, `set_state(0)` (force comms), `set_state(0x21)`.
The next RDY edges run the full init (section 1.5), then `D0 = 05`, then state 5.

---

## 2. Gesture decoder

Two pieces: the per-sample function `0x42025ca4` (main task, once per RDY event in state 5) and
the end-of-touch function `0x42025afc` ("gesture_timer_cbb", UI task). **CONFIRMED** from the
disassembly of both.

The "ggesture" 200 ms esp_timer (`0x42014d0c`, `brush_toolbox_task`, callback 0x420143f8) is
**not** touch: it belongs to the IMU brushing-posture code. The touch timer is `ui.gesture_timer`
("touch_timer", handle 0x3fca50b0, callback 0x4201f648), a **70 ms one-shot**.

### 2.1 State

| Variable | Address | Meaning |
|---|---|---|
| x, y | 0x3fca5a3c, 0x3fca5a40 (u32) | last position, `255 - raw`; both 60000 when no finger |
| hist[3] | 0x3fca5a1a (u16 x3) | hist[0] = current x, hist[1] = previous, hist[2] = one before |
| count | 0x3fca5a32 (u16) | samples in this touch |
| start_x, start_y | 0x3fca5a26, 0x3fca5a28 (s16) | position of sample 2 |
| min_x, max_x | 0x3fc9c480 (init 1000), 0x3fca5a24 (init 0) | extremes, lagging one sample |
| dx, dy | 0x3fca5a2a, 0x3fca5a2c (s16) | see below |
| long_fired | 0x3fca5a20 (u8) | long touch already reported |
| was_running | 0x3fca5a18 (u8) | a sample arrived while the motor was running |

### 2.2 Per sample: `touch_sample(X, Y)` (0x42025ca4)

```c
if (X > 0xEA5F && Y > 0xEA5F) { x = y = 60000; return; }   // no finger
x = 255 - X;  y = 255 - Y;
hist[2] = hist[1]; hist[1] = hist[0]; hist[0] = (u16)x;     // 0x42025ae4
touch_counter (0x3fca4e14)++;                               // 0x4201b5d8
esp_timer_stop(gesture_timer); esp_timer_start_once(gesture_timer, 70000 us);   // 0x42021348
count++;
if (x > 130) cnt_x_high (0x3fca5a30)++;                     // written, never read
if (brushing && !brush_paused_or_idle()) was_running = 1;   // 0x420197dc / 0x420197e8
if (count < 3) { start_y = (s16)y; start_x = (s16)x; return; }
/* production test only: if (0x3fca4d8d == 0xD7 && 0x3fc9c47f) { "266_touch" counter++ } */
if (x < (u32)(s32)min_x) min_x = hist[1];
if ((u32)(s32)max_x < x) max_x = hist[1];
ext = (abs(max_x - start_x) >= abs(min_x - start_x)) ? max_x : min_x;
dy  = (s16)(y - start_y);            // current y
dx  = (s16)(ext - start_x);          // farthest x excursion so far
if (!long_fired && abs(dx) <= 49) {
    if (count == 80) { ui_post(0x0B, payload {2}); count = 0; long_fired = 1; }
    else if (count > 100) count = 0;
}
```

`brushing` = byte 0x3fca4d5b. `0x420197e8()` returns `brushing` when byte 0x3fc9aba5 < 31, i.e.
"session active but motor paused" (0x3fc9aba5 is 200 while the motor runs, 0 when paused).

### 2.3 End of touch: 70 ms after the last finger sample (0x42025afc)

The timer callback sets UI event bit 0x10. The UI task (0x42022564) calls `0x42025afc` only when
the charge state `*(int*)0x3fca4b74 == 2` (running on battery); on the charger the event is
dropped and the gesture state is not reset.

```c
if (count >= 6 && !long_fired && cfg[0x69] != 1 && current_screen != 94) {
    idle_timer_restart();                                  // 0x42014298
    if (brushing && brush_paused && was_running) goto reset;
    adx = abs(dx); ady = abs(dy);
    if (adx >= 20 && !reset_flag (0x3fca4ea0) && ady < adx) {
        if (dx > 0) ui_post(0x0A, payload {p});            // "swipe up"    0x420214e4
        if (dx < 0) ui_post(0x08, payload {p});            // "swipe down"  0x420214c0
    } else if ((adx <= 19 && ady >= 20) || (adx >= 20 && adx <= 49 && ady > adx)) {
        if (dy > 0) ui_post(0x09);                         // "swipe right" 0x420214d4
        if (dy < 0) ui_post(0x07);                         // "swipe left"  0x420214b0
    }
}
reset: count = 0; was_running = 0; cnt_x_high = 0; min_x = 1000; long_fired = 0; max_x = 0;
```

`p` = 0 while brushing, low byte of `start_y` otherwise. The UI task does not read it.
`cfg[0x69]` is byte 0x3fc9a707 of the persisted config block at 0x3fc9a69e (loaded by 0x42018a74);
any button event clears it in RAM (0x4201cb05). Screen 94 is the low-battery screen.

Thresholds: minimum 6 samples; 20 counts of travel on a 0..255 axis; 49 = cross-axis limit for
a left/right swipe; long touch at exactly 80 samples with less than 50 counts of x travel.
**INFERRED** timing: with the normal-power report rate of 12 ms, 6 samples is about 70 ms and 80
samples about 1 s.

Axis naming: the IC's X axis (after inversion) is the "up/down" direction of the UI and its Y
axis the "left/right" direction. Which physical direction on the handle is "up" cannot be read
from the code.

### 2.4 UI messages

`ui_post(cmd, payload, len)` = `0x4201f840`: appends a 14-byte record `{u16 cmd; u16 pad; u8
payload[10]}` to the array at 0x3fca5012 (max 10 entries, count at 0x3fca509e, no lock) and sets
UI event bit 0x01 on group 0x3fca50e4.

| cmd | Poster | Meaning | UI task handling (0x42022564) |
|---|---|---|---|
| 0x07 | 0x420214b0 | swipe left | if locked on a lockable screen: show lock popup (0x42028640). Else if `swipe_allowed()`: `page_nav(3)` (0x420208f8) |
| 0x09 | 0x420214d4 | swipe right | same with `page_nav(4)` |
| 0x0A | 0x420214e4 | swipe up | lock popup as above. Else if `swipe_allowed()` and (motor wave playing 0x4201f178, or not brushing, or paused): not brushing or paused -> `0x42020e14(1)` (pick next screen) then `page_nav(1)`; motor running -> `strength_step(1)` (0x4201b50c). Afterwards, if brushing: touch `set_state(6)` |
| 0x08 | 0x420214c0 | swipe down | mirror: `0x42020e14(2)` + `page_nav(2)`, or `strength_step(0)` |
| 0x0B | 0x42021508 | long touch | stores the payload in 0x3fca509f and calls 0x42102270, which is an empty function. **No visible effect** |
| 0x0C | 0x420214f8 | auto "swipe down" | posted every 3 s by the 1 Hz sequencer in shop-display mode (0x4201b1e4); same as 0x08 without the lock check |

`swipe_allowed()` = `0x4202860c`: not locked, or current screen is 92, 83 or 84.
"locked on a lockable screen" = `0x420285e0`: lock flag set and current screen in
{76,77,78,79,80,81,87,82} (table at 0x3c11cf24).

`strength_step(up)` (0x4201b50c): level = `0x42019398()` (1..5 from the current motor gear byte);
up: +1 capped at 5, down: -1 floored at 1; stored in RTC byte 0x50001019; if the current screen
is 87, `0x420192e8(level)` shows it and applies the gear (level 1..5 -> gear codes 0x33, 0x34,
0x01, 0x18, 0x20).

`page_nav(dir)` and the next/previous screen pickers (`0x4201a9bc` "get_next_screen",
`0x4201ac14` "get_last_screen", `0x4201af48`) belong to the UI spec. What the input side needs:

- `0x4201af48(new_screen, dir)` sets the brushing mode from the screen: 81->0, 79->1, 77->2,
  76->3, 78->4, 80->5 (bytes 0x3fca4c8b/0x3fca4c8c) and restarts the idle timeout (30 s; 10 s in
  post-brush states). **CONFIRMED**.
- Up/down on screen 82, 101 or 102 while paused ends the session (`0x42019528`). **CONFIRMED**.
- Up/down on screen 92 cycles the info page index 0..3 (0x3fca4dcb). **CONFIRMED**.
- "Next" order on the mode screens: 80 -> 79 -> 77 -> 76 -> 78 -> (81 if the custom mode exists,
  else 80), 81 -> 80 (0x4201a9bc). **CONFIRMED** for the listed transitions; conditions around
  them are simplified here.

---

## 3. Physical button (GPIO3)

### 3.1 Pin and ISR

- **CONFIRMED** `gpio_config{pin 3, INPUT, pull_up 1, pull_down 0, intr ANYEDGE}`;
  `gpio_isr_handler_add(3, 0x40377d88, (void*)3)` (0x4200d77c). `gpio_install_isr_service(0)`.
- The same ISR function `0x40377d88(arg = pin)` serves pins 2, 3, 8, 9 and 12. It calls
  `0x40377c98(pin)` directly, in interrupt context. For pin 3 it also prints
  "KEY = 3 level init" and, while `init_ok (0x3fc9f309) < 5`, increments `init_ok` on every edge.
- **CONFIRMED** the `key_int` task (0x4201b560, priority 30, stack 0x800) blocks on queue
  0x3fca4df4 (length 50, item 1 byte, created at 0x4201c56c). The literal 0x3fca4df4 occurs once
  in the image and no send call uses it. The task never runs. Do not reproduce it.
- `init_ok` is set to 5 by the main task at the end of its init. Before that all input is
  ignored, except that GPIO3 or GPIO9 being low sets `key_press_last` (0x3fc9f301) for the boot
  code.

### 3.2 Press / release (ISR, 0x40377c98 pin-3 branch)

State: `released` (0x3fc9f302, `.data` initial 1), `pressed` (0x3fca4d9b), `press_ms`
(0x3fca4db8), `wdt_flag` (0x3fca4d9c), `event_code` (0x3fca4d9d, initial 5 = none).

```c
level = gpio_get_level(3);
if (level == 0) {                          // falling edge
    if (released != 1) return;             // already down
    /* 0x40377dc0 */
    if (wdt_flag == 1) { puts("sys_abnormal == 1"); esp_restart(); }
    press_ms = xTaskGetTickCount();        // 1 kHz tick -> ms
    stop t2s, t8s, t3s, t5s;
    start_once(t2s, 2000000); start_once(t8s, 8000000);
    start_once(t3s, 3000000); start_once(t5s, 5000000);
    wdt_flag = 1; start_once(t_wdt, 100 us);   // callback 0x42019c74 clears wdt_flag
    pressed = 1;
    released = 0;
} else {                                   // rising edge
    /* 0x40377e74 */
    held = now_ms - press_ms;
    if (held >= 80 && held <= 1499 && pressed == 1) { event_code = 0; post_button_event(); }
    pressed = 0;
    stop t2s, t8s, t3s, t5s;
    released = 1;
}
```

- **Debounce**: none beyond the `released` flag and the 80 ms minimum hold. A press shorter
  than 80 ms or between 1500 ms and 1999 ms produces nothing.
- `wdt_flag`: if a second press arrives before the esp_timer task ran the 100 us timer, the
  device restarts. It is a stuck-timer-task watchdog.

Timers (created in `brush_button_init` 0x42019cf8; all one-shot, restarted on every press):

| Handle | Name string | Period | Callback | event_code |
|---|---|---|---|---|
| 0x3fca4db4 | btnperiodic1 / btn_tim | 2 s | 0x42019cb4 | 1 |
| 0x3fca4dac | btnperiodic3 / btn_3s_tim | 3 s | 0x42019c9c | 2 |
| 0x3fca4da8 | btnperiodic5 / btn_5s_tim | 5 s | 0x42019c84 | 3 |
| 0x3fca4db0 | btnperiodic2 / btn_reset_tim | 8 s | 0x42019ccc | 4, then `0x4201fd54()` |
| 0x3fca4da4 | wdtcheck | 100 us | 0x42019c74 | - |

Each callback first checks `gpio_get_level(3) == 0` (0x4200d730) and does nothing if the button
was released. They are not mutually exclusive: holding for 8 s delivers codes 1, 2, 3 and 4 in
turn.

`post_button_event()` = `0x4201b680`: if byte 0x3fca4198 == 1 set it to 2 (cancels the pending
low-battery auto-sleep step, see 0x420142d0); if `init_ok >= 5` set main event bit 0x80.
The main task then calls `handler(event_code)` = `0x4201cab8` through the pointer at 0x3fca4da0
(0x42019e0c).

A BLE command (opcode byte 0xA4, 0x420105b4) can inject a short press with `0x42019ce8`:
parameter 1 starts a session (sets "remote start" 0x3fca308e), parameter 0 stops one (sets
0x3fca50e1).

### 3.3 State variables used by the handler

| Name here | Address | Values |
|---|---|---|
| ota | 0x3fca4194 (0x42013a50) | non-zero while an OTA is running |
| reset_flag | 0x3fca4ea0 | 1 after a factory reset reboot ("button_long_reset_flag") |
| asleep | 0x3fca41a4 (0x420143b0) | 1 = screen off / light-sleep state |
| chg | `*(int*)0x3fca4b74` | 2 = on battery, 1 = charging, 3 = other charger state (**INFERRED** full) |
| batt | byte 0x3fca4b7a | battery percent |
| prod | 0x3fca4e9b | 1 = normal, 2 = production-test mode (NVS blob, 0x42027c98) |
| aging | 0x3fca4e91 | 1 = factory aging ("cooker"/"laohua") mode |
| shop | 0x3fca4dca | 1 = shop display mode (NVS "IntoShow") |
| brushing | 0x3fca4d5b | session active |
| running | 0x3fca4e03 | motor running (0 = paused) |
| screen | 0x3fc9ab9a (0x4201a13c) | current screen id |
| lock, lock_popup | 0x3fca5d9c, 0x3fca5d94 | touch lock on; lock popup task alive |
| subpage | 0x3fca4fda | 1 = UI is on a left/right sub-page |
| touch_state | 0x3fca4ded | section 1.4 |

### 3.4 Dispatcher `button_event(code)` (0x4201cab8) - CONFIRMED from disassembly

```c
if (code == 4) puts("BTN_REBOOT_PRESS");
if (ota)        { puts("get_ota_start_status = true");   return; }   // OTA: key locked
if (reset_flag) { puts("button_long_reset_flag = true"); return; }
if (0x4201829c() && chg != 2) led(4, 0, 4);          // 0x4201dcc8
cfg[0x69] = 0;
if (asleep == 1 && chg != 2) {                       // asleep on the charger
    if (code != 0) return;
    if (batt > 99) 0x4200d6b0(0);                    // wireless-charge enable handling
    0x3fc9ab9f = 0;
    wake_for_charge();                               // 0x4201bee8
    return;
}
if (chg != 2) return;                                // awake on the charger: button ignored
if (byte 0x3fca4dff != 0) return;                    // never written: always 0
switch (code) {
case 0:                                              // short press
    press_count (0x3fca4e92)++;
    if (prod == 1 && !shop && lock == 1 && lock_popup == 1) lock_button(0);   // 0x4202865c
    else if (subpage == 1) page_nav(1);              // 0x42020ab8: back to the main page
    else short_press();                              // 0x4201c790
    break;
case 1:                                              // 2 s
    if (prod == 1 && !shop) { lock_button(1); 0x3fc9ab9c = mode (0x3fca4c8b); }
    break;
case 2:                                              // 3 s
    if (prod == 2) {
        if (aging == 0) { if (brushing) motor_stop(); touch_irq(0); enter_aging(); leds_all(3); }
        else if (aging == 1) { touch_irq(1); leave_aging(); leds_all(1); }
    } else if (screen == 97) { rtc_byte 0x50001001 = 0; show_main(); }   // 0x42011968, 0x4201a5ac
    break;
case 3:                                              // 5 s
    if (screen >= 76 && screen <= 81) {              // 0x4201a380
        0x3fca4dcb = 0; show_info_screen();          // 0x4201a94c: screen 92, UI cmd 0x5C {0}
        idle_timeout(30);
    }
    break;
case 4: break;                                       // the work is done by 0x4201fd54, below
}
```

`enter_aging` = 0x42019bc0 (aging = 1, 0x3fca4d8d = 0xD7, screen 70); `leave_aging` = 0x42019b7c;
`leds_all(m)` = 0x4201dd48 (LED channels 0..3).

### 3.5 Short press `short_press()` (0x4201c790) - CONFIRMED from disassembly

Reached only when awake-or-asleep on battery, not OTA, not reset_flag.

```c
printf("handle_button_short_press %d %d,get_state() %d", asleep, chg, touch_state);
0x3fca4f50 = 1;  0x3fca4b18 = 0;
if (aging == 1) return;
if (batt == 0 || ota) {
    printf("low power.ota lock key %d %d", batt, ota);
    if (batt != 0) return;
    idle_timeout(1);  show_low_battery();            // screen 94 (0x4201a260); sleeps 1 s later
    return;
}
if (shop && prod != 2) { leave_shop_mode(); return; }          // 0x4201a9a0, "close guitai"
if (asleep == 1) {                                   // WAKE ONLY, no brushing
    clear_gyro_wake_count();                         // 0x4201b740, RTC byte 0x50001022
    wake();                                          // 0x4201bd70
    0x42019008();                                    // clamp the stored mode
    touch_irq(1);
    return;
}
if (dismiss_screen() && !remote_start) return;       // 0x4201b0d8, 0x42011340
if (!brushing) {
    if (touch_state != 5) return;                    // touch IC must be running
    start_session();                                 // see below
    return;
}
/* session active */
if (0x3fc9aba3 != 0xFF) 0x420185b4();
if (0x3fca4f51 == 2 || 0x3fca50e1 != 0) { motor_stop(); 0x3fca50e1 = 0; return; }
if (elapsed (u16 0x3fca4d00) >= total (u16 0x3fca4cfe)) { motor_stop(); return; }
if (running) {                                       // PAUSE "suspend_brush_by_profile"
    0x3fc9ab97 = 0; 0x420198dc(); running = 0;
    0x420197c0();                                    // 0x3fca4c7d = 1, 0x3fc9aba5 = 0, motor off
    touch_set_state(0x21);
    show_paused();                                   // 0x4201c73c
    0x42015af0(2);
    vTaskDelay(100);
    touch_irq(1);
} else {                                             // RESUME "resume_brush_by_profile"
    0x420198dc(); 0x3fc9ab97 = 0; 0x420162f4();
    touch_irq(0);
    vTaskDelay(50);
    0x3fca4c76 = 1; running = 1;
    0x4201977c();                                    // 0x3fc9aba5 = 200, motor on at the gear
    0x42015af0(1);
    sleep_monitor_off();                             // 0x42014268
    zone_mode (0x3fca4e7a) ? 0x4201a894() : 0x4201bb18();      // screen 99 or brushing screen 82
}
```

`motor_stop()` = `0x42019528` ("handle_motor_stop"): brushing = 0, 0x3fc9aba5 = 200, main event
bit 0x80000 (end-of-session processing `0x4201c2b8`), motor off, `touch_irq(1)`.

`dismiss_screen()` (0x4201b0d8): when the current screen is one of 71, 72, 73, 83, 92, 93, 94,
97, 100 (bit mask 0x12700803 over `screen - 72`, plus 71), or 84 while byte 0x3fca4e8f is 3 or 4:
screen 97 -> `sleep_monitor_off(); 0x42011a00()`; otherwise show the main screen
(`0x4201a3fc`) and set the idle timeout to 10 s (from 83, 84, 100), 5 s (from 92, 94) or 60 s
(others); returns 1. Otherwise returns 0.

`start_session()` (0x4201c858..0x4201c980), in order: save mode to 0x3fc9ab9c; `led(4,0,4)`;
clear gyro wake count; `0x420198dc()`; 0x3fca4e8f = 2; cfg[0x37] = (prod != 2); clear
0x3fca4d82, 0x3fc9aee7, 0x3fca4dc8; 0x3fca4c76 = 1; brushing = 1; 0x3fca4197 = 1; 0x3fca4c7d = 0;
u16 0x3fca4e00 = 0; running = 1; 0x3fc9aba6 = 1; 0x3fca4c7c = 0; `led(2,1,4)`; `led(3,1,4)`;
"WAKE UP AND BRUAH"; timestamp into 0x3fca4cb2; u16 0x3fca4c82 = 0; `0x42018514()`;
`0x4201ba38()`; `0x42015af0(1)`; call the function pointer at 0x3fc9a1ec (0x4200ca10); then

- zone mode set: `0x4201a894()` (screen 99);
- else mode != 5: `touch_irq(0)`, show brushing screen `0x4201bb18()`;
- else mode == 5 (touch stays on): prod == 2: show strength screen 87 with value 2
  (`0x4201a270(2)`), gear for level 3 (`0x42019368(3)`); otherwise level = RTC byte 0x50001019
  (reset to 3 if not 1..5), `0x4201a270(level-1)`, `0x42019368(level)`;

then `vTaskDelay(30)`; take the CPU PM lock (0x42014184); `sleep_monitor_off()`; u16 0x3fca4e2a
= 0; main event bit 0x40000 (`0x4201b608(1)`); `0x42014580()`; start the 30 ms fast timer
(0x4201c1d0); cfg[0x35] = 1; 0x3fc9aba5 = 200.

### 3.6 Behaviour table (derived from 3.4 and 3.5)

| Device state | Short press (80..1499 ms) | 2 s | 3 s | 5 s | 8 s |
|---|---|---|---|---|---|
| OTA running | ignored | ignored | ignored | ignored | factory reset still fires (UI path, not gated) |
| After factory reset (reset_flag) | ignored | ignored | ignored | ignored | as above |
| On charger, screen off | wake the screen (`0x4201bee8`) | ignored | ignored | ignored | factory reset |
| On charger, screen on | ignored | ignored | ignored | ignored | factory reset |
| Battery 0 % (on battery) | low-battery screen 94, sleep after 1 s | as idle | as idle | as idle | factory reset |
| Asleep (on battery) | wake only; a second press starts brushing | not gated by sleep: acts on the stale screen id (lock popup if it is 76..81) | as idle | as idle (info screen if stale id is 76..81) | factory reset |
| Idle on a mode screen 76..81 | start session (needs touch_state 5) | lock on (if unlocked) or lock off; popup 91 for 1 s when locking; 400 ms motor buzz | nothing | info screen 92 | factory reset |
| Screen 97 | `0x42011a00()`, no start | nothing | RTC byte 0x50001001 = 0, back to main | nothing | factory reset |
| Other dismissable screen (71-73, 83, 92-94, 100; 84 in post-brush states) | back to main screen, no start | unlock only on 83 | nothing | nothing | factory reset |
| Brushing, motor running | pause | unlock only (82, 87); locking needs 76..81 | nothing | nothing | factory reset |
| Brushing, paused | resume (stop if time is up or a remote stop is pending) | unlock only | nothing | nothing | factory reset |
| Lock popup (screen 91) showing | dismiss popup, restore screen | nothing (91 is not in the unlock list) | nothing | nothing | factory reset |
| Shop display mode | leave shop mode | nothing | nothing | as idle | factory reset |
| Production-test mode (prod 2), not aging | as normal (shop check skipped) | nothing | stop motor, enter aging mode | as idle | factory reset |
| Aging mode | ignored | nothing | leave aging mode | as idle | factory reset |

A hold passes through every shorter threshold first: a 5 s hold on a mode screen toggles the
lock at 2 s and then opens the info screen at 5 s.

There is **no "hold to power off"**, no reboot-only hold and no pairing hold in the stock button
code. Power-off is the idle timeout.

### 3.7 Eight-second hold: factory reset (CONFIRMED)

`0x42019ccc` -> `0x4201fd54` sets UI event bit 0x200. The UI task (end of 0x42022564) then runs,
unconditionally:

1. `0x4201a9a0()`: leave shop mode, show main screen.
2. `0x42024fc4(&shop, 1, "IntoShow")`: write the NVS key.
3. `0x4201c6b8()`: `touch_irq(0)`; 20 ms; `sleep_monitor_off()`; cfg[0x0d] = 0; cfg[0x0e] =
   0x16; cfg[0x34] = 0x0b; `0x42017990()`; `0x42018d74()` (save config); 100 ms; `0x42018cf8()`;
   `esp_restart()`.

On the next boot the main task sees cfg[0x34] == 0x0b: idle timeout 8 s, `0x4204e5a0()`,
`0x4201c0cc()`, reset_flag = 1, then cfg[0x34] = 0 is saved. With reset_flag set the button is
ignored and `0x4201c5d0` sends the device to deep sleep when the idle timer expires
(**INFERRED**: this is the shipping state; the next button/charger wake boots normally with
cfg[0x34] = 0 -> first-boot flow).

### 3.8 Touch lock (0x42028544..0x4202865c) - CONFIRMED

State: `lock` 0x3fca5d9c, `lock_popup` 0x3fca5d94, `saved_screen` 0x3fca5d98, task handle
0x3fca5d90.

- `lock_button(1)` (2 s hold, `0x4202865c(1)`):
  - not locked: only if the current screen is 76..81: `saved_screen = screen`; if no popup,
    create task "show_lock_ui_task" (0x42028544, stack 0x1000, priority 3). Then feedback
    (`0x420285bc`: unless screen is 87, motor gear 0x35 for 400 ms, then motor off) and
    `idle_timeout(30)`.
  - locked: allowed on screens {76..81, 87, 82, 83} (table 0x3c11cf44, 9 entries); on 82 not
    when byte 0x3fca50d6 == 1. `lock = 0; saved_screen = 0;` if the popup is up, restore the
    screen (`0x420284c4`). Then the same feedback and `idle_timeout(30)`.
- `show_lock_ui_task`: `lock = 1; lock_popup = 1;` show screen 91 (`0x4201a244`, UI cmd 0x5B);
  poll every 10 ms for 1 s; then `0x420284c4()` re-shows `saved_screen` (76..81, 87 with the
  stored strength, or 82 via `0x4201c73c`) and clears `lock_popup`; delete the task.
- `lock_button(0)` (short press while the popup is up): restore the screen at once.
- A swipe while locked on a lockable screen calls `0x42028640`: `saved_screen = screen`, start the
  popup task (which also re-asserts `lock = 1`).
- The lock does **not** block the button: a short press still starts/pauses brushing. It only
  blocks swipes. The lock flag is RAM only (not persisted) (**CONFIRMED** no NVS access in these
  functions).

"low power.ota lock key" is unrelated to this lock: it is the log line for a short press that is
refused because the battery is at 0 % or an OTA is running.

---

## 4. Event plumbing

Main task event group `0x3fca4eac` (wait mask 0x7fffea, clear on exit). Input-related bits:

| Bit | Set by | Handled by |
|---|---|---|
| 0x02 | GPIO8 ISR (motion), `0x40377f24` | raise-to-wake path `0x4201cf9d` if cfg[8] == 1 and chg == 2 |
| 0x20 | GPIO9 ISR (charger inserted), `0x4201b6b4` | `0x4201d888` -> callback at 0x3fca4eb0 = `0x42017a0c(0)` |
| 0x40 | 10 ms periodic timer, `0x4201b5a0` | housekeeping tick; every 100th runs the 1 s block |
| 0x80 | button events, `0x4201b680` | `0x42019e0c` -> `0x4201cab8(code)` |
| 0x100 | GPIO12 ISR (touch RDY), `0x40377f30` | `0x4201b458` |

Setter: `0x40377f10(bits)` = `xEventGroupSetBits(*(0x3fca4eac), bits)`, called from ISR context
for the GPIO sources.

UI task event group `0x3fca50e4` (wait mask 0x3ff): 0x01 message queued, 0x02 50 ms blink
timer, 0x10 gesture timer (70 ms after last touch sample), 0x20 LCD re-init, 0x200 factory
reset.

---

## 5. What the custom firmware needs, in short

1. Bit-bang I2C on SCL13/SDA14 with clock-stretch polling, shared by IQS7222D (0x44) and
   AW8686X (0x6A). One bus owner task or a mutex (stock uses only the main task).
2. GPIO12: drive low at boot; later input + pull-up + falling-edge interrupt.
3. On each RDY falling edge run the state machine of 1.4. Cold start: write `FF 00`, wait for
   RDY, (optional id read), wait for RDY, send the 67 writes of 1.5 while RDY is low, wait for
   RDY, `D0 = 05`, then on every RDY read 12 bytes from 0x10 and feed X/Y to the decoder,
   bracketed by `DB = 0D` and `FF = 00`.
4. Reseed (`D0 = 08`) every 3 s; re-init when `status & 0x0A`.
5. Decoder of section 2 with a 70 ms one-shot timer.
6. Button: ISR + four one-shot timers as in section 3.

---

## 6. Other input lines

All share ISR `0x40377d88` and are gated by `init_ok >= 5`. Configured in `0x4200d77c`.

| Pin | Config (CONFIRMED) | Role | Evidence |
|---|---|---|---|
| GPIO8 | input, no pulls, ANYEDGE | **IMU any-motion interrupt, active high** | EXT0 wake `esp_sleep_enable_ext0_wakeup(8, 1)` (0x420141cc); in `app_main` the EXT0 branch prints "222Wake up from GPIO8" and calls 0x4200ca64, which logs "count_wakeup_gyro"; the ISR branch starts the "anymotion_timeout" timer |
| GPIO9 | input, no pulls, ANYEDGE | **charger present, active LOW** | `0x4201840c`: chg == 2 and GPIO9 low -> `0x42017a0c(0)` which prints "USB_IN_ACTION" and sets chg = 1; chg == 1 and GPIO9 high for more than 150 ticks -> `0x42017a0c(1)` (removed). Boot code skips the battery-voltage check when GPIO9 is low |
| GPIO2 | input, no pulls, NEGEDGE | unknown source; only resets a counter | ISR: `u16 0x3fc9f304 = 0` |
| GPIO12 | section 1.3 | touch RDY | |
| GPIO3 | section 3 | button | |

This contradicts CONTEXT.md, which lists GPIO8 as charger detect and GPIO9 as the IMU interrupt
(both marked "likely" there). The code evidence above is direct; treat the CONTEXT assignment as
wrong unless a measurement on the device says otherwise.

### GPIO8 (motion) - ISR branch 0x40377d0c

```c
allow_anymotion (0x3fca2885) = 1;
esp_timer_stop(t_anymotion 0x3fca2ae0); esp_timer_start_once(t_anymotion, 5000000);
                                           // callback 0x4200b734 clears allow_anymotion
if (wake_gate (0x3fc9f308) >= 4 && waking (0x3fc9f307) == 0) set main event bit 0x02;
```

The interrupt handler is removed while awake (`0x4200d75c(0)` in the wake functions and the
main-task init) and added when going to sleep (`0x4200d75c(1)`). `wake_gate` is set to 5 by the
sleep function 0x4201b764. Main task on bit 0x02 (only if cfg[8] == 1 and on battery):
`waking = allow_anymotion`; if the gyro wake count (RTC byte 0x50001022) is not above 4:
increment it (cap 20, `0x4201b714`) and wake the screen (`0x4201bd70`). A button press clears
that count (`0x4201b740`). **INFERRED**: cfg[8] is the "raise to wake" setting.

### GPIO9 (charger) - ISR branch 0x40377d35

```c
if (gpio_get_level(9) == 0) key_press_last (0x3fc9f301) = 1;
if (init_ok >= 5 && gpio_get_level(9) == 0 && byte 0x3fca4b80 != 0) {
    0x3fca4eb4 = 0; set main event bit 0x20;      // -> 0x42017a0c(0): charger inserted
}
```

The removal side is polled every 10 ms in `0x4201840c` (150 consecutive high samples, counter
0x3fc9f304). Insertion is also polled there.

### GPIO2 - ISR branch 0x40377d6d

`if (init_ok >= 5) u16 0x3fc9f304 = 0;` Nothing else. 0x3fc9f304 is the charger-removal
debounce counter, so a falling edge on GPIO2 restarts the 1.5 s removal debounce.
**INFERRED**: a pulsing charger/coil status signal. Its source is not identifiable from code.

### Deep-sleep wake sources (for completeness)

EXT1 mask 0x208 (GPIO3 | GPIO9), any-low (`0x42014220`, `0x420141cc`); EXT0 GPIO8 high only in
the "deep sleep2" path (`0x420141cc`). In `app_main` (0x4200c034): EXT1 from GPIO3 clears the
gyro wake count and logs "count_wakeup_button" (0x4200cacc); EXT0 sets byte 0x3fca2884, which
makes the boot code apply the motion-wake limit.

---

## 7. Open questions

1. **IQS7222D register semantics** (bit names of 0xD0, 0xDA, 0xDB, the meaning of 0xDC/0xDD, the
   status bits 0x0A) come from memory of the public IQS7222 map, not from the image. The byte
   sequences are exact; the names need a datasheet check.
2. **Why GPIO12 is driven low at boot** and whether the chip needs a minimum low time. Stock
   holds it low from `brush_gpio_cfg` until the main task has finished its init (more than
   500 ms).
3. **Bit-bang timing**: the stock code has no delays; the real SCL frequency is unknown. If the
   custom firmware's GPIO calls are faster, add a small delay.
4. **Report rate**, and therefore the real duration of "6 samples" and "80 samples". The 12 ms
   figure is inferred from the 0xD0 block.
5. **Physical orientation**: which direction on the handle is "swipe up" (dx > 0 after the
   `255 - X` inversion).
6. **How RDY behaves around the state-5 transfers**: the 12-byte read ends with STOP before the
   `DB = 0D` / `FF = 00` writes, so those two writes may fall outside the comms window. Whether
   they are NACKed or act as a force-comms request cannot be told from the code.
7. **GPIO2 signal source.**
8. **GPIO8/GPIO9 roles** should be confirmed once on hardware, because the code reading here
   contradicts the earlier assumption.
9. **cfg[0x69]** (gesture inhibit), **cfg[8]** (raise to wake), **cfg[0x0d]/cfg[0x0e]** written by
   the factory reset: meanings inferred or unknown.
10. **chg == 3**: assumed "charge complete"; not verified here.
11. The `page_nav` screen graph (0x420208f8, 0x4201a9bc, 0x4201ac14, 0x4201af48) is only
    summarised; it belongs to the UI spec.

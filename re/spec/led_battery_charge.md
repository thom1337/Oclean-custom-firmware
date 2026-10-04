# Stock firmware spec: indicator LEDs, backlight PWM, battery gauge, charging

Scope: `ocleanhal/platform_devices.c` LED/ADC/charge-pin HAL, the LED pattern module
(0x4201d9a4..0x4201e03c), the battery/charge module (0x42017744..0x42018530) and the
sleep-time pin parking that touches these pins.

Every item is tagged **CONFIRMED** (read from code/data at the address given, usually
cross-checked in the Xtensa disassembly with `fd.py`) or **INFERRED** (reasoning given).
Stock tick is 1 ms, so `vTaskDelay(n)` = n ms.

---------------------------------------------------------------------------------------

## 0. Findings that contradict earlier notes (read first)

| # | Earlier assumption | What the stock code actually does | Status |
|---|---|---|---|
| 1 | GPIO8 = charger detect (active-high), GPIO9 = IMU motion INT | **GPIO9 is the charger-present input, active-LOW** (0 = on charger). Every charge decision reads `gpio_get_level(9)`: plug/unplug poll 0x4201840c, boot low-battery gate in 0x4200d77c, wake-screen chooser 0x42029218, motor/music task 0x4201f24c (will not start playing while GPIO9 is low). Deep-sleep EXT1 mask is 0x208 = GPIO3\|GPIO9 (0x42014220). **GPIO8 is the IMU any-motion line**: its ISR branch (0x40377d0c) restarts the 5 s "anymotion" timer and posts wake bit 2; it is EXT0 wake level 1 (0x420141cc); its ISR is added only for sleep (0x4200d75c). | CONFIRMED (code). Electrical meaning of GPIO9 = INFERRED from the `"USB_IN_ACTION"` log printed when it goes low. |
| 2 | GPIO26 "CHARGE_EN" is push-pull, active-high = charging on | `set_CHARGE_EN_IO_level(level)` 0x4200d650: **level 1 (charging allowed) = pin switched to INPUT (high-Z)**; **level 0 (charging blocked) = pin driven OUTPUT HIGH**. The pin is never driven low. Over-temperature (>72 °C) calls level 0, i.e. drives the pin HIGH. So **GPIO26 high = charging disabled**. | CONFIRMED (code). The current custom `hw_charge.c` drives 26 high for "on" — that is the stock "off" state. |
| 3 | GPIO45 "WLC_EN" is driven high to enable wireless charging | GPIO45 is written **0 in every code path** (boot, 0x4200d6b0 both branches, sleep prep, main loop). It is never set to 1. | CONFIRMED |
| 4 | LEDC ch0..3 are four equal indicator LEDs | They differ: ch0/ch1 use `output_invert=1`, ch2 is non-inverted (parked LOW = off → active-high LED), ch3 is software-inverted (`8191-level`). ch0..2 are clamped to level 4000 (49 %), ch3 to 8191. | CONFIRMED (config), active-high/low of the LEDs = INFERRED from parked levels |
| 5 | Battery table "not decoded" | Decoded, see §5.2. Percent is not a plain function of voltage: it is rate-limited, monotonic while discharging and slew-limited by a charge-time table while charging. | CONFIRMED |
| 6 | Charge state values 1/2 | Three states at `0x3fca4b6c+8`: **1 = charging, 2 = not on charger, 3 = full (still on charger)**. | CONFIRMED |

---------------------------------------------------------------------------------------

## 1. LED hardware abstraction (platform_devices.c)

### 1.1 LEDC timer — `led_init` 0x4200d2a8 — CONFIRMED

`ledc_timer_config_t` copied from rodata 0x3c116d50 (20 bytes, verified raw):

```c
// 0x3c116d50: 00000000 0000000d 00000000 00001388 00000000
ledc_timer_config_t ledc_timer = {
    .speed_mode      = LEDC_LOW_SPEED_MODE, // 0
    .duty_resolution = LEDC_TIMER_13_BIT,   // 13
    .timer_num       = LEDC_TIMER_0,        // 0
    .freq_hz         = 5000,
    .clk_cfg         = LEDC_AUTO_CLK,       // 0
};
```
Then `led_channel_init(1..5)` (0x4200d160) in that order.

### 1.2 Channels — `led_channel_init` 0x4200d160 — CONFIRMED

`ledc_channel_config_t` layout (IDF 5.1.1): gpio_num, speed_mode, channel, intr_type,
timer_sel, duty, hpoint, flags.output_invert (8 × u32).

| "LED n" (HAL id) | source of config | GPIO | LEDC ch | initial duty | output_invert | error string |
|---|---|---|---|---|---|---|
| 1 | rodata 0x3c116d10 = `{17,0,0,0,0,0,0,1}` | 17 | 0 | 0 | **1** | ledc1_channel |
| 2 | rodata 0x3c116d30 = `{18,0,1,0,0,0,0,1}` | 18 | 1 | 0 | **1** | ledc2_channel |
| 3 | stack (memset 0): gpio=19, channel=2 | 19 | 2 | 0 | 0 | ledc3_channel |
| 4 | stack: gpio=20, channel=3, **duty=0x1fff** ("ledc4_channelr", used when `g_3fc9abba==1`) | 20 | 3 | 8191 | 0 | ledc4_channelr |
| 4 (alt) | stack: gpio=20, channel=3, duty=0 ("ledc4_channel", `g_3fc9abba!=1`) | 20 | 3 | 0 | 0 | ledc4_channel |
| 5 | stack: gpio=21, channel=4, **duty=0x1fff** | 21 | 4 | 8191 | 0 | ledc5_channel |

`g_3fc9abba` (u8, 0x3fc9abba, .data initial 1) is forced to 1 at boot (0x4200d77c) and on
every LED tick (0x4201ddf8); the non-"r" variant is dead code. CONFIRMED.

So after `led_init` every LED is off: LED1/2 duty 0 (inverted output → pin constantly
high), LED3 duty 0 (pin low), LED4/5 duty 8191 (pin high 8191/8192).

Electrical polarity:
* LED5 = LCD backlight on GPIO21, active-low — CONFIRMED on the device (CONTEXT.md) and
  consistent with the code.
* LED1 (GPIO17), LED2 (GPIO18), LED4 (GPIO20): lit while the pin is LOW — INFERRED
  (inverted outputs / `8191-level`, and the "off" level parked before sleep is 1).
* LED3 (GPIO19): lit while the pin is HIGH — INFERRED (non-inverted, parked level 0).

Boot GPIO preset before `led_init` (brush_gpio_cfg 0x4200d77c, CONFIRMED):
`gpio_hold_dis` on 13,14,21,42,41,37,17,18,19,20,12,3; then
GPIO17: level 1, OUTPUT, pull-up on; GPIO18: level 1, OUTPUT, pull-down on;
GPIO19: level 0, OUTPUT; GPIO20: level 1, OUTPUT; then `led_init()`.

### 1.3 `led_configs[]` — CONFIRMED (raw bytes)

Two identical copies, one per source file:

```c
// 0x3c117394 (used by set_led_light_level_process, platform_devices.c)
// 0x3c11a864 (used by the fade engine in the LED module)
// index = HAL LED id; element = { uint32_t max_duty; }
static const uint32_t led_max_duty[6] = { 0, 4000, 4000, 4000, 8191, 6000 };
```

### 1.4 The three HAL calls — CONFIRMED

"level" is always a brightness (0 = dark). HAL ids are 1..5.

```c
// set_led_light_level  0x4200d2f0  (static set; used by the state setter)
void set_led_light_level(uint8_t led, uint16_t level)
{
    switch (led) {
    case 1: ledc_set_duty(0, 0, MIN(level, 4000)); ledc_update_duty(0, 0); break;
    case 2: ledc_set_duty(0, 1, MIN(level, 4000)); ledc_update_duty(0, 1); break;
    case 3: ledc_set_duty(0, 2, MIN(level, 4000)); ledc_update_duty(0, 2); break;
    case 4: ledc_set_duty(0, 3, 8191 - level);     ledc_update_duty(0, 3); break;
    case 5:                                   // backlight
        if (level > 6000) level = 6000;       // lit_42000e2c = 0x1770
        ledc_set_duty(0, 4, level == 0 ? 8191 : 6000 - level);
        ledc_update_duty(0, 4);
        break;
    }
}   // also stores the GPIO number (17..21) in g_3fc9a218 — never read elsewhere

// set_led_light_level_process  0x4200d49c  (used by every animation)
void set_led_light_level_process(uint8_t led, uint16_t level)
{
    if (level > led_max_duty[led]) return;    // request silently DROPPED, not clamped
    switch (led) {
    case 1: case 2: case 3: ledc_set_duty(0, led - 1, level);     break;
    case 4:                 ledc_set_duty(0, 3, 8191 - level);    break;
    case 5:                 ledc_set_duty(0, 4, 8191 - level);    break;   // NOT 6000-level
    }
    ledc_update_duty(0, led - 1);
}

// get level  0x4200d604
uint32_t get_led_light_level(uint8_t led)
{
    if (led >= 1 && led <= 3) return ledc_get_duty(0, led - 1);
    if (led == 4 || led == 5) return 8191 - ledc_get_duty(0, led - 1);
    return 0;
}
```

Consequences worth copying exactly:
* Backlight "static on" (`set_led_light_level(5, ≥6000)`) = LEDC duty **0** (100 % lit).
* Backlight at the end of an animated fade-in (`process` level 6000) = LEDC duty **2191**
  (lit 73 %). The two paths give different brightness. CONFIRMED.
* Backlight off = duty 8191 in both paths.
* On LED1..3 any `process` level above 4000 is ignored (see blink quirk, §2.6).

---------------------------------------------------------------------------------------

## 2. LED pattern module (0x4201d9a4 .. 0x4201e0c3)

### 2.1 Logical LEDs and state bytes — CONFIRMED

The application addresses LEDs by index 0..4 (plus an unused pseudo index 5).

| app idx | HAL id | GPIO / ch | state byte | max level | role in stock UI (trigger evidence in §3) |
|---|---|---|---|---|---|
| 0 | 1 | 17 / ch0 | 0x3fc9abb9 | 4000 | Wi-Fi status light (driven by its own ramp, §2.7) + animations |
| 1 | 2 | 18 / ch1 | 0x3fc9abb8 | 4000 | "awake / ready" light: steady on while awake off-charger with battery > 0 |
| 2 | 3 | 19 / ch2 | 0x3fc9abb7 | 4000 | charging light (breathing = charging, steady = full); "pressure OK" light while brushing |
| 3 | 4 | 20 / ch3 | 0x3fc9abb6 | 8191 | over-pressure warning (steady / blink) |
| 4 | 5 | 21 / ch4 | 0x3fc9abb5 | 6000 | LCD backlight |
| 5 | – | – | 0x3fc9abb4 | – | "alternate LED3/LED4" pseudo LED; no caller uses it |

All six state bytes are 0xFF at boot (.data), so the first request always takes effect.

State values (argument 2 of `led_set`):

| state | meaning | hardware write done when the state is entered (0x4201d9a4) |
|---|---|---|
| 0 | **ON** (steady) | `set_led_light_level(hal, 8191)` (idx0 passes 4000) → LED1..3 = 4000, LED4 = 8191, backlight = duty 0 |
| 1 | **OFF** | `set_led_light_level(hal, 0)` |
| 2 | **BLINK** | `set_led_light_level(hal, 0)`, then animated by the 10 ms tick |
| 3 | **BREATHE** | `set_led_light_level(hal, 0)`, then animated by the 10 ms tick |
| other | stored, no write | |

The setter returns without doing anything if the new state equals the stored one — except
idx 2, where state 0 is always re-applied (`if (old==new && old!=0) return;`). CONFIRMED.

### 2.2 `led_set(led, state, anim)` = 0x4201dcc8 — CONFIRMED

```c
// g_anim   @0x3fc9aba8 (u32, .data = 4)     4 = idle, 0..3 = script running
// g_script @0x3fca4ef8 (ptr)   g_time @0x3fca4efc (u16)
// g_pend_led @0x3fca4f0c, g_pend_state @0x3fca4f08
void led_set(uint32_t led, uint32_t state, int anim)
{
    if (g_anim != 4) return;                 // any request during a script is LOST
    if (anim == 4) { led_apply_state(led, state); return; }   // 0x4201d9a4, immediate
    g_anim = anim; g_pend_led = led; g_pend_state = state;
    switch (anim) {
    case 0: g_script = SCRIPT0; break;                              // 0x3fc9ae44
    case 1: g_script = (wifi_status == 1) ? SCRIPT1B : SCRIPT1; break; // 0x3fc9ad00 / 0x3fc9ada2
    case 2: g_script = SCRIPT2; break;                              // 0x3fc9ac5e
    case 3: g_script = SCRIPT3; break;                              // 0x3fc9abbc
    }
    g_time = 0;
    memset(slots, 0, 50);                    // 5 fade slots @0x3fca4ec4
}
void led_abort_script(void) { g_anim = 4; }   // 0x4201dcbc (pending state is NOT applied)
void led_all(uint32_t state)                  // 0x4201dd48
{ led_set(0,state,4); led_set(1,state,4); led_set(2,state,4); led_set(3,state,4); }
```
`wifi_status` = u8 0x3fca2ad9: 0 after reset (0x4200b814), 1 on WIFI_EVENT_STA_CONNECTED,
2 on disconnect / Wi-Fi not started (0x4200bbac, 0x4200bb5c). CONFIRMED.

### 2.3 The tick — `led_tick` 0x4201e03c, every 10 ms — CONFIRMED

Driven by the esp_timer "periodic" (`pxp_periodic_1s_timer`, created in 0x4201bb98, period
**10000 µs**). Its callback 0x4201b5a0 sets event bit 0x40; the brush_app task then calls
`led_tick()` followed by the charger poll (§6.2).

```c
void led_tick(void)
{
    if (g_anim == 4) { led_steady_tick(); return; }             // §2.5
    if (g_time > g_script->e[g_script->count - 1].end) {        // script finished
        if (g_anim == 3) g_wifi_led_enable = 0;                 // 0x3fc9abac
        g_anim = 4;
        g_wifi_level = 0;                                       // 0x3fca4f00
        if (!(batt_pct == 0 && charge_state == 2))
            led_apply_state(g_pend_led, g_pend_state);
        g_breath_down = 0; g_breath = 0;                        // 0x3fca4f04, 0x3fc9abb0
    } else {
        g_wifi_led_enable = 1;
        led_script_step(g_time);                                // 0x4201db20
        g_time += 10;
    }
}
```

### 2.4 Scripts and the fade engine — CONFIRMED (tables verified against .data bytes)

```c
typedef struct { uint16_t start_ms, end_ms; uint8_t dir /*0=fade out,1=fade in*/;
                 uint8_t hal_led /*1..5*/; uint8_t skip_if_done; uint8_t pad; } led_step_t;
typedef struct { uint8_t count; uint8_t pad; led_step_t e[20]; } led_script_t;   // 162 bytes

// anim 3 @0x3fc9abbc — "going to sleep": everything fades out in 0.5 s
static const led_script_t SCRIPT3 = { 4, 0, {
  {0,500,0,1,0}, {0,500,0,2,0}, {0,500,0,3,0}, {0,500,0,5,0} }};

// anim 2 @0x3fc9ac5e — "put on charger"
static const led_script_t SCRIPT2 = { 11, 0, {
  {0,100,0,1,0}, {0,100,0,2,0}, {0,100,0,3,0}, {0,100,0,5,0},
  {100,400,1,3,0}, {400,700,1,2,0}, {700,1000,1,1,0}, {1000,1500,1,5,0},
  {1700,2000,0,3,0}, {2000,2300,0,2,0}, {2300,2600,0,1,0} }};

// anim 1, Wi-Fi connected @0x3fc9ad00 — chase 1→2→3 twice, LED1 stays lit
static const led_script_t SCRIPT1B = { 11, 0, {
  {0,300,1,1,0}, {200,300,0,2,0}, {300,600,1,2,0}, {600,1000,1,3,0}, {900,1000,0,1,0},
  {1000,1300,1,1,0}, {1200,1300,0,2,0}, {1300,1600,1,2,0}, {1500,1600,0,3,0},
  {1600,2000,1,3,0}, {2200,2300,0,3,0} }};

// anim 1, Wi-Fi not connected @0x3fc9ada2 — same plus LED1 off at the end
static const led_script_t SCRIPT1 = { 12, 0, {
  {0,300,1,1,0}, {200,300,0,2,0}, {300,600,1,2,0}, {600,1000,1,3,0}, {900,1000,0,1,0},
  {1000,1300,1,1,0}, {1200,1300,0,2,0}, {1300,1600,1,2,0}, {1500,1600,0,3,0},
  {1600,2000,1,3,0}, {2200,2300,0,3,0}, {2200,2300,0,1,0} }};

// anim 0 @0x3fc9ae44 — "wake up"
static const led_script_t SCRIPT0 = { 10, 0, {
  {0,0,0,1,0}, {0,0,0,2,0}, {0,0,0,3,0}, {0,0,0,5,0},      // instant off
  {10,500,1,5,1},                                           // backlight soft-start
  {700,1000,1,1,0}, {1000,1300,1,2,0}, {1300,1700,1,3,0},
  {1700,2000,0,3,0}, {2000,2300,0,1,0} }};
```
(unused entries of every table are zero; verified.)

Fade engine `led_script_step(t)` 0x4201db20, called with t = 0,10,20,… up to and including
the last entry's `end_ms`:

```c
typedef struct { uint8_t active, led, dir, _p; uint16_t step, cur; uint8_t flag, _p2; } slot_t; // 10 B
static slot_t slots[5];                               // 0x3fca4ec4

static bool want(uint8_t led, uint8_t dir, uint8_t flag)   // 0x4201dadc
{
    if (flag != 1) return true;
    uint32_t lv = get_led_light_level(led);
    if (lv >= led_max_duty[led] && dir == 1) return false;  // already fully on
    return !(lv == 0 && dir == 0);                          // already off
}

void led_script_step(int16_t t)
{
    for (unsigned i = 0; i < g_script->count; i++) {        // 1) start entries due now
        const led_step_t *e = &g_script->e[i];
        if (e->start_ms != t || !want(e->hal_led, e->dir, e->skip_if_done)) continue;
        slot_t *s = first slot with active == 0;  if (!s) continue;
        s->active = 1; s->led = e->hal_led; s->dir = e->dir; s->flag = e->skip_if_done;
        s->cur = get_led_light_level(e->hal_led);
        int dur = e->end_ms - e->start_ms;
        s->step = (dur > 10) ? led_max_duty[s->led] / (dur / 10) : 60000 /*instant*/;
        set_led_light_level_process(s->led, s->cur);
    }
    for (slot_t *s = slots; s < slots + 5; s++) {           // 2) advance all slots
        if (s->active != 1) continue;
        if (s->step == 60000) {                             // instant
            if (s->dir == 0)      set_led_light_level_process(s->led, 0);
            else if (s->dir == 1) set_led_light_level_process(s->led, led_max_duty[s->led]);
            s->cur = 0; s->active = 0;
        } else if (s->dir == 0) {                           // fade out
            int16_t v = (int16_t)(s->cur - s->step);
            if (v < 0) { s->cur = 0; s->active = 0; } else s->cur = v;
            set_led_light_level_process(s->led, s->cur);
        } else if (s->dir == 1) {                           // fade in
            int16_t v = (int16_t)(s->cur + s->step);
            if (s->led == 5) v = (int16_t)s->cur < 20 ? s->cur + 1 : s->cur + 199; // backlight
            s->cur = v;
            if ((uint32_t)(int)v > led_max_duty[s->led]) { s->cur = led_max_duty[s->led]; s->active = 0; }
            set_led_light_level_process(s->led, s->cur);
        }
    }
}
```
Numbers that fall out (10 ms per step):
* LED1..3: step = 4000/(dur/10): 300 ms → 133/tick, 400 ms → 100, 100 ms → 400, 500 ms → 80.
* Backlight fade-in ignores `step`: +1 per tick for the first 20 ticks (levels 1..20, soft
  start), then +199 per tick until > 6000 → 6000. From 0 that is 51 ticks (510 ms) and
  ends at LEDC duty 8191-6000 = **2191**.
* Backlight fade-out: 500 ms → 120/tick, 100 ms → 600/tick. If the backlight was at
  static full (level 8191 > 6000), the first writes are dropped by the max check until
  the running value falls to ≤ 6000, and SCRIPT3 ends (t > 500) before the fade is done:
  51 steps × 120 = 6120 → level 2071 is left on the pin until the sleep code sets the
  backlight state to OFF. From the normal post-script level 6000 the fade completes in
  500 ms. CONFIRMED by arithmetic on the code.
* A script lasts `last.end_ms/10 + 2` ticks: SCRIPT0 2.32 s, SCRIPT1/1B 2.32 s,
  SCRIPT2 2.62 s, SCRIPT3 0.52 s.

### 2.5 Steady tick `led_steady_tick` 0x4201ddf8 (every 10 ms when no script) — CONFIRMED

```c
// g_breath @0x3fc9abb0 (s32, .data = 8191), g_breath_down @0x3fca4f04 (u8)
// g_cnt @0x3fca4ec2 (u8), g_blink @0x3fca4ec1 (u8), g_alt @0x3fca4ec0 (u8)
void led_steady_tick(void)
{
    g_led4_inverted = 1;  g_cnt++;
    if (!demo_mode) {                          // FUN_4201a994(): u8 0x3fca4dca ("guitai")
        if (g_breath_down) { g_breath -= 50; if (g_breath >= -300) goto done; g_breath_down = 0; }
        g_breath += 50;  if (g_breath > 10000) g_breath_down = 1;
    } else {
        if (g_breath_down) { g_breath -= 400; if (g_breath >= 0) goto done; g_breath_down = 0; g_alt ^= 1; }
        g_breath += 400; if (g_breath > 8191) g_breath_down = 1;
    }
done:
    if ((uint8_t)(g_cnt * 225u) < 8) g_blink ^= 1;      // true for g_cnt = 0,33,66,...,231

    if (batt_pct == 0 && charge_state == 2) {           // dead battery, not on charger
        for (led = 1; led <= 4; led++) set_led_light_level_process(led, 0);
    } else if (product_mode == 2) {                     // u8 0x3fca4e9b
        wifi_led_ramp(4000, 40, 1);
    } else if (g_wifi_led_enable == 1) {                // u8 0x3fc9abac (.data = 1)
        int mode = 2;
        if (sys_cfg[0x0c] == 2 && wifi_status != 2 && charge_state == 2)
            mode = (wifi_status == 1) ? 1 : 0;
        wifi_led_ramp(4000, 40, mode);                  // §2.7, drives HAL LED1
    }
    int b = g_breath > 8191 ? 8191 : (g_breath < 0 ? 0 : g_breath);
    for (idx = 0; idx <= 3; idx++) {                    // HAL LED = idx+1
        if (state[idx] == 2)      set_led_light_level_process(idx + 1, g_blink ? 8191 : 0);
        else if (state[idx] == 3) set_led_light_level_process(idx + 1, b);
    }
    if (state_pseudo5 == 3) set_led_light_level_process(g_alt ? 4 : 3, b);  // unused in practice
}
```
`sys_cfg` = config blob at 0x3fc9a69e (NVS "sys_config"); byte 0x0c == 2 means Wi-Fi is
provisioned (it is cleared by `clear_bonding_wifi_record` 0x4200bfb8) — INFERRED meaning.

### 2.6 Waveforms (duty vs. time) — CONFIRMED by arithmetic on §2.5

**BREATHE (state 3)**, normal mode: an internal triangle runs −300 … 10050 in ±50 per 10 ms
(period 415 ticks = **4.15 s**). Because `process` drops values above the LED's max:

| LED | rise | plateau | fall | dark | level range |
|---|---|---|---|---|---|
| LED1..3 (max 4000) | 0→4000 in 0.80 s (50/tick) | held at 4000 for ≈2.42 s | 4000→0 in 0.80 s | ≈0.13 s at 0 | LEDC duty 0..4000 |
| LED4 (max 8191) | 0→8150 in 1.63 s | held ≈0.76 s (values > 8191 are dropped; clamped write 8191) | 1.63 s | ≈0.13 s | level 0..8191 → duty 8191..0 |

The triangle restarts from 0 (rising) after every script. In demo mode ("guitai") the
step is ±400 over 0..8191 (period ≈ 0.43 s).

**BLINK (state 2)**: phase toggles when the 8-bit tick counter is a multiple of 33, i.e.
every 330 ms (one interval in eight is 250 ms because of the 256 wrap) → ≈1.5 Hz, 50 %.
"On" writes level 8191. **This only works on LED4**; on LED1..3 the 8191 write is dropped
by the max check, so a blinking LED1..3 simply stays dark. The only stock user of BLINK is
LED4 (over-pressure). CONFIRMED.

**ON (state 0)**: LED1..3 duty 4000/8192 (48.8 %), LED4 fully on, backlight fully on.

### 2.7 Wi-Fi light ramp `wifi_led_ramp(max, step, mode)` 0x4201dd74 — CONFIRMED

Own counter `g_wifi_level` (s32 0x3fca4f00), direction flag u8 0x3fca4efe; always HAL LED1.

```c
void wifi_led_ramp(int max, int step, int mode)        // called with (4000, 40, mode)
{
    int v = g_wifi_level;
    if (v >= max) {
        if (mode == 0 || mode == 2) { g_dir = 1; g_wifi_level = v - step; }   // turn around
        else if (mode == 1)         { g_wifi_level = max; }                   // hold on
        return;                                                               // no LED write
    }
    if (v <= 0) {
        if (mode < 2)       { g_dir = 0; g_wifi_level = v + step; }           // start rising
        else if (mode == 2) { g_wifi_level = 0; }                             // stay off
        return;
    }
    if (g_dir == 0) g_wifi_level = v + step; else if (g_dir == 1) g_wifi_level = v - step;
    set_led_light_level_process(1, g_wifi_level <= 50 ? 0 : g_wifi_level);
}
```
* mode 0 (Wi-Fi provisioned, not yet connected, off charger): triangle 0↔4000, 40 per
  10 ms → **1 s up, 1 s down, 2 s period**.
* mode 1 (connected, or product mode): ramps to 4000 and holds.
* mode 2 (Wi-Fi off/failed/not provisioned, or on charger): ramps down to 0 and holds.
The ramp is suspended (`g_wifi_led_enable = 0`) after the sleep script (anim 3) and
re-enabled by the next script of any kind; counter reset to 0 at every script end.

---------------------------------------------------------------------------------------

## 3. Pattern catalogue: who starts what

All LED control goes through `led_set`/`led_all`/`led_abort_script`, plus one direct HAL
call (0x4201dacc). There are no other `ledc_*` users. Arguments were read from the
disassembly at each call site. CONFIRMED unless noted.

| Event | Code | LED calls (app idx, state, anim) | Visible result |
|---|---|---|---|
| brush_app task start | 0x4201cc82 | `(4,ON,4)` | backlight full on |
| task start, after init | 0x4201cd5e | if `charge_state != 1 && pct != 0`: `(1,ON,4)` | LED2 steady |
| Wi-Fi STA connected | 0x4200bc62 (wifi_event_handler) | if `pct != 0`: `(1,ON,0)` | wake script 0, LED2 stays on |
| wake from idle-sleep ("motorwakeup") | 0x4201bd70 → 0x4201be0a | off charger & pct≠0: `(1,ON,0)`; else `(4,ON,4)` | wake script / backlight only |
| wake while on charger ("motorwakeup_for_charge") | 0x4201bee8 | `(4,ON,4)`; then state 3: `(2,ON,4)`; state 1: `(2,BREATHE,4)`; state 2: pct==0 ? `(4,ON,4)` : `(1,ON,0)` | backlight on + charge light |
| charger attached | 0x42017a0c(0) @0x42017ae2 | `led_abort_script()`; `(0,OFF,4)`; `(1,OFF,4)`; `(4,OFF,4)`; `(2,BREATHE,2)` | script 2, then LED3 breathing; backlight ends lit (level 6000) |
| 3rd battery tick after a plug-in that woke the brush | 0x42017f26 (`g_3fc9ab8c == 3`) | `(0,OFF,4)`; `(1,OFF,4)`; `(2,BREATHE,2)` | script 2 again, then breathing |
| 4 s on charger | 0x42018398 (counter 0x3fca4b0e == 400 ticks) | direct `set_led_light_level(2, 0)` | LED2 forced dark |
| 30 s on charger | 0x42018250 (`g_3fc9ab89 == 30`, not in product test) | `(4,ON,4)` then `(4,OFF,4)` | backlight off (the ON first guarantees the OFF is not skipped as "no change") |
| button press on charger after that | 0x4201cab8 @0x4201cafd | `(4,ON,4)` | backlight on for another 30 s |
| battery reaches full | 0x42017f08 @0x42018140 | `(2,ON,4)` | LED3 steady |
| charger removed (awake) | 0x42017a0c(1) @0x42017c73 | `led_abort_script()`; `(2,OFF,4)`; `(1,ON,0)` | wake script 0, LED2 on |
| charger removed (asleep) | 0x42017bd0 | `(2,OFF,4)` then wake path 0x4201bee8 | |
| 4 s after boot / after removal from charger | 0x420183d0 (counter 0x3fca4b0c == 400) | `(4,ON,4)` | backlight full on |
| brushing starts (short press) | 0x4201c790 @0x4201c88b, 0x4201c8f3 | `(4,ON,4)`; `(2,OFF,4)`; `(3,OFF,4)` | |
| during brushing, every 30 ms (pxp_fast_timer, event 0x200) | 0x42018530 | see §3.1 | pressure lights |
| brushing paused via event 0x200 with pressure off | 0x4201d4f0 (main loop) | `(2,OFF,4)`; `(3,OFF,4)` | |
| brushing finished (event 0x80000) | 0x4201ce6f | `(2,OFF,4)`; `(3,OFF,4)` | |
| daily-goal celebration (1 Hz sequencer step 10; RTC counters 0x50001020 > 360, 0x5000101e > 200, 0x5000101c > 2 and flag 0x3fca4dfe) | 0x4201c168 | `(1,ON,1)` | chase script 1/1B, LED2 on — meaning of the counters INFERRED |
| idle timeout → sleep | main loop @0x4201d4ad | `(1,OFF,3)` then task `sleep_brush_task` | script 3 (0.5 s fade of LED1,2,3 and backlight) |
| sleep_brush_task (500 ms later) | 0x4201b764 | LCD cmd 0x10; `(4,OFF,4)`; `gpio_set_level(21,1)`; `(0..3,OFF,4)`; `(4,OFF,4)` | everything off |
| idle-sleep entered while on charger | main loop @0x4201d2ea / 0x4201d51a | `gpio_hold_dis(19)`; `led_init()`; `(2, state==3 ? ON : BREATHE, 4)` | only the charge light keeps running (other LED pins are held) |
| battery 0 % and not on charger | 0x4201de65 (steady tick) | LED1..4 forced to 0 every tick; pending script state not applied | all indicators dark |
| product (factory) mode `product_mode == 2` | 0x4201df73 | Wi-Fi ramp mode 1 | LED1 steady |
| factory ageing ("laohua") | 0x4201cab8 param 2, 0x4201d581/0x4201d58a, main loop | `led_all(BREATHE)` when ageing runs, `led_all(OFF)` when it stops | all four breathing |
| shop demo mode ("guitai") | 0x4201af48 @0x4201b0d3 | `(3,OFF,4)`; breathing uses the fast rate | |

Not present in the stock image (searched all 47 call sites): no LED pattern specific to
BLE advertising/pairing, OTA progress or error conditions. BLE has no light at all; Wi-Fi
state is shown only through LED1 (§2.7). CONFIRMED (absence).

### 3.1 Pressure lights while brushing — `0x42018530`, every 30 ms — CONFIRMED

```c
int p = MAX(0, pressure);               // s16 0x3fca5c80 → stored to s16 0x3fca4c74
if (p <= 400) {
    if (press_led_mode == 1)      led_set(2, ON, 4);     // u8 0x3fc9ab97 (.data = 1)
    else if (press_led_mode == 0) led_set(2, OFF, 4);
    else goto skip;                                      // mode 2: leave LEDs untouched
    led_set(3, OFF, 4);
} else if (p <= 599) { led_set(2, OFF, 4); led_set(3, ON, 4); }
else                 { led_set(2, OFF, 4); led_set(3, BLINK, 4); }
skip: ...over-pressure bookkeeping (0x420198fc), brushing-score update (0x420156a0)
```
`press_led_mode` is managed by 0x420198fc/0x420194a8/0x4201c790 (pressure subsystem).

---------------------------------------------------------------------------------------

## 4. Backlight (LED5, GPIO21, LEDC ch4) — summary of everything stock does

CONFIRMED from the call sites above.

| LEDC duty | when |
|---|---|
| 8191 (off) | after `led_init`; state OFF; instant-off entries at the start of scripts 0 and 2 |
| 0 (100 % lit) | state ON via `led_set(4,ON,4)`: task start, brushing start, wake-on-charger, wake with empty battery, 4 s after boot/unplug, button press on charger |
| 2191 (73 % lit) | end of the fade-in of script 0 (wake) and script 2 (placed on charger) |
| ramp | script 0: off at t=0, soft-start from t=10 ms (≈510 ms); script 2: fade out 0–100 ms, fade in 1000–1510 ms; script 3: fade out over 500 ms |

There is no user-adjustable brightness in this module. Off conditions: sleep
(`sleep_brush_task`), 30 s after being placed on the charger, at plug-in (before script 2
lights it again), before deep sleep (pin re-configured, §7).

`gpio_set_level(21, 1)` in 0x4201b764 and `gpio_set_level(21, 0)` in 0x4201bd70/0x4201bee8
are executed while LEDC owns the pin, so they do not change the output — INFERRED
(the GPIO matrix routes the LEDC signal, the GPIO_OUT bit is not used).

---------------------------------------------------------------------------------------

## 5. Battery

### 5.1 ADC path — CONFIRMED

Init (brush_gpio_cfg 0x4200d77c):
```c
cal_ok = 0;
esp_err_t e = esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP_FIT /*3*/);     // 0x4209b8d8
if (e == ESP_OK) {
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11 /*3*/, ADC_WIDTH_BIT_12 /*12*/, 0, &chars /*0x3fca2b70*/);
    esp_adc_cal_characterize(ADC_UNIT_2, 3, 12, 0, &chars2 /*0x3fca2b4c, unused*/);
    cal_ok = 1;                                                                // u8 0x3fca2be4
}   // ESP_ERR_NOT_SUPPORTED / ESP_ERR_INVALID_VERSION: silently cal_ok = 0; others: log "Invalid arg"
adc1_config_width(ADC_WIDTH_BIT_12);
gpio_config(GPIO1 input);  adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);
gpio_config(GPIO10 input); adc1_config_channel_atten(ADC1_CHANNEL_9, ADC_ATTEN_DB_11);  // NTC: never read
```
Read — `batt_read_mv()` 0x4200cc94:
```c
int raw = adc1_get_raw(ADC1_CHANNEL_0);        // also stored in 0x3fca2b94
int16_t mv = cal_ok ? esp_adc_cal_raw_to_voltage(raw, &chars) : 0;
return mv * 2;                                  // ×2 divider
```
Without eFuse calibration every reading is 0 mV. One "sample" used by the gauge is
`(batt_read_mv() + [10 ms later] batt_read_mv()) >> 1`, followed by another 10 ms delay.

### 5.2 Tables — CONFIRMED (raw bytes at 0x3c1192c8 / 0x3c1192e2 / 0x3c1192fc)

```c
// 0x3c1192c8, 13 × u16 LE: state of charge in 0.01 % at 3000 mV + 100 mV * i
static const uint16_t soc_x100[13] = {
/* 3000 */ 0, /* 3100 */ 0, /* 3200 */ 0, /* 3300 */ 0, /* 3400 */ 0,
/* 3500 */ 1000, /* 3600 */ 3000, /* 3700 */ 5000, /* 3800 */ 6500,
/* 3900 */ 7700, /* 4000 */ 9000, /* 4100 */ 10000, /* 4200 */ 10000 };

// 0x3c1192e2, 13 × u16 LE: cumulative charge time in MINUTES at the same voltages
static const uint16_t chg_min[13] = {
  0, 0, 0, 0, 0, 36, 94, 172, 215, 241, 275, 285, 290 };

// 0x3c1192fc: default "brush_battery" record when NVS has none
static const uint8_t batt_rec_default[3] = { 0xff, 0xff, 0xff };
```

### 5.3 Voltage → percent — CONFIRMED

```c
static uint32_t soc_lookup_x100(uint32_t mv)           // common part of 0x42017744 / 0x4201783c
{
    uint32_t v = MAX(mv, 3000);
    uint32_t i = (v / 100) % 10;  if (mv > 3999) i += 10;          // i = (v-3000)/100 for v < 5000
    return soc_x100[i] + (uint32_t)(soc_x100[i + 1] - soc_x100[i]) * (v - (i * 100 + 3000)) / 100;
}
uint8_t soc_raw(uint32_t mv) { return (uint8_t)(soc_lookup_x100(mv) / 100); }   // 0x4201783c
```
Stock bug to avoid: for mv ≥ 4200 the code reads `soc_x100[13]` and beyond (uninitialised
stack). Re-implement with `i` clamped so that mv ≥ 4100 → 10000.

Linear segments: 3400 mV 0 %, 3500 10 %, 3600 30 %, 3700 50 %, 3800 65 %, 3900 77 %,
4000 90 %, ≥4100 100 %.

`soc_filtered(mv)` 0x42017744 adds clamps and a discharge latch:
```c
// g_soc_x100 @0x3fca4b64 (u32), g_first_done @0x3fca4b69 (u8)
uint8_t soc_filtered(uint32_t mv)
{
    uint32_t val = soc_lookup_x100(mv);
    if (g_first_done && charge_state != 2) {            // on charger: no latch, not stored
        if (mv > 4109) return 100;                      // 0x100d
        if (mv <= 3445) return 0;                       // 0x0d75
        return MIN(val, 10000) / 100;
    }
    if (!g_first_done || val < g_soc_x100) g_soc_x100 = val;   // off charger: only falls
    if (g_soc_x100 > 10000) g_soc_x100 = 10000;
    if (mv > 4109)       g_soc_x100 = 10000;
    else if (mv <= 3445) g_soc_x100 = 0;
    return g_soc_x100 / 100;
}
```
So 3445 mV is the hard 0 % point (the table alone would give 4 % there) and anything
above 4109 mV reads 100 %.

### 5.4 State kept — CONFIRMED

| address | type | name here | notes |
|---|---|---|---|
| 0x3fca4b6c+8 | u32 | `charge_state` | 1 charging, 2 off charger, 3 full |
| 0x3fca4b6c+0xc | u16 | published mV | copy of the filtered voltage |
| 0x3fca4b6c+0xe | u8 | `batt_pct` | the percentage everything else uses (UI, BLE, locks) |
| 0x3fca4b64 | u32 | `g_soc_x100` | |
| 0x3fca4b80 | u8 | `g_inited` | first measurement done (bss, 0 at every boot) |
| RTC 0x50001016 | u16 | `rtc_mv` | filtered battery voltage |
| RTC 0x50001018 | u8 | `rtc_pct` | last saved percent, 0xff = none |
| 0x3fc9ab94 / 0x3fc9ab96 | u16 / u8 | `per_cnt` / `period` | .data 63 / 64 |
| 0x3fc9ab92 | u8 | `settle` | .data 2 |
| 0x3fc9ab90 | u16 | `comp_mv` | .data 50 ("buchang" = compensation) |
| 0x3fc9ab88 / 0x3fc9ab8b | u8 / u8 | last reported pct / report counter | .data 0xff / 10 |
| 0x3fc9ab8c | u8 | `plug_cnt` | .data 100 |
| 0x3fc9ab8a | u8 | thermal flag | .data 2 (2 = charging allowed, 1 = cut) |
| 0x3fc9ab8d / 0x3fc9ab8e | u8 / u8 | one-shot extra drop allowance / "fresh" flag | .data 5 / 1 |
| 0x3fca4b5b, 0x3fca4b60, 0x3fca4b68 | u8 | charge slew counter, 99→100 counter, full counter | |
| 0x3fca4b50, 0x3fca4b54, 0x3fca4b58, 0x3fca4b5a | u32,u32,u16,u8 | drop-rate limiter time stamps / credits | |

### 5.5 NVS record `brush_battery` — CONFIRMED (0x4202490c write, 0x42024940 read)

Namespace `"storage"`, key `"brush_battery"`, blob of **20 bytes**; only the first 3 are
meaningful (the writer stores 17 bytes of uninitialised stack after them):

| byte | content |
|---|---|
| 0 | percent 0..100 (0xff = no record) |
| 1 | filtered mV, high byte |
| 2 | filtered mV, low byte |

Read: `nvs_open("storage", NVS_READONLY)`, `nvs_get_blob` (length query, then data); on
ESP_ERR_NVS_NOT_FOUND (0x1102) / 0x1106 / 0x1107 / 0x110c the default ff ff ff is kept.
Loaded into `rtc_pct`/`rtc_mv` at three places: brush_gpio_cfg (0x420182d0), brush_app
start (0x4201831c) and just before the first measurement (0x42017944). Written
(0x42017990 = `{rtc_pct, rtc_mv>>8, rtc_mv}`) whenever the percent changes, on the first
five gauge updates after a BLE event resets the counter (0x42017e74), before deep sleep
(0x4201c5d0) and from four OTA/reset paths (0x420111d0, 0x42013a94, 0x42013b40,
0x4201c6b8). Factory reset writes the default record (0x420179b8).

### 5.6 Start-up sequence — CONFIRMED

1. **Boot gate** (brush_gpio_cfg 0x4200d77c, before any task): while GPIO9 is high (not on
   charger): read one `batt_read_mv()`; if it is **≤ 3299 mV** (0xce3) the IMU is put to
   sleep (0x4201e74c) and the chip enters deep sleep with EXT1 wake on GPIO3|GPIO9 only
   (0x42014240). If GPIO9 is low the gate is skipped. (Above 3299 mV a second filter on
   spurious motion wakes applies — not part of this module.)
2. **brush_app start** (0x4201831c): `batt_pct = soc_raw(batt_read_mv())` (single read);
   load the NVS record; `if (rtc_pct == 0 && batt_pct < 10) batt_pct = 0;`
   `charge_state = 2`; register the charger callback 0x42017a0c.
3. **First real measurement**, driven by the 1 Hz battery tick `batt_tick(sec)` with the
   task's seconds counter (u16 0x3fca4e2a, 0 at boot and at brushing start):
   * sec 0,1: nothing. sec 2: `charge_enable(0)` (charging blocked so the cell relaxes).
   * sec ≥ 3: `charge_enable(0)`; reload NVS record; then 0x420178b8:
     ```c
     charge_enable(0); charge_state = 2; g_first_done = 0;
     avg = two-read sample (§5.1);
     pct = soc_filtered(avg);  batt_pct = pct;
     if (rtc_pct == 0xff)      { rtc_pct = pct; rtc_mv = avg; }            // no history
     else if (rtc_pct == 0)    { if (pct > 39) rtc_mv = avg;               // unlock at ≥ 40 %
                                 else { batt_pct = 0; g_soc_x100 = 0; } }  // stay locked at 0
     else { if (avg <= 3299) rtc_pct = 0;
            batt_pct = rtc_pct; g_soc_x100 = rtc_pct * 100; }              // trust the saved value
     g_first_done = 1; charge_enable(1);
     ```
     then `g_inited = 1`. Charger detection (§6.2) only starts once `g_inited` is set.

### 5.7 Periodic gauge — `batt_tick` 0x42017f08 — CONFIRMED

Called once per second from the brush_app 1 s section **only while not brushing**
(`g_3fca4d5b == 0`); the percent is frozen during a brushing session.

```c
void batt_tick(uint16_t sec)
{
    if (plug_cnt <= 9) plug_cnt++;
    if (plug_cnt == 3) { led_set(0,OFF,4); led_set(1,OFF,4); led_set(2,BREATHE,2); }
    ... first-measurement handling of §5.6, return if !g_inited ...

    if (charge_state == 1 || charge_state == 3) { if (per_cnt >= period) per_cnt = 0;          per_cnt++; }
    else if (charge_state == 2)                 { if (per_cnt >= period) per_cnt = period - 2; per_cnt++; }

    uint32_t avg = two-read sample;  uint16_t mv = avg;
    if (charge_state == 2) period = 64;
    else {
        period = charge_period(avg);                 // §5.8
        if (avg <= 3999) mv -= comp_mv;              // −50 mV while charging below 4.0 V
        if (idle_sleep_flag) mv -= 18;               // u8 0x3fca41a4 (screen off / idle)
    }
    rtc_mv = (uint16_t)(rtc_mv * 0.3 + mv * 0.7);    // IIR, doubles 0x3fd3333333333333 / 0x3fe6666666666666
    uint8_t pct = soc_filtered(rtc_mv);

    if (per_cnt == period) {                         // "update" tick
        if (charge_state == 1) {                     // charging: slow upward slew only
            if (settle < 200) settle++;
            if (++slew_cnt > 9) {                    // every 10th update
                if (batt_pct < pct) {
                    if (batt_pct <= 98) batt_pct++;
                    else if (pct > 99 && ++cnt_99 >= 3) batt_pct++;        // 99 → 100
                }
                slew_cnt = 0;
            }
            g_soc_x100 = batt_pct * 100;
        } else {                                     // off charger, or full
            if (settle < 200) settle++;
            if (settle >= 2) {
                if (pct < batt_pct) {
                    if (pct <= 9) { int d = MAX(1, (batt_pct - pct) / 3);
                                    batt_pct = (d >= batt_pct) ? pct : batt_pct - d; }
                    else { int d = drop_allowance();                       // §5.9
                           if (d) batt_pct = (batt_pct - pct < d) ? pct : batt_pct - d; }
                }
                uint8_t p2 = soc_raw(rtc_mv);
                if (p2 > 50 && batt_pct == 0) { batt_pct = p2; g_soc_x100 = p2 * 100; }  // leave the 0 % lock
            }
            if (charge_state == 3) { batt_pct = 100; g_soc_x100 = 10000; }
        }
        rtc_pct = batt_pct;
        ui_post(0x2a, &batt_pct, 1);                 // 0x42021704
        if (last_reported != batt_pct || report_cnt < 5) {
            last_reported = batt_pct; report_cnt++;
            ble_battery_notify(batt_pct);            // 0x4200e6c8
            batt_record_save();                      // 0x42017990
        }
        published_mv = rtc_mv;
    }
    if (batt_pct > 99 && charge_state == 1) {        // full detection, checked every second
        if (full_cnt < 10) full_cnt++;
        if (full_cnt >= 6) { charge_state = 3; led_set(2, ON, 4); }
    }
    charge_thermal_check();                          // §6.5
}
```
Cadence that results:
* **Off charger**: `period` = 64 and `per_cnt` is re-armed to `period-2`, so after the
  first hit the update branch runs **every 2 s**. `per_cnt` starts at 63 → first update on
  the first tick. After un-plugging, `settle` is reset to 0 so the first update is skipped.
* **Charging**: update every `period` seconds (6..23 s, §5.8); the percent may rise by one
  point only on every 10th update. At plug-in `per_cnt` is set to 0 (0x420179f4).
* The percent never rises while off the charger (except the 0 % → `p2` recovery above
  50 %), and never falls while charging.

### 5.8 Charge slew period — `charge_period` 0x42017ca0 — CONFIRMED

```c
uint8_t charge_period(uint16_t avg_mv)
{
    int i = (int8_t)(avg_mv / 100 - 29);  if (i > 11) i = 11;  if (i < 5) i = 5;
    uint16_t dsoc = soc_x100[i] - soc_x100[i - 1];  if (dsoc == 0) dsoc = 1;
    uint8_t p = (uint8_t)(((int)(chg_min[i] - chg_min[i - 1]) * 600) / dsoc);
    return p ? p : 1;                       // logged as "index = %d,PEROID_CHARGE_PLUS=%d"
}
```
One percent point takes `10 * period` s = (minutes of the segment × 60) / (percent of the
segment):

| avg mV | i | period (s) | s per 1 % |
|---|---|---|---|
| < 3500 | 5 | 21 | 210 |
| 3500–3599 | 6 | 17 | 170 |
| 3600–3699 | 7 | 23 | 230 |
| 3700–3799 | 8 | 17 | 170 |
| 3800–3899 | 9 | 13 | 130 |
| 3900–3999 | 10 | 15 | 150 |
| ≥ 4000 | 11 | 6 | 60 |

### 5.9 Discharge drop limiter — 0x42017de0 and helpers — CONFIRMED (as coded)

```c
uint8_t drop_allowance(void)
{
    uint32_t now = xTaskGetTickCount() / 1000;                 // 0x4201b5e8
    uint32_t acc = credit8 /*0x3fca4b5a*/ + credit16 /*0x3fca4b58*/;
    int el = (now - t_mark /*0x3fca4b50*/) + acc;  uint8_t d = 0;
    if (el > 99) { credit8 = acc % 100; d = el / 100; t_mark = now; fresh = 1; }
    if (fresh) fresh = 0; else extra = 0;                      // extra @0x3fc9ab8d, fresh @0x3fc9ab8e
    return d + extra;
}
```
* base rate: 1 point per 100 s;
* `extra` is a one-shot allowance: 5 at boot; after a brushing session
  `0x42017d28(duration)` sets it to 240 (≥ 16 s brushed, i.e. unlimited) or 3 and sets `fresh`;
* sleep credits: on entering idle-sleep (0x42017d54) `credit8 += (now - t_mark) * 15000 / 36000`;
  on wake (0x42017da4) `credit16 += (now - t_sleep) * 5500 / 36000` and `t_mark = now`.
  `credit16` is never cleared, so after long accumulated sleep the limiter lets ≥ 1 point
  through on every update — stock quirk; a re-implementation can use "1 % per 100 s awake,
  free drop right after a brushing session" without visible difference. INFERRED equivalence.

### 5.10 Low-battery behaviour — CONFIRMED

| condition | where | effect |
|---|---|---|
| VBAT ≤ 3299 mV at boot, not on charger | 0x4200d77c | straight back to deep sleep (wake only by button or charger) |
| VBAT ≤ 3299 mV at first measurement with a saved non-zero percent | 0x420178b8 | saved percent set to 0 → `batt_pct = 0` |
| filtered VBAT ≤ 3445 mV | 0x42017744 | percent 0 |
| **0 % lock**: saved percent 0 | 0x420178b8, 0x4201831c | stays 0 until a measurement gives ≥ 40 % at boot, the running filtered value gives > 50 %, or the charge slew raises it |
| `batt_pct == 0`, short button press | 0x4201c790 | logs `"low power.ota lock key %d %d"`, idle timeout set to 1 s, UI message **0x5e (94)** (low-battery screen); brushing does not start |
| `batt_pct == 0`, wake, not on charger (GPIO9 == 1) | 0x42029218 | UI message 94, idle timeout 0 (sleep at once) |
| `batt_pct == 0`, not on charger | 0x4201de65 / 0x4201e095 | LED1..4 forced dark, script end states not applied |
| `batt_pct == 0`, idle | 0x4201c5d0 | deep sleep variant with EXT1 (button/charger) only — no motion wake |
| `batt_pct ≤ 10`, off charger, not product mode, at the idle timeout | 0x420142d0 | UI message 94; if `batt_pct != 0` and a brushing session ran since the last warning: 3 motor pulses (0x4201babc: 400 ms on / 400 ms off); sleep is delayed 3 s |
| `batt_pct < 20` | 0x42013a78 | OTA refused (also refused while brushing) |
| `batt_pct ≤ 10` while shop demo mode ("guitai", 0x3fca4dca) is on | 0x4201b1e4 (1 Hz sequencer) | demo mode is switched off and the main screen shown (0x4201a3fc) |
| filtered mV ≤ 3200 in factory ageing | main loop (0xc80) | ageing aborted ("quit laohua") |

---------------------------------------------------------------------------------------

## 6. Charging

### 6.1 Pins — CONFIRMED

| pin | use | driven how |
|---|---|---|
| GPIO9 | charger present, **0 = present** | input, no pull, ANYEDGE interrupt (0x4200d77c); ISR branch 0x40377d35 |
| GPIO2 | "charger still alive" pulse | input, NEGEDGE interrupt; ISR branch 0x40377d6d clears the un-plug debounce counter (low byte of u16 0x3fc9f304). What produces the edges is unknown (INFERRED: charger/receiver status line) |
| GPIO26 | charge inhibit | `charge_enable(1)`: `gpio_hold_dis(26); gpio_set_direction(26, GPIO_MODE_INPUT); gpio_hold_en(26)` — `charge_enable(0)`: `gpio_hold_dis(26); gpio_config{bit 26, OUTPUT, no pulls, no intr}; gpio_set_direction(26, GPIO_MODE_OUTPUT); gpio_set_level(26, 1); gpio_hold_en(26)` (0x4200d650, logs `"set_CHARGE_EN_IO_level %d"`) |
| GPIO45 | "WLC_EN" | always 0: boot (`gpio_set_level(45,0); gpio_hold_en(45); gpio_sleep_sel_en(45)`), 0x4200d6b0, sleep prep, main loop |

`wlc_set(x)` 0x4200d6b0: `x == 0` → GPIO45 = 0 (unhold/set/hold), `g_3fc9a21c = 0`,
`charge_enable(1)`; `x != 0` → `charge_enable(0)`, GPIO45 = 0, `g_3fc9a21c = x`. The only
non-zero caller is an app/AT command handler (0x4200fdb8, command byte 0x60), i.e. the
phone can inhibit charging; firmware-internal calls all pass 0.

### 6.2 Detection — CONFIRMED

Two paths call the same action function `usb_action(removed)` 0x42017a0c:
* **Interrupt**: GPIO9 edge with level 0, once `g_inited` → 0x4201d868 sets argument 0 and
  event bit 0x20 → brush_app calls `usb_action(0)` (0x4201d888).
* **Poll**, every 10 ms right after `led_tick()` — `charger_poll(sec)` 0x4201840c:

```c
void charger_poll(uint16_t sec)
{
    if (charge_state == 2) on_chg_ticks = 0;                       // u16 0x3fca4b0e
    else { if (on_chg_ticks < 1000) on_chg_ticks++;
           if (on_chg_ticks == 400) set_led_light_level(2, 0); }
    if (charge_state == 2) { if (off_chg_ticks < 1000) off_chg_ticks++;   // u16 0x3fca4b0c
                             if (off_chg_ticks == 400) led_set(4, ON, 4); }
    else off_chg_ticks = 0;

    if (brushing && !g_inited && gpio_get_level(9) == 0) batt_tick(sec);   // lets the gauge init while brushing on the dock
    if (!g_inited) return;

    if (charge_state == 1 && gpio_get_level(9) != 0) {
        if (++unplug_cnt > 150) usb_action(1);                     // u16 0x3fc9f304, 1.51 s
    } else if (charge_state == 2) {
        if (gpio_get_level(9) == 0) usb_action(0);
    } else if (charge_state == 3 && gpio_get_level(9) != 0) {
        if (++unplug_cnt > 150) { wlc_set(0); usb_action(1); }
    }
    if (gpio_get_level(9) == 0) unplug_cnt = 0;
}
```
Removal therefore needs GPIO9 high for 151 consecutive 10 ms ticks with no falling edge on
GPIO2 in between.

### 6.3 State machine — CONFIRMED

```
            usb_action(0)                         batt_pct == 100 for 6 s
   [2 off charger] ───────────────► [1 charging] ───────────────────────► [3 full]
        ▲   ▲                            │                                    │
        │   └──── usb_action(1) ─────────┘                                    │
        └──────────────────────── usb_action(1) ──────────────────────────────┘
   first measurement (0x420178b8) and task start force state 2
```
"Full" is decided only by the gauge (§5.7): filtered voltage above 4109 mV makes the
target 100 %, the slew brings `batt_pct` up one point at a time (the last point needs
three confirmations), and after `batt_pct ≥ 100` has held for 6 consecutive battery ticks
the state becomes 3. There is no charger-IC status input. In state 3 charging is **not**
switched off by the firmware (GPIO26 stays released).

`usb_action` does nothing when the long-press-reset flag (0x3fca4ea0) is set or a sleep
check is in progress (0x3fca4198); in shop demo mode it only does `wlc_set(0);
charge_enable(1)`.

**Attach — `usb_action(0)`, only from state 2** (in this order):
```
close_screen_cnt = 0 (0x3fc9ab89); settle = 0; cnt_99 = 0; slew_cnt = 0;
wlc_set(0);  unplug_cnt = 0;  full_cnt = 0;  puts("USB_IN_ACTION");
rtc_50001015 = 0;  plug_cnt = 100;  charge_state = 1;
log the session start time (up to 5 sessions, table 0x3fca4b1a, count at +0x32);
if (idle_sleep_flag == 1) { puts("WAKE UP AND BRUAH"); wake (0x4201bd70); plug_cnt = 0; }
stop the motor (callback 0x3fc9a1e4+0x28); if brushing: stop the session (0x42019528);
per_cnt = 0;  idle timeout = 30 s (0x42014278(30));
charge_enable(1);
led_abort_script(); led_set(0,OFF,4); led_set(1,OFF,4); led_set(4,OFF,4); led_set(2,BREATHE,2);
if (!ota_running && !(product_mode == 2 && ageing))  ui: current screen = 93, post 0x5d with batt_pct;
g_3fca4b5c = 1;  comp_mv = 50;
```
**Remove — `usb_action(1)`**:
```
plug_cnt = 100;  close the session log entry (duration in s, +1);  wlc_set(0);
if (idle_sleep_flag == 1) {                    // was asleep on the dock
    led_set(2,OFF,4); charge_state = 2; wake_for_charge() (0x4201bee8, posts UI 0x78);
    g_3fc9ab9f = 0; rtc_50001015 = 0;
    if (batt_pct == 0) { idle timeout = 1 s; close_screen_cnt = 100; }
    return;
}
0x4201b430(6);
if (batt_pct == 0) { idle timeout = 1 s; close_screen_cnt = 100; charge_state = 2; return; }
idle timeout = 30 s;
if (!(product_mode == 2 && ageing) && !ota_running && rtc_50001015 == 0) {
    if (close_screen_cnt < 30) show main screen (0x4201a3fc);
    else { rtc_50001015 = 0; g_3fc9ab9f = 0; pending_screen = 1 (0x3fca4b18); }  // handled 4 s later in the 1 s loop
}
close_screen_cnt = 100;  per_cnt = period - 2;  settle = 0;  cnt_99 = 0;  charge_state = 2;
led_abort_script(); led_set(2,OFF,4); led_set(1,ON,0);
```

### 6.4 What the UI is told — CONFIRMED (message ids posted with 0x4201f840)

| id | posted by | when | payload |
|---|---|---|---|
| 0x5d (93) | 0x4201a2a4 → 0x42021648 | charger attached; also 4 s after `pending_screen` if still on the charger | 1 byte: `batt_pct` |
| 0x78 (120) | 0x4202151c | `wake_for_charge` 0x4201bee8: button press while idle-asleep on the charger, or removal while asleep | none |
| 0x5e (94) | 0x4201a260 → 0x42021638 | low-battery cases of §5.10 | none |
| 0x2a (42) | 0x42021704 | every gauge update | 1 byte: `batt_pct` |

The variable "current screen" (u8 0x3fc9ab9a) is set to 0x5d / 0x5e by the same helpers.
Nothing extra is posted at the charging → full transition; the UI only sees 0x2a with 100
and the LED turns steady. How these ids are drawn belongs to the UI spec.

The 1 Hz screen sequencer 0x4201b1e4 returns at once when `charge_state != 2`, so no
screen auto-advance happens on the charger.

On the charger the 1 s loop also runs `0x42018250`: `close_screen_cnt++` (cap 200) and at
exactly 30 the backlight is switched off (§3). `0x4201829c` (called on any button event)
returns true and zeroes the counter when it is ≥ 30, which re-lights the backlight.

### 6.5 Thermal cut-off — `charge_thermal_check` 0x42017e84 — CONFIRMED

Runs at the end of every `batt_tick` (1 Hz, not while brushing), only when
`charge_state != 2`:
```c
float t = imu_temp_c();          // 0x4201e240: QMI8658 regs 0x33/0x34 as s16 LE, × (1/256)
printf("PABA_FUNC_TEMP:%f", t);
if (t > 72.0f && therm == 2) { charge_enable(0); therm = 1; }   // 0x42900000
if (t < 67.0f && therm == 1) { charge_enable(1); therm = 2; }   // 0x42860000
```
The NTC on ADC1_CH9/GPIO10 is configured but never sampled.

---------------------------------------------------------------------------------------

## 7. Sleep-related pin handling done for these pins — CONFIRMED

**Idle-sleep parking** `0x4200dbbc` (called from the brush_app loop on the
SLEEP_SEND / BLE-timeout events, after `0x4201c5d0`):

| pin | action |
|---|---|
| 17, 18 | `gpio_hold_dis`; `gpio_config` OUTPUT (this detaches LEDC); level **1**; `gpio_hold_en` |
| 19 | same, level **0** |
| 20 | same, level `(g_3fc9abba == 1)` = **1** |
| 21 (backlight) | `gpio_config` **OUTPUT_OD** (mode 6), level **1** (released → off), `gpio_hold_en` |
| 26 | `charge_enable(1)` (input, held) |
| 45 | OUTPUT, level 0 |
| also | GPIO2 input NEGEDGE; GPIO12 mode 0 level 0 hold; GPIO48 = 0; GPIO37 = 1 hold; GPIO42 = 0 hold; GPIO41 = 0 hold; sleep direction off for 38/39/40; finally `gpio_force_unhold_all()` |

Then, **only if on the charger** (`charge_state != 2`): `gpio_hold_dis(19); led_init();
led_set(2, charge_state == 3 ? ON : BREATHE, 4)` — LEDC is restarted and, because 17/18/
20/21 are still held, only the charge light on GPIO19 keeps animating. Off the charger the
10 ms tick timer is stopped instead (`esp_timer_stop(pxp_periodic_1s_timer)`), so no LED
code runs.

**Deep-sleep parking** `0x4200dda4` (from `0x4201c5d0`): identical treatment of 17/18/19/
20/21/26/45 (17=1, 18=1, 19=0, 20=1 as plain held outputs, 21 open-drain high held, 26
input held, 45 low), plus the bus pins. `0x4201c5d0` returns immediately when
`charge_state != 2`: **the brush never deep-sleeps on the charger.** Before sleeping it
saves the battery record (0x42017990).

**Wake re-init** `0x4200df88` (from 0x4201bd70 / 0x4201bee8): `gpio_hold_dis` on
13,14,21,42,17,18,19,20,37,41,12; levels 17=1, 18=1, 19=0, 20=1 as OUTPUT; `led_init()`
(all LEDs off, §1.2); GPIO45 = 0; `charge_enable(1)`. The caller then restarts the 10 ms
timer and issues the wake LED calls of §3.

Deep-sleep wake sources set by 0x42014220 / 0x420141cc: `esp_sleep_enable_ext1_wakeup(
0x208 /*GPIO3|GPIO9*/, 0)` always; `esp_sleep_enable_ext0_wakeup(8, 1)` (motion) only in
the "deep sleep2" variant. Details belong to the power-management spec.

---------------------------------------------------------------------------------------

## 8. Implementation notes for the custom firmware

1. Charger present = `gpio_get_level(9) == 0`. `hardware.h` currently uses GPIO8 high.
2. Charging allowed = GPIO26 as input (high-Z). Charging blocked = GPIO26 output high.
   Never drive GPIO26 low and never drive GPIO45 high — stock does neither, so the effect
   on this board is unknown. `hw_charge.c` currently does the opposite on both pins.
3. LED polarity per channel differs (§1.2); use the three HAL functions of §1.4 verbatim
   and keep the 4000 / 8191 / 6000 limits.
4. One 10 ms tick runs: `led_tick()` then `charger_poll()`. One 1 s tick (not while
   brushing) runs `batt_tick()`.
5. Requests made while a script runs are dropped, so stock code calls
   `led_abort_script()` first when a request must not be lost (plug / unplug).
6. Clamp the table index in `soc_lookup_x100` (stock overruns the table at ≥ 4200 mV).
7. Stock uses the legacy `esp_adc_cal` line-fit ("TP_FIT") conversion; the custom code
   uses `adc_cali` curve fitting. The mV thresholds above (3299, 3445, 3999, 4109) were
   tuned against the stock conversion; expect a few mV of difference.

---------------------------------------------------------------------------------------

## 9. Open questions

1. **Colour and position of the four indicator LEDs** (GPIO17/18/19/20). Nothing in the
   image names a colour. Roles are known (§2.1), colours need one look at the device.
2. **GPIO9 polarity on the bench.** The code is unambiguous (0 = charger), but this
   contradicts the earlier "GPIO8, active-high" note; confirm by logging both pins while
   placing the brush on the dock.
3. **GPIO26 circuit.** Stock only uses high-Z (charge) and driven-high (inhibit). Whether
   the line has an external pull-down, and what driving it low does, is unknown.
4. **GPIO45.** Named WLC_EN in the log strings but held low everywhere. Purpose unknown.
5. **GPIO2 falling edges** that hold off un-plug detection: source unknown (charger or
   wireless-receiver status?). If the custom firmware ignores GPIO2 it may report
   "removed" when stock would not.
6. **`sys_cfg[0x0c] == 2`** is read as "Wi-Fi provisioned" from one writer; the full
   meaning of that byte and of `wifi_status` 0 vs 2 during BluFi provisioning is not traced.
7. **LEDC in automatic light sleep** (charge light while idle on the dock): which clock
   LEDC_AUTO_CLK picked and whether PWM keeps running between the 10 ms wake-ups was not
   analysed.
   Custom firmware, 2026-10-04 (read from IDF v5.1.1 `ledc.c` and `main/hw_led.c`, not
   measured on the device): for 5 kHz at 13 bit LEDC_AUTO_CLK resolves to the APB clock (the
   first source the driver tries; the crystal and the RC oscillator give no valid divider); no
   light sleep is ever entered (tickless idle is off); and the APB clock stays at 80 MHz in
   this build (`NOTES_power.md` section 5).
8. **BLINK on LED1..3 is ineffective** in stock (8191 > 4000 is dropped). Probably an OEM
   oversight; no stock path uses it, so it is only a question if new patterns are added.
9. **Backlight brightness mismatch** (duty 0 after a direct ON, duty 2191 after a scripted
   fade-in) is what the code does; whether it is visible/intentional is unknown.
10. **Charge-session log** at 0x3fca4b1a (5 × 10-byte records: 7-byte timestamp, u16
    duration at +8, count at +0x32) is filled here but consumed by the app-sync code; its
    wire format was not traced.
11. **`rtc_50001015`**, `g_3fc9ab9f`, `pending_screen` (0x3fca4b18): they sequence which
    screen appears ~4 s after removal from the charger when the screen had timed out; the
    consumer is in the brush_app 1 s loop (fragment 0x4201d38d) and was only skimmed.
12. Drop-limiter credits (§5.9): implemented as coded, intent not fully understood.

Helper scripts used for the raw-byte checks: `spec/work_led/dump.py`, `spec/work_led/scripts.py`.

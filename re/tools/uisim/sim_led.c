// Host test of the LED pattern engine (main/oem_led.c) on top of the real LED driver
// (main/hw_led.c) and a fake LEDC: prints the raw LEDC duty of the five channels over
// time for every script, breathe, blink and the Wi-Fi light, and checks them against
// the waveform tables of re/spec/led_battery_charge.md (2.4, 2.6, 2.7, 4).
//
//   cc -std=gnu11 -Wall -Wextra -Imain -Ire/tools/uisim/fake_idf
//      re/tools/uisim/sim_led.c main/hw_led.c -o /tmp/sim_led && /tmp/sim_led [-v]
//
// -v prints every 10 ms tick instead of one row per 100 ms.
// Raw duty 0..8191 of LEDC channel 0..4 (GPIO17..21). Lit means: ch0..2 duty > 0
// (4000 = full), ch3 duty < 8191 (0 = full), ch4 = backlight, duty < 8191
// (0 = 100 %, 2191 = the 73 % a scripted fade-in ends at).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oem_led.c"            // white box: s_anim tells when a script has ended
#include "hardware.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

// ---- fakes -----------------------------------------------------------------------
oem_state_t g_oem;
bool hw_emulated(void) { return false; }
void hal_log(const char *fmt, ...) { (void)fmt; }

static uint32_t f_duty[5], f_pending[5];
static int f_updates, f_redundant, f_timer_cfgs, f_chan_cfgs;
static ledc_timer_config_t f_timer;
static ledc_channel_config_t f_chan[5];
static int f_level[49], f_hold[49], f_mode[49], f_pullup[49], f_pulldown[49];

esp_err_t ledc_timer_config(const ledc_timer_config_t *c) { f_timer = *c; f_timer_cfgs++; return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *c)
{
    f_chan[c->channel] = *c;
    f_duty[c->channel] = f_pending[c->channel] = c->duty;
    f_chan_cfgs++;
    return ESP_OK;
}
esp_err_t ledc_set_duty(ledc_mode_t m, ledc_channel_t ch, uint32_t duty)
{
    (void)m;
    if (duty > 8191) { printf("  !! duty %u out of range on ch%d\n", (unsigned)duty, (int)ch); exit(1); }
    f_pending[ch] = duty;
    return ESP_OK;
}
esp_err_t ledc_update_duty(ledc_mode_t m, ledc_channel_t ch)
{
    (void)m;
    f_updates++;
    if (f_duty[ch] == f_pending[ch]) f_redundant++;
    f_duty[ch] = f_pending[ch];
    return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t *c)
{
    for (int p = 0; p < 49; p++)
        if (c->pin_bit_mask >> p & 1) { f_mode[p] = c->mode; f_pullup[p] = c->pull_up_en; f_pulldown[p] = c->pull_down_en; }
    return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t p, uint32_t l) { f_level[p] = (int)l; return ESP_OK; }
esp_err_t gpio_hold_en(gpio_num_t p)  { f_hold[p] = 1; return ESP_OK; }
esp_err_t gpio_hold_dis(gpio_num_t p) { f_hold[p] = 0; return ESP_OK; }

// ---- helpers ---------------------------------------------------------------------
static int g_fail, g_checks, g_verbose;
static void check(bool ok, const char *fmt, ...)
{
    va_list ap;
    g_checks++;
    if (!ok) g_fail++;
    printf("  %s ", ok ? "ok  " : "FAIL");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

#define MAXT 2000
static uint16_t tr[MAXT][5];    // duty after tick k (k = 0 is the tick at script time 0)
static int tr_n;

static void row(int k)
{
    printf("  %5d  %5u %5u %5u %5u %5u\n", k * 10, tr[k][0], tr[k][1], tr[k][2], tr[k][3], tr[k][4]);
}
static void dump(const char *title, int n)
{
    printf("%s\n   t ms    ch0   ch1   ch2   ch3   ch4(backlight)\n", title);
    for (int k = 0; k < n; k++)
        if (g_verbose || k % 10 == 0 || k == n - 1) row(k);
}
static void tick_rec(int n)
{
    for (int i = 0; i < n && tr_n < MAXT; i++) {
        oem_led_tick();
        for (int c = 0; c < 5; c++) tr[tr_n][c] = (uint16_t)f_duty[c];
        tr_n++;
    }
}
// Run the script that was just requested to its end; returns the number of ticks.
static int run_script(void)
{
    tr_n = 0;
    while (s_anim != ANIM_IDLE && tr_n < MAXT) tick_rec(1);
    return tr_n;
}
// first tick index at which channel ch has duty v (from tick k0 on), or -1
static int first_at(int ch, unsigned v, int k0)
{
    for (int k = k0; k < tr_n; k++) if (tr[k][ch] == v) return k;
    return -1;
}

static void reset_engine(void)
{
    // back to the power-on state of the module and a normal, awake, off-charger brush
    memset(s_state, 0xff, sizeof s_state);
    s_anim = ANIM_IDLE; s_breath = 8191; s_breath_down = 0; s_cnt = s_blink = s_alt = 0;
    s_wifi_enable = 1; s_wifi_level = 0; s_wifi_dir = 0;
    memset(&g_oem, 0, sizeof g_oem);
    g_oem.power_state = OEM_PWR_BATTERY;
    g_oem.batt_pct = 80;
    g_oem.dev_mode = 1;
    oem_led_init();
}
static void awake_state(void)   // brush_app start: backlight on, "ready" light on
{
    reset_engine();
    oem_led_set(4, ST_ON, 4);
    oem_led_set(1, ST_ON, 4);
}

// ---- tests -----------------------------------------------------------------------
static void t_init(void)
{
    printf("== init (led_init) and static states\n");
    reset_engine();
    check(f_timer.freq_hz == 5000 && f_timer.duty_resolution == 13 && f_timer.timer_num == 0,
          "timer 0: 5 kHz, 13 bit");
    bool ok = f_chan_cfgs == 5;
    static const int pin[5] = { 17, 18, 19, 20, 21 }, inv[5] = { 1, 1, 0, 0, 0 };
    for (int c = 0; c < 5; c++)
        ok = ok && f_chan[c].gpio_num == pin[c] && (int)f_chan[c].flags.output_invert == inv[c];
    check(ok, "channels 0..4 on GPIO17..21, output_invert 1 1 0 0 0");
    check(f_duty[0] == 0 && f_duty[1] == 0 && f_duty[2] == 0 && f_duty[3] == 8191 && f_duty[4] == 8191,
          "initial duty 0 0 0 8191 8191 (all dark)");
    check(f_level[17] == 1 && f_level[18] == 1 && f_level[19] == 0 && f_level[20] == 1 &&
          f_pullup[17] == 1 && f_pulldown[18] == 1, "boot preset 17=1 (pull-up) 18=1 (pull-down) 19=0 20=1");

    oem_led_set(4, ST_ON, 4);  check(f_duty[4] == 0, "backlight ON  -> duty 0 (100 %%)");
    oem_led_set(4, ST_OFF, 4); check(f_duty[4] == 8191, "backlight OFF -> duty 8191");
    oem_led_all(ST_ON);
    check(f_duty[0] == 4000 && f_duty[1] == 4000 && f_duty[2] == 4000 && f_duty[3] == 0,
          "LED1..4 ON -> 4000 4000 4000 0");
    oem_led_all(ST_OFF);
    check(f_duty[0] == 0 && f_duty[1] == 0 && f_duty[2] == 0 && f_duty[3] == 8191, "LED1..4 OFF -> 0 0 0 8191");
    oem_led_level(5, 3000);    check(f_duty[4] == 3000, "direct level 3000 on the backlight -> duty 6000 - 3000");
    oem_led_level(2, 8191);    check(f_duty[1] == 4000, "direct level 8191 on LED2 is clamped to 4000");
}

static void t_wake(void)
{
    printf("== script 0 (wake): oem_led_set(1, ON, 0) from backlight static on\n");
    awake_state();
    oem_led_set(1, ST_OFF, 4);
    oem_led_set(1, ST_ON, 0);
    int n = run_script();
    dump("  wake script", n);
    check(n == 232, "script lasts 232 ticks (2.32 s): %d", n);
    check(tr[0][4] == 8191, "t=0: backlight off (8191)");
    check(tr[1][4] == 8190 && tr[20][4] == 8171, "soft start: +1 per tick for 20 ticks (t=10 %u, t=200 %u)", tr[1][4], tr[20][4]);
    check(tr[21][4] == 7972 && tr[50][4] == 2201, "then +199 per tick (t=210 %u, t=500 %u)", tr[21][4], tr[50][4]);
    check(first_at(4, 2191, 0) == 51 && tr[n - 1][4] == 2191, "backlight reaches raw 2191 at t=510 and stays (%d)", first_at(4, 2191, 0) * 10);
    check(tr[69][0] == 0 && tr[70][0] == 133 && first_at(0, 4000, 0) == 100, "LED1 fades in 700..1000 ms, 133 per tick");
    check(tr[99][1] == 0 && first_at(1, 4000, 0) == 130, "LED2 fades in 1000..1300 ms");
    check(tr[130][2] == 100 && first_at(2, 4000, 0) == 169 && first_at(2, 0, 170) == 200, "LED3 in 1300..1700 (100 per tick), out by 2000 ms");
    check(tr[199][0] == 4000 && first_at(0, 0, 200) == 230, "LED1 fades out 2000..2300 ms");
    check(f_duty[0] == 0 && f_duty[1] == 4000 && f_duty[2] == 0 && f_duty[3] == 8191 && f_duty[4] == 2191,
          "end: 0 4000 0 8191 2191 (LED2 = pending ON)");
}

static void t_sleep(void)
{
    printf("== script 3 (sleep): oem_led_set(1, OFF, 3) after the wake script\n");
    awake_state();
    oem_led_set(1, ST_OFF, 4);
    oem_led_set(1, ST_ON, 0);
    run_script();
    oem_led_set(1, ST_OFF, 3);
    int n = run_script();
    dump("  sleep script (backlight was at raw 2191)", n);
    check(n == 52, "script lasts 52 ticks (0.52 s): %d", n);
    check(tr[0][4] == 2311 && tr[0][1] == 3920, "first step: backlight 120 per tick, LEDs 80 per tick");
    check(first_at(4, 8191, 0) == 49 && first_at(1, 0, 0) == 49, "everything dark at t=490 ms");
    check(f_duty[4] == 8191 && f_duty[0] == 0 && f_duty[1] == 0 && f_duty[2] == 0, "end: backlight raw 8191, LEDs 0");
    check(s_wifi_enable == 0, "Wi-Fi light suspended after the sleep script");

    printf("== script 3 from the static backlight (duty 0)\n");
    awake_state();
    oem_led_set(1, ST_OFF, 3);
    n = run_script();
    dump("  sleep script (backlight was at static full)", n);
    check(tr[17][4] == 0 && tr[18][4] == 8191 - 5911, "writes above 6000 are dropped: first visible step at t=180 (%u)", tr[18][4]);
    check(f_duty[4] == 8191 - 2071, "fade not finished at script end: level 2071 left (raw %u)", (unsigned)f_duty[4]);
    oem_led_set(4, ST_OFF, 4);
    check(f_duty[4] == 8191, "the sleep code's backlight OFF then gives 8191");
}

static void t_charger(void)
{
    printf("== script 2 (put on the charger): abort, (0,OFF) (1,OFF) (4,OFF), (2,BREATHE,2)\n");
    awake_state();
    g_oem.power_state = OEM_PWR_CHARGING;
    oem_led_abort_script();
    oem_led_set(0, ST_OFF, 4); oem_led_set(1, ST_OFF, 4); oem_led_set(4, ST_OFF, 4);
    oem_led_set(2, ST_BREATHE, 2);
    int n = run_script();
    dump("  charger script", n);
    check(n == 262, "script lasts 262 ticks (2.62 s): %d", n);
    check(tr[9][2] == 0 && tr[10][2] == 133 && first_at(2, 4000, 0) == 40, "LED3 in 100..400 ms");
    check(first_at(1, 4000, 0) == 70 && first_at(0, 4000, 0) == 100, "LED2 in 400..700, LED1 in 700..1000 ms");
    check(tr[99][4] == 8191 && tr[100][4] == 8190 && first_at(4, 2191, 0) == 150, "backlight in from t=1000, raw 2191 at t=1500");
    check(first_at(2, 0, 170) == 200 && first_at(1, 0, 200) == 230 && first_at(0, 0, 230) == 260, "LED3, LED2, LED1 out by 2000 / 2300 / 2600 ms");
    check(f_duty[4] == 2191, "backlight ends lit at raw 2191");

    printf("== breathe on LED3 (charging), right after the script\n");
    tr_n = 0; tick_rec(415 * 2 + 10);
    dump("  breathe LED3 (first 4.3 s)", 430);
    int p4000 = 0, p0 = 0;
    for (int k = 415; k < 830; k++) { p4000 += tr[k][2] == 4000; p0 += tr[k][2] == 0; }
    bool periodic = true;
    for (int k = 0; k < 415; k++) periodic = periodic && tr[k][2] == tr[k + 415][2];
    check(tr[0][2] == 50 && first_at(2, 4000, 0) == 79, "restarts from 0, rises 50 per tick, 4000 after 0.80 s");
    check(periodic, "period 415 ticks (4.15 s)");
    check(p4000 == 243 && p0 == 14, "per period: %d ticks at 4000 (2.43 s), %d ticks dark (0.14 s)", p4000, p0);
    int mx = 0; for (int k = 0; k < tr_n; k++) if (tr[k][2] > mx) mx = tr[k][2];
    check(mx == 4000, "never above 4000 (levels over the limit are dropped)");

    printf("== battery full: (2, ON, 4); then idle-on-charger re-init\n");
    oem_led_set(2, ST_ON, 4);
    check(f_duty[2] == 4000, "LED3 steady 4000");
    g_oem.power_state = OEM_PWR_FULL;
    oem_led_park();
    check(f_hold[17] && f_hold[18] && f_hold[19] && f_hold[20] && f_hold[21] && f_level[19] == 0 &&
          f_level[17] == 1 && f_level[21] == 1 && f_mode[21] == GPIO_MODE_OUTPUT_OD, "park: 17..21 held at off, 21 open drain");
    oem_led_reinit_charge_light();
    check(!f_hold[19] && f_hold[17] && f_hold[21], "re-init releases GPIO19 only");
    check(f_duty[2] == 4000, "full: charge light written again after led_init (4000)");
    g_oem.power_state = OEM_PWR_CHARGING;
    oem_led_set(2, ST_BREATHE, 4);
    oem_led_park();
    oem_led_reinit_charge_light();
    tr_n = 0; tick_rec(100);
    int top = 0; for (int k = 0; k < tr_n; k++) if (tr[k][2] > top) top = tr[k][2];
    check(top > 0, "charging: charge light keeps breathing after the re-init (up to %d in 1 s)", top);
}

static void t_chase(void)
{
    for (int connected = 0; connected <= 1; connected++) {
        printf("== script 1 (daily goal), Wi-Fi %s: oem_led_set(1, ON, 1)\n", connected ? "connected" : "not connected");
        awake_state();
        g_oem.wifi_status = connected ? 1 : 2;
        oem_led_set(1, ST_ON, 1);
        int n = run_script();
        dump("  chase script", n);
        check(n == 232, "script lasts 232 ticks: %d", n);
        check(first_at(0, 4000, 0) == 30 && tr[60][1] == 4000 && tr[100][2] == 4000, "LED1 0..300, LED2 300..600, LED3 600..1000 ms");
        check(tr[99][0] == 0 && tr[130][0] == 4000 && tr[160][1] == 4000 && tr[200][2] == 4000, "second round 1000..2000 ms");
        check(first_at(2, 0, 220) == 229, "LED3 out 2200..2300 ms");
        if (connected) check(f_duty[0] == 4000 && f_duty[1] == 4000, "end: LED1 stays lit, LED2 on");
        else           check(f_duty[0] == 0 && f_duty[1] == 4000, "end: LED1 off, LED2 on");
    }
    // The script end zeroes the Wi-Fi light counter: with Wi-Fi provisioned and
    // connected the ramp starts over from the bottom (stock quirk).
    g_oem.sys[0x0c] = 2;
    tr_n = 0; tick_rec(120);
    check(tr[0][0] == 4000 && tr[1][0] == 80 && first_at(0, 4000, 1) == 99,
          "after the chase, connected and provisioned: LED1 restarts its ramp (4000, 80, ... 4000 after 1 s)");
}

static void t_breathe4_blink(void)
{
    printf("== breathe on LED4 (limit 8191)\n");
    reset_engine();
    s_breath = 0;
    oem_led_set(3, ST_BREATHE, 4);
    tr_n = 0; tick_rec(830);
    dump("  breathe LED4 (first 4.3 s)", 430);
    int full = 0, dark = 0;
    for (int k = 415; k < 830; k++) { full += tr[k][3] == 0; dark += tr[k][3] == 8191; }
    check(tr[0][3] == 8191 - 50 && tr[162][3] == 8191 - 8150 && tr[163][3] == 0, "level 0 -> 8150 in 1.63 s, then full");
    check(full == 75 && dark == 14, "per period: %d ticks full (0.75 s), %d ticks dark", full, dark);

    printf("== blink on LED4 (over-pressure) and on LED1..3\n");
    reset_engine();
    oem_led_all(ST_BLINK);
    tr_n = 0; tick_rec(1024);
    int last = -1, n33 = 0, n25 = 0, other = 0;
    for (int k = 1; k < tr_n; k++) {
        if (tr[k][3] == tr[k - 1][3]) continue;
        if (last >= 0) { int d = k - last; if (d == 33) n33++; else if (d == 25) n25++; else other++; }
        last = k;
    }
    printf("  LED4 toggles: %d intervals of 330 ms, %d of 250 ms, %d other\n", n33, n25, other);
    check(other == 0 && n33 >= 6 * n25 && n25 >= 3, "LED4 toggles every 330 ms (one in eight is 250 ms)");
    bool only = true;
    for (int k = 0; k < tr_n; k++) only = only && (tr[k][3] == 0 || tr[k][3] == 8191);
    check(only, "LED4 is either fully on (0) or off (8191)");
    bool off = true;
    for (int k = 0; k < tr_n; k++) off = off && tr[k][0] == 0 && tr[k][1] == 0 && tr[k][2] == 0;
    check(off, "blink on LED1..3 stays dark (8191 is over their limit) — stock quirk");
}

static void t_wifi(void)
{
    printf("== Wi-Fi light (LED1): provisioned, not connected -> triangle\n");
    reset_engine();
    g_oem.sys[0x0c] = 2; g_oem.wifi_status = 0;
    tr_n = 0; tick_rec(600);
    dump("  Wi-Fi light, searching (first 2.2 s)", 220);
    bool periodic = true;
    for (int k = 0; k < 400; k++) periodic = periodic && tr[k][0] == tr[k + 200][0];
    check(periodic && first_at(0, 4000, 0) == 99 && tr[1][0] == 80, "0 <-> 4000, 40 per tick, period 200 ticks (2 s)");
    printf("== Wi-Fi light: connected -> ramps up and holds; down -> ramps down and holds\n");
    g_oem.wifi_status = 1;
    tr_n = 0; tick_rec(400);
    check(tr[399][0] == 4000 && tr[300][0] == 4000, "connected: holds 4000");
    g_oem.wifi_status = 2;
    tr_n = 0; tick_rec(400);
    check(first_at(0, 0, 0) >= 0 && first_at(0, 0, 0) <= 101 && tr[399][0] == 0, "down: off within 1 s (%d ms), stays off", first_at(0, 0, 0) * 10);
    g_oem.wifi_status = 1; g_oem.power_state = OEM_PWR_CHARGING;
    tr_n = 0; tick_rec(400);
    check(tr[399][0] == 0, "on the charger: off even when connected");
}

static void t_rules(void)
{
    printf("== requests during a script are dropped; abort\n");
    awake_state();
    oem_led_set(1, ST_OFF, 4);
    oem_led_set(1, ST_ON, 0);
    tr_n = 0; tick_rec(30);
    oem_led_set(3, ST_ON, 4);
    check(f_duty[3] == 8191, "(3, ON, 4) during the wake script is lost");
    oem_led_abort_script();
    oem_led_set(3, ST_ON, 4);
    check(f_duty[3] == 0, "after oem_led_abort_script() it takes effect");
    tr_n = 0; tick_rec(300);
    check(f_duty[1] == 0, "the aborted script's pending state (LED2 ON) is not applied");

    printf("== battery 0 %% off the charger forces the indicators dark\n");
    awake_state();
    oem_led_set(3, ST_ON, 4); oem_led_set(2, ST_ON, 4);
    g_oem.batt_pct = 0;
    tr_n = 0; tick_rec(1);
    check(f_duty[0] == 0 && f_duty[1] == 0 && f_duty[2] == 0 && f_duty[3] == 8191 && f_duty[4] == 0,
          "one tick: LED1..4 dark, backlight untouched");
    oem_led_set(1, ST_OFF, 4);
    oem_led_set(1, ST_ON, 0);
    run_script();
    tick_rec(2);
    check(f_duty[1] == 0, "wake script: the pending ON is not applied at the end");
    g_oem.power_state = OEM_PWR_CHARGING;
    oem_led_set(2, ST_ON, 4);
    tr_n = 0; tick_rec(5);
    check(f_duty[2] == 4000, "on the charger at 0 %%: LEDs work");

    printf("== LEDC is written only when a duty changes\n");
    awake_state();
    tr_n = 0; tick_rec(10);
    f_updates = f_redundant = 0;
    tr_n = 0; tick_rec(1000);
    check(f_updates == 0, "10 s steady, nothing animating: %d LEDC updates", f_updates);
    oem_led_set(2, ST_BREATHE, 4);
    f_updates = f_redundant = 0;
    tr_n = 0; tick_rec(415);
    check(f_redundant == 0 && f_updates <= 162, "one breathe period: %d updates, %d redundant", f_updates, f_redundant);
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    t_init();
    t_wake();
    t_sleep();
    t_charger();
    t_chase();
    t_breathe4_blink();
    t_wifi();
    t_rules();
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

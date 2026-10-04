// Host check of the two input drivers that can be run without the chips:
// main/hw_bus.c (bit-bang I2C) against a pin-level model of an I2C slave, and
// main/hw_button.c against a virtual clock.
//   cc -std=gnu11 -Wall -Wextra -I re/tools/uisim/fake_idf_input -I main re/tools/uisim/sim_input_hw.c
//      main/hw_bus.c main/hw_button.c -o sim_input_hw && ./sim_input_hw
//
// Bus: every gpio call of the driver moves the modelled SCL / SDA lines (open drain:
// a line is low when the master or the slave pulls it). A monitor decodes START,
// STOP, bytes and ACK bits from the lines; the slave acknowledges its address, takes
// written bytes and returns bytes on a read. Reading a pin that the driver has
// switched to output returns 0, as on the chip (the input stage is off there).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hardware.h"
#include "hw_bus.h"
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_state.h"

oem_state_t g_oem;
static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

bool hw_emulated(void) { return false; }

// ---- pins ---------------------------------------------------------------------------
static struct { int mode, latch; } g_pin[49];
static int g_btn_level = 1;
static gpio_isr_t g_isr[49];
static void *g_isr_arg[49];
static unsigned g_delay_us;

// ---- I2C slave + monitor --------------------------------------------------------------
static int  sl_addr = 0x44;        // 7-bit address the slave answers to (-1: nobody home)
static int  sl_sda = 1, sl_scl = 1;  // slave's drive (0 = pulling low)
static int  sl_stretch;            // polls the slave keeps SCL low after each master release
static int  sl_stretch_left;
static uint8_t sl_tx[32]; static int sl_tx_n, sl_tx_i;
static uint8_t sl_rx[64]; static int sl_rx_n;
static char mon[512];              // decoded traffic: S, P, bytes, + ack, - nack
static int  mon_scl_pulses, mon_starts;
static int  last_scl = 1, last_sda = 1;
static int  bitn, shift, addressed, reading, in_frame, first;

static int m_drive(int pin) { return g_pin[pin].mode == GPIO_MODE_OUTPUT ? g_pin[pin].latch : 1; }
static int line_scl(void) { return m_drive(HW_PIN_BUS_SCL) & sl_scl; }
static int line_sda(void) { return m_drive(HW_PIN_BUS_SDA) & sl_sda; }
static void mon_add(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    size_t n = strlen(mon);
    vsnprintf(mon + n, sizeof mon - n, fmt, ap);
    va_end(ap);
}

static void bus_update(void)
{
    for (;;) {
        int scl = line_scl(), sda = line_sda();
        if (scl == last_scl && sda == last_sda) return;
        if (last_scl && scl && last_sda && !sda) {               // START
            mon_add("S ");
            mon_starts++;
            in_frame = 1; first = 1; bitn = 0; shift = 0; addressed = 0; reading = 0;
            sl_sda = 1;
        } else if (last_scl && scl && !last_sda && sda) {        // STOP
            mon_add("P ");
            in_frame = 0; addressed = 0; sl_sda = 1;
        } else if (!last_scl && scl) {                           // rising edge: sample
            mon_scl_pulses++;
            if (in_frame) {
                if (bitn < 8) {
                    shift = (shift << 1) | sda;
                    bitn++;
                    if (bitn == 8) mon_add("%02X", shift);
                } else {
                    mon_add(sda ? "- " : "+ ");
                    if (addressed && reading && sda) addressed = 0;   // master NACK ends the read
                }
            }
        } else if (last_scl && !scl) {                           // falling edge: slave sets up SDA
            if (in_frame) {
                if (bitn == 8) {                                 // the ACK slot follows
                    int ack = 0;
                    sl_sda = 1;
                    if (first) {                                 // address byte
                        first = 0;
                        addressed = sl_addr >= 0 && (shift >> 1) == sl_addr;
                        reading = shift & 1;
                        sl_tx_i = 0;
                        ack = addressed;
                    } else if (addressed && !reading) {
                        if (sl_rx_n < (int)sizeof sl_rx) sl_rx[sl_rx_n++] = (uint8_t)shift;
                        ack = 1;
                    }
                    if (ack) sl_sda = 0;
                    bitn = 9;
                } else {
                    if (bitn == 9) { bitn = 0; shift = 0; }
                    if (addressed && reading) {
                        uint8_t b = sl_tx_i < sl_tx_n ? sl_tx[sl_tx_i] : 0xFF;
                        sl_sda = (b >> (7 - bitn)) & 1;
                        if (bitn == 7) sl_tx_i++;
                    } else {
                        sl_sda = 1;
                    }
                }
            }
            if (sl_stretch) { sl_scl = 0; sl_stretch_left = sl_stretch; }
        }
        last_scl = scl; last_sda = sda;
    }
}

static void sl_reset(int addr)
{
    sl_addr = addr; sl_sda = sl_scl = 1; sl_stretch = sl_stretch_left = 0;
    sl_tx_n = sl_tx_i = sl_rx_n = 0; mon[0] = 0; mon_scl_pulses = mon_starts = 0; g_delay_us = 0;
}

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    for (int p = 0; p < 49; p++) if (cfg->pin_bit_mask >> p & 1) g_pin[p].mode = cfg->mode;
    bus_update();
    return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) { g_pin[pin].mode = mode; bus_update(); return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) { g_pin[pin].latch = (int)level; bus_update(); return ESP_OK; }
int gpio_get_level(gpio_num_t pin)
{
    if (pin == HW_PIN_BUTTON) return g_btn_level;
    if (g_pin[pin].mode != GPIO_MODE_INPUT) return 0;
    if (pin == HW_PIN_BUS_SCL) {
        if (!sl_scl && sl_stretch_left && --sl_stretch_left == 0) { sl_scl = 1; bus_update(); }
        return line_scl();
    }
    if (pin == HW_PIN_BUS_SDA) return line_sda();
    return 1;
}
esp_err_t gpio_hold_dis(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t fn, void *arg) { g_isr[pin] = fn; g_isr_arg[pin] = arg; return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t pin) { g_isr[pin] = NULL; return ESP_OK; }
void esp_rom_delay_us(uint32_t us) { g_delay_us += us; }

static void test_bus(void)
{
    printf("bit-bang bus\n");
    uint8_t buf[16];

    hw_bus_init();
    CHECK(g_pin[HW_PIN_BUS_SCL].mode == GPIO_MODE_OUTPUT && g_pin[HW_PIN_BUS_SCL].latch == 1 &&
          g_pin[HW_PIN_BUS_SDA].mode == GPIO_MODE_OUTPUT && g_pin[HW_PIN_BUS_SDA].latch == 1, "init: both lines driven high");

    sl_reset(0x44);
    bool ok = hw_bus_write8(0x44, 0xD0, (const uint8_t[]){ 0x05 }, 1);
    CHECK(ok && !strcmp(mon, "S 88+ D0+ 05+ P "), "write8: '%s' ok=%d", mon, ok);
    CHECK(sl_rx_n == 2 && sl_rx[0] == 0xD0 && sl_rx[1] == 0x05, "write8: slave received D0 05");
    CHECK(g_pin[HW_PIN_BUS_SCL].mode == GPIO_MODE_INPUT && g_pin[HW_PIN_BUS_SDA].mode == GPIO_MODE_INPUT &&
          line_scl() && line_sda(), "after STOP both lines are released and high");
    CHECK(mon_scl_pulses == 28, "write8: 3 x 9 clock pulses + the STOP's rising edge (%d)", mon_scl_pulses);
    CHECK(g_delay_us >= 2u * 2 * 27 && g_delay_us < 400, "hold delays: %u us for 27 clocks", g_delay_us);

    sl_reset(0x44);
    ok = hw_bus_write16(0x44, 0xA0, 0x00, (const uint8_t[]){ 0x33, 0x5D, 0x45, 0x64, 0xE4, 0x2F, 0xEA, 0x69 }, 8);
    CHECK(ok && !strcmp(mon, "S 88+ A0+ 00+ 33+ 5D+ 45+ 64+ E4+ 2F+ EA+ 69+ P "), "write16: '%s'", mon);

    sl_reset(0x6A);
    sl_tx[0] = 0x61; sl_tx_n = 1;
    ok = hw_bus_read(0x6A, 0x00, buf, 1);
    CHECK(ok && buf[0] == 0x61 && !strcmp(mon, "S D4+ 00+ S D5+ 61- P "), "read 1: '%s' value %02x", mon, buf[0]);

    sl_reset(0x44);
    for (int i = 0; i < 12; i++) sl_tx[i] = (uint8_t)(0x10 + 17 * i);
    sl_tx_n = 12;
    memset(buf, 0, sizeof buf);
    ok = hw_bus_read(0x44, 0x10, buf, 12);
    CHECK(ok && !memcmp(buf, sl_tx, 12), "read 12: data");
    CHECK(!strcmp(mon, "S 88+ 10+ S 89+ 10+ 21+ 32+ 43+ 54+ 65+ 76+ 87+ 98+ A9+ BA+ CB- P "), "read 12: '%s'", mon);

    // a slave that stretches the clock after every falling edge
    sl_reset(0x44); sl_stretch = 600;
    ok = hw_bus_write8(0x44, 0xFF, (const uint8_t[]){ 0x00 }, 1);
    CHECK(ok && !strcmp(mon, "S 88+ FF+ 00+ P "), "clock stretch 600 polls: '%s'", mon);
    sl_reset(0x6A); sl_stretch = 900; sl_tx[0] = 0xA5; sl_tx[1] = 0x3C; sl_tx_n = 2;
    ok = hw_bus_read(0x6A, 0xB0, buf, 2);
    CHECK(ok && buf[0] == 0xA5 && buf[1] == 0x3C, "read with clock stretch: %02x %02x", buf[0], buf[1]);

    // nobody answers: four address attempts, the 4th without an ACK clock, then the
    // rest of the transfer is clocked out anyway and ends with STOP (stock behaviour)
    sl_reset(-1);
    ok = hw_bus_write8(0x44, 0xFF, (const uint8_t[]){ 0x00 }, 1);
    CHECK(!ok && mon_starts == 4, "absent slave: %d START(s), ok=%d", mon_starts, ok);
    // SCL rising edges: 4 address bytes, 3 ACK clocks, 3 repeated STARTs, register + data byte, STOP
    CHECK(mon_scl_pulses == 4 * 8 + 3 + 3 + 2 * 9 + 1, "absent slave: %d SCL rising edges (want 57)", mon_scl_pulses);
    CHECK(!strncmp(mon, "S 88- S 88- S 88- S 88", 22) && !strcmp(mon + strlen(mon) - 2, "P "), "absent slave: '%s'", mon);
    CHECK(line_scl() && line_sda(), "absent slave: bus released at the end");
    sl_reset(-1);
    memset(buf, 0, sizeof buf);
    ok = hw_bus_read(0x6A, 0x00, buf, 1);
    CHECK(!ok && buf[0] == 0xFF, "absent slave: read returns 0xFF and false (%02x)", buf[0]);

    // back-to-back transfers
    sl_reset(0x44);
    hw_bus_write8(0x44, 0xDB, (const uint8_t[]){ 0x0D }, 1);
    sl_rx_n = 0;
    hw_bus_write8(0x44, 0xFF, (const uint8_t[]){ 0x00 }, 1);
    CHECK(!strcmp(mon, "S 88+ DB+ 0D+ P S 88+ FF+ 00+ P "), "two transfers: '%s'", mon);
}

// ---- virtual esp_timer ----------------------------------------------------------------
struct sim_timer { esp_timer_cb_t cb; void *arg; int64_t due; int armed; };
static struct sim_timer g_timers[8];
static int g_ntimers;
static int64_t g_now_us;
static bool g_timer_task_stuck;

esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out)
{
    struct sim_timer *t = &g_timers[g_ntimers++];
    t->cb = a->callback; t->arg = a->arg; t->armed = 0;
    *out = t;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    if (t->armed) return ESP_ERR_INVALID_STATE;
    t->armed = 1; t->due = g_now_us + (int64_t)us;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if (!t->armed) return ESP_ERR_INVALID_STATE;
    t->armed = 0;
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return g_now_us; }

static void advance_us(int64_t us)
{
    int64_t end = g_now_us + us;
    for (;;) {
        struct sim_timer *next = NULL;
        for (int i = 0; i < g_ntimers; i++)
            if (g_timers[i].armed && g_timers[i].due <= end && (!next || g_timers[i].due < next->due)) next = &g_timers[i];
        if (!next || g_timer_task_stuck) break;
        g_now_us = next->due;
        next->armed = 0;
        next->cb(next->arg);
    }
    g_now_us = end;
}
static void advance_ms(int ms) { advance_us((int64_t)ms * 1000); }

// ---- what hw_button.c calls -------------------------------------------------------------
static uint8_t g_codes[16]; static int g_ncodes;
static uint32_t g_ev, g_uiev; static int g_ev_posts;
static int g_aborts;
void oem_button_push(uint8_t code) { if (g_ncodes < 16) g_codes[g_ncodes] = code; g_ncodes++; }
void hal_event_post(uint32_t bits) { g_ev |= bits; g_ev_posts++; }
void hal_ui_event_post(uint32_t bits) { g_uiev |= bits; }
void esp_system_abort(const char *details) { (void)details; g_aborts++; }
static esp_reset_reason_t g_reset = ESP_RST_DEEPSLEEP;
esp_reset_reason_t esp_reset_reason(void) { return g_reset; }

static void btn(int level)
{
    g_btn_level = level;
    if (g_isr[HW_PIN_BUTTON]) g_isr[HW_PIN_BUTTON](g_isr_arg[HW_PIN_BUTTON]);
}
static void btn_reset(void)
{
    if (!g_btn_level) btn(1);
    advance_ms(10000);
    g_ncodes = 0; g_ev = g_uiev = 0; g_ev_posts = 0; g_aborts = 0;
}
// press for `ms`, release; returns the number of codes delivered, first one in *code
static int press(int ms, int *code)
{
    btn_reset();
    btn(0); advance_ms(ms); btn(1); advance_ms(50);
    *code = g_ncodes ? g_codes[0] : -1;
    return g_ncodes;
}

static void test_button(void)
{
    printf("button\n");
    int code;
    // The driver comes up 0.7 s into a boot whose waking press is still held (not stock:
    // only the 8 s factory-reset hold counts such a press, from the boot).
    g_oem.init_ok = 0;
    g_now_us = 700000;
    g_btn_level = 0;
    oem_button_init();
    CHECK(g_isr[HW_PIN_BUTTON] != NULL && g_ntimers == 5, "ISR installed, 5 timers created (%d)", g_ntimers);
    advance_ms(500); g_oem.init_ok = 5;                    // the brush logic is up at 1.2 s
    advance_ms(6700);                                      // 7.9 s since the boot
    CHECK(g_ncodes == 0 && g_uiev == 0, "held from the boot: nothing before 8 s, no 2 / 3 / 5 s codes (%d)", g_ncodes);
    advance_ms(200);                                       // 8.1 s
    CHECK(g_ncodes == 1 && g_codes[0] == 4 && g_uiev == OEM_UIEV_FACTORY, "held from the boot for 8 s: factory reset (%d)", g_ncodes);
    btn(1); advance_ms(100);
    CHECK(g_ncodes == 1, "its release adds nothing (%d)", g_ncodes);
    g_oem.init_ok = 0;
    btn_reset();

    btn(0); advance_ms(300); btn(1); advance_ms(100);
    CHECK(g_ncodes == 0 && g_ev == 0, "edges before init_ok are ignored");
    g_oem.init_ok = 5;

    CHECK(press(300, &code) == 1 && code == 0 && g_ev == OEM_EV_BUTTON, "300 ms: short press (n %d, code %d)", g_ncodes, code);
    CHECK(press(79, &code) == 0, "79 ms: nothing");
    CHECK(press(80, &code) == 1 && code == 0, "80 ms: short press");
    CHECK(press(1499, &code) == 1 && code == 0, "1499 ms: short press");
    CHECK(press(1500, &code) == 0, "1500 ms: nothing");
    CHECK(press(1999, &code) == 0, "1999 ms: nothing");
    CHECK(press(2000, &code) == 1 && code == 1, "2000 ms: code 1 (n %d code %d)", g_ncodes, code);
    CHECK(press(2900, &code) == 1 && code == 1, "2.9 s: code 1 only, no short press at release");
    CHECK(press(3500, &code) == 2 && g_codes[0] == 1 && g_codes[1] == 2, "3.5 s: codes 1, 2");
    CHECK(press(5500, &code) == 3 && g_codes[2] == 3 && g_uiev == 0, "5.5 s: codes 1, 2, 3");
    CHECK(press(9000, &code) == 4 && g_codes[0] == 1 && g_codes[1] == 2 && g_codes[2] == 3 && g_codes[3] == 4,
          "9 s: codes 1, 2, 3, 4 (%d)", g_ncodes);
    CHECK(g_uiev == OEM_UIEV_FACTORY, "8 s hold raises the factory-reset UI event");
    CHECK(g_ev_posts == 4, "one OEM_EV_BUTTON per code (%d)", g_ev_posts);

    // bounce while down is ignored; the hold timers restart with every press
    btn_reset(); btn(0); advance_ms(40); btn(0); advance_ms(40); btn(0); advance_ms(40); btn(1); advance_ms(50);
    CHECK(g_ncodes == 1 && g_codes[0] == 0, "repeated falling edges while down: one short press (%d)", g_ncodes);
    btn_reset(); btn(0); advance_ms(1900); btn(1); advance_ms(20); btn(0); advance_ms(1900); btn(1); advance_ms(50);
    CHECK(g_ncodes == 0, "two holds of 1.9 s do not add up to a 2 s hold (%d codes)", g_ncodes);
    // released just before a timer fires: the callback sees the button up
    btn_reset(); btn(0); advance_ms(1700); g_btn_level = 1; advance_ms(400); btn(1); advance_ms(50);
    CHECK(g_ncodes == 0, "timer callback with the button already up: nothing (%d)", g_ncodes);

    // "sys_abnormal": a new press while the 100 us timer of the previous one has not run
    btn_reset(); btn(0); advance_ms(100); btn(1); advance_ms(100); btn(0); advance_ms(100); btn(1);
    CHECK(g_aborts == 0, "normal presses: no restart");
    btn_reset(); g_timer_task_stuck = true;
    btn(0); advance_ms(100); btn(1); advance_ms(100); btn(0);
    CHECK(g_aborts == 1, "second press with the timer task stuck: restart requested (%d)", g_aborts);
    g_timer_task_stuck = false;
}

int main(void)
{
    test_bus();
    test_button();
    if (g_fail) printf("\n%d of %d checks FAILED\n", g_fail, g_checks);
    else        printf("\nall %d checks passed\n", g_checks);
    return g_fail != 0;
}

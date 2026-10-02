// Host harness for main/hw_touch.c and main/hw_pressure.c: the two drivers run
// against a fake bus that logs every transfer, so that the byte sequences can be
// compared with what the stock code sends.
//   cc -std=gnu11 -Wall -Wextra -ffp-contract=off -I re/tools/uisim/fake_idf_input -I main
//      re/tools/uisim/sim_input_drv.c main/hw_touch.c main/hw_pressure.c main/oem_force.c -o sim_input_drv
//   python3 re/tools/uisim/check_input_drv.py
// The checker executes the stock driver functions in the Xtensa interpreter
// (re/tools/xt_emu.py) with the same three bus calls hooked and compares the logs.
//
// Commands on stdin, one per line:
//   rdy N            level of the RDY pin (GPIO12)
//   rx HEX           data for the next bus read (queued; a read without data gets 0s)
//   nak N            the next N bus reads are not acknowledged
//   tinit | irq N | tset N | step | tick
//   nvs KEY HEX      NVS blob;  temp C | notemp   IMU temperature
//   pinit | pstart | pstop | psample
//   touching N       what oem_gesture_touching() returns;  session A STOPDELAY
// Log on stdout:
//   W8 addr reg data.. | W16 addr reghi reglo data.. | R addr reg n | D us | G x y | T start ms / T stop
//   and after each command a result line starting with "="
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "hardware.h"
#include "hw_bus.h"
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_input.h"
#include "oem_state.h"

oem_state_t g_oem;
bool hw_emulated(void) { return false; }

static int g_rdy = 1, g_touching, g_nak;
static struct { uint8_t d[32]; int n; } g_rx[64];
static int g_rx_head, g_rx_tail;
static struct { char key[24]; uint8_t d[32]; int n; } g_nvs[4];
static int g_nnvs;
static bool g_have_temp; static float g_temp;

// ---- fake bus -------------------------------------------------------------------------
void hw_bus_init(void) { printf("BUSINIT\n"); }
bool hw_bus_write8(uint8_t addr7, uint8_t reg, const uint8_t *data, uint8_t n)
{
    printf("W8 %02X %02X", addr7, reg);
    for (int i = 0; i < n; i++) printf(" %02X", data[i]);
    printf("\n");
    return true;
}
bool hw_bus_write16(uint8_t addr7, uint8_t reg_hi, uint8_t reg_lo, const uint8_t *data, uint8_t n)
{
    printf("W16 %02X %02X %02X", addr7, reg_hi, reg_lo);
    for (int i = 0; i < n; i++) printf(" %02X", data[i]);
    printf("\n");
    return true;
}
bool hw_bus_read(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t n)
{
    printf("R %02X %02X %d\n", addr7, reg, n);
    if (g_nak) { g_nak--; memset(buf, 0xFF, n); return false; }
    memset(buf, 0, n);
    if (g_rx_head != g_rx_tail) {
        memcpy(buf, g_rx[g_rx_head].d, (size_t)(n < g_rx[g_rx_head].n ? n : g_rx[g_rx_head].n));
        g_rx_head = (g_rx_head + 1) % 64;
    }
    return true;
}

// ---- fake pins, HAL, other modules ------------------------------------------------------
esp_err_t gpio_config(const gpio_config_t *cfg)
{
    printf("GPIO config mask %llx mode %d pullup %d intr %d\n", (unsigned long long)cfg->pin_bit_mask, cfg->mode,
           cfg->pull_up_en, cfg->intr_type);
    return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) { (void)pin; (void)mode; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) { printf("GPIO %d level %u\n", pin, (unsigned)level); return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { return pin == HW_PIN_TOUCH_RDY ? g_rdy : 1; }
esp_err_t gpio_hold_dis(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t fn, void *arg) { (void)fn; (void)arg; printf("ISR add %d\n", pin); return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t pin) { printf("ISR remove %d\n", pin); return ESP_OK; }
void esp_rom_delay_us(uint32_t us) { printf("D %u\n", (unsigned)us); }

void hal_event_post(uint32_t bits) { printf("EV %x\n", (unsigned)bits); }
void hal_timer_start(hal_timer_t t, uint32_t ms, bool periodic) { printf("T start %d %u %d\n", (int)t, (unsigned)ms, periodic); }
void hal_timer_stop(hal_timer_t t) { printf("T stop %d\n", (int)t); }
size_t hal_nvs_get(const char *key, void *buf, size_t len)
{
    for (int i = 0; i < g_nnvs; i++) {
        if (!strcmp(g_nvs[i].key, key)) {
            memcpy(buf, g_nvs[i].d, len < (size_t)g_nvs[i].n ? len : (size_t)g_nvs[i].n);
            return (size_t)g_nvs[i].n;
        }
    }
    return 0;
}
bool oem_imu_temp(float *c) { if (g_have_temp) *c = g_temp; return g_have_temp; }
void oem_gesture_sample(uint16_t x, uint16_t y) { printf("G %u %u\n", x, y); }
bool oem_gesture_touching(void) { return g_touching; }

static int unhex(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (s[0] && s[1] && n < max) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

int main(void)
{
    char line[256], key[24], hex[160];
    int a, b;
    float f;
    g_oem.init_ok = 5;
    g_oem.stop_delay = 200;
    while (fgets(line, sizeof line, stdin)) {
        if (sscanf(line, "rdy %d", &a) == 1) g_rdy = a;
        else if (sscanf(line, "rx %159s", hex) == 1) {
            g_rx[g_rx_tail].n = unhex(hex, g_rx[g_rx_tail].d, 32);
            g_rx_tail = (g_rx_tail + 1) % 64;
        }
        else if (sscanf(line, "nak %d", &a) == 1) g_nak = a;
        else if (!strncmp(line, "tinit", 5)) { oem_touch_init(); printf("= state %d\n", oem_touch_state()); }
        else if (sscanf(line, "irq %d", &a) == 1) { oem_touch_irq(a); printf("=\n"); }
        else if (sscanf(line, "tset %d", &a) == 1) { oem_touch_set_state((uint8_t)a); printf("= state %d\n", oem_touch_state()); }
        else if (!strncmp(line, "step", 4)) { oem_touch_step(); printf("= state %d\n", oem_touch_state()); }
        else if (!strncmp(line, "tick", 4)) { for (int i = 0; i < 100; i++) oem_touch_tick_10ms(); printf("= state %d\n", oem_touch_state()); }
        else if (sscanf(line, "nvs %23s %159s", key, hex) == 2) {
            strcpy(g_nvs[g_nnvs].key, key);
            g_nvs[g_nnvs].n = unhex(hex, g_nvs[g_nnvs].d, 32);
            g_nnvs++;
        }
        else if (sscanf(line, "temp %f", &f) == 1) { g_have_temp = true; g_temp = f; }
        else if (!strncmp(line, "notemp", 6)) g_have_temp = false;
        else if (!strncmp(line, "pinit", 5)) { oem_pressure_init(); printf("= avail %d coef %u\n", oem_pressure_available(), g_oem.force_coef); }
        else if (!strncmp(line, "pstart", 6)) { oem_pressure_start(); printf("=\n"); }
        else if (!strncmp(line, "pstop", 5)) { oem_pressure_stop(); printf("=\n"); }
        else if (!strncmp(line, "psample", 7)) { oem_pressure_sample(); printf("= pressure %d avail %d\n", g_oem.pressure, oem_pressure_available()); }
        else if (sscanf(line, "touching %d", &a) == 1) g_touching = a;
        else if (sscanf(line, "session %d %d", &a, &b) == 2) { g_oem.session_active = (uint8_t)a; g_oem.stop_delay = (uint8_t)b; }
    }
    return 0;
}

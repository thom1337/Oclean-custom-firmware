#include "hardware.h"
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_state.h"
#include "driver/gpio.h"
#include "esp_log.h"

// Charger pins (re/spec/led_battery_charge.md section 6.1).
//
//   GPIO9   charger present, active LOW. Input, any-edge interrupt.
//   GPIO2   "charger still alive" pulses: a falling edge restarts the un-plug debounce.
//   GPIO26  charge inhibit: input (high-Z) = charging allowed, driven HIGH = blocked.
//           Stock never drives it low, and neither does this file.
//   GPIO45  "WLC_EN": stock writes 0 in every code path and holds it; it is never 1.
//
// There is no charger-IC status input: "charging" and "full" are decided by the
// charge state machine (app module) and the gauge.

#define PIN_WLC HW_PIN_GPIO45

static const char *TAG = "hw_charge";
static bool s_init_done;
static volatile bool s_alive_edge;

// GPIO9 branch of the stock GPIO ISR (0x40377c98): a low level, once the main task
// is up and the gauge has its first measurement, asks the main task to run the
// "charger attached" action. (Stock also sets its wake flag 0x3fc9f301 here; that
// flag belongs to the boot gate of the power module.)
static void charger_isr(void *arg)
{
    (void)arg;
    if (g_oem.init_ok < 5) return;
    if (gpio_get_level(HW_PIN_CHARGER) != 0) return;
    if (g_oem.gauge_inited) hal_event_post(OEM_EV_CHARGER);
}

// GPIO2 branch: stock zeroes the un-plug debounce counter (0x3fc9f304) directly; here
// the edge is latched and the 10 ms charger poll collects it.
static void alive_isr(void *arg)
{
    (void)arg;
    if (g_oem.init_ok >= 5) s_alive_edge = true;
}

bool oem_charger_alive_take(void)
{
    if (!s_alive_edge) return false;
    s_alive_edge = false;
    return true;
}

static void wlc_low(void)
{
    gpio_hold_dis(PIN_WLC);
    gpio_set_level(PIN_WLC, 0);
    gpio_hold_en(PIN_WLC);
}

static void input_irq(int pin, gpio_int_type_t edge, gpio_isr_t isr)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = edge,
    };
    gpio_config(&c);
    esp_err_t e = gpio_isr_handler_add(pin, isr, NULL);
    if (e != ESP_OK) ESP_LOGE(TAG, "isr gpio%d: %d", pin, (int)e);
}

// The charger part of brush_gpio_cfg 0x4200d77c.
void oem_charge_pins_init(void)
{
    if (s_init_done) return;
    s_init_done = true;

    gpio_config_t wlc = {
        .pin_bit_mask = 1ULL << PIN_WLC,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&wlc);
    gpio_set_level(PIN_WLC, 0);
    gpio_hold_en(PIN_WLC);
    gpio_sleep_sel_en(PIN_WLC);

    oem_charge_allow(true);

    // Several drivers share the per-pin ISR service; whoever comes first installs it.
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGE(TAG, "isr service: %d", (int)e);
    input_irq(HW_PIN_CHARGER, GPIO_INTR_ANYEDGE, charger_isr);
    input_irq(HW_PIN_AUX_IN, GPIO_INTR_NEGEDGE, alive_isr);

    ESP_LOGI(TAG, "charger pins ready: charger %s, charging allowed",
             oem_charger_present() ? "present" : "absent");
}

bool oem_charger_present(void)
{
    if (hw_emulated()) return false;
    oem_charge_pins_init();
    return gpio_get_level(HW_PIN_CHARGER) == 0;
}

// set_CHARGE_EN_IO_level 0x4200d650. allow: the pin becomes an input (high-Z).
// block: the pin is driven high. The hold keeps the state through light sleep.
void oem_charge_allow(bool allow)
{
    static int8_t s_last = -1;
    oem_charge_pins_init();
    if (s_last != (int8_t)allow) {
        s_last = (int8_t)allow;
        ESP_LOGI(TAG, "charging %s", allow ? "allowed" : "blocked");
    }
    gpio_hold_dis(HW_PIN_CHARGE_BLOCK);
    if (allow) {
        gpio_set_direction(HW_PIN_CHARGE_BLOCK, GPIO_MODE_INPUT);
    } else {
        // Stock configures the output first and sets the level after, which drives
        // the pin low for a moment the first time (the output latch starts at 0).
        // Set the latch before enabling the driver so the pin only ever goes high.
        gpio_set_level(HW_PIN_CHARGE_BLOCK, 1);
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << HW_PIN_CHARGE_BLOCK,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&c);
        gpio_set_level(HW_PIN_CHARGE_BLOCK, 1);
    }
    gpio_hold_en(HW_PIN_CHARGE_BLOCK);
}

// set_WLC_EN_IO_level(0) 0x4200d6b0, called by the charge state machine on every
// attach and removal. (The non-zero form, charging inhibited by a phone-app command,
// is not ported.)
void oem_wlc_off(void)
{
    oem_charge_pins_init();
    wlc_low();
    oem_charge_allow(true);
}

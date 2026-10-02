#include "hardware.h"
#include "oem_hal.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

// Indicator LEDs and LCD backlight: the LED part of the stock "ocleanhal"
// (platform_devices.c), re/spec/led_battery_charge.md section 1.
//
//   HAL id  GPIO  LEDC ch  limit  polarity
//     1      17     0      4000   output_invert = 1   (lit while the pin is low)
//     2      18     1      4000   output_invert = 1
//     3      19     2      4000   plain               (lit while the pin is high)
//     4      20     3      8191   written as 8191 - level
//     5      21     4      6000   written as 8191 - level; backlight, active low
//
// LEDC timer 0, low speed mode, 5 kHz, 13 bit. Only oem_led.c calls this file (main
// task, core lock held). The duty of each channel is cached: the hardware is written
// only when a duty changes, and "get" returns the cache (stock reads the register
// back 10 ms after writing it, which gives the same value).

#define LED_MODE      LEDC_LOW_SPEED_MODE
#define LED_DUTY_MAX  8191u                 // 13 bit
#define LED_CHANNELS  5

static const char *TAG = "hw_led";

// led_configs[] (0x3c117394 and 0x3c11a864, identical), index = HAL id
const uint32_t hal_led_max[6] = { 0, 4000, 4000, 4000, 8191, 6000 };

static const uint8_t s_pin[LED_CHANNELS] = {
    HW_PIN_LED1, HW_PIN_LED2, HW_PIN_LED3, HW_PIN_LED4, HW_PIN_BACKLIGHT,
};
// Pin level that keeps each LED dark while LEDC does not drive the pin
// (0x4200d77c, 0x4200df88, 0x4200dbbc: 17 = 1, 18 = 1, 19 = 0, 20 = 1, 21 = 1).
static const uint8_t s_off_level[LED_CHANNELS] = { 1, 1, 0, 1, 1 };
// Duty after led_channel_init (0x4200d160): every LED dark.
static const uint16_t s_init_duty[LED_CHANNELS] = { 0, 0, 0, LED_DUTY_MAX, LED_DUTY_MAX };

static uint32_t s_duty[LED_CHANNELS];
static bool     s_ready;

static void duty_write(int ch, uint32_t duty)
{
    if (!s_ready || s_duty[ch] == duty) return;
    s_duty[ch] = duty;
    if (hw_emulated()) return;
    esp_err_t e = ledc_set_duty(LED_MODE, (ledc_channel_t)ch, duty);
    if (e == ESP_OK) e = ledc_update_duty(LED_MODE, (ledc_channel_t)ch);
    if (e != ESP_OK) ESP_LOGE(TAG, "ledc ch%d duty %u: %d", ch, (unsigned)duty, (int)e);
}

// led_init 0x4200d2a8 + led_channel_init 0x4200d160 (channels 1..5 in that order)
void hal_led_hw_init(void)
{
    for (int ch = 0; ch < LED_CHANNELS; ch++) s_duty[ch] = s_init_duty[ch];
    s_ready = true;
    if (hw_emulated()) return;

    ledc_timer_config_t timer = {              // rodata 0x3c116d50
        .speed_mode      = LED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t e = ledc_timer_config(&timer);
    if (e != ESP_OK) ESP_LOGE(TAG, "ledc_timer_config: %d", (int)e);

    for (int ch = 0; ch < LED_CHANNELS; ch++) {
        ledc_channel_config_t c = {
            .gpio_num   = s_pin[ch],
            .speed_mode = LED_MODE,
            .channel    = (ledc_channel_t)ch,
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = s_init_duty[ch],
            .hpoint     = 0,
            .flags.output_invert = (ch <= 1),  // 0x3c116d10 / 0x3c116d30: LED1 and LED2 only
        };
        e = ledc_channel_config(&c);
        if (e != ESP_OK) ESP_LOGE(TAG, "ledc_channel_config ch%d: %d", ch, (int)e);
    }
}

// set_led_light_level 0x4200d2f0: the "static" set used when a state is entered.
void hal_led_set(int hal_id, uint32_t level)
{
    level &= 0xffff;                           // stock takes a u16
    switch (hal_id) {
    case 1: case 2: case 3:
        duty_write(hal_id - 1, level < 4000 ? level : 4000);
        break;
    case 4:
        // Stock writes 8191 - level unchecked; no caller passes more than 8191.
        duty_write(3, LED_DUTY_MAX - (level < LED_DUTY_MAX ? level : LED_DUTY_MAX));
        break;
    case 5:
        // Backlight: 6000 and above = duty 0 (pin constantly low, fully lit).
        if (level > 6000) level = 6000;
        duty_write(4, level == 0 ? LED_DUTY_MAX : 6000 - level);
        break;
    default:
        break;
    }
}

// set_led_light_level_process 0x4200d49c: used by every animation. A level above
// the LED's limit is dropped, not clamped, and the backlight is 8191 - level here
// (not 6000 - level), so a faded-in backlight ends at duty 2191.
void hal_led_process(int hal_id, uint32_t level)
{
    level &= 0xffff;
    if (hal_id < 1 || hal_id > 5 || level > hal_led_max[hal_id]) return;
    if (hal_id <= 3) duty_write(hal_id - 1, level);
    else             duty_write(hal_id - 1, LED_DUTY_MAX - level);
}

// 0x4200d604
uint32_t hal_led_get(int hal_id)
{
    if (hal_id >= 1 && hal_id <= 3) return s_duty[hal_id - 1];
    if (hal_id == 4 || hal_id == 5) return LED_DUTY_MAX - s_duty[hal_id - 1];
    return 0;
}

static void pin_output(int ch, bool pull_up, bool pull_down)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << s_pin[ch],
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
}

// What stock does to the LED pins right before led_init at boot (brush_gpio_cfg
// 0x4200d77c) and on wake (0x4200df88): drop the sleep holds and drive GPIO17..20 to
// their "off" level as plain outputs. GPIO21 is only un-held; led_init then starts
// it at duty 8191 (off). The boot variant enables a pull-up on GPIO17 and a
// pull-down on GPIO18 (as stock; meaningless on a driven pin), wake uses no pulls.
void hal_led_pins_release(bool boot)
{
    for (int ch = 0; ch < LED_CHANNELS; ch++) gpio_hold_dis(s_pin[ch]);
    for (int ch = 0; ch < 4; ch++) {
        gpio_set_level(s_pin[ch], s_off_level[ch]);
        pin_output(ch, boot && ch == 0, boot && ch == 1);
    }
}

void hal_led_pin_release_charge(void)
{
    gpio_hold_dis(HW_PIN_LED3);
}

// LED part of the idle-sleep / deep-sleep parking (0x4200dbbc, 0x4200dda4).
// gpio_config() routes the pin back to the GPIO output register, which detaches the
// LEDC signal; the LEDC timer is left running, as in stock. The output latch of
// GPIO17..20 already holds the "off" level (hal_led_pins_release), so the pin does
// not glitch when the LEDC signal goes away.
void hal_led_pins_park(void)
{
    for (int ch = 0; ch < 4; ch++) gpio_hold_dis(s_pin[ch]);
    for (int ch = 0; ch < 4; ch++) {
        pin_output(ch, false, false);
        gpio_set_level(s_pin[ch], s_off_level[ch]);
        gpio_hold_en(s_pin[ch]);
    }
    gpio_config_t bl = {                       // backlight: open drain, released = off
        .pin_bit_mask = 1ULL << HW_PIN_BACKLIGHT,
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl);
    gpio_set_level(HW_PIN_BACKLIGHT, 1);
    gpio_hold_en(HW_PIN_BACKLIGHT);
    s_ready = false;                           // nothing reaches the pins until the next led_init
}

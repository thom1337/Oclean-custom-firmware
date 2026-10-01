#include "hardware.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

static const char *TAG = "hw_io";
static const int LED_GPIO[] = HW_LED_GPIOS;
#define NLED (sizeof(LED_GPIO)/sizeof(LED_GPIO[0]))

void hw_io_init(void)
{
    // Buttons / input lines (SAFE: inputs only).
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << HW_BTN_PRIMARY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,       // primary button is active-low w/ pull-up
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);
    gpio_config_t in2 = {
        .pin_bit_mask = (1ULL << HW_BTN_CHARGE_DET) | (1ULL << HW_BTN_GYRO_WAKE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in2);

    // Indicator LEDs via LEDC (SAFE: low-current LEDs on GPIO17-21).
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    for (int i = 0; i < (int)NLED; i++) {
        ledc_channel_config_t c = {
            .speed_mode = LEDC_LOW_SPEED_MODE, .channel = i, .timer_sel = LEDC_TIMER_0,
            .gpio_num = LED_GPIO[i], .duty = 0, .hpoint = 0,
            .flags.output_invert = (i == 0 || i == 1) ? 1 : 0,  // ch0/ch1 hw-inverted per stock
        };
        ledc_channel_config(&c);
    }
    ESP_LOGI(TAG, "IO ready: button GPIO%d (+%d,%d), %d LEDs", HW_BTN_PRIMARY, HW_BTN_CHARGE_DET, HW_BTN_GYRO_WAKE, (int)NLED);
}

bool hw_button_pressed(void)
{
    return gpio_get_level(HW_BTN_PRIMARY) == 0;   // active-low
}

bool hw_charger_present(void)
{
    return gpio_get_level(HW_BTN_CHARGE_DET) == 1;   // active-high (likely)
}

void hw_led_set(int idx, bool on)
{
    if (idx < 0 || idx >= (int)NLED) return;
    if (idx == 4) return;   // LEDC ch4 / GPIO21 is the LCD backlight — owned by hw_display, not an indicator
    ledc_set_duty(LEDC_LOW_SPEED_MODE, idx, on ? 8191 : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, idx);
}

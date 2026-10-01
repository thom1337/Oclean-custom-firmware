#include "hardware.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

// The brush motor is a voice-coil actuator driven over I2S through an external
// amp (stock FW streams PCM/MP3 into it — "music through the motor"). We drive a
// simple tone whose amplitude+frequency scale with the gear/intensity.
// Pins recovered as "likely": BCK=33, WS=47, DOUT=34. VERIFY at bring-up.
#define I2S_BCK   33
#define I2S_WS    47
#define I2S_DOUT  34
#define SAMPLE_RATE 16000

static const char *TAG = "hw_motor";
static i2s_chan_handle_t s_tx;
static volatile int s_gear;        // 0 = stop
static TaskHandle_t s_task;

// Motor amp enable (GPIO48, active-high). Mirrors stock set_motor_power: the I2S
// stream alone does nothing; the coil only moves while the amp is enabled.
static void motor_amp(bool on)
{
    gpio_hold_dis(HW_MOTOR_AMP_EN);
    gpio_set_level(HW_MOTOR_AMP_EN, on ? 1 : 0);
    gpio_hold_en(HW_MOTOR_AMP_EN);
}

// gear -> (fundamental Hz, amplitude 0..1). Tuned conservatively; the stock
// per-gear waveform tables were not fully recovered.
static void gear_params(int g, float *hz, float *amp)
{
    switch (g) {
        case 1: *hz = 240; *amp = 0.35f; break;
        case 2: *hz = 260; *amp = 0.50f; break;
        case 3: *hz = 280; *amp = 0.65f; break;
        case 4: *hz = 300; *amp = 0.80f; break;
        case 5: *hz = 320; *amp = 1.00f; break;
        default:*hz = 0;   *amp = 0;     break;
    }
}

static void motor_task(void *arg)
{
    static int16_t buf[256];
    float phase = 0;
    while (1) {
        int g = s_gear;
        if (g <= 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        float hz, amp; gear_params(g, &hz, &amp);
        float step = 2.0f * (float)M_PI * hz / SAMPLE_RATE;
        for (int i = 0; i < 256; i++) {
            float s = sinf(phase); phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            buf[i] = (int16_t)(s * amp * 32000.0f);
        }
        size_t wr;
        i2s_channel_write(s_tx, buf, sizeof(buf), &wr, 100);   // timeout is in ms, not ticks
    }
}

void hw_motor_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;   // send silence once the task stops writing; otherwise the last tone loops forever
    if (i2s_new_channel(&cc, &s_tx, NULL) != ESP_OK) { ESP_LOGE(TAG, "i2s chan"); return; }
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = I2S_BCK, .ws = I2S_WS,
            .dout = I2S_DOUT, .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) { ESP_LOGE(TAG, "i2s std"); return; }
    i2s_channel_enable(s_tx);
    gpio_config_t amp = {
        .pin_bit_mask = 1ULL << HW_MOTOR_AMP_EN, .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&amp);
    motor_amp(false);   // park the amp off until a brushing command arrives
    xTaskCreate(motor_task, "motor", 3072, NULL, 6, &s_task);
    ESP_LOGI(TAG, "motor ready (I2S bck=%d ws=%d dout=%d, amp_en=%d)", I2S_BCK, I2S_WS, I2S_DOUT, HW_MOTOR_AMP_EN);
}

void hw_motor_set(int gear)
{
    if (gear < 0) gear = 0;
    if (gear > 5) gear = 5;
    if (gear == s_gear) return;   // hw_task re-applies the state every 100 ms
    bool was = s_gear > 0, now = gear > 0;
    s_gear = gear;
    // Gate the amp on the stop<->run edge: enable it (with a short settle) before
    // the coil is driven, disable it when stopping so idle draws nothing.
    if (now && !was) { motor_amp(true); vTaskDelay(pdMS_TO_TICKS(10)); }
    else if (!now && was) { motor_amp(false); }
    ESP_LOGI(TAG, "motor gear=%d amp=%d", gear, now);
}

bool hw_motor_running(void) { return s_gear > 0; }

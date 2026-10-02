// Motor output: the I2S channel to the motor amplifier, the task that streams the
// waveform into it and the amp-enable pin (re/spec/brushing.md 1.1 - 1.3). What is
// played and when is decided in oem_wave.c; this file only moves samples.
#include "hardware.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "oem_api.h"
#include "oem_wave.h"

// Stock waits up to 100 s for room in the DMA buffers. A buffer frees every 12.5 ms,
// so a write that cannot make progress for 200 ms means the channel is not running.
#define WRITE_TIMEOUT_MS 200

static const char *TAG = "hw_motor";
static i2s_chan_handle_t s_tx;           // NULL: emulated, or the channel could not be set up
static EventGroupHandle_t s_events;      // stock 0x3fca4fc8
static portMUX_TYPE s_amp_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_owed;                  // samples not sent to I2S and not yet waited for
static uint32_t s_write_errors;
// Underrun watch: a DMA buffer that finishes while two are already waiting for the
// writer means the task fell behind and zeros go out (a gap in the vibration).
static volatile uint32_t s_q_full;       // counted in the I2S interrupt, also while idle
static uint32_t s_q_full_seen, s_underruns;
static bool s_streaming;                 // a write has happened since the last restart

static const i2s_std_clk_config_t s_clk = I2S_STD_CLK_DEFAULT_CONFIG(OEM_WAVE_RATE);

// 0x4200d148: release the pad hold, set the level, hold again. Called from the main
// task (motor off, zone cue) and from the motor task; the lock keeps the three steps
// of two callers from interleaving (stock has none).
void oem_motor_amp(bool on)
{
    // No clock, no data: there is nothing to amplify, so the amp stays off.
    if (on && !s_tx && !hw_emulated()) on = false;
    portENTER_CRITICAL(&s_amp_mux);
    gpio_hold_dis(HW_PIN_MOTOR_AMP);
    gpio_set_level(HW_PIN_MOTOR_AMP, on ? 1 : 0);
    gpio_hold_en(HW_PIN_MOTOR_AMP);
    portEXIT_CRITICAL(&s_amp_mux);
}

void hw_motor_post(uint32_t bits)
{
    if (s_events) xEventGroupSetBits(s_events, bits);
}

// Let the time of n samples pass (tick resolution, the remainder is carried over).
static void wait_samples(int n)
{
    const uint32_t per_tick = OEM_WAVE_RATE / configTICK_RATE_HZ;
    s_owed += (uint32_t)n;
    TickType_t ticks = s_owed / per_tick;
    if (ticks) {
        vTaskDelay(ticks);
        s_owed -= ticks * per_tick;
    }
}

static bool IRAM_ATTR on_send_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle; (void)event; (void)ctx;
    s_q_full++;
    return false;
}

// 0x4200d130: blocks until the samples are in the DMA buffers, which paces the task.
void hw_motor_write(const int16_t *buf, int n)
{
    if (!s_tx) { wait_samples(n); return; }
    uint32_t q_full = s_q_full;
    if (s_streaming) s_underruns += q_full - s_q_full_seen;   // only between writes of one run
    s_q_full_seen = q_full;
    s_streaming = true;
    size_t done = 0;
    esp_err_t err = i2s_channel_write(s_tx, buf, (size_t)n * sizeof buf[0], &done, WRITE_TIMEOUT_MS);
    if (err != ESP_OK) {
        if (s_write_errors++ % 256 == 0) ESP_LOGE(TAG, "i2s write: %s (%u of %d bytes)", esp_err_to_name(err),
                                                  (unsigned)done, n * (int)sizeof buf[0]);
        wait_samples(n);                 // never spin on a dead channel
    }
}

// Stock calls i2s_set_clk(0, 24000, 16, I2S_CHANNEL_MONO) on every start, also when
// only the gear changes: the legacy driver stops TX, writes the same clock and slot
// setup again and restarts the DMA at its first buffer. Same thing with the std
// driver. What the buffers still hold is sent once more (at most 37.5 ms).
void hw_motor_restart(void)
{
    if (!s_tx) return;
    if (s_underruns) ESP_LOGW(TAG, "%u DMA buffers went out empty since the last start", (unsigned)s_underruns);
    s_underruns = 0;
    s_streaming = false;
    i2s_channel_disable(s_tx);
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &s_clk);
    if (err != ESP_OK) ESP_LOGE(TAG, "i2s clock: %s", esp_err_to_name(err));
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) ESP_LOGE(TAG, "i2s enable: %s", esp_err_to_name(err));
}

// Stock music task: a start request is only executed while GPIO9 reads high, that
// is while the brush is not on the charger (charger present = GPIO9 low); otherwise
// it is dropped without a word. The factory override 0x3fca308f is not ported.
bool hw_motor_start_allowed(void)
{
    if (hw_emulated()) return true;
    if (gpio_get_level(HW_PIN_CHARGER) != 0) return true;
    ESP_LOGW(TAG, "start dropped: GPIO%d low (on the charger)", HW_PIN_CHARGER);
    return false;
}

// 0x4201f24c brush_music_app. Stock also registers with the task watchdog around
// each pass; the longest a pass blocks here is the start pre-roll (about 0.2 s).
static void motor_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_events, OEM_WAVE_EV_ALL, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        oem_wave_task_step(bits & OEM_WAVE_EV_ALL);
    }
}

// 0x4200d0c4. Stock uses the legacy driver: master TX, 16 bit, I2S_CHANNEL_FMT_ONLY_RIGHT,
// I2S_COMM_FORMAT_STAND_MSB, 3 DMA buffers of 300 samples, tx_desc_auto_clear, then
// i2s_set_clk(24000, 16, MONO). In IDF 5.1.1 the legacy driver turns that into the
// slot setup below (i2s_legacy.c: i2s_config_transfer, i2s_set_clk) and hands it to
// the same HAL call the std driver uses, so the registers come out identical: mono
// data, sent in the RIGHT slot only (TDM channel mask 0x02, no mono copy), the left
// slot idles at zero. The S3 default macro I2S_STD_MSB_SLOT_DEFAULT_CONFIG would put
// the sample in both slots; if the amp mixes L and R that doubles the drive.
static void i2s_setup(void)
{
    i2s_chan_config_t cc = {
        .id = I2S_NUM_0, .role = I2S_ROLE_MASTER,
        .dma_desc_num = 3, .dma_frame_num = 300,     // 3 x 300 mono samples = 37.5 ms
        .auto_clear = true,                          // underrun sends zeros, not the last buffer again
    };
    i2s_std_config_t std = {
        .clk_cfg  = s_clk,                           // PLL, MCLK = 256 x 24 kHz (not routed), BCK 768 kHz
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_RIGHT,
            .ws_width = 16, .ws_pol = false,
            .bit_shift = false,                      // MSB format: no 1-bit delay after WS
            .left_align = false, .big_endian = false, .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = HW_PIN_I2S_BCK, .ws = HW_PIN_I2S_WS,
            .dout = HW_PIN_I2S_DOUT, .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    i2s_chan_handle_t tx = NULL;
    esp_err_t err = i2s_new_channel(&cc, &tx, NULL);
    if (err == ESP_OK) {
        const i2s_event_callbacks_t cbs = { .on_send_q_ovf = on_send_q_ovf };
        err = i2s_channel_init_std_mode(tx, &std);
        if (err == ESP_OK) err = i2s_channel_register_event_callback(tx, &cbs, NULL);
        if (err == ESP_OK) err = i2s_channel_enable(tx);   // the clocks run from boot on, as in stock
        if (err != ESP_OK) i2s_del_channel(tx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s setup: %s; the motor stays off", esp_err_to_name(err));
        return;
    }
    s_tx = tx;
}

// Stock: I2S and GPIO48 in the board init 0x4200d77c, event group and task in
// 0x4201f18c (stack 3072, priority 3, core 0).
void oem_motor_init(void)
{
    if (s_events) return;

    // Amp pin: output, low. Stock writes the level without touching the hold here;
    // releasing and re-applying it also covers a pad left held high by a reset in
    // the middle of brushing.
    gpio_config_t amp = {
        .pin_bit_mask = 1ULL << HW_PIN_MOTOR_AMP, .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&amp);
    oem_motor_amp(false);

    if (hw_emulated()) ESP_LOGW(TAG, "emulated: no I2S, the motor task only keeps time");
    else i2s_setup();

    s_events = xEventGroupCreate();
    if (!s_events || xTaskCreatePinnedToCore(motor_task, "brush_music", 4096, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no motor task");
        return;
    }
    ESP_LOGI(TAG, "motor ready (I2S bck=%d ws=%d dout=%d, %d Hz mono in the right slot; amp=%d)",
             HW_PIN_I2S_BCK, HW_PIN_I2S_WS, HW_PIN_I2S_DOUT, OEM_WAVE_RATE, HW_PIN_MOTOR_AMP);
}

#include "hardware.h"
#include <math.h>
#include "esp_log.h"
#include "driver/gpio.h"

// Charge control recovered from stock FW: CHARGE_EN=GPIO26 (confirmed, active-high
// push-pull), WLC_EN=GPIO45 (wireless-charge enable, "likely"). Thermal protection:
// disable charging at >=72 C, re-enable below 67 C (hysteresis), using the IMU die
// temperature (the NTC channel is configured but unused in stock FW).
// Charging is on by default. It is also cut when a temperature that was being
// read goes missing; if none has ever been read (IMU not detected) it stays on,
// without a thermal guard.
#define CHARGE_EN 26
#define WLC_EN    45
#define T_OFF_C   72.0f
#define T_ON_C    67.0f
#define MAX_MISSED 3      // consecutive ticks without a reading before acting on it

static const char *TAG = "hw_charge";
static bool s_charging = true;
static bool s_seen;       // a temperature has been read since boot
static int  s_missed;

static void apply(bool on)
{
    gpio_set_level(CHARGE_EN, on ? 1 : 0);
    gpio_set_level(WLC_EN,    on ? 1 : 0);
    s_charging = on;
}

void hw_charge_init(void)
{
    gpio_config_t o = {
        .pin_bit_mask = (1ULL << CHARGE_EN) | (1ULL << WLC_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&o);
    apply(true);   // charging enabled by default (like stock)
    ESP_LOGI(TAG, "charge rails: CHARGE_EN=%d WLC_EN=%d (thermal %.0f/%.0f C)", CHARGE_EN, WLC_EN, T_OFF_C, T_ON_C);
}

void hw_charge_tick(float imu_temp_c)
{
    if (isnan(imu_temp_c)) {
        // A few misses in a row are tolerated as I2C glitches. Beyond that, a
        // reading that was there and is gone fails safe: stop charging until it
        // returns. If there never was one, charging stays on, unguarded.
        if (s_missed < MAX_MISSED && ++s_missed == MAX_MISSED) {
            if (s_seen) {
                ESP_LOGE(TAG, "IMU temperature lost -> charging OFF (fail-safe)");
                apply(false);
            } else {
                ESP_LOGE(TAG, "no IMU temperature: charging WITHOUT a thermal cutoff");
            }
        }
        return;
    }
    s_seen = true;
    s_missed = 0;
    if (s_charging && imu_temp_c >= T_OFF_C) {
        ESP_LOGW(TAG, "over-temp %.1f C -> charging OFF", imu_temp_c);
        apply(false);
    } else if (!s_charging && imu_temp_c < T_ON_C) {
        ESP_LOGI(TAG, "temp %.1f C -> charging ON", imu_temp_c);
        apply(true);
    }
}

bool hw_charge_enabled(void) { return s_charging; }

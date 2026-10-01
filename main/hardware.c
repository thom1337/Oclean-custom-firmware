#include "hardware.h"
#include "metrics.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "hw";

// Session count persisted to NVS namespace "storage" (stock uses a raw "brushdata"
// partition record format that wasn't fully reversed; we persist the count here).
static void sessions_load(uint32_t *v)
{
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "sessions", v);
        nvs_close(h);
    }
}
static void sessions_save(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "sessions", v);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Shared control state (button, MQTT, BLE all update these; the task applies them).
static volatile bool s_brushing;
static volatile int  s_gear = 3;
static volatile uint32_t s_total_sessions;
static volatile bool s_reset_head_req;

void hw_cmd_brushing(bool on) { s_brushing = on; }
void hw_cmd_gear(int g) { if (g >= 1 && g <= 5) s_gear = g; }
void hw_cmd_reset_head(void) { s_reset_head_req = true; }
bool hw_is_brushing(void) { return s_brushing; }
int  hw_get_gear(void) { return s_gear; }

void hardware_init(void)
{
    hw_battery_init();
    hw_i2c_init();
    hw_io_init();
    hw_motor_init();
    hw_charge_init();
    hw_display_init();
    uint32_t sess = 0; sessions_load(&sess); s_total_sessions = sess;
    ESP_LOGI(TAG, "hardware init done (battery, i2c, buttons, LEDs, motor, charge, display); %lu sessions", (unsigned long)sess);
}

// Periodic sampling: read battery / IMU temp / pressure into the metrics model,
// and turn a short primary-button press into a brushing on/off toggle.
static void hw_task(void *arg)
{
    bool was_down = false, last_brushing = false;
    int  down_ms = 0, tick = 0;
    while (1) {
        // --- button edge/duration (100 ms poll) ---
        bool down = hw_button_pressed();
        if (down) {
            down_ms += 100;
        } else if (was_down) {
            if (down_ms < 2000) {                         // short press -> toggle brushing
                hw_cmd_brushing(!s_brushing);
            } else if (down_ms < 4000 && s_brushing) {    // long-ish press -> cycle gear
                hw_cmd_gear(s_gear >= 5 ? 1 : s_gear + 1);
            }
            down_ms = 0;
        }
        was_down = down;

        // Apply control state to the motor + indicator LED (idempotent).
        if (s_brushing != last_brushing) {
            hw_led_set(0, s_brushing);
            last_brushing = s_brushing;
            if (s_brushing) { s_total_sessions++; sessions_save(s_total_sessions); }
        }
        hw_motor_set(s_brushing ? s_gear : 0);

        if (s_reset_head_req) { s_reset_head_req = false; ESP_LOGI(TAG, "brush-head counter reset"); }

        // --- sensors every ~2 s ---
        if (++tick >= 20) {
            tick = 0;
            brush_state_t s; metrics_get_brush_state(&s);
            int bat = hw_battery_pct();  if (bat >= 0) s.battery_pct = bat;
            s.pressure = hw_pressure_raw();
            float tc; s.imu_temp_c = hw_imu_temp(&tc) ? tc : NAN;
            hw_charge_tick(s.imu_temp_c);   // every cycle: a lost reading must trip the cutoff too
            s.charging = hw_charger_present() && hw_charge_enabled();
            s.brushing = s_brushing;
            s.mode = s_brushing ? s_gear : 0;
            s.total_sessions = s_total_sessions;
            metrics_set_brush_state(&s);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void hardware_start(void)
{
    xTaskCreate(hw_task, "hw_task", 4096, NULL, 5, NULL);
    hw_display_start();
}

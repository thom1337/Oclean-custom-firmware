#include "esp_log.h"
#include "esp_rom_sys.h"
#include "hardware.h"
#include "hw_bus.h"
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_input.h"
#include "oem_state.h"

// AW8686X force sensor (0x6A on the bit-bang bus): the brushing-force value in
// g_oem.pressure. Port of the stock driver "pressure_sensor.c" (0x42026a24..
// 0x42027bd0): register setup, one ADC sample every 20 ms, the Awinic algorithm
// (oem_force.c) and the temperature coefficient. The factory test paths
// (0x42026f5c, 0x42027368) are not ported; their calibration results are read from
// NVS where the factory left them.
//
// This image always runs with the hardware-version flag 0x3fc9abba = 1, so only
// that branch of the stock code is here.
static const char *TAG = "pressure";

#define ADDR      HW_AW8686X_ADDR
#define FAIL_MAX  50          // unanswered samples in a row (1 s) before the value is withdrawn

static bool    s_present;                 // chip answered the id probe and is set up
static bool    s_fresh;                   // initialised and not started yet
static bool    s_running;                 // 0x3fca5c74: the 20 ms timer runs
static float   s_temp_coef = 1.0f;        // 0x3fc9fd78
static uint8_t s_fail;

static void delay_ms(uint32_t ms) { esp_rom_delay_us(ms * 1000); }   // stock vTaskDelay at 1 kHz

// 0x42026a50, 0x42026a3c, 0x42026a6c
static void wr(uint8_t reg, uint8_t v) { hw_bus_write8(ADDR, reg, &v, 1); }
static bool rd(uint8_t reg, uint8_t *v) { *v = 0; return hw_bus_read(ADDR, reg, v, 1); }
static void rmw(uint8_t reg, uint8_t keep, uint8_t set)
{
    uint8_t v;
    rd(reg, &v);
    wr(reg, (uint8_t)((v & keep) | (set & (uint8_t)~keep)));
}

// 0x42026b04: register 0 must read 0x61, 0x62 or 0x64; three attempts.
static bool probe(void)
{
    uint8_t id = 0;
    bool ack = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        ack = rd(0x00, &id);
        if (id == 0x61 || id == 0x62 || id == 0x64) {
            ESP_LOGI(TAG, "AW8686X id 0x%02x", id);
            return true;
        }
    }
    ESP_LOGW(TAG, "AW8686X not found (id 0x%02x, address %s)", id, ack ? "acknowledged" : "not acknowledged");
    return false;
}

// 0x42026d08: analog front end and offset setup
static void afe_setup(void)
{
    wr(0x05, 0xA5);
    wr(0x10, 0x03);
    wr(0x32, 0x02);
    wr(0x33, 0x02);
    delay_ms(1);
    wr(0x32, 0x10);
    wr(0x33, 0x10);
    rmw(0x31, 0x7F, 0x00);
    wr(0x39, 0x39);
    wr(0xC0, 0x4B);
    wr(0xC2, 0x0B);
    wr(0xDA, 0x01);
    wr(0xC1, 0x08);
    delay_ms(5);
    wr(0xC1, 0x00);
    wr(0x39, 0x19);
    wr(0xDA, 0x00);
    wr(0x10, 0x00);
    wr(0xC2, 0x0A);
    rmw(0x31, 0x7F, 0x80);
    wr(0xC2, 0x2C);
    rmw(0xC0, 0xDF, 0x20);
    wr(0xC1, 0x08);
}

// 0x42027894: chip part
static bool chip_init(void)
{
    // register / value pairs, rodata 0x3c11c9ab (22 pairs)
    static const uint8_t k_setup[] = {
        0x17, 0x01,  0x18, 0x00,  0x19, 0x00,  0x1A, 0x00,  0x1B, 0x00,  0x16, 0x40,
        0x16, 0x20,  0xC1, 0x08,  0x1F, 0x00,  0x21, 0x22,  0x30, 0x01,  0x31, 0xB0,
        0x34, 0x40,  0x35, 0x0A,  0x37, 0x42,  0xC2, 0xFF,  0xC8, 0x06,  0xCC, 0x07,
        0xCD, 0x05,  0xD0, 0x07,  0xD1, 0x05,  0xD4, 0x01,
    };
    if (!probe()) return false;
    wr(0x05, 0xA5);
    wr(0x2F, 0x10);
    delay_ms(20);
    wr(0x05, 0xA5);
    for (unsigned i = 0; i < sizeof(k_setup); i += 2) wr(k_setup[i], k_setup[i + 1]);
    afe_setup();
    wr(0x17, 0x81);           // 0x42026ae8
    wr(0x16, 0x20);
    wr(0x1B, 0x06);
    return true;
}

// 0x403787cc: the force is scaled by 1 % per degree away from the temperature
// recorded at the factory calibration (NVS "rec_temperature"), within 0.75..1.25.
static float temp_coef(void)
{
    uint8_t base = 0;
    float now;
    hal_nvs_get("rec_temperature", &base, 1);
    if (!oem_imu_temp(&now)) return 1.0f;
    return oem_force_temp_coef((int16_t)(int)now, base);
}

// pressure_sensor_timer_init 0x42027bd0 (main-task init and both wake functions)
void oem_pressure_init(void)
{
    uint8_t rec[16] = {0};
    uint16_t noise, coef;

    s_present = false;
    s_fresh = true;
    s_fail = 0;
    g_oem.pressure = 0;
    if (hw_emulated()) return;
    if (!chip_init()) return;
    hal_nvs_get("aw8686x_config", rec, sizeof(rec));
    bool cal = oem_force_parse_cal(rec, &noise, &coef);
    oem_force_init(noise, coef);
    s_temp_coef = temp_coef();
    g_oem.force_coef = coef;
    s_present = true;
    ESP_LOGI(TAG, "%s calibration: coef %u, noise %u; temperature coefficient %.2f",
             cal ? "factory" : "no stored calibration, default", coef, noise, (double)s_temp_coef);
}

bool oem_pressure_available(void)
{
    return s_present;
}

// 0x42027b4c. Stock runs the init above right before every start (main-task init and
// both wake functions), so a start that was not preceded by oem_pressure_init() does
// it here: after a screen-off period the chip is set up again and the algorithm
// starts from a new baseline.
void oem_pressure_start(void)
{
    if (s_running) return;
    if (!s_fresh) oem_pressure_init();
    s_fresh = false;
    if (!s_present) return;
    hal_timer_start(HAL_TMR_PRESSURE20, 20, true);
    s_running = true;
}

// 0x42027ba0
void oem_pressure_stop(void)
{
    hal_timer_stop(HAL_TMR_PRESSURE20);
    s_running = false;
    s_fresh = false;
}

// 0x420277d4, on OEM_EV_PRESSURE
void oem_pressure_sample(void)
{
    if (!s_present) return;
    wr(0x16, 0x44);           // start a conversion
    wr(0x16, 0x20);
    delay_ms(2);
    uint8_t b[2] = {0, 0};
    if (!hw_bus_read(ADDR, 0xB0, b, 2)) {
        // Stock would run the algorithm on whatever the idle bus reads (0xFFFF).
        // That would pull the baseline away, so an unanswered sample is dropped,
        // and after a second without answers the value is withdrawn.
        if (++s_fail >= FAIL_MAX) {
            ESP_LOGE(TAG, "AW8686X stopped answering; pressure off until the next init");
            s_present = false;
            g_oem.pressure = 0;
            oem_pressure_stop();
        }
        return;
    }
    s_fail = 0;
    int16_t raw = (int16_t)((uint16_t)(b[0] | (b[1] << 8)) - 0x2000);
    int16_t force = oem_force_step(raw);
    g_oem.pressure = oem_force_scale(force, s_temp_coef);
    g_oem.force_raw = raw;
    g_oem.force_base = oem_force_baseline();
}

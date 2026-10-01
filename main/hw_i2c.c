#include "hardware.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "driver/i2c.h"   // legacy I2C driver (ESP-IDF v5.1; matches stock FW)

static const char *TAG = "hw_i2c";
#define PORT I2C_NUM_0
static uint8_t s_imu_addr, s_aw_addr;   // 0 = not found

static bool rd(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    if (!addr) return false;
    return i2c_master_write_read_device(PORT, addr, &reg, 1, buf, n, pdMS_TO_TICKS(100)) == ESP_OK;
}
static void wr(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    if (addr) i2c_master_write_to_device(PORT, addr, b, 2, pdMS_TO_TICKS(100));
}

void hw_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = HW_I2C_SDA, .scl_io_num = HW_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,   // external pulls on the board
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = HW_I2C_HZ,
    };
    if (i2c_param_config(PORT, &conf) != ESP_OK ||
        i2c_driver_install(PORT, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "i2c init failed");
        return;
    }
    // Scan + identify by WHOAMI (reg 0x00). AW8686X=0x61/62/64, QMI8658=0x05.
    for (uint8_t a = 0x08; a < 0x78; a++) {
        uint8_t who = 0;
        if (!rd(a, 0x00, &who, 1)) continue;
        ESP_LOGI(TAG, "i2c dev 0x%02x whoami=0x%02x", a, who);
        if (who == 0x05 && !s_imu_addr) s_imu_addr = a;
        else if ((who == 0x61 || who == 0x62 || who == 0x64) && !s_aw_addr) s_aw_addr = a;
    }
    if (s_imu_addr) {
        ESP_LOGI(TAG, "QMI8658 IMU @0x%02x", s_imu_addr);
        wr(s_imu_addr, 0x02, 0x60);   // CTRL1
        wr(s_imu_addr, 0x08, 0x03);   // CTRL7: enable accel+gyro so TEMP updates
    } else ESP_LOGW(TAG, "QMI8658 not found");
    if (s_aw_addr) ESP_LOGI(TAG, "AW8686X pressure @0x%02x", s_aw_addr);
    else           ESP_LOGW(TAG, "AW8686X not found");
}

bool hw_imu_temp(float *out_c)
{
    uint8_t lo = 0, hi = 0;
    if (!rd(s_imu_addr, 0x33, &lo, 1) || !rd(s_imu_addr, 0x34, &hi, 1)) return false;
    *out_c = (int16_t)((hi << 8) | lo) / 256.0f;
    return true;
}

// Best-effort raw force (exact AW8686X data reg not recovered; 0x06 candidate).
int hw_pressure_raw(void)
{
    uint8_t b[2] = {0, 0};
    if (!rd(s_aw_addr, 0x06, b, 2)) return -1;
    return (b[0] << 8) | b[1];
}

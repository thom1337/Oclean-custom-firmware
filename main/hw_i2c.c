#include "hardware.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

// Sensor buses, corrected from the stock firmware RE. Despite the file name, the
// two sensors are NOT on hardware-I2C (pins 35/36 carry neither — that is why the
// old WHOAMI scan found nothing):
//   - QMI8658 IMU  -> SPI3 (MISO4 MOSI5 SCLK6 CS7, mode 0, 10 MHz), WHOAMI reg 0x00 = 0x05
//   - AW8686X force -> software bit-bang I2C (SCL=13, SDA=14), addr 0x6A
// Interface (hw_i2c_init / hw_imu_temp / hw_pressure_raw) is unchanged so the rest
// of the firmware is untouched. NOTE: RE-derived, not yet confirmed on hardware.
static const char *TAG = "hw_sens";

// ---------- QMI8658 IMU on SPI3 ----------
static spi_device_handle_t s_imu;

static bool imu_rd(uint8_t reg, uint8_t *buf, int n)
{
    if (!s_imu || n < 1 || n > 8) return false;
    uint8_t tx[9] = { (uint8_t)(reg | 0x80) };   // read transfers set the MSB of the reg
    uint8_t rx[9] = {0};
    spi_transaction_t t = { .length = 8 * (1 + n), .tx_buffer = tx, .rx_buffer = rx };
    if (spi_device_polling_transmit(s_imu, &t) != ESP_OK) return false;
    memcpy(buf, rx + 1, n);
    return true;
}
static void imu_wr(uint8_t reg, uint8_t val)
{
    if (!s_imu) return;
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_polling_transmit(s_imu, &t);
}
static void imu_enable(void)
{
    imu_wr(0x02, 0x60);   // CTRL1
    imu_wr(0x08, 0x03);   // CTRL7: enable accel+gyro so the TEMP register updates
}
static void imu_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = HW_IMU_SPI_MOSI, .miso_io_num = HW_IMU_SPI_MISO, .sclk_io_num = HW_IMU_SPI_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 32,
    };
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "IMU SPI bus init failed"); return; }
    spi_device_interface_config_t dev = {
        .mode = 0, .clock_speed_hz = 10 * 1000 * 1000, .spics_io_num = HW_IMU_SPI_CS, .queue_size = 7,
    };
    if (spi_bus_add_device(SPI3_HOST, &dev, &s_imu) != ESP_OK) { ESP_LOGE(TAG, "IMU SPI dev add failed"); s_imu = NULL; return; }
    uint8_t who = 0;
    if (imu_rd(0x00, &who, 1) && who == 0x05) {
        ESP_LOGI(TAG, "QMI8658 IMU @SPI3 (whoami=0x%02x)", who);
        imu_enable();
    } else {
        ESP_LOGW(TAG, "QMI8658 not found on SPI3 (whoami=0x%02x)", who);
    }
}

bool hw_imu_temp(float *out_c)
{
    uint8_t who = 0;
    if (!imu_rd(0x00, &who, 1) || who != 0x05) return false;   // absent / wrong device -> no reading
    uint8_t c7 = 0;
    if (!imu_rd(0x08, &c7, 1)) return false;
    if ((c7 & 0x03) != 0x03) { imu_enable(); return false; }   // sensors off (reset?) -> restart, no reading this cycle
    uint8_t lo = 0, hi = 0;
    if (!imu_rd(0x33, &lo, 1) || !imu_rd(0x34, &hi, 1)) return false;
    *out_c = (int16_t)((hi << 8) | lo) / 256.0f;
    return true;
}

// ---------- AW8686X force sensor on software bit-bang I2C ----------
static bool s_aw_ok;

static inline void scl(int v) { gpio_set_level(HW_AW_SCL, v); }
static inline void sda(int v) { gpio_set_level(HW_AW_SDA, v); }
static inline int  sda_rd(void) { return gpio_get_level(HW_AW_SDA); }
static inline void qd(void) { esp_rom_delay_us(5); }   // ~100 kHz half-period

static void aw_start(void) { sda(1); scl(1); qd(); sda(0); qd(); scl(0); qd(); }
static void aw_stop(void)  { sda(0); scl(1); qd(); sda(1); qd(); }

static bool aw_wr_byte(uint8_t b)
{
    for (int i = 0; i < 8; i++) { sda((b & 0x80) ? 1 : 0); b <<= 1; qd(); scl(1); qd(); scl(0); qd(); }
    sda(1); qd(); scl(1); qd();           // release SDA, clock the ACK
    int ack = (sda_rd() == 0);            // ACK = slave pulls SDA low
    scl(0); qd();
    return ack;
}
static uint8_t aw_rd_byte(bool ack)
{
    uint8_t b = 0;
    sda(1);                               // release SDA so the slave can drive it
    for (int i = 0; i < 8; i++) { qd(); scl(1); qd(); b = (b << 1) | (sda_rd() & 1); scl(0); }
    sda(ack ? 0 : 1); qd(); scl(1); qd(); scl(0); qd(); sda(1);
    return b;
}
static bool aw_read(uint8_t reg, uint8_t *buf, int n)
{
    aw_start();
    if (!aw_wr_byte((HW_AW8686X_ADDR << 1) | 0) || !aw_wr_byte(reg)) { aw_stop(); return false; }
    aw_start();
    if (!aw_wr_byte((HW_AW8686X_ADDR << 1) | 1)) { aw_stop(); return false; }
    for (int i = 0; i < n; i++) buf[i] = aw_rd_byte(i < n - 1);
    aw_stop();
    return true;
}
static void aw_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << HW_AW_SCL) | (1ULL << HW_AW_SDA),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,   // open-drain: '1' releases to the pull-up, read while released
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    scl(1); sda(1);
    uint8_t who = 0;
    if (aw_read(0x00, &who, 1) && (who == 0x61 || who == 0x62 || who == 0x64)) {
        s_aw_ok = true;
        ESP_LOGI(TAG, "AW8686X @bit-bang I2C 0x%02x (whoami=0x%02x)", HW_AW8686X_ADDR, who);
    } else {
        ESP_LOGW(TAG, "AW8686X not found on bit-bang I2C (whoami=0x%02x)", who);
    }
}

// Best-effort raw force. The exact AW8686X data register was not recovered from the
// RE; 0x06 is a guess (the chip-id probe at 0x00 is the confirmed part).
int hw_pressure_raw(void)
{
    if (!s_aw_ok) return -1;
    uint8_t b[2] = {0, 0};
    if (!aw_read(0x06, b, 2)) return -1;
    return (b[0] << 8) | b[1];
}

void hw_i2c_init(void)
{
    imu_init();
    aw_init();
}

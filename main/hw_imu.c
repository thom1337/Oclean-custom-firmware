#include "hardware.h"
#include "oem_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

// QMI8658 IMU on SPI3 (re/spec/power.md §5): the three operating modes of the stock
// driver (the QST reference driver) and the die temperature.
//   normal      accel + gyro running, any-motion detection and INT1 off (awake)
//   any-motion  accel only at the low-power rate, INT1 (GPIO8) pulses on motion (asleep)
//   power-down  soft reset, everything off (motion wake disabled)
// All entry points run in the main task with the core lock held, so the bus needs no
// lock of its own.

static const char *TAG = "hw_imu";

#define R_WHO_AM_I   0x00
#define R_REVISION   0x01
#define R_CTRL1      0x02
#define R_CTRL2      0x03
#define R_CTRL3      0x04
#define R_CTRL5      0x06
#define R_CTRL7      0x08
#define R_CTRL8      0x09
#define R_CTRL9      0x0A
#define R_CAL1_L     0x0B
#define R_STATUSINT  0x2D
#define R_TEMP_L     0x33
#define R_FW_ID      0x49
#define R_UUID       0x51
#define R_RESET      0x60
#define WHO_AM_I_ID  0x05

static spi_device_handle_t s_imu;
static bool    s_bus_tried;
static uint8_t s_ctrl8;      // stock driver state +0x18 "ctrl8_value"
static uint8_t s_enabled;    // stock driver state +4: sensors enabled in CTRL7

static void wait_ms(int ms)
{
    // The tick is 10 ms here (1 ms in stock): short waits are busy-waits.
    if (ms < 10) esp_rom_delay_us(ms * 1000);
    else vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

// SPI bus + device (stock 0x4200ccc0). The four pins are parked as held outputs
// before deep sleep (hw_power.c) and the hold survives the wake.
static bool bus_ready(void)
{
    if (hw_emulated()) return false;
    if (s_bus_tried) return s_imu != NULL;
    s_bus_tried = true;

    gpio_hold_dis(HW_PIN_IMU_CS);
    gpio_hold_dis(HW_PIN_IMU_SCLK);
    gpio_hold_dis(HW_PIN_IMU_MOSI);
    gpio_hold_dis(HW_PIN_IMU_MISO);
    spi_bus_config_t bus = {
        .mosi_io_num = HW_PIN_IMU_MOSI, .miso_io_num = HW_PIN_IMU_MISO, .sclk_io_num = HW_PIN_IMU_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 32,
    };
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI3 bus init failed");
        return false;
    }
    spi_device_interface_config_t dev = {
        .mode = 0, .clock_speed_hz = 10 * 1000 * 1000, .spics_io_num = HW_PIN_IMU_CS, .queue_size = 7,
    };
    if (spi_bus_add_device(SPI3_HOST, &dev, &s_imu) != ESP_OK) {
        ESP_LOGE(TAG, "SPI3 device add failed");
        s_imu = NULL;
        return false;
    }
    return true;
}

// Register write: one transfer [reg, value] (stock 0x4201e0c4 -> 0x4200cdf4).
static void wr(uint8_t reg, uint8_t val)
{
    if (!s_imu) return;
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_polling_transmit(s_imu, &t);
}

// Register read. Framing: reg | 0x80 and the data byte in one transfer — the framing
// that works on the device (stock's 0x4200ce60 looks different, power.md open
// question 6). Like stock: one register per transfer with its own address (no
// reliance on address auto-increment) and 1 ms before every byte.
static uint8_t rd(uint8_t reg)
{
    if (!s_imu) return 0;
    wait_ms(1);
    uint8_t tx[2] = { (uint8_t)(reg | 0x80), 0 };
    uint8_t rx[2] = { 0, 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    if (spi_device_polling_transmit(s_imu, &t) != ESP_OK) return 0;
    return rx[1];
}

// CTRL9 command with handshake (stock 0x4201e1bc): STATUSINT bit 7 goes high when the
// command is done and low again after the acknowledge; at most 100 extra reads each.
static void ctrl9(uint8_t cmd)
{
    wr(R_CTRL9, cmd);
    uint8_t st = rd(R_STATUSINT);
    for (int n = 100; !(st & 0x80) && n > 0; n--) { wait_ms(1); st = rd(R_STATUSINT); }
    wr(R_CTRL9, 0x00);
    st = rd(R_STATUSINT);
    for (int n = 100; (st & 0x80) && n > 0; n--) { wait_ms(1); st = rd(R_STATUSINT); }
}

// Motion-detection parameters (stock 0x4201e508 qmi8658_config_motion, spec §5.1).
static void config_motion(void)
{
    s_ctrl8 &= (uint8_t)~0x02;
    wr(R_CTRL8, s_ctrl8);            // any-motion off while the parameters change
    wr(0x0B, 0x06);                  // any-motion threshold X
    wr(0x0C, 0x06);                  //                      Y
    wr(0x0D, 0x06);                  //                      Z
    wr(0x0E, 0x09);                  // no-motion threshold X
    wr(0x0F, 0x09);                  //                     Y
    wr(0x10, 0x09);                  //                     Z
    wr(0x11, 0xF7);                  // motion mode control
    wr(0x12, 0x01);                  // first command
    ctrl9(0x0E);
    wr(0x0B, 0x03);                  // any-motion window
    wr(0x0C, 0x01);                  // no-motion window
    wr(0x0D, 0x2C);                  // significant-motion wait window 0x012C
    wr(0x0E, 0x01);
    wr(0x0F, 0x64);                  // significant-motion confirm window 0x0064
    wr(0x10, 0x00);
    wr(0x12, 0x02);                  // second command
    ctrl9(0x0E);
}

// CTRL7 (stock 0x4201e368 qmi8658_enableSensors).
static void enable_sensors(uint8_t v)
{
    wr(R_CTRL7, v);
    s_enabled = v & 0x03;
    wait_ms(1);
}

// Range / rate (stock 0x4201e3c4 qmi8658_config_reg with 0x4201e0d8 / 0x4201e138):
// sensors off, then accel (and gyro in normal mode), low-pass filters off.
static void config_reg(bool low_power)
{
    enable_sensors(0);
    if (!low_power) {
        s_enabled = 0x03;
        wr(R_CTRL2, 0x06);                    // accel +-2 g, ODR code 6
        wr(R_CTRL5, rd(R_CTRL5) & 0xF0);
        wr(R_CTRL3, 0x76);                    // gyro +-2048 dps, ODR code 6
        wr(R_CTRL5, rd(R_CTRL5) & 0x0F);
    } else {
        s_enabled = 0x01;                     // accel only
        wr(R_CTRL2, 0x2E);                    // accel +-8 g, low-power ODR code 0xE
        wr(R_CTRL5, rd(R_CTRL5) & 0xF0);
    }
}

// Stock 0x4201e38c: CTRL1..CTRL8 to the log.
static void dump_ctrl(void)
{
    uint8_t c[8];
    for (int i = 0; i < 8; i++) c[i] = rd(R_CTRL1 + i);
    ESP_LOGI(TAG, "ctrl1..8: %02x %02x %02x %02x %02x %02x %02x %02x",
             c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
}

// Stock 0x4201e414: up to 5 reads, the whole attempt twice.
static bool probe(void)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        for (int i = 0; i < 5; i++) {
            if (rd(R_WHO_AM_I) == WHO_AM_I_ID) return true;
        }
    }
    return false;
}

// Normal operation (stock 0x4201e730 -> 0x4201e6a8, spec §5.2).
void oem_imu_normal(void)
{
    if (!bus_ready()) return;
    if (!probe()) {
        ESP_LOGE(TAG, "qmi8658_init fail (WHO_AM_I)");
        return;
    }
    s_ctrl8 = 0xC0;
    wr(R_CTRL1, 0x60);               // address auto-increment, big-endian, INT outputs off
    uint8_t rev = rd(R_REVISION);
    uint8_t fw[3], id[6];
    for (int i = 0; i < 3; i++) fw[i] = rd(R_FW_ID + i);
    for (int i = 0; i < 6; i++) id[i] = rd(R_UUID + i);
    wr(R_CTRL7, 0x00);               // all sensors off
    wr(R_CTRL8, s_ctrl8);            // CTRL9 handshake on STATUSINT, motion events to INT1, any-motion off
    ESP_LOGI(TAG, "QMI8658 rev 0x%02x fw %02x %02x %02x uuid %02x%02x%02x %02x%02x%02x",
             rev, fw[2], fw[1], fw[0], id[5], id[4], id[3], id[2], id[1], id[0]);

    config_motion();
    config_reg(false);
    enable_sensors(s_enabled);       // CTRL7 = 0x03: accel + gyro
    dump_ctrl();
}

// Any-motion mode for the screen-off stage and deep sleep (stock 0x4201e74c ->
// 0x4201e6dc -> 0x4201e598(1,1,1), spec §5.3). Stock does not check WHO_AM_I here.
void oem_imu_amd(void)
{
    if (!bus_ready()) return;
    ESP_LOGI(TAG, "QMI8658 any-motion mode");
    config_motion();
    s_ctrl8 = (uint8_t)((s_ctrl8 & ~0x02) | 0x40);
    wr(R_CTRL8, s_ctrl8);
    wait_ms(2);
    enable_sensors(0);
    config_reg(true);                // starts by switching the sensors off once more
    wr(R_CTRL1, rd(R_CTRL1) | 0x08); // INT1 output on (0x60 -> 0x68)
    s_ctrl8 |= 0x02;                 // any-motion on (0xC2)
    wr(R_CTRL8, s_ctrl8);
    wait_ms(1);
    enable_sensors(s_enabled);       // CTRL7 = 0x01
}

// Soft reset and everything off (stock 0x4201e6f0, spec §5.4). INT1 is disabled
// afterwards, so GPIO8 is no longer driven.
void oem_imu_power_down(void)
{
    if (!bus_ready()) return;
    ESP_LOGI(TAG, "QMI8658 power down");
    wr(R_RESET, 0xB0);
    wait_ms(10);
    wr(R_CTRL1, 0x60);
    wr(R_CTRL2, 0x0F);
    wr(R_CTRL3, 0x0F);
    wr(R_CTRL7, 0x00);
    dump_ctrl();
    wait_ms(10);
}

// Die temperature (stock 0x4201e240): registers 0x33 / 0x34, signed, 1/256 degC.
// Not stock: WHO_AM_I is checked first, so a sensor that stopped answering reports
// "no reading" instead of a made-up temperature (the charge thermal check uses this).
bool oem_imu_temp(float *celsius)
{
    if (!bus_ready() || !celsius) return false;
    if (rd(R_WHO_AM_I) != WHO_AM_I_ID) return false;
    uint8_t lo = rd(R_TEMP_L);
    uint8_t hi = rd(R_TEMP_L + 1);
    *celsius = (float)(int16_t)((hi << 8) | lo) / 256.0f;
    return true;
}

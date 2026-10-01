#pragma once
#include <stdbool.h>
#include <stdint.h>
// On-device hardware abstraction, reconstructed from the stock firmware
// (see re/HARDWARE_MAP.md): battery ADC, I2C sensors, buttons, indicator LEDs,
// display, the motor (I2S voice-coil) and the charge rails. All of it is driven
// from boot, including the motor and charge pins that are only "likely".

// Confirmed GPIO/peripheral map (high confidence unless noted):
#define HW_ADC_VBAT_CH      0      // ADC1_CH0 = GPIO1 (×2 external divider)
#define HW_BTN_PRIMARY      3      // pull-up, active-low (power/mode button)
#define HW_BTN_CHARGE_DET   8      // charger/dock present (active-high, likely)
#define HW_BTN_GYRO_WAKE    9      // QMI8658 motion INT / wake
#define HW_LED_GPIOS        {17,18,19,20,21}   // LEDC indicator LEDs
// Motor amp enable (active-high). RE-confirmed from stock set_motor_power: the I2S
// data streams on GPIO34 but the external amp stays off — and the coil can't move —
// until this pin is driven high. This is the piece brushing was missing.
#define HW_MOTOR_AMP_EN     48
// Sensors (RE-corrected — they are NOT on hardware-I2C, which is why the old scan
// found neither): the QMI8658 IMU is on SPI3, and the AW8686X force sensor is on a
// software bit-bang I2C bus. The HW-I2C pins 35/36 carry neither sensor.
#define HW_IMU_SPI_MISO     4
#define HW_IMU_SPI_MOSI     5
#define HW_IMU_SPI_SCLK     6
#define HW_IMU_SPI_CS       7      // SPI3_HOST, mode 0, 10 MHz; WHOAMI reg 0x00 == 0x05
#define HW_AW_SCL           13     // bit-bang I2C clock
#define HW_AW_SDA           14     // bit-bang I2C data
#define HW_AW8686X_ADDR     0x6A   // 7-bit; WHOAMI reg 0x00 ∈ {0x61,0x62,0x64}

void hardware_init(void);        // configure peripherals
void hardware_start(void);       // spawn the periodic sensor/button task

// Control interface (button, MQTT, and BLE all drive these).
void hw_cmd_brushing(bool on);
void hw_cmd_gear(int gear);      // 1..5
void hw_cmd_reset_head(void);
bool hw_is_brushing(void);
int  hw_get_gear(void);

// Per-subsystem init (called by hardware_init).
void hw_battery_init(void);
void hw_i2c_init(void);
void hw_io_init(void);
void hw_display_init(void);
void hw_display_start(void);

// Motor = I2S voice-coil ("music through the motor"). gear 0=stop, 1..5 intensity.
// Pins (BCK=33, WS=47, DOUT=34) are "likely" from the RE; verify at bring-up.
void  hw_motor_init(void);
void  hw_motor_set(int gear);      // 0 stop, 1..5
bool  hw_motor_running(void);
// Charge rails (GPIO26 CHARGE_EN confirmed, GPIO45 WLC_EN likely) with the stock
// thermal cutoff (off >=72C, on <67C) driven from the IMU die temperature.
void  hw_charge_init(void);
void  hw_charge_tick(float imu_temp_c);  // call every sensor cycle; NAN = no reading (a lost reading cuts charging)
bool  hw_charge_enabled(void);           // false while charging is held off (over-temp or no reading)

// Individual reads (also used by the task). Return <0 / NAN on failure.
int   hw_battery_pct(void);      // 0..100, -1 on error
int   hw_battery_mv(void);
bool  hw_imu_temp(float *out_c); // QMI8658 die temp
int   hw_pressure_raw(void);     // AW8686X raw force, -1 on error
bool  hw_button_pressed(void);   // primary button currently down (active-low)
bool  hw_charger_present(void);  // charger/dock detect line high (role "likely")
void  hw_led_set(int idx, bool on);

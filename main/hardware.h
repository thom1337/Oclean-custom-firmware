#pragma once
#include <stdbool.h>
#include <stdint.h>
// Board definitions for the Oclean X Ultra 20 (ESP32-S3), as recovered from the stock
// firmware (re/spec/*.md, re/HARDWARE_MAP.md), and the driver entry points that are
// not part of the oem core API (oem_api.h).

// ---- pin map ----
#define HW_PIN_VBAT_ADC      1    // ADC1_CH0, battery through a 1:1 divider (x2)
#define HW_PIN_AUX_IN        2    // input, falling edge; only resets the unplug debounce in stock
#define HW_PIN_BUTTON        3    // active low, pull-up
#define HW_PIN_IMU_MISO      4    // QMI8658 on SPI3 (mode 0, 10 MHz)
#define HW_PIN_IMU_MOSI      5
#define HW_PIN_IMU_SCLK      6
#define HW_PIN_IMU_CS        7
#define HW_PIN_MOTION_INT    8    // QMI8658 INT1 any-motion, active high (EXT0 wake)
#define HW_PIN_CHARGER       9    // charger present, active LOW (EXT1 wake together with the button)
#define HW_PIN_NTC_ADC       10   // ADC1_CH9, configured but unused by stock
#define HW_PIN_TOUCH_RDY     12   // IQS7222D RDY, falling edge; driven low at boot
#define HW_PIN_BUS_SCL       13   // bit-bang I2C: IQS7222D (0x44) and AW8686X (0x6A)
#define HW_PIN_BUS_SDA       14
#define HW_PIN_LED1          17   // LEDC ch0 (inverted output)
#define HW_PIN_LED2          18   // LEDC ch1 (inverted output)
#define HW_PIN_LED3          19   // LEDC ch2
#define HW_PIN_LED4          20   // LEDC ch3 (written as 8191 - level)
#define HW_PIN_BACKLIGHT     21   // LEDC ch4 (written as 8191 - level), active low
#define HW_PIN_CHARGE_BLOCK  26   // driven high = charging blocked; input (high-Z) = allowed
#define HW_PIN_I2S_BCK       33
#define HW_PIN_I2S_DOUT      34
#define HW_PIN_LCD_PWR       37   // 0 awake, 1 asleep
#define HW_PIN_LCD_CS        38
#define HW_PIN_LCD_SCLK      39
#define HW_PIN_LCD_MOSI      40
#define HW_PIN_LCD_DC        41
#define HW_PIN_LCD_RST       42
#define HW_PIN_GPIO45        45   // stock always writes 0
#define HW_PIN_I2S_WS        47
#define HW_PIN_MOTOR_AMP     48   // amp enable, active high, kept with gpio_hold

#define HW_IQS7222D_ADDR     0x44
#define HW_AW8686X_ADDR      0x6A

// True when running under QEMU (no real peripherals): drivers skip bus traffic that
// would never complete there and report benign values.
bool hw_emulated(void);

// ---- display (hw_display.c) ----
void hw_display_init(void);                    // SPI + panel reset/init + black frame; backlight channel off
void hw_display_blit(const uint8_t *fb);       // 80x160 RGB565, high byte first
void hw_display_sleep(void);                   // panel SLPIN
void hw_display_wake(void);                    // reset + init table + black frame
bool hw_display_awake(void);

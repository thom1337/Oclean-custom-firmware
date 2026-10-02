#pragma once
#include <stdbool.h>
#include <stdint.h>

// Bit-bang I2C bus on SCL13 / SDA14 shared by the IQS7222D touch controller (0x44)
// and the AW8686X force sensor (0x6A). Port of the stock routines
// 0x4201e844..0x4201ec90 and their wrappers 0x4200cbd0 / 0x4200cbec / 0x4200cc30.
//
// Not thread safe and not for interrupt context: stock touches both chips only from
// the main task, and so do hw_touch.c and hw_pressure.c.
//
// Every transfer runs to its end whatever the slave answers (stock does not look at
// the ACK bits after the address). The return value says whether the address byte
// was acknowledged, which stock does not report; callers use it for logging and for
// the "chip present" test only.

void hw_bus_init(void);   // 0x4200cb84: release the pad holds, both lines driven high

// START, addr<<1, reg, data[0..n-1], STOP
bool hw_bus_write8(uint8_t addr7, uint8_t reg, const uint8_t *data, uint8_t n);
// START, addr<<1, reg_hi, reg_lo, data[0..n-1], STOP (n <= 38)
bool hw_bus_write16(uint8_t addr7, uint8_t reg_hi, uint8_t reg_lo, const uint8_t *data, uint8_t n);
// START, addr<<1, reg, START, addr<<1|1, n bytes (NACK after the last), STOP (n >= 1)
bool hw_bus_read(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t n);

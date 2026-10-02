#include "hw_bus.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "hardware.h"

// Bit-bang I2C exactly as the stock firmware does it (input.md 1.2). Open drain is
// emulated with the pin direction: "release" = input (the external pull-up raises
// the line), "low" = output driving 0. No internal pull-ups, as in stock.
//
// After every release stock polls the pin until it reads high, at most 1001 polls
// (counter 0x3fca4f54). On SCL that is the clock-stretch handling; on SDA it lets a
// slow rising edge finish before the next step.
//
// Stock has no delays: its bus speed is whatever the GPIO calls take at its CPU
// clock, which is not known. The same calls are used here, plus a short hold after
// each SCL edge so that the clock phases stay inside the I2C fast-mode limits
// (low >= 1.3 us, high >= 0.6 us, START hold >= 0.6 us, bus free >= 1.3 us) even if
// this build runs the CPU faster than stock.
#define SCL          HW_PIN_BUS_SCL
#define SDA          HW_PIN_BUS_SDA
#define RELEASE_POLLS 1001
#define ACK_POLLS     160
#define SCL_HOLD_US   2

static inline void release(gpio_num_t pin)
{
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    for (int i = 0; i < RELEASE_POLLS; i++) {
        if (gpio_get_level(pin)) break;
    }
}

static inline void drive_low(gpio_num_t pin)
{
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
}

static inline void scl_high(void) { release(SCL); esp_rom_delay_us(SCL_HOLD_US); }
static inline void scl_low(void)  { drive_low(SCL); esp_rom_delay_us(SCL_HOLD_US); }

// 0x4201e89c (+ 0x4201e844)
static void bus_start(void)
{
    release(SDA);
    scl_high();
    drive_low(SDA);
    esp_rom_delay_us(SCL_HOLD_US);
    scl_low();
}

// 0x4201e8c0
static void bus_stop(void)
{
    drive_low(SDA);
    scl_high();
    release(SDA);
    esp_rom_delay_us(SCL_HOLD_US);
}

// 0x4201e920: MSB first; SDA is released after the last bit
static void bus_send(uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        if (b & 0x80) release(SDA); else drive_low(SDA);
        b <<= 1;
        scl_high();
        scl_low();
    }
    gpio_set_direction(SDA, GPIO_MODE_INPUT);
}

// 0x4201e9b4: clock the ACK bit; SDA is polled up to 160 times for the slave's low
static bool bus_read_ack(void)
{
    bool ack = false;
    gpio_set_direction(SDA, GPIO_MODE_INPUT);
    scl_high();
    for (int i = 0; i < ACK_POLLS; i++) {
        if (gpio_get_level(SDA) == 0) { ack = true; break; }
    }
    scl_low();
    release(SDA);
    return ack;
}

// 0x4201eae8
static uint8_t bus_recv(void)
{
    uint8_t b = 0;
    gpio_set_direction(SDA, GPIO_MODE_INPUT);
    for (int i = 0; i < 8; i++) {
        scl_high();
        b = (uint8_t)(b << 1);
        if (gpio_get_level(SDA)) b |= 1;
        scl_low();
    }
    return b;
}

// 0x4201ea6c
static void bus_send_ack(bool nack)
{
    if (nack) release(SDA); else drive_low(SDA);
    scl_high();
    scl_low();
}

// Common head of the three transfers: START + address, repeated until the slave
// acknowledges. Stock makes at most 4 attempts and after the 4th goes on without
// clocking the ACK bit at all (the loop exits before the ACK read); the rest of the
// transfer is then sent to nobody. This is also how the "force comms" request to the
// IQS7222D outside its window goes out, so the wire pattern is kept as it is.
static bool bus_address(uint8_t addr8)
{
    for (int attempt = 1; ; attempt++) {
        bus_start();
        bus_send(addr8);
        if (attempt > 3) return false;
        if (bus_read_ack()) return true;
    }
}

void hw_bus_init(void)
{
    if (hw_emulated()) return;
    // 0x4200d77c releases the pad holds left by the sleep code, then 0x4200cb84
    // makes both lines plain outputs at level 1. The first START turns them into
    // inputs; from then on they idle released.
    gpio_hold_dis(SCL);
    gpio_hold_dis(SDA);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << SCL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(SCL, 1);
    io.pin_bit_mask = 1ULL << SDA;
    gpio_config(&io);
    gpio_set_level(SDA, 1);
}

// 0x4200cbd0 -> 0x4201ec30
bool hw_bus_write8(uint8_t addr7, uint8_t reg, const uint8_t *data, uint8_t n)
{
    if (hw_emulated()) return false;
    bool ok = bus_address((uint8_t)(addr7 << 1));
    bus_send(reg);
    bus_read_ack();
    for (uint8_t i = 0; i < n; i++) {
        bus_send(data[i]);
        bus_read_ack();
    }
    bus_stop();
    return ok;
}

// 0x4200cbec -> 0x4201ebdc
bool hw_bus_write16(uint8_t addr7, uint8_t reg_hi, uint8_t reg_lo, const uint8_t *data, uint8_t n)
{
    if (hw_emulated()) return false;
    uint8_t buf[40];                   // same size as the stock stack buffer
    if (n > sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[0] = reg_hi;
    buf[1] = reg_lo;
    memcpy(buf + 2, data, n);
    bool ok = bus_address((uint8_t)(addr7 << 1));
    for (uint8_t i = 0; i < n + 2; i++) {
        bus_send(buf[i]);
        bus_read_ack();
    }
    bus_stop();
    return ok;
}

// 0x4200cc30 -> 0x4201eb54
bool hw_bus_read(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t n)
{
    if (hw_emulated() || n == 0) return false;
    bool ok = bus_address((uint8_t)(addr7 << 1));
    bus_send(reg);
    bus_read_ack();
    bus_start();                       // repeated START
    bus_send((uint8_t)(addr7 << 1) | 1);
    bus_read_ack();
    for (uint8_t i = 0; i < n - 1; i++) {
        buf[i] = bus_recv();
        bus_send_ack(false);
    }
    buf[n - 1] = bus_recv();
    bus_send_ack(true);
    bus_stop();
    return ok;
}

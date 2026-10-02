#pragma once
#include <stdbool.h>
#include <stdint.h>

// Motor waveform: what oem_wave.c (platform independent) and hw_motor.c (I2S task,
// amp pin) share. Other modules use the "motor" block of oem_api.h only.
// Spec: re/spec/brushing.md section 1.

#define OEM_WAVE_RATE        24000   // samples per second, 16-bit mono
#define OEM_WAVE_PERIODS     9       // periods written per chunk (stock: a 3-period buffer written 3 times)
#define OEM_WAVE_PREROLL     5       // chunks queued before the amp is enabled (45 periods)
#define OEM_WAVE_FULL        16383   // peak sample value at amplitude 50
#define OEM_WAVE_MAX_PERIOD  2400    // samples in the period buffer: 10 Hz and up

// Waveform types (gear entry byte 4)
#define OEM_WAVE_T_PLAIN     0x00    // table wave, constant amplitude (app gear table)
#define OEM_WAVE_T_PULSE     0x1f    // table wave, steady / triangle sections by brushing second
#define OEM_WAVE_T_TRIANGLE  0x20    // table wave, amplitude base-16 .. base
#define OEM_WAVE_T_SWELL     0x21    // table wave, amplitude base .. base+20 .. base
#define OEM_WAVE_T_PLAIN22   0x22    // table wave, constant amplitude
#define OEM_WAVE_T_BOOST     0x50    // table wave, amplitude duty + 6
#define OEM_WAVE_T_SINE      0x51    // sine (idle hum)

// Stock gear entry (rodata 0x3c1194b8, 7 bytes). The motor code reads f10, frac,
// duty and type only.
typedef struct { uint8_t f10, frac, duty, b3, type, b5, b6; } oem_gear_t;
#define OEM_GEAR_COUNT 54
const oem_gear_t *oem_wave_gear(uint8_t gear_id);   // gear_id 1..54, else NULL

// Generator state: the stock globals the generator 0x40377f70 works on.
typedef struct {
    uint16_t freq;       // 0x3fc9aeec  Hz, boot value 250
    uint8_t  base;       // 0x3fc9aeeb  duty of the gear, boot value 20
    uint8_t  type;       // 0x3fc9aeea  OEM_WAVE_T_*, boot value 0x50
    int16_t  amp;        // 0x3fca4f6c  current amplitude
    uint8_t  dir;        // 0x3fc9aee8  1 = triangle going down, boot value 1
    uint8_t  pulse_on;   // 0x3fca4f56
    uint8_t  sw_hold;    // 0x3fca4f57
    int8_t   sw_off;     // 0x3fca4f58
    uint8_t  sw_idx;     // 0x3fca4f59
    uint8_t  sw_state;   // 0x3fca4f5a
} oem_wave_t;

// The generator-side half of stock motor_wave() 0x4201f074: new frequency, duty and
// type; amplitude back to the duty, triangle direction down. The modulation state
// (pulse_on, sw_*) is not reset, as in stock.
void oem_wave_set(oem_wave_t *w, uint16_t freq_hz, uint8_t duty, uint8_t type);

// Samples in one period for a frequency (24000 / freq), or 0 if the frequency is 0
// or the period does not fit OEM_WAVE_MAX_PERIOD.
int  oem_wave_period_len(uint16_t freq_hz);

// One call of the stock generator: computes one period into buf (at most cap
// samples), then runs the modulation step of the type once, which may change w->amp
// for the next call. Returns the period length n; the caller sends the period
// OEM_WAVE_PERIODS times. Returns 0 and leaves w alone if the period does not fit.
// elapsed_s is the brushing second counter (only the pulse type reads it).
int  oem_wave_period(oem_wave_t *w, uint16_t elapsed_s, int16_t *buf, int cap);

// ---- playback (stock music task 0x4201f24c) ----
#define OEM_WAVE_EV_START  0x08000u   // start / new parameters
#define OEM_WAVE_EV_STOP   0x10000u   // stop
#define OEM_WAVE_EV_NEXT   0x20000u   // produce the next chunk
#define OEM_WAVE_EV_ALL    0x38000u

// One pass of the music task's loop body for the event bits that were set. Runs in
// the motor task, without the core lock.
void oem_wave_task_step(uint32_t bits);

// ---- provided by hw_motor.c (fakes in re/tools/uisim/sim_wave.c) ----
void hw_motor_post(uint32_t bits);                 // set bits in the motor event group (stock 0x4201ec90)
void hw_motor_write(const int16_t *buf, int n);    // blocking write of n samples (stock 0x4200d130)
void hw_motor_restart(void);                       // stock i2s_set_clk(0, 24000, 16, MONO): stop, same clock, start
bool hw_motor_start_allowed(void);                 // stock gate: GPIO9 high (not on the charger)

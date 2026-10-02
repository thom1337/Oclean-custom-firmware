#pragma once
#include <stdbool.h>
#include <stdint.h>

// Internal interface of the input module: what the drivers (hw_touch.c,
// hw_pressure.c) and the platform-independent parts (oem_gesture.c, oem_force.c)
// share. Other modules use the "input" block of oem_api.h only.
// Spec: re/spec/input.md, re/spec/brushing.md section 4, re/spec/NOTES_input.md.

// ---- gesture decoder (oem_gesture.c) ------------------------------------------------
// One trackpad sample (IQS7222D registers 0x14 / 0x15), main task. Stock 0x42025ca4.
void oem_gesture_sample(uint16_t x, uint16_t y);
// False after a "no finger" sample (stock 0x3fca5a40 == 60000).
bool oem_gesture_touching(void);

// ---- force algorithm (oem_force.c) --------------------------------------------------
// Port of the Awinic "touch algorithm" library the stock firmware links for the
// AW8686X (0x42071e64..0x42072e72): input filter, zero-force baseline tracking and
// press state for one channel. hw_pressure.c feeds it one ADC sample every 20 ms.
#define OEM_FORCE_STATE_SIZE 236            // size of the stock state block (0x42103868)
#define OEM_FORCE_DEF_NOISE  9              // used when NVS has no valid calibration
#define OEM_FORCE_DEF_COEF   23

// noise, coef: factory calibration (NVS "aw8686x_config"); see oem_force_parse_cal.
void    oem_force_init(uint16_t noise, uint16_t coef);
// One sample (ADC value - 0x2000). Returns the force in stock units, before the
// temperature coefficient: (filtered sample - baseline) * coef / 100, may be negative.
int16_t oem_force_step(int16_t raw);
int16_t oem_force_baseline(void);           // tracked zero level (stock getter 0x4210374c)
int16_t oem_force_filtered(void);           // the sample after the input filter
uint16_t oem_force_coef(void);              // stock getter 0x421037ec
bool    oem_force_pressed(void);            // the library's own press state (not used by the brush logic)
const uint8_t *oem_force_state(void);       // raw state block, for the host test
// Decode the 16-byte calibration record written by the factory test (0x42026c08).
// Returns false (and the defaults) when the record is absent or damaged.
bool    oem_force_parse_cal(const uint8_t rec[16], uint16_t *noise, uint16_t *coef);
// Temperature coefficient 0x403787cc: 1 + (now - base) / 100, limited to 0.75..1.25.
// base_c = 0 means "no base recorded": the result is 1.
float   oem_force_temp_coef(int now_c, uint8_t base_c);
// What 0x420277d4 stores: max(0, force) * coef, truncated to 16 bits.
int16_t oem_force_scale(int16_t force, float temp_coef);

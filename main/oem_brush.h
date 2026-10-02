#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Interface between the two files of the app module: oem_brush.c (configuration,
// profiles, session engine) and oem_app.c (main-loop body). Not for other modules;
// what they may call is in oem_api.h.

// ---- configuration blobs (stock NVS namespace "storage")
void    brush_sys_config_load(void);            // 0x42018cf8  "sys_config"
void    brush_sys_config_save(void);            // 0x42018d74
void    brush_config_load(void);                // 0x42018dd8: "user_config" (0x42018a74) or the defaults (0x420187b4)
void    brush_user_config_save(void);           // 0x420185b4
void    brush_mode_clamp(void);                 // 0x42019008

// ---- daily history totals (RTC memory + NVS "shuanhuan")
void    brush_hist_load(void);                  // 0x4201c100
void    brush_hist_reset(void);                 // 0x4201c0cc
void    brush_hist_update(uint16_t seconds, uint8_t score);   // 0x4201bc88; (0, 0) = day roll-over check only
bool    brush_hist_new_day(void);               // 0x3fca4dfe: the totals were reset by a new day since boot

// ---- session engine
void    brush_session_begin(void);              // engine part of start_session (0x4201c858..): flags, profile, motor
uint8_t brush_strength_level(void);             // RTC 0x50001019, 1..5 (anything else becomes 3)
void    brush_strength_set(uint8_t level);      // 0x420192e8 without the screen: gear, and the motor if it runs
void    brush_pause(void);                      // engine part of "suspend_brush_by_profile"
void    brush_resume(void);                     // engine part of "resume_brush_by_profile" (0x4201977c)
void    brush_tick_1hz(void);                   // 0x42019560, pressure log, score refresh
void    brush_tick_30ms(void);                  // zone cue 0x4201962c, pressure 0x42018530 / 0x420198fc
uint8_t brush_score(void);                      // 0x4201c260
bool    brush_session_finish(void);             // score, record, history; true when the session counted

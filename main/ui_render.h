#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Picture renderer for the OEM screen system (port of the stock compositor).
//
// The stock UI is retained-mode: a screen is a list of picture elements, each with
// a position, a base picture id and a "variant" byte (digit, animation frame,
// language...). Pictures are raw RGB565 (high byte first) in the brush's resource
// partition; stock_ui_tables.h carries only the layout. A frame is composed into an
// 80x160 buffer (same byte order as the panel) and then pushed to the LCD.
//
// This file has no ESP-IDF dependencies so the same code runs in the host simulator
// (re/tools/uisim).

#define UI_W 80
#define UI_H 160
#define UI_FB_BYTES (UI_W * UI_H * 2)

// Reads len bytes at offset off of the resource partition. Returns false on failure.
typedef bool (*ui_res_read_fn)(uint32_t off, void *dst, size_t len);

void ui_render_init(uint8_t *fb, ui_res_read_fn rd);
void ui_render_set_lang(uint8_t lang);       // UI language index (0..16)
uint8_t ui_render_lang(void);
void ui_render_set_bank_b(bool b);            // picture 998 bank (stock byte 0x3fc9a719 == 'B')

// Element state (indices are EL_* / DS_* from stock_ui_tables.h).
void ui_el_reset_all(void);                   // back to the stock power-on values
void ui_el_show(int el, bool visible);
bool ui_el_visible(int el);
void ui_el_set_var(int el, uint8_t variant);  // variant byte of the element's descriptor
uint8_t ui_el_var(int el);
void ui_el_move(int el, int x, int y);
void ui_el_resize(int el, int w, int h);

// Compose: clear the frame to black, draw the visible elements of a screen list
// (0xff-terminated element indices) in order.
void ui_render_clear(void);
void ui_render_list(const uint8_t *elems);
void ui_render_elem(int el);
// Draw one picture directly (base id + variant) at x,y with size w x h.
void ui_render_pic(uint16_t base, uint8_t variant, int x, int y, int w, int h);
// Solid rectangle (used by the built-in fallback UI), colour in RGB565.
void ui_render_fill(int x, int y, int w, int h, uint16_t rgb565);

const uint8_t *ui_screen_list(int screen_id);  // NULL if the id has no stock screen

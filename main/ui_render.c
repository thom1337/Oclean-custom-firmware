#include "ui_render.h"
#include <string.h>
#include "stock_ui_tables.h"

// Port of the stock picture drawer (0x40378440) and compositor (0x40378298).

#define RES_LIMIT      0x800000u   // stock reader refuses offsets above this (0x40378794)
#define ZONE_BASE      0x77e000u   // 3-byte-per-pixel zone overlays (ids 927..950)
#define ZONE_ROWS      44
#define PIC998_BANK_A  0x75e000u
#define PIC998_BANK_B  0x76e000u

static uint8_t *s_fb;
static ui_res_read_fn s_rd;
static uint8_t s_lang;
static bool s_bank_b;

static struct { uint8_t x, y, w, h, flags; } s_el[STOCK_ELEM_COUNT];
static uint8_t s_var[STOCK_DESC_COUNT];

void ui_render_init(uint8_t *fb, ui_res_read_fn rd)
{
    s_fb = fb;
    s_rd = rd;
    ui_el_reset_all();
}

void ui_render_set_lang(uint8_t lang) { s_lang = lang; }
uint8_t ui_render_lang(void) { return s_lang; }
void ui_render_set_bank_b(bool b) { s_bank_b = b; }

void ui_el_reset_all(void)
{
    for (int i = 0; i < STOCK_ELEM_COUNT; i++) {
        s_el[i].x = STOCK_ELEMS[i].x; s_el[i].y = STOCK_ELEMS[i].y;
        s_el[i].w = STOCK_ELEMS[i].w; s_el[i].h = STOCK_ELEMS[i].h;
        s_el[i].flags = STOCK_ELEMS[i].flags0;
    }
    for (int i = 0; i < STOCK_DESC_COUNT; i++) s_var[i] = STOCK_DESCS[i].var0;
}

static bool el_ok(int el) { return el >= 0 && el < STOCK_ELEM_COUNT; }
void ui_el_show(int el, bool v) { if (el_ok(el)) s_el[el].flags = v ? (s_el[el].flags | 2) : (s_el[el].flags & ~2); }
bool ui_el_visible(int el) { return el_ok(el) && (s_el[el].flags & 2); }
void ui_el_set_var(int el, uint8_t v) { if (el_ok(el)) s_var[STOCK_ELEMS[el].desc] = v; }
uint8_t ui_el_var(int el) { return el_ok(el) ? s_var[STOCK_ELEMS[el].desc] : 0; }
void ui_el_move(int el, int x, int y) { if (el_ok(el)) { s_el[el].x = (uint8_t)x; s_el[el].y = (uint8_t)y; } }
void ui_el_resize(int el, int w, int h) { if (el_ok(el)) { s_el[el].w = (uint8_t)w; s_el[el].h = (uint8_t)h; } }

// Colour-key rule of the stock compositor, selected by the element's BASE picture id:
// 0 = opaque, 1 = black (0x0000) is transparent, else the 16-bit key (as the two
// picture bytes read little-endian, like stock).
static uint16_t key_for(uint16_t b)
{
    if (b == 185) return 0x4646;
    if (b == 195) return 0xc7f9;
    if (b > 937) return 1;
    if (b >= 129 && b <= 133) return 1;
    if (b >= 842 && b <= 847) return 1;
    if (b >= 849 && b <= 874 && b != 853 && b != 866) return 1;
    switch (b) {
    case 0: case 53: case 118: case 297: case 331: case 348: case 365: case 382: case 416:
    case 592: case 609: case 626: case 661: case 708: case 709: case 715: case 732: case 733: case 735:
        return 1;
    }
    return 0;
}

// Copy one row of w picture pixels into the frame at (x, y). Like stock, a row that
// does not fit the frame is dropped whole (x + w <= 80, y < 160).
static void blit_row(uint16_t base, int x, int y, int w, const uint8_t *src)
{
    if (!s_fb || x < 0 || y < 0 || w <= 0 || x + w > UI_W || y >= UI_H) return;
    uint16_t key = key_for(base);
    uint8_t *dst = s_fb + (y * UI_W + x) * 2;
    if (key == 0) { memcpy(dst, src, (size_t)w * 2); return; }
    for (int i = 0; i < w; i++, src += 2, dst += 2) {
        uint16_t p = (uint16_t)(src[0] | (src[1] << 8));
        if (key == 1 ? p == 0 : p == key) continue;
        dst[0] = src[0]; dst[1] = src[1];
    }
}

static bool res_read(uint32_t off, void *dst, size_t len)
{
    if (!s_rd || off > RES_LIMIT) return false;
    return s_rd(off, dst, len);
}

void ui_render_pic(uint16_t base, uint8_t variant, int x, int y, int w, int h)
{
    static uint8_t line[240];
    if (h > UI_H || w > UI_W || w <= 0 || h <= 0) return;
    uint32_t idx;
    if (variant == 100)      idx = (uint32_t)s_lang + 609;
    else if (variant == 101) idx = (uint32_t)s_lang + 592;
    else if (variant == 102) idx = (uint32_t)s_lang + 626;
    else                     idx = (uint32_t)base + variant;

    if (base == 998) {                       // full-screen custom picture slot, two banks
        uint32_t off = s_bank_b ? PIC998_BANK_B : PIC998_BANK_A;
        for (int r = 0; r < h; r++, off += (uint32_t)w * 2) {
            if (!res_read(off, line, (size_t)w * 2)) return;
            blit_row(base, x, y + r, w, line);
        }
        return;
    }
    if (base >= 927) {                       // zone overlays: 80 px x {b0, b1, alpha}
        if (idx < 927 || idx > 950) return;
        uint32_t off = ZONE_BASE + STOCK_ZONE_OFF[idx - 927];
        int row = base > 937 ? 64 : 20;
        for (int r = 0; r < ZONE_ROWS; r++, row++, off += 240) {
            if (!res_read(off, line, 240)) return;
            // Runs of pixels with alpha != 0 are blitted; like stock, a run is only
            // flushed when a transparent pixel follows it.
            int run = 0;
            static uint8_t px[UI_W * 2];
            for (int c = 0; c < UI_W; c++) {
                if (line[c * 3 + 2] == 0) {
                    if (run) { blit_row(base, x + run, row, c - run, px + run * 2); run = 0; }
                } else {
                    px[c * 2] = line[c * 3]; px[c * 2 + 1] = line[c * 3 + 1];
                    if (!run) run = c;
                }
            }
        }
        return;
    }
    if (idx >= STOCK_PIC_COUNT) return;
    uint32_t off = STOCK_PIC_OFF[idx];
    for (int r = 0; r < h; r++, off += (uint32_t)w * 2) {
        if (y + r >= UI_H) break;            // rows below the frame are clipped
        if (!res_read(off, line, (size_t)w * 2)) return;
        blit_row(base, x, y + r, w, line);
    }
}

void ui_render_elem(int el)
{
    if (!el_ok(el) || !(s_el[el].flags & 2)) return;
    uint8_t d = STOCK_ELEMS[el].desc;
    if (s_var[d] == 0xff) s_var[d] = 0;      // stock 0x42023c88
    ui_render_pic(STOCK_DESCS[d].base_id, s_var[d], s_el[el].x, s_el[el].y, s_el[el].w, s_el[el].h);
}

void ui_render_clear(void) { if (s_fb) memset(s_fb, 0, UI_FB_BYTES); }

void ui_render_list(const uint8_t *elems)
{
    if (!elems) return;
    for (; *elems != 0xff; elems++) ui_render_elem(*elems);
}

void ui_render_fill(int x, int y, int w, int h, uint16_t c)
{
    if (!s_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    for (int r = 0; r < h; r++) {
        uint8_t *p = s_fb + ((y + r) * UI_W + x) * 2;
        for (int i = 0; i < w; i++) { *p++ = (uint8_t)(c >> 8); *p++ = (uint8_t)c; }
    }
}

const uint8_t *ui_screen_list(int id)
{
    for (unsigned i = 0; i < sizeof(STOCK_SCREENS) / sizeof(STOCK_SCREENS[0]); i++)
        if (STOCK_SCREENS[i].id == id) return STOCK_SCREENS[i].elems;
    return NULL;
}

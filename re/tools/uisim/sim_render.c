// Host check of the picture renderer: draw every stock screen list with its power-on
// element state and write PPMs.  cc -I main sim_render.c main/ui_render.c -o sim_render
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_render.h"

static FILE *g_res;
static bool rd(uint32_t off, void *dst, size_t len)
{
    return fseek(g_res, off, SEEK_SET) == 0 && fread(dst, 1, len, g_res) == len;
}
static void save(const uint8_t *fb, const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
    for (int i = 0; i < UI_W * UI_H; i++) {
        unsigned v = (fb[2 * i] << 8) | fb[2 * i + 1];
        unsigned char px[3] = { ((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31 };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s res.bin outdir\n", argv[0]); return 2; }
    g_res = fopen(argv[1], "rb");
    if (!g_res) { perror(argv[1]); return 1; }
    static uint8_t fb[UI_FB_BYTES];
    ui_render_init(fb, rd);
    for (int id = 70; id <= 120; id++) {
        const uint8_t *l = ui_screen_list(id);
        if (!l) continue;
        ui_el_reset_all();
        ui_render_clear();
        ui_render_list(l);
        char p[512]; snprintf(p, sizeof p, "%s/scr_%03d.ppm", argv[2], id);
        save(fb, p);
    }
    return 0;
}

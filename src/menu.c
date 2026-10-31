#include "menu.h"
#include "pico/stdlib.h"
#include "draw.h"
#include <string.h>

#define SCALE       2
#define GW          (GLYPH_W * SCALE)
#define GH          (GLYPH_H * SCALE)
#define BAN_PAD     24
#define BAN_BORDER  4
#define BAN_H       (GH + 24)

void banner_rect(const char *text, int *x, int *y, int *w, int *h)
{
    int tw = (int)strlen(text) * GW + 2 * BAN_PAD;
    if (tw > IT8951_W - 32) tw = IT8951_W - 32;
    *w = (tw + 15) & ~15;
    *h = BAN_H;
    *x = 16;
    *y = IT8951_H - 16 - BAN_H;
}

void banner_overlay_row(const char *text, int y, uint8_t row[IT8951_ROW_BYTES])
{
    int bx, by, bw, bh;
    banner_rect(text, &bx, &by, &bw, &bh);
    if (y < by || y >= by + bh) return;
    static uint8_t lines[2][IT8951_W];      /* one per core: preview on 0, panel on 1 */
    uint8_t *line = lines[get_core_num()];
    int ly = y - by;
    memset(line, G_WHITE, (size_t)bw);
    if (ly < BAN_BORDER || ly >= bh - BAN_BORDER) memset(line, G_BLACK, (size_t)bw);
    else { memset(line, G_BLACK, BAN_BORDER); memset(line + bw - BAN_BORDER, G_BLACK, BAN_BORDER); }
    draw_text_row(line, bw, BAN_PAD, text, ly - 12, SCALE, G_BLACK);
    draw_pack(row, bx, line, bw);
}

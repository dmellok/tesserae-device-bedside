#include "draw.h"
#include "fonts.h"
#include <string.h>

int draw_text_w(const char *text, int scale)
{
    return text ? (int)strlen(text) * GLYPH_W * scale : 0;
}

void draw_text_row(uint8_t *line, int w, int x, const char *text, int grow, int scale, uint8_t colour)
{
    if (!text || scale < 1 || grow < 0 || grow >= GLYPH_H * scale) return;
    const uint8_t *tbl = Font24.table + (grow / scale) * 3;
    for (; *text; text++, x += GLYPH_W * scale) {
        int c = (unsigned char)*text;
        if (c < 32 || c > 126) c = '?';
        const uint8_t *g = tbl + (c - 32) * 72;
        for (int col = 0; col < GLYPH_W; col++) {
            if (!(g[col >> 3] & (0x80 >> (col & 7)))) continue;
            for (int s = 0; s < scale; s++) {
                int px = x + col * scale + s;
                if (px >= 0 && px < w) line[px] = colour;
            }
        }
    }
}

void draw_pack(uint8_t *row, int x0, const uint8_t *line, int n)
{
    for (int px = 0; px < n; px++) {
        int x = x0 + px;
        if (x < 0 || x >= IT8951_W) continue;
        uint8_t g = line[px];
        if (x & 1) row[x >> 1] = (uint8_t)((row[x >> 1] & 0xF0) | g);
        else       row[x >> 1] = (uint8_t)((row[x >> 1] & 0x0F) | (g << 4));
    }
}

static const bglyph_t *glyph(const bfont_t *f, unsigned code)
{
    int lo = 0, hi = (int)f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].code == code) return &f->glyphs[mid];
        if (f->glyphs[mid].code < code) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

int draw_font_w(const bfont_t *f, const char *text)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)text; p && *p; p++) {
        const bglyph_t *g = glyph(f, *p);
        w += g ? g->adv : f->line_h / 3;
    }
    return w;
}

void draw_font_row(const bfont_t *f, uint8_t *line, int w, int x, const char *text, int grow, uint8_t colour)
{
    if (!text || grow < 0 || grow >= f->line_h) return;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        const bglyph_t *g = glyph(f, *p);
        if (!g) { x += f->line_h / 3; continue; }
        int gy = grow - g->yoff;
        if (gy >= 0 && gy < g->h) {
            const uint8_t *row = f->bits + g->off + (size_t)gy * ((g->w + 1) / 2);
            for (int i = 0; i < g->w; i++) {
                int px = x + g->xoff + i;
                if (px < 0 || px >= w) continue;
                int a = (i & 1) ? (row[i >> 1] & 0x0F) : (row[i >> 1] >> 4);
                if (!a) continue;
                int cur = line[px];
                line[px] = (uint8_t)(cur + ((colour - cur) * a + 7) / 15);
            }
        }
        x += g->adv;
    }
}

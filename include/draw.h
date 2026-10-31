/*
 * draw.h: per-row drawing primitives shared by the overlays. Everything works
 * on an 8-bit grey scratch line (one value per logical column, 0x0 black ..
 * 0xF white) that is then packed over the panel row. Font24 (17x24) is the
 * only face; it is drawn at integer scales.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "it8951.h"
#include "bfont.h"

#define G_WHITE 0xF
#define G_BLACK 0x0
#define G_GREY  0x7
#define G_LIGHT 0xB
#define G_DARK  0x3

#define GLYPH_W 17
#define GLYPH_H 24

/* Width in px of `text` at `scale`. */
int  draw_text_w(const char *text, int scale);

/* Paint scan line `grow` (0 .. 24*scale-1) of `text` into line[] from column x. */
void draw_text_row(uint8_t *line, int w, int x, const char *text, int grow, int scale, uint8_t colour);

/* Pack n grey values from line[] over the panel row starting at column x0. */
void draw_pack(uint8_t *row, int x0, const uint8_t *line, int n);

/* Anti-aliased bitmap fonts (bfont.h). Width in px of `text`. */
int  draw_font_w(const bfont_t *f, const char *text);
/* Paint scan line `grow` (0 .. f->line_h-1, measured from the top of the line
 * box) of `text` into line[] from column x, blending the glyph coverage toward
 * `colour` (works for black on white, white on black, and greys). */
void draw_font_row(const bfont_t *f, uint8_t *line, int w, int x, const char *text, int grow, uint8_t colour);

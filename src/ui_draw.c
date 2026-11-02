/*
 * ui_draw.c: the draw list: ops, text and shapes, and rendering a layer row by row
 */
#include "ui_internal.h"

/* ---- draw list ------------------------------------------------------------ */

void layer_reset(layer_t *l, int x, int y, int w, int h, uint8_t bg)
{
    l->n = 0; l->x = x; l->y = y; l->w = w; l->h = h; l->bg = bg; l->on = true;
}

static op_t *op_new(layer_t *l)
{
    if (l->n >= MAX_OPS) return &l->ops[MAX_OPS - 1];
    op_t *o = &l->ops[l->n++];
    memset(o, 0, sizeof *o);
    return o;
}

void op_fill(layer_t *l, int x, int y, int w, int h, uint8_t g)
{ op_t *o = op_new(l); o->kind = OP_FILL; o->x = x; o->y = y; o->w = w; o->h = h; o->g = g; }

void op_frame(layer_t *l, int x, int y, int w, int h, int b, uint8_t g)
{ op_t *o = op_new(l); o->kind = OP_FRAME; o->x = x; o->y = y; o->w = w; o->h = h; o->border = b; o->g = g; }

void op_text(layer_t *l, int x, int y, const bfont_t *f, uint8_t g, const char *t)
{ op_t *o = op_new(l); o->kind = OP_TEXT; o->x = x; o->y = y; o->font = f; o->g = g;
  o->w = draw_font_w(f, t); o->h = f->line_h; snprintf(o->text, sizeof o->text, "%s", t); }

void op_text_c(layer_t *l, int cx, int y, const bfont_t *f, uint8_t g, const char *t)
{ op_text(l, cx - draw_font_w(f, t) / 2, y, f, g, t); }

void op_text_r(layer_t *l, int xr, int y, const bfont_t *f, uint8_t g, const char *t)
{ op_text(l, xr - draw_font_w(f, t), y, f, g, t); }

void op_rrect(layer_t *l, int x, int y, int w, int h, int r, uint8_t g)
{ op_t *o = op_new(l); o->kind = OP_RRECT; o->x = x; o->y = y; o->w = w; o->h = h; o->border = (uint8_t)r; o->g = g; }
void op_line(layer_t *l, int x0, int y0, int x1, int y1, int t, uint8_t g)
{ op_t *o = op_new(l); o->kind = OP_LINE; o->x = x0; o->y = y0; o->w = x1 - x0; o->h = y1 - y0; o->border = (uint8_t)t; o->g = g; }
void op_dash(layer_t *l, int x, int y, int w, int h, int d, uint8_t g)
{ op_t *o = op_new(l); o->kind = OP_DASH; o->x = x; o->y = y; o->w = w; o->h = h; o->border = (uint8_t)d; o->g = g; }
void op_circle(layer_t *l, int cx, int cy, int r, uint8_t g)
{ op_t *o = op_new(l); o->kind = OP_CIRCLE; o->x = cx; o->y = cy; o->w = r; o->g = g; }

__attribute__((unused)) static void op_ring(layer_t *l, int cx, int cy, int r, int b, uint8_t g, uint8_t inner)
{ op_t *o = op_new(l); o->kind = OP_RING; o->x = cx; o->y = cy; o->w = r; o->border = b; o->g = g; o->h = inner; }

static int isqrt_i(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

static void layer_row(const layer_t *l, int y, uint8_t *line, uint8_t row[IT8951_ROW_BYTES])
{
    if (!l->on || y < l->y || y >= l->y + l->h) return;
    /* Only the columns being streamed: a 59 px gutter beside the menu used to
     * cost a full-width render of every row. Ops entirely outside are skipped. */
    int wx0, wx1; it8951_stream_window(&wx0, &wx1);
    if (wx0 < l->x) wx0 = l->x;
    if (wx1 > l->x + l->w) wx1 = l->x + l->w;
    if (wx1 <= wx0) return;
    memset(line + wx0, l->bg, (size_t)(wx1 - wx0));
    for (int i = 0; i < l->n; i++) {
        const op_t *o = &l->ops[i];
        if (o->kind == OP_FILL || o->kind == OP_FRAME || o->kind == OP_TEXT || o->kind == OP_RRECT || o->kind == OP_DASH) {
            if (o->x >= wx1 || o->x + o->w <= wx0) continue;
        }
        switch (o->kind) {
        case OP_FILL:
            if (y >= o->y && y < o->y + o->h) memset(line + o->x, o->g, (size_t)o->w);
            break;
        case OP_FRAME:
            if (y < o->y || y >= o->y + o->h) break;
            if (y < o->y + o->border || y >= o->y + o->h - o->border) memset(line + o->x, o->g, (size_t)o->w);
            else { memset(line + o->x, o->g, (size_t)o->border); memset(line + o->x + o->w - o->border, o->g, (size_t)o->border); }
            break;
        case OP_TEXT:
            if (y >= o->y && y < o->y + o->h) draw_font_row(o->font, line, W, o->x, o->text, y - o->y, o->g);
            break;
        case OP_CIRCLE: {
            int dy = y - o->y;
            if (dy < -o->w || dy > o->w) break;
            int half = isqrt_i(o->w * o->w - dy * dy);
            int x0 = o->x - half; if (x0 < 0) x0 = 0;
            int x1 = o->x + half; if (x1 >= W) x1 = W - 1;
            memset(line + x0, o->g, (size_t)(x1 - x0 + 1));
            break; }
        case OP_RRECT: {
            if (y < o->y || y >= o->y + o->h) break;
            int r = o->border, d = y - o->y, d2 = o->y + o->h - 1 - y;
            if (d2 < d) d = d2;
            int in = 0;
            if (d < r) { int k = r - d; in = r - isqrt_i(r * r - k * k); }
            if (o->w - 2 * in > 0) memset(line + o->x + in, o->g, (size_t)(o->w - 2 * in));
            break; }
        case OP_LINE: {
            int t = o->border, x0 = o->x, y0 = o->y, x1 = o->x + o->w, y1 = o->y + o->h;
            int ylo = (y0 < y1 ? y0 : y1) - t / 2, yhi = (y0 > y1 ? y0 : y1) + t / 2;
            if (y < ylo || y > yhi) break;
            int xl = x0 < x1 ? x0 : x1, xr = x0 > x1 ? x0 : x1, a, b;
            if (y1 == y0) { a = xl; b = xr; }
            else {
                a = x0 + (y - t / 2 - y0) * (x1 - x0) / (y1 - y0);
                b = x0 + (y + t / 2 - y0) * (x1 - x0) / (y1 - y0);
                if (a > b) { int s = a; a = b; b = s; }
                if (a < xl) a = xl;
                if (b > xr) b = xr;
                if (a > b) break;
            }
            a -= t / 2; b += t / 2;
            if (a < 0) a = 0;
            if (b >= W) b = W - 1;
            memset(line + a, o->g, (size_t)(b - a + 1));
            break; }
        case OP_DASH:
            if (y < o->y || y >= o->y + o->h) break;
            for (int xx = o->x; xx < o->x + o->w; xx += 2 * o->border) {
                int e = xx + o->border; if (e > o->x + o->w) e = o->x + o->w;
                memset(line + xx, o->g, (size_t)(e - xx));
            }
            break;
        case OP_RING: {
            int dy = y - o->y;
            if (dy < -o->w || dy > o->w) break;
            int half = isqrt_i(o->w * o->w - dy * dy);
            memset(line + o->x - half, o->g, (size_t)(2 * half + 1));
            int ri = o->w - o->border;
            if (dy >= -ri && dy <= ri) {
                int hi = isqrt_i(ri * ri - dy * dy);
                memset(line + o->x - hi, (uint8_t)o->h, (size_t)(2 * hi + 1));
            }
            break; }
        }
    }
    draw_pack(row, wx0, line + wx0, wx1 - wx0);
}

/* An op's footprint on the panel. */
void op_bbox(const op_t *o, int *x, int *y, int *w, int *h)
{
    switch (o->kind) {
    case OP_CIRCLE: case OP_RING: *x = o->x - o->w; *y = o->y - o->w; *w = 2 * o->w + 2; *h = 2 * o->w + 2; break;
    case OP_LINE: { int x0 = o->w < 0 ? o->x + o->w : o->x, y0 = o->h < 0 ? o->y + o->h : o->y;
                    *x = x0 - o->border; *y = y0 - o->border; *w = (o->w < 0 ? -o->w : o->w) + 2 * o->border + 1; *h = (o->h < 0 ? -o->h : o->h) + 2 * o->border + 1; break; }
    default: *x = o->x; *y = o->y; *w = o->w; *h = o->h; break;
    }
}
/* Each core has its own scratch line: core 0 renders the web preview from the
 * live layers while core 1 paints the panel from the snapshot. */
static uint8_t s_line[2][W];

void local_overlay_row(int y, uint8_t row[IT8951_ROW_BYTES])
{
    uint8_t *line = s_line[get_core_num()];
    layer_row(&L_body, y, line, row);
    layer_row(&L_top, y, line, row);
    layer_row(&L_sb, y, line, row);
    layer_row(&L_ring, y, line, row);      /* the alarm screen and confirmations: over everything */
}

/* The panel paints from a frozen copy of the layers, so core 0 can rebuild
 * them while core 1 is still streaming the previous state. */
static layer_t P_top, P_body, P_sb, P_ring;

void layer_copy(layer_t *d, const layer_t *s)
{
    d->n = s->n; d->x = s->x; d->y = s->y; d->w = s->w; d->h = s->h; d->bg = s->bg; d->on = s->on;
    memcpy(d->ops, s->ops, sizeof(op_t) * (size_t)s->n);
}

void local_snapshot(void)
{
    layer_copy(&P_top, &L_top); layer_copy(&P_body, &L_body);
    layer_copy(&P_sb, &L_sb);   layer_copy(&P_ring, &L_ring);
}

void local_snapshot_row(int y, uint8_t row[IT8951_ROW_BYTES])
{
    uint8_t *line = s_line[get_core_num()];
    layer_row(&P_body, y, line, row);
    layer_row(&P_top, y, line, row);
    layer_row(&P_sb, y, line, row);
    layer_row(&P_ring, y, line, row);
}

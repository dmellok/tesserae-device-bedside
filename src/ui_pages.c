/*
 * ui_pages.c: the Live page's other layouts (Settings > Home screen > Live
 * layout): a big clock, the weather, and tonight's plan. The bento lives in
 * ui_live.c, which also owns the values, the demo data and the helpers used
 * here. Each layout builds the whole body (TB_H to SB_Y); build_live() inverts
 * the finished layer for the dark colours, and the minute tick repaints only
 * the ops that changed (mark_changed), so a layout need not plan its regions.
 */
#include "ui_internal.h"

#define BODY_H (SB_Y - TB_H)

static void clock_text(char *b, size_t n, int *sec_out)
{
    int wd = 0, sec = 0; int64_t mn = 0;
    bool have = clock_now(&wd, &sec, &mn);
    if (sec_out) *sec_out = have ? sec : -1;
    if (!have) snprintf(b, n, "--:--");
    else if (S.clock12) { int hh = sec / 3600 % 12; snprintf(b, n, "%d:%02d", hh ? hh : 12, sec / 60 % 60); }
    else snprintf(b, n, "%02d:%02d", sec / 3600, sec / 60 % 60);
}

/* "in 7 h 20 min" (to 5 min, 15 past three hours, so it changes rarely), "in 40 min", "on Saturday". */
static void alarm_rel(char *c, size_t n, int32_t delta)
{
    int wd = 0, sec = 0; int64_t mn = 0; clock_now(&wd, &sec, &mn);
    int mins = (delta + 59) / 60;
    static const char *const DAYF[7] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
    if (delta < 0) c[0] = 0;
    else if (mins >= 24 * 60) snprintf(c, n, "on %s", DAYF[(wd + (sec + delta) / 86400) % 7]);
    else if (mins >= 60) {
        int step = mins >= 180 ? 15 : 5, r = (mins + step / 2) / step * step;
        if (r % 60) snprintf(c, n, "in %d h %d min", r / 60, r % 60); else snprintf(c, n, "in %d h", r / 60);
    } else snprintf(c, n, "in %d min", mins);
}

/* An icon and a line of text, centred on cx; the text in f, the icon beside it. */
static void fact_c(layer_t *l, int cx, int y, const char *icon, const bfont_t *f, const char *t)
{
    const bfont_t *fi = f == F_L ? F_IM : F_IR;      /* the icon sized to the text beside it */
    int iw = draw_font_w(fi, icon), w = iw + 20 + draw_font_w(f, t), x = cx - w / 2;
    op_text(l, x, y + (f->line_h - fi->line_h) / 2, fi, G_BLACK, icon);
    op_text(l, x + iw + 20, y, f, G_BLACK, t);
}

/* A small label over a value, left-aligned in a column w wide. */
static void column(layer_t *l, int x, int y, int w, const char *label, const char *value)
{
    op_text(l, x, y, F_LBL, 0x4, label);
    op_text(l, x, y + 52, lv_fit(value, w, F_M, F_LBL, F_S), G_BLACK, value);
}

/* ---- Big clock: the time, then the alarm, the sound and the weather in a line each ---- */
void build_live_clock(layer_t *l)
{
    layer_reset(l, 0, TB_H, W, BODY_H, G_WHITE);
    char b[96], c[96];
    clock_text(b, sizeof b, NULL);
    int y = TB_H + 150;
    clock_slots(l, (W - clock_slots(NULL, 0, 0, &num270, G_BLACK, b)) / 2, y, &num270, G_BLACK, b);
    y += num270.line_h + 90;
    int h, m, early; int32_t delta;
    if (live_alarm(&h, &m, &early, &delta)) {
        int cm = ((h * 60 + m - early) % 1440 + 1440) % 1440;
        snprintf(c, sizeof c, "Alarm %02d:%02d " DOT " %s %02d:%02d", h, m, S.live_demo || S.birds ? "dawn chorus" : "wake window", cm / 60, cm % 60);
    } else snprintf(c, sizeof c, "No alarm set");
    fact_c(l, W / 2, y, ICON_ALARM, F_L, c); y += 135;
    live_sound_line(c, sizeof c);
    fact_c(l, W / 2, y, S.rain_on || S.live_demo ? ICON_SPEAKER_HIGH : ICON_PAUSE, F_L, c); y += 135;
    char t[16], w[48], bt[16];
    fmt_temp(LV_OUT_TEMP, t, sizeof t); fmt_word(live_val(LV_OUT_COND), w, sizeof w); fmt_temp(LV_BED_TEMP, bt, sizeof bt);
    snprintf(c, sizeof c, "%s %.30s " DOT " bedroom %s", t, w, bt);
    fact_c(l, W / 2, y, ICON_CLOUD_SUN, lv_fit(c, W - 300, F_L, F_M, F_M), c);
}

/* ---- Weather: outside and the bedroom on the left, the hourly chance of rain on the right ---- */
void build_live_weather(layer_t *l)
{
    layer_reset(l, 0, TB_H, W, BODY_H, G_WHITE);
    char b[96], c[96];
    int x = 36, y = TB_H + 20, lw = 860, ch = BODY_H - 40;
    op_rrect(l, x, y, lw, ch, 36, 0xE);
    card_title(l, x, y, lw, ICON_CLOUD_SUN, "Outside", false, NULL);
    fmt_temp(LV_OUT_TEMP, b, sizeof b);
    op_text(l, x + 40, y + 100, F_NUM, G_BLACK, b);
    fmt_word(live_val(LV_OUT_COND), c, sizeof c);
    op_text(l, x + 40, y + 340, lv_fit(c, lw - 80, F_XL, F_M, F_LBL), G_BLACK, c);
    char hi[16], lo[16]; fmt_temp(LV_OUT_HI, hi, sizeof hi); fmt_temp(LV_OUT_LO, lo, sizeof lo);
    snprintf(c, sizeof c, "High %s " DOT " Low %s", hi, lo);
    op_text(l, x + 40, y + 480, F_M, G_BLACK, c);
    op_fill(l, x + 40, y + ch - 340, lw - 80, 2, 0xB);
    op_text(l, x + 40, y + ch - 300, F_LBL, 0x4, "Bedroom");
    fmt_temp(LV_BED_TEMP, b, sizeof b);
    op_text(l, x + 40, y + ch - 250, F_XL, G_BLACK, b);
    if (live_is_known(LV_BED_HUM)) snprintf(c, sizeof c, "%s%% humidity", live_val(LV_BED_HUM)); else snprintf(c, sizeof c, "Humidity --");
    size_t k = strlen(c);
    if (S.live_demo) snprintf(c + k, sizeof c - k, " " DOT " Cool 22" DEG);
    else if (S.ac_mode) snprintf(c + k, sizeof c - k, " " DOT " %s %d" DEG, AC_MODES[S.ac_mode], S.sp_x2 / 2);
    op_text(l, x + 40, y + ch - 120, lv_fit(c, lw - 80, F_M, F_LBL, F_S), G_BLACK, c);

    int rx = 940, rw = W - 36 - rx;
    op_rrect(l, rx, y, rw, ch, 36, 0xE);
    card_title(l, rx, y, rw, ICON_CLOUD_RAIN, "Chance of rain", false, NULL);
    clock_text(b, sizeof b, NULL);                              /* the time, small, in the title row */
    clock_slots(l, rx + rw - 40 - clock_slots(NULL, 0, 0, F_XL, G_BLACK, b), y + 12, F_XL, G_BLACK, b);
    int vals[8], nv = 0;
    for (const char *p = live_val(LV_RAIN); *p && nv < 8; ) {
        while (*p == ' ' || *p == ',' || *p == '[') p++;
        if (!*p || *p == ']') break;
        vals[nv++] = atoi(p);
        while (*p && *p != ',') p++;
    }
    int cw = 84, gap = (rw - 64 - 8 * cw) / 7, bottom = y + ch - 110, maxh = 540;
    if (!nv) op_text(l, rx + 40, y + 180, F_M, G_BLACK, "No forecast yet");
    for (int i = 0; i < nv; i++) {
        int v = vals[i] < 0 ? 0 : vals[i] > 100 ? 100 : vals[i];
        int bh = 24 + v * maxh / 100, bx = rx + 32 + i * (cw + gap);
        op_rrect(l, bx, bottom - bh, cw, bh, 14, v >= 60 ? G_BLACK : v >= 30 ? 0x6 : 0xA);
        snprintf(c, sizeof c, "%d%%", v);
        op_text(l, bx + (cw - draw_font_w(F_LBL, c)) / 2, bottom - bh - 14 - F_LBL->line_h, F_LBL, G_BLACK, c);
        const char *lab = i == 0 ? "Now" : (snprintf(c, sizeof c, "+%dh", i), c);
        op_text(l, bx + (cw - draw_font_w(F_LBL, lab)) / 2, bottom + 16, F_LBL, 0x4, lab);
    }
}

/* ---- Tonight: the clock and the alarm, the night as a line from now to the alarm, then the room ---- */
static void tl_mark(layer_t *l, int cx, int align, int ly, const char *t, const char *label, bool big)
{
    /* align: 0 centre, 1 start (cx is the left edge), 2 end (cx is the right edge) */
    int r = big ? 16 : 11;
    const bfont_t *ft = big ? F_L : F_M;
    int tw = draw_font_w(ft, t), lw = label && label[0] ? draw_font_w(F_LBL, label) : 0;
    int dot = align == 1 ? cx + r : align == 2 ? cx - r : cx;
    int tx = align == 1 ? cx : align == 2 ? cx - tw : cx - tw / 2;
    int lx = align == 1 ? cx : align == 2 ? cx - lw : cx - lw / 2;
    op_text(l, tx, ly - 34 - ft->line_h, ft, G_BLACK, t);
    op_circle(l, dot, ly + 3, r, G_BLACK);
    if (lw) op_text(l, lx, ly + r + 22, F_LBL, 0x4, label);
}

void build_live_tonight(layer_t *l)
{
    layer_reset(l, 0, TB_H, W, BODY_H, G_WHITE);
    char b[96], c[96]; int sec;
    clock_text(b, sizeof b, &sec);
    clock_slots(l, 68, TB_H + 40, &clock214, G_BLACK, b);
    int h, m, early; int32_t delta; bool alarm = live_alarm(&h, &m, &early, &delta);
    int am = h * 60 + m, cm = ((am - early) % 1440 + 1440) % 1440, xr = W - 68;
    if (alarm) snprintf(c, sizeof c, "%02d:%02d", h, m); else snprintf(c, sizeof c, "No alarm");
    op_text_r(l, xr, TB_H + 40, F_XL, G_BLACK, c);
    if (alarm) alarm_rel(c, sizeof c, delta); else snprintf(c, sizeof c, "Set one from Menu, Alarms");
    op_text_r(l, xr, TB_H + 176, F_M, 0x4, c);
    if (alarm) snprintf(c, sizeof c, "%s from %02d:%02d", S.live_demo || S.birds ? "Dawn chorus" : "Wake window", cm / 60, cm % 60);
    else c[0] = 0;
    op_text_r(l, xr, TB_H + 240, F_M, G_BLACK, c);

    /* the night: now -> sound stops -> sleep -> chorus -> sunrise -> alarm, on a rail */
    int x0 = 68, tw = W - 2 * 68, ly = TB_H + 600;
#define TLX(p) (x0 + tw * (p) / 100)
    op_fill(l, x0, ly, TLX(22) - x0, 6, 0xC);
    op_fill(l, x0, ly, TLX(4) - x0, 6, G_BLACK);
    op_dash(l, TLX(22), ly, TLX(64) - TLX(22), 6, 18, 0xB);
    op_fill(l, TLX(64), ly, TLX(100) - TLX(64), 6, 0xC);
    {   int sw = draw_font_w(F_IM, ICON_MOON) + 14 + draw_font_w(F_M, "Sleep"), sx = TLX(43) - sw / 2;
        op_fill(l, sx - 20, ly - 30, sw + 40, 66, G_WHITE);
        op_text(l, sx, ly + 3 - F_IM->line_h / 2, F_IM, 0x5, ICON_MOON);
        op_text(l, sx + draw_font_w(F_IM, ICON_MOON) + 14, ly + 3 - F_M->line_h / 2, F_M, 0x5, "Sleep");
    }
    tl_mark(l, x0, 1, ly, "Now", "", false);
    int left = S.live_demo ? 5 * 3600 : sleep_left_s();
    if (left > 0 && sec >= 0) {
        int end = (sec + left) / 60 % 1440;
        snprintf(b, sizeof b, "%02d:%02d", end / 60, end % 60);
        tl_mark(l, TLX(14), 0, ly, b, S.rain_on || S.live_demo ? "Sound stops" : "Timer ends", false);
    }
    if (alarm) {
        snprintf(b, sizeof b, "%02d:%02d", cm / 60, cm % 60);
        tl_mark(l, TLX(70), 0, ly, b, S.live_demo || S.birds ? "Chorus" : "Wake window", false);
        int sr = live_sunrise_today(am);
        if (sr >= 0 && sr > cm && sr < am) { snprintf(b, sizeof b, "%02d:%02d", sr / 60, sr % 60); tl_mark(l, TLX(84), 0, ly, b, "Sunrise", false); }
        snprintf(b, sizeof b, "%02d:%02d", h, m);
        tl_mark(l, x0 + tw, 2, ly, b, "Alarm", true);
    }
#undef TLX

    /* the room, three columns */
    int by = TB_H + 820, colw = 540;
    live_sound_line(c, sizeof c);
    column(l, 68, by, colw, "Sound", c);
    char bt[16]; fmt_temp(LV_BED_TEMP, bt, sizeof bt);
    if (S.live_demo) snprintf(c, sizeof c, "%s " DOT " Cool 22" DEG, bt);
    else if (S.ac_mode) snprintf(c, sizeof c, "%s " DOT " %s %d" DEG, bt, AC_MODES[S.ac_mode], S.sp_x2 / 2);
    else if (live_is_known(LV_BED_HUM)) snprintf(c, sizeof c, "%s " DOT " %s%% humidity", bt, live_val(LV_BED_HUM));
    else snprintf(c, sizeof c, "%s", bt);
    column(l, 68 + 600, by, colw, "Bedroom", c);
    bool attention; live_home_line(c, sizeof c, &attention);
    if (attention) {                                        /* a black card: something needs you */
        int cw2 = draw_font_w(F_M, c) + 64;
        op_rrect(l, 68 + 1200 - 24, by + 40, cw2 > colw + 48 ? colw + 48 : cw2, 84, 20, G_BLACK);
        op_text(l, 68 + 1200, by, F_LBL, 0x4, "Home");
        op_text(l, 68 + 1200 + 8, by + 54, lv_fit(c, colw, F_M, F_LBL, F_S), G_WHITE, c);
    } else column(l, 68 + 1200, by, colw, "Home", c);
}

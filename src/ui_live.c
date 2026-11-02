/*
 * ui_live.c: the live dashboard, the status bar, date and clock
 */
#include "ui_internal.h"

/* ---- date ------------------------------------------------------------------- */
void local_set_time(const char *iso)
{
    char own[16];
    if (S.tz || !S.auto_time) {                       /* the device's own zone or time: its own date */
        int y, m, d, wd;
        if (local_ymd(&y, &m, &d, &wd)) { snprintf(own, sizeof own, "%04d-%02d-%02d", y, m, d); iso = own; }
    }
    static const char *const DN[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const MN[12] = { "January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December" };
    int y, m, d;
    if (!iso || sscanf(iso, "%d-%d-%d", &y, &m, &d) != 3) return;
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = y - (m < 3);
    int wd = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + d) % 7;
    char b[40];
    snprintf(b, sizeof b, "%s %d %s", DN[wd], d, MN[(m - 1) % 12]);
    if (strcmp(b, s_date) != 0) { snprintf(s_date, sizeof s_date, "%s", b); rebuild(); mark(0, 0, 1080, TB_H, IT8951_MODE_GL16); }
}

/* ---- the status bar ------------------------------------------------------------
 * Left: what is playing (volume, sound). Right, right-aligned: tonight's settings
 * when they are set (sleep timer, alarm, A/C, lights), then the links (Home
 * Assistant, Wi-Fi). The unusual (night mode, battery, offline) only when true. */

static int sb_item_r(layer_t *l, int xr, int cy, const char *icon, const char *text, uint8_t fg)
{
    int tw = text && text[0] ? draw_font_w(F_LBL, text) : 0;
    int x = xr - tw;
    if (tw) op_text(l, x, cy - F_LBL->line_h / 2, F_LBL, fg, text);
    if (icon) { x -= (tw ? 14 : 0) + draw_font_w(F_I, icon); op_text(l, x, cy - F_I->line_h / 2, F_I, fg, icon); }
    return x;
}

void build_status(layer_t *l, bool inv)
{
    uint8_t fg = inv ? G_WHITE : G_BLACK, bg = inv ? G_BLACK : G_WHITE, dim = inv ? G_LIGHT : 0x5, track = inv ? G_DARK : 0xD;
    layer_reset(l, 0, SB_Y, W, SB_H, bg);
    op_fill(l, 0, SB_Y, W, 3, inv ? G_DARK : G_BLACK);
    int cy = SB_Y + 3 + (SB_H - 3) / 2;
    char b[40];
    /* left: volume, then the sound */
    int x = MARGIN;
    op_text(l, x, cy - F_I->line_h / 2, F_I, fg, S.vol ? ICON_SPEAKER_HIGH : ICON_SPEAKER_SLASH);
    x += draw_font_w(F_I, ICON_SPEAKER_HIGH) + 16;
    op_rrect(l, x, cy - 4, 220, 8, 4, track);
    if (S.vol) op_rrect(l, x, cy - 4, 220 * S.vol / 10 < 8 ? 8 : 220 * S.vol / 10, 8, 4, fg);
    x += 220 + 16;
    snprintf(b, sizeof b, "%d", S.vol);
    op_text(l, x, cy - F_LBL->line_h / 2, F_LBL, fg, b);
    x += draw_font_w(F_LBL, b) + 48;
    if (S.rain_on) {
        op_text(l, x, cy - F_I->line_h / 2, F_I, fg, ICON_CLOUD_RAIN); x += draw_font_w(F_I, ICON_CLOUD_RAIN) + 14;
        snprintf(b, sizeof b, "%s %d", KINDS[S.rain_kind], S.rain_lvl);
    } else {
        op_text(l, x, cy - F_I->line_h / 2, F_I, dim, ICON_PAUSE); x += draw_font_w(F_I, ICON_PAUSE) + 14;
        snprintf(b, sizeof b, "Paused");
    }
    op_text(l, x, cy - F_LBL->line_h / 2, F_LBL, S.rain_on ? fg : dim, b);

    /* right, from the edge inwards */
    int xr = W - MARGIN;
    if (!net_connected()) xr = sb_item_r(l, xr, cy, ICON_WIFI_SLASH, "Offline", fg) - 30;
    else xr = sb_item_r(l, xr, cy, ICON_WIFI_HIGH, NULL, fg) - 30;
    if (ha_connected()) xr = sb_item_r(l, xr, cy, ICON_HOUSE, NULL, fg) - 30;
    op_fill(l, xr - 2, cy - 28, 2, 56, inv ? G_DARK : 0xC);
    xr -= 2 + 48;
    if (S.on_battery) { snprintf(b, sizeof b, "%d%%", S.batt_pct); xr = sb_item_r(l, xr, cy, ICON_BATTERY_CHARGING, b, fg) - 48; }
    if (S.night) xr = sb_item_r(l, xr, cy, ICON_MOON, NULL, dim) - 48;
    if (S.ac_mode) { snprintf(b, sizeof b, "%s %d" DEG, AC_MODES[S.ac_mode], S.sp_x2 / 2); xr = sb_item_r(l, xr, cy, ICON_SNOWFLAKE, b, fg) - 48; }
    if (S.alarm_on) { alarm_txt(b, sizeof b); xr = sb_item_r(l, xr, cy, ICON_ALARM, b, fg) - 48; }
    int left = sleep_left_s(), wd, sec; int64_t mn;
    if (left > 0 && clock_now(&wd, &sec, &mn)) {
        int end = (sec + left) / 60 % 1440;
        snprintf(b, sizeof b, "Stops %02d:%02d", end / 60, end % 60);
        xr = sb_item_r(l, xr, cy, ICON_TIMER, b, fg) - 48;
    }
    if (S.lights_on) { snprintf(b, sizeof b, "%d%%", S.bright); xr = sb_item_r(l, xr, cy, ICON_LIGHTBULB, b, fg) - 48; }
}

/* ---- the live dashboard (D1) -------------------------------------------------------
 * Panel coordinates. Header (clock, next alarm), the tonight timeline, then a
 * 4 x 2 bento of soft tiles; black only for what needs you. Values come from
 * live.c (Home Assistant) and the device's own state. */

static const bfont_t *fit(const char *t, int w, const bfont_t *a, const bfont_t *b, const bfont_t *c)
{
    if (draw_font_w(a, t) <= w) return a;
    if (draw_font_w(b, t) <= w) return b;
    return c;
}

static bool is_num(const char *v)
{
    if (!*v) return false;
    char *e; strtod(v, &e);
    return e != v && (*e == 0 || *e == ' ');
}

/* "21.43" -> "21.4°", anything else as it came; "--" when unknown. */
static void fmt_temp(int slot, char *b, size_t n)
{
    const char *v = live_value(slot);
    if (!v[0]) { snprintf(b, n, "--"); return; }
    if (is_num(v)) { char d[16]; live_fmt_dec(d, sizeof d, strtod(v, NULL), strchr(v, '.') ? 1 : 0); snprintf(b, n, "%s" DEG, d); }
    else snprintf(b, n, "%s", v);
}

/* "unlocked" -> "Unlocked"; underscores to spaces. */
static void fmt_word(const char *v, char *b, size_t n)
{
    if (!v[0]) { snprintf(b, n, "--"); return; }
    size_t k = 0;
    for (; *v && k + 1 < n; v++) b[k++] = *v == '_' ? ' ' : *v;
    b[k] = 0;
    if (b[0] >= 'a' && b[0] <= 'z') b[0] = (char)(b[0] - 32);
}

static bool word_in(const char *v, const char *const *list)
{
    for (; *list; list++) if (!strcasecmp(v, *list)) return true;
    return false;
}

static void tile_head(layer_t *l, int x, int y, int w, int h, bool dark, const char *icon, const char *label)
{
    op_rrect(l, x, y, w, h, 32, dark ? G_BLACK : G_TILE);
    op_text(l, x + 40, y + 36, F_I, dark ? G_WHITE : G_BLACK, icon);
    op_text(l, x + 40 + draw_font_w(F_I, icon) + 14, y + 36, F_S, dark ? 0xD : 0x5, label);
}

/* Value and one line under it, anchored to the tile's bottom. Returns the value's top. */
static int tile_foot(layer_t *l, int x, int y, int w, int h, bool dark, const char *value, const bfont_t *vf, const char *sub)
{
    int ysub = y + h - 36 - F_LBL->line_h;
    int yval = ysub - 10 - vf->line_h + (vf->line_h - vf->ascent) / 2;
    op_text(l, x + 40, yval, vf, dark ? G_WHITE : G_BLACK, value);
    if (sub && sub[0]) op_text(l, x + 40, ysub, F_LBL, dark ? 0xD : 0x4, sub);
    (void)w;
    return yval;
}

static void tl_mark(layer_t *l, int cx, int align, const char *t, const char *label, bool big)
{
    /* align: 0 centre, 1 start (cx is the left edge), 2 end (cx is the right edge) */
    int r = big ? 13 : 9, ly = LV_TL_Y + 47 + 2;
    const bfont_t *ft = big ? F_LBL : F_S;
    int tw = draw_font_w(ft, t), lw = label && label[0] ? draw_font_w(F_S, label) : 0;
    int dot = align == 1 ? cx + r : align == 2 ? cx - r : cx;
    int tx = align == 1 ? cx : align == 2 ? cx - tw : cx - tw / 2;
    int lx = align == 1 ? cx : align == 2 ? cx - lw : cx - lw / 2;
    op_text(l, tx, LV_TL_Y, ft, G_BLACK, t);
    op_circle(l, dot, ly, r, G_BLACK);
    if (lw) op_text(l, lx, ly + r + 14, F_S, 0x4, label);
}

void build_live(layer_t *l)
{
    layer_reset(l, 0, TB_H, W, SB_Y - TB_H, G_WHITE);
    char b[64], c[64];
    int wday = 0, sec = 0; int64_t minute = 0;
    bool have_clock = clock_now(&wday, &sec, &minute);

    /* header: clock | next alarm */
    if (!have_clock) snprintf(b, sizeof b, "--:--");
    else if (S.clock12) { int hh = sec / 3600 % 12; snprintf(b, sizeof b, "%d:%02d", hh ? hh : 12, sec / 60 % 60); }
    else snprintf(b, sizeof b, "%02d:%02d", sec / 3600, sec / 60 % 60);
    int base = LV_HEAD_Y + LV_HEAD_H - 8;
    clock_slots(l, MARGIN, base - clock214.ascent, &clock214, G_BLACK, b);   /* one op per digit: a minute tick repaints one cell */
    int xd = MARGIN + draw_font_w(&clock214, "00:00") + 56;
    op_fill(l, xd, base - 132, 3, 130, 0xC);
    int xa = xd + 3 + 56;
    int ai; int32_t delta;
    bool alarm = next_alarm(&ai, &delta);
    op_text(l, xa, base - 140 - F_S->ascent, F_S, 0x5, "NEXT ALARM");
    if (alarm) {
        int h, m, on, mask, early;
        local_alarm_get(ai, &h, &m, &on, &mask, &early);
        snprintf(b, sizeof b, "%02d:%02d", h, m);
        op_text(l, xa, base - 58 - ui72.ascent, &ui72, G_BLACK, b);
        int mins = (delta + 59) / 60;
        static const char *const DAYF[7] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
        if (delta < 0) c[0] = 0;
        else if (mins >= 24 * 60) snprintf(c, sizeof c, "on %s", DAYF[(wday + (sec + delta) / 86400) % 7]);
        else if (mins >= 60) {                              /* hours away: to 5 min (15 past 3 h), so the text, and its partial refresh, change rarely */
            int step = mins >= 180 ? 15 : 5, r = (mins + step / 2) / step * step;
            if (r % 60) snprintf(c, sizeof c, "in %d h %d min", r / 60, r % 60); else snprintf(c, sizeof c, "in %d h", r / 60);
        } else snprintf(c, sizeof c, "in %d min", mins);
        op_text(l, xa + draw_font_w(&ui72, b) + 22, base - 58 - F_M->ascent, F_M, 0x4, c);
        int win = early > 0 ? early : 10, cm = ((h * 60 + m - win) % 1440 + 1440) % 1440;
        if (S.birds && S.rain_on) snprintf(c, sizeof c, "Dawn chorus from %02d:%02d " DOT " rain fades out", cm / 60, cm % 60);
        else if (S.birds) snprintf(c, sizeof c, "Dawn chorus from %02d:%02d", cm / 60, cm % 60);
        else snprintf(c, sizeof c, "Wake window from %02d:%02d", cm / 60, cm % 60);
        op_text(l, xa, base - F_LBL->ascent, F_I, 0x4, ICON_BIRD);
        op_text(l, xa + draw_font_w(F_I, ICON_BIRD) + 14, base - F_LBL->ascent, F_LBL, 0x4, c);
    } else {
        op_text(l, xa, base - 58 - ui72.ascent, &ui72, 0x5, "No alarm");
        op_text(l, xa, base - F_LBL->ascent, F_LBL, 0x4, "Set one from Menu, Alarms");
    }

    /* the tonight timeline: now -> sound stops -> (sleep) -> chorus -> sunrise -> alarm */
    int x0 = MARGIN, tw = W - 2 * MARGIN, ly = LV_TL_Y + 47;
#define TLX(p) (x0 + tw * (p) / 100)
    op_fill(l, x0, ly, TLX(22) - x0, 4, 0xC);
    op_fill(l, x0, ly, TLX(4) - x0, 4, G_BLACK);
    op_dash(l, TLX(22), ly, TLX(64) - TLX(22), 4, 14, 0xB);
    op_fill(l, TLX(64), ly, TLX(100) - TLX(64), 4, 0xC);
    {   /* "Sleep" over the dashes */
        int sw = draw_font_w(F_I, ICON_MOON) + 10 + draw_font_w(F_S, "Sleep");
        int sx = TLX(43) - sw / 2;
        op_fill(l, sx - 16, ly - 24, sw + 32, 50, G_WHITE);
        op_text(l, sx, ly + 2 - F_I->line_h / 2, F_I, 0x5, ICON_MOON);
        op_text(l, sx + draw_font_w(F_I, ICON_MOON) + 10, ly + 2 - F_S->line_h / 2, F_S, 0x5, "Sleep");
    }
    tl_mark(l, x0, 1, "Now", "", false);
    int left = sleep_left_s();
    if (left > 0 && have_clock) {
        int end = (sec + left) / 60 % 1440;
        snprintf(b, sizeof b, "%02d:%02d", end / 60, end % 60);
        tl_mark(l, TLX(14), 0, b, S.rain_on ? "Sound stops" : "Timer ends", false);
    }
    if (alarm) {
        int h, m, on, mask, early;
        local_alarm_get(ai, &h, &m, &on, &mask, &early);
        int am = h * 60 + m, win = early > 0 ? early : 10, cm = ((am - win) % 1440 + 1440) % 1440;
        snprintf(b, sizeof b, "%02d:%02d", cm / 60, cm % 60);
        tl_mark(l, TLX(70), 0, b, S.birds ? "Chorus" : "Wake window", false);
        int y = 0, mo = 0, d = 0;
        if (sscanf(tesserae_local_time(), "%d-%d-%d", &y, &mo, &d) == 3) {
            int sr = live_sunrise_min(y, mo, d + (sec / 60 > am ? 1 : 0), tesserae_tz_offset_s());
            if (sr >= 0 && sr > cm && sr < am) { snprintf(b, sizeof b, "%02d:%02d", sr / 60, sr % 60); tl_mark(l, TLX(84), 0, b, "Sunrise", false); }
        }
        snprintf(b, sizeof b, "%02d:%02d", h, m);
        tl_mark(l, x0 + tw, 2, b, "Alarm", true);
    }
#undef TLX

    /* row 1: outside (wide), bedroom, washing */
    {   int x = LV_COL(0), y = LV_ROW1, w = 2 * LV_COL_W + LV_GAP, h = LV_TILE_H;
        tile_head(l, x, y, w, h, false, ICON_CLOUD_RAIN, "OUTSIDE");
        fmt_temp(LV_OUT_TEMP, b, sizeof b);
        op_text(l, x + 40, y + 80, F_XL, G_BLACK, b);                      /* temperature, baseline y + 172 */
        fmt_word(live_value(LV_OUT_COND), c, sizeof c);
        if (live_known(LV_OUT_COND)) op_text(l, x + 40, y + 222 - F_M->ascent, fit(c, 320, F_M, F_LBL, F_S), G_BLACK, c);
        char hi[16], lo[16]; fmt_temp(LV_OUT_HI, hi, sizeof hi); fmt_temp(LV_OUT_LO, lo, sizeof lo);
        if (live_known(LV_OUT_HI) || live_known(LV_OUT_LO)) { snprintf(c, sizeof c, "High %s " DOT " Low %s", hi, lo); op_text(l, x + 40, y + h - 36 - F_LBL->line_h, F_LBL, 0x4, c); }
        /* chance of rain, hourly */
        int rx = x + 40 + 320 + 44, rw = x + w - 40 - rx;
        const char *lab = "CHANCE OF RAIN, %";
        op_text(l, rx + rw - draw_font_w(F_S, lab), y + 36 + 4, F_S, 0x5, lab);
        int vals[8], nv = 0;
        for (const char *p = live_value(LV_RAIN); *p && nv < 8; ) {
            while (*p == ' ' || *p == ',' || *p == '[') p++;
            if (!*p || *p == ']') break;
            vals[nv++] = atoi(p);
            while (*p && *p != ',') p++;
        }
        int cw = (rw - 7 * 14) / 8, bottom = y + h - 36 - F_S->line_h - 10, maxh = 110;
        if (!nv) op_text(l, rx + rw - draw_font_w(F_LBL, "No forecast yet"), bottom - 60, F_LBL, 0x5, "No forecast yet");
        for (int i = 0; i < nv; i++) {
            int v = vals[i] < 0 ? 0 : vals[i] > 100 ? 100 : vals[i];
            int bh = v * maxh / 70; if (bh > maxh) bh = maxh; if (bh < 10) bh = 10;
            uint8_t g = v >= 60 ? 0x2 : v >= 40 ? 0x4 : v >= 25 ? 0x6 : v >= 15 ? 0x8 : v >= 8 ? 0xA : 0xB;
            int bx = rx + i * (cw + 14);
            op_rrect(l, bx, bottom - bh, cw, bh, 8, g);
            snprintf(c, sizeof c, "%d", v);
            op_text(l, bx + (cw - draw_font_w(F_S, c)) / 2, bottom - bh - 8 - F_S->line_h, F_S, 0x4, c);
            const char *hl = i == 0 ? "Now" : (snprintf(c, sizeof c, "%d", i), c);
            op_text(l, bx + (cw - draw_font_w(F_S, hl)) / 2, bottom + 8, F_S, 0x4, hl);
        }
    }
    {   int x = LV_COL(2), y = LV_ROW1, w = LV_COL_W, h = LV_TILE_H;
        tile_head(l, x, y, w, h, false, ICON_THERMOMETER, "BEDROOM");
        fmt_temp(LV_BED_TEMP, b, sizeof b);
        if (live_known(LV_BED_HUM)) snprintf(c, sizeof c, "Humidity %s%%", live_value(LV_BED_HUM)); else c[0] = 0;
        int top = tile_foot(l, x, y, w, h, false, b, F_XL, c);
        float hs[25]; int n = live_history(hs, 25);
        if (n >= 2) {                            /* the last 12 hours */
            float lo = hs[0], hi = hs[0];
            for (int i = 1; i < n; i++) { if (hs[i] < lo) lo = hs[i]; if (hs[i] > hi) hi = hs[i]; }
            if (hi - lo < 1.0f) { float mid = (hi + lo) / 2; lo = mid - 0.5f; hi = mid + 0.5f; }
            int sx = x + 40, sw = w - 80, sy = top - 6 - 60, sh = 52;
            for (int i = 1; i < n; i++) {
                int xa2 = sx + sw * (i - 1) / (n - 1), xb2 = sx + sw * i / (n - 1);
                int ya2 = sy + sh - (int)((hs[i - 1] - lo) / (hi - lo) * (float)sh);
                int yb2 = sy + sh - (int)((hs[i] - lo) / (hi - lo) * (float)sh);
                op_line(l, xa2, ya2, xb2, yb2, 4, G_BLACK);
            }
        }
    }
    {   int x = LV_COL(3), y = LV_ROW1, w = LV_COL_W, h = LV_TILE_H;
        static const char *const DONE[] = { "done", "finished", "complete", "completed", "end", NULL };
        const char *v = live_value(LV_WASHER);
        bool dark = word_in(v, DONE);
        tile_head(l, x, y, w, h, dark, ICON_WASHING_MACHINE, "WASHING");
        fmt_word(v, b, sizeof b);
        uint32_t age = live_age_s(LV_WASHER) / 60;
        if (!live_known(LV_WASHER)) c[0] = 0;
        else if (age < 1) snprintf(c, sizeof c, dark ? "Just now " DOT " hang it out" : "Just now");
        else if (age < 120) snprintf(c, sizeof c, dark ? "%lu min ago " DOT " hang it out" : "%lu min", (unsigned long)age);
        else snprintf(c, sizeof c, "%lu h ago", (unsigned long)(age / 60));
        tile_foot(l, x, y, w, h, dark, b, fit(b, w - 80, F_XL, &ui72, F_M), c);
    }

    /* row 2: security, bins, next event, A/C */
    {   int x = LV_COL(0), y = LV_ROW2, w = LV_COL_W, h = LV_TILE_H;
        static const char *const SHUT[] = { "locked", "closed", "off", "shut", NULL };
        static const int SL[3] = { LV_FRONT, LV_BACK, LV_GARAGE };
        static const char *const SN[3] = { "Front door", "Back door", "Garage" };
        int open = -1, known = 0;
        for (int i = 0; i < 3; i++) if (live_known(SL[i])) { known++; if (!word_in(live_value(SL[i]), SHUT) && open < 0) open = i; }
        bool dark = open >= 0;
        tile_head(l, x, y, w, h, dark, ICON_LOCK, "SECURITY");
        if (dark) { fmt_word(live_value(SL[open]), c, sizeof c); c[0] = (char)tolower((unsigned char)c[0]); snprintf(b, sizeof b, "%.20s %.30s", SN[open], c); }
        else snprintf(b, sizeof b, known ? "All shut" : "--");
        op_text(l, x + 40, y + 36 + 42 + 14, fit(b, w - 80, F_M, F_LBL, F_S), dark ? G_WHITE : G_BLACK, b);
        for (int i = 0; i < 3; i++) {
            int ry = y + h - 36 - F_S->line_h - (2 - i) * 44;
            bool ok = live_known(SL[i]) && word_in(live_value(SL[i]), SHUT);
            uint8_t fg = dark ? G_WHITE : G_BLACK;
            if (live_known(SL[i])) op_text(l, x + 40, ry + 1, F_I, fg, ok ? ICON_CHECK : ICON_BELL_RINGING);
            op_text(l, x + 40 + 34 + 12, ry, F_S, fg, SN[i]);
            fmt_word(live_value(SL[i]), c, sizeof c);
            op_text(l, x + w - 40 - draw_font_w(F_S, c), ry, F_S, dark ? 0xD : 0x4, c);
        }
    }
    {   int x = LV_COL(1), y = LV_ROW2, w = LV_COL_W, h = LV_TILE_H;
        tile_head(l, x, y, w, h, false, ICON_TRASH, "BINS");
        fmt_word(live_value(LV_BINS), b, sizeof b);
        tile_foot(l, x, y, w, h, false, b, fit(b, w - 80, &ui72, F_M, F_LBL), live_value(LV_BINS_WHEN));
    }
    {   int x = LV_COL(2), y = LV_ROW2, w = LV_COL_W, h = LV_TILE_H;
        tile_head(l, x, y, w, h, false, ICON_CALENDAR_BLANK, "NEXT EVENT");
        const char *v = live_value(LV_EVENT);
        int hh, mm;
        if (sscanf(v, "%d:%d", &hh, &mm) == 2 && strlen(v) >= 5) {
            snprintf(b, sizeof b, "%02d:%02d", hh, mm);
            const char *rest = v + 5; while (*rest == ' ' || *rest == '-') rest++;
            snprintf(c, sizeof c, "%.24s", rest);
            tile_foot(l, x, y, w, h, false, b, &ui72, c);
        } else {
            snprintf(b, sizeof b, "%.40s", v[0] ? v : "--");
            tile_foot(l, x, y, w, h, false, b, fit(b, w - 80, &ui72, F_M, F_LBL), "");
        }
    }
    {   int x = LV_COL(3), y = LV_ROW2, w = LV_COL_W, h = LV_TILE_H;
        tile_head(l, x, y, w, h, false, ICON_SNOWFLAKE, "A/C");
        if (S.ac_mode) snprintf(b, sizeof b, "%s %d" DEG, AC_MODES[S.ac_mode], S.sp_x2 / 2); else snprintf(b, sizeof b, "Off");
        char fan[8]; snprintf(fan, sizeof fan, "%s", FANS[S.fan]); fan[0] = (char)tolower((unsigned char)fan[0]);
        snprintf(c, sizeof c, "Room %d.%d" DEG " " DOT " fan %s", S.room_x10 / 10, S.room_x10 % 10, fan);
        tile_foot(l, x, y, w, h, false, b, fit(b, w - 80, &ui72, F_M, F_LBL), c);
    }
}

/* Which regions to repaint for a change of live slot s. */
static void live_mark_slot(int s)
{
    int x = 0, y = LV_ROW1, w = LV_COL_W;
    switch (s) {
    case LV_OUT_TEMP: case LV_OUT_COND: case LV_OUT_HI: case LV_OUT_LO: case LV_RAIN: x = LV_COL(0); w = 2 * LV_COL_W + LV_GAP; break;
    case LV_BED_TEMP: case LV_BED_HUM: x = LV_COL(2); break;
    case LV_WASHER: x = LV_COL(3); break;
    case LV_FRONT: case LV_BACK: case LV_GARAGE: x = LV_COL(0); y = LV_ROW2; break;
    case LV_BINS: case LV_BINS_WHEN: x = LV_COL(1); y = LV_ROW2; break;
    default: x = LV_COL(2); y = LV_ROW2; break;
    }
    mark(x, y, w, LV_TILE_H, IT8951_MODE_GL16);
}

bool numeric(const char *t)
{
    for (; *t; t++) if (!strchr("0123456789:.%-Of" DEG, *t)) return false;
    return true;
}

/* ---- the clock: the network's time, the server's zone, or the user's -------------
 * Automatic: UTC from the server's Date headers and Tesserae's UTC offset. A
 * chosen zone replaces the offset (with its daylight saving); switching
 * automatic off runs a clock set by hand from the board's timer. */
/* s_man_base: manual UTC = base + seconds since boot (declared above) */
static bool    s_man_set;
const char *const MONTHS[12] = { "January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December" };

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1; *m = mp < 10 ? mp + 3 : mp - 9; *y = (int)(yoe + era * 400) + (*m <= 2);
}
static int64_t first_sunday(int y, int m)      /* day number */
{
    int64_t d = days_from_civil(y, m, 1); int wd = (int)((d % 7 + 7 + 3) % 7);   /* 0 = Monday */
    return d + (6 - wd + 7) % 7;
}
static int64_t last_sunday(int y, int m)
{
    int64_t d = days_from_civil(y, m + 1, 1) - 1; int wd = (int)((d % 7 + 7 + 3) % 7);
    return d - (wd + 1) % 7;
}
/* Daylight saving at local standard time lstd, southern rules (2 am standard). */
static bool dst_on(int64_t lstd, int rule)
{
    int y, m, d; civil_from_days(lstd >= 0 ? lstd / 86400 : (lstd - 86399) / 86400, &y, &m, &d);
    int64_t start = (rule == 2 ? last_sunday(y, 9) : first_sunday(y, 10)) * 86400 + 7200;
    int64_t end = first_sunday(y, 4) * 86400 + 7200;
    return lstd >= start || lstd < end;
}
int32_t tz_offset_at(int64_t utc)
{
    if (S.tz <= 0 || S.tz >= N_TZ) return tesserae_tz_offset_s();
    const tzone_t *z = &TZONES[S.tz];
    return z->std_s + (z->dst && dst_on(utc + z->std_s, z->dst) ? 3600 : 0);
}
int64_t utc_now(void)
{
    if (!S.auto_time && s_man_set) return s_man_base + (int64_t)(time_us_64() / 1000000u);
    return (int64_t)rest_epoch_now();
}
/* Automatic off: start the hand-set clock from the time now (or 1 Jan 2026). */
void manual_start(void)
{
    uint32_t e = rest_epoch_now();
    s_man_base = (e ? (int64_t)e : 1767225600) - (int64_t)(time_us_64() / 1000000u);
    s_man_set = true; s_man_days = 0;
}
bool local_ymd(int *y, int *m, int *d, int *wd)
{
    int64_t e = utc_now();
    if (!e) return false;
    int64_t l = e + tz_offset_at(e), days = l / 86400;
    civil_from_days(days, y, m, d);
    *wd = (int)((days + 3) % 7);                   /* 0 = Monday, as DAY_NAMES */
    return true;
}

/* The live page: the clock every minute, a tile when its value changes, the
 * header and timeline when the alarms, sleep timer or sound change. */
bool local_live_tick(void)
{
    bool late = false;
    if (s_toast_until && (int32_t)(now_ms() - s_toast_until) >= 0) {        /* the confirmation goes */
        s_toast_until = 0; rebuild();
        mark(TOAST_X - 32, TOAST_Y - 32, TOAST_W + 64, TOAST_H + 64, IT8951_MODE_GL16);
        late = true;
    }
    {   static int64_t ov_min = -1;                                           /* the overlay's clock */
        int owd, osec; int64_t omn = -1;
        if (ov_active() && (S.show & 1) && clock_now(&owd, &osec, &omn) && omn != ov_min) {
            if (ov_min >= 0) { rebuild(); mark_ov(); late = true; }
            ov_min = omn;
        }
    }
    if (late) { bool more = local_live_tick_inner(); return true | more; }
    return local_live_tick_inner();
}

bool local_live_tick_inner(void)
{
    uint32_t changed = live_service();
    static int64_t last_min = -1;
    static uint32_t last_sig;
    bool vis = S.dash_page == 0 && s_mode == M_HOME && S.phase != PHASE_RINGING;
    int wd = 0, sec = 0; int64_t mn = -1;
    clock_now(&wd, &sec, &mn);
    int left = sleep_left_s();
    int ai = -1; int32_t dl = 0; next_alarm(&ai, &dl);
    int ah = 0, am = 0, aon = 0, amask = 0, ae = 0;
    if (ai >= 0) local_alarm_get(ai, &ah, &am, &aon, &amask, &ae);
    uint32_t sig = (uint32_t)(left > 0 ? (sec + left) / 60 : -1) * 2654435761u ^ (uint32_t)(ai * 7 + ah * 60 + am) * 40503u
                 ^ (uint32_t)ae * 31u ^ (uint32_t)S.rain_on * 977u ^ (uint32_t)S.birds * 13u
                 ^ (uint32_t)(S.ac_mode * 100 + S.sp_x2 + S.fan * 1000 + S.room_x10 * 7) * 2246822519u;
    if (!vis) { last_min = mn; last_sig = sig; return false; }
    bool any = false;
    if (mn != last_min) {
        last_min = mn;
        rebuild();
        mark_changed(&s_prev_body, &L_body, 0, LV_HEAD_Y, W, LV_HEAD_H, IT8951_MODE_GL16);   /* the digits that changed, not the whole header */
        any = true;
    }
    if (sig != last_sig) {
        last_sig = sig;
        rebuild();
        mark_changed(&s_prev_body, &L_body, 0, LV_HEAD_Y, W, LV_TL_Y + LV_TL_H - LV_HEAD_Y, IT8951_MODE_GL16);
        mark(LV_COL(3), LV_ROW2, LV_COL_W, LV_TILE_H, IT8951_MODE_GL16);
        any = true;
    }
    if (changed) {
        rebuild();
        for (int i = 0; i < LV_COUNT; i++) if (changed & (1u << i)) live_mark_slot(i);
        any = true;
    }
    return any;
}

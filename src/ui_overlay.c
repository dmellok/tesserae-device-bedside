/*
 * ui_overlay.c: the Mk II home overlay, toasts and the update card
 */
#include "ui_internal.h"

/* ---- Mk II home: the dashboard and one overlay ------------------------------------
 * Persistent local UI lives only in the overlay: a strip along the bottom
 * (default), cards down the left, cards along the bottom, or nothing. What it
 * leaves is the dashboard's viewport, reported to Tesserae on every heartbeat.
 * Each item is one line: an icon and a value. Live (dashboard 0) keeps its own
 * full-screen layout. */

/* The overlay in force: nothing to show means none. */
int ov_kind(void) { return (S.show & 0x1F) ? S.overlay : 3; }
bool ov_active(void) { return s_mode == M_HOME && S.dash_page > 0 && S.phase != PHASE_RINGING; }

static bool ov_rect(int *x, int *y, int *w, int *h)
{
    switch (ov_kind()) {
    case 0: *x = 0; *y = H - OV_STRIP_H; *w = W; *h = OV_STRIP_H; return true;
    case 1: *x = 0; *y = 0; *w = OV_LEFT_W; *h = H; return true;
    case 2: *x = 0; *y = H - OV_BOTTOM_H; *w = W; *h = OV_BOTTOM_H; return true;
    default: *x = *y = *w = *h = 0; return false;
    }
}
void viewport(int *x, int *y, int *w, int *h)
{
    switch (ov_kind()) {
    case 0: *x = 0; *y = 0; *w = W; *h = H - OV_STRIP_H; break;
    case 1: *x = OV_LEFT_W; *y = 0; *w = W - OV_LEFT_W; *h = H; break;
    case 2: *x = 0; *y = 0; *w = W; *h = H - OV_BOTTOM_H; break;
    default: *x = 0; *y = 0; *w = W; *h = H; break;
    }
}
void mark_ov(void) { int x, y, w, h; if (ov_rect(&x, &y, &w, &h)) mark_changed(&s_prev_sb, &L_sb, x, y, w, h, strip_du() ? IT8951_MODE_DU : IT8951_MODE_GL16); }
/* The overlay settings changed: tell Tesserae what the dashboard has now. */
void overlay_apply(void)
{
    int x, y, w, h; viewport(&x, &y, &w, &h);
    tesserae_set_viewport(x, y, w, h);
}

void fmt_hm(char *b, size_t n, int mins)
{
    int h = mins / 60 % 24, m = mins % 60;
    if (S.clock12) snprintf(b, n, "%d:%02d", h % 12 ? h % 12 : 12, m);
    else snprintf(b, n, "%02d:%02d", h, m);
}

/* One overlay item: icon and value (and an optional dim tail), drawn from x
 * with its vertical centre at cy. Returns the x after it. */
static int ov_item(layer_t *l, int x, int cy, const char *icon, const char *val, const char *tail, uint8_t fg, uint8_t dim, const bfont_t *f)
{
    if (icon) { op_text(l, x, cy - F_IM->line_h / 2, F_IM, dim, icon); x += draw_font_w(F_IM, icon) + 18; }
    if (val && val[0]) { op_text(l, x, cy - f->line_h / 2, f, fg, val); x += draw_font_w(f, val); }
    if (tail && tail[0]) { x += 18; op_text(l, x, cy - f->line_h / 2, f, dim, tail); x += draw_font_w(f, tail); }
    return x;
}

/* The values the overlay shows, in order; returns how many (clock first when shown). */
static int ov_values(ov_val_t *v, bool *clock, char *clk, char *clock_ampm, char *date)
{
    int n = 0, wd, sec; int64_t mn;
    bool have = clock_now(&wd, &sec, &mn);
    bool solo = S.night && S.at_night == 1;
    *clock = (S.show & 1) != 0;
    clock_ampm[0] = 0;
    if (have) {
        int m = sec / 60;
        fmt_hm(clk, 8, m);
        if (S.clock12) snprintf(clock_ampm, 4, "%s", m < 720 ? "am" : "pm");
    } else snprintf(clk, 8, "--:--");
    { int y, mo, d, w; if (local_ymd(&y, &mo, &d, &w)) snprintf(date, 24, "%.3s %d %.3s", DAY_NAMES[w], d, MONTHS[mo - 1]); else date[0] = 0; }
    if (solo && *clock) return 0;
    if (S.show & 2) {
        int ai = -1; int32_t dl = 0; next_alarm(&ai, &dl);
        v[n].icon = ICON_ALARM; v[n].tail[0] = 0;
        if (ai >= 0) { int h, m, on, mask, early; local_alarm_get(ai, &h, &m, &on, &mask, &early); fmt_hm(v[n].val, sizeof v[n].val, h * 60 + m); }
        else snprintf(v[n].val, sizeof v[n].val, "Off");
        n++;
    }
    if (S.show & 4) {
        v[n].icon = S.rain_on ? ICON_CLOUD_RAIN : ICON_PAUSE;
        snprintf(v[n].val, sizeof v[n].val, "%s", !S.rain_on ? "Paused" : S.rain_kind == RS_RADIO ? radio_station() : KINDS[S.rain_kind]);
        snprintf(v[n].tail, sizeof v[n].tail, "%d", S.vol);
        n++;
    }
    if (S.show & 8) {
        v[n].icon = S.ac_mode ? ICON_SNOWFLAKE : (S.lights_on ? ICON_LIGHTBULB : ICON_SNOWFLAKE);
        if (S.ac_mode) snprintf(v[n].val, sizeof v[n].val, "%d" DEG, S.sp_x2 / 2); else snprintf(v[n].val, sizeof v[n].val, "Off");
        snprintf(v[n].tail, sizeof v[n].tail, "%s", S.lights_on ? "lights" : "");
        n++;
    }
    return n;
}

/* The clock, one op per character in fixed slots (every digit as wide as the
 * widest), so a minute tick changes one small op and nothing to the right of
 * the clock moves: the diff then repaints one digit cell, not the whole strip.
 * (Overnight that was a GC16 partial over most of the strip every minute.)
 * l == NULL only measures. Returns the width. */
int clock_slots(layer_t *l, int x, int y, const bfont_t *f, uint8_t g, const char *ct)
{
    int dw = 0;
    for (char d = '0'; d <= '9'; d++) { char s[2] = { d, 0 }; int w = draw_font_w(f, s); if (w > dw) dw = w; }
    int x0 = x;
    for (const char *p = ct; *p; p++) {
        char s[2] = { *p, 0 };
        int w = draw_font_w(f, s);
        bool digit = *p >= '0' && *p <= '9';
        if (l) op_text(l, digit ? x + (dw - w) / 2 : x, y, f, g, s);
        x += digit ? dw : w;
    }
    return x - x0;
}

/* Clocks painted two-level (DU): no flash, and none of the GC16 partial-refresh
 * fade; the digits are black on white, so no anti-aliasing. Settings > Home
 * screen > Clock updates. The card layouts keep GC16: their grey cards would
 * threshold to white. */
bool clock_du_active(void)
{
    return S.clock_du == 2 || (S.clock_du == 1 && S.night);
}

bool strip_du(void)
{
    return ov_kind() == 0 && clock_du_active();
}

void build_overlay(layer_t *l)
{
    int ox, oy, ow, oh;
    if (!ov_rect(&ox, &oy, &ow, &oh)) { l->on = false; return; }
    layer_reset(l, ox, oy, ow, oh, G_WHITE);
    ov_val_t v[4]; bool clock; char ct[8], ap[4], date[24];
    int n = ov_values(v, &clock, ct, ap, date);
    bool solo = clock && S.night && S.at_night == 1;
    bool links = (S.show & 16) || !net_connected();       /* a lost connection always shows */
    const char *wifi = net_connected() ? ICON_WIFI_HIGH : ICON_WIFI_SLASH;
    int kind = ov_kind();

    if (kind == 0) {                                         /* the strip */
        uint8_t dim = strip_du() ? G_BLACK : G_DARK;
        op_fill(l, 0, oy, W, 4, G_BLACK);
        int cy = oy + 4 + (OV_STRIP_H - 4) / 2, x = MARGIN;
        if (solo) {
            int tw = clock_slots(NULL, 0, 0, F_XL, 0, ct) + (ap[0] ? 12 + draw_font_w(F_M, ap) : 0);
            x = (W - tw) / 2;
        }
        if (clock) {
            x += clock_slots(l, x, cy - F_XL->line_h / 2, F_XL, G_BLACK, ct);
            if (ap[0]) { x += 12; op_text(l, x, cy - F_M->line_h / 2 + 14, F_M, dim, ap); x += draw_font_w(F_M, ap); }
            x += 96;
        }
        for (int i = 0; i < n; i++) x = ov_item(l, x, cy, v[i].icon, v[i].val, v[i].tail, G_BLACK, dim, F_L) + 96;
        if (links && !solo) {
            int xr = W - MARGIN;
            if (!net_connected()) { xr -= draw_font_w(F_L, "Offline"); op_text(l, xr, cy - F_L->line_h / 2, F_L, G_BLACK, "Offline"); xr -= 18; }
            xr -= draw_font_w(F_IM, wifi); op_text(l, xr, cy - F_IM->line_h / 2, F_IM, G_BLACK, wifi);
            if (ha_connected()) { xr -= 30 + draw_font_w(F_IM, ICON_HOUSE); op_text(l, xr, cy - F_IM->line_h / 2, F_IM, G_BLACK, ICON_HOUSE); }
        }
        return;
    }

    /* cards: a dark clock card, then one light card per item */
    bool left = kind == 1;
    if (left) op_fill(l, OV_LEFT_W - 4, 0, 4, H, G_LIGHT);
    else op_fill(l, 0, oy, W, 4, G_LIGHT);
    int cards = (clock ? 1 : 0) + n + (links && !solo && left ? 1 : 0);
    if (!cards) return;
    int x = OV_PAD, y = oy + OV_PAD;
    int cw = left ? OV_LEFT_W - 2 * OV_PAD : (W - 2 * OV_PAD - (cards - 1) * 26) / cards;
    int ch_clock = left ? 250 : OV_BOTTOM_H - 2 * OV_PAD, ch = left ? 136 : OV_BOTTOM_H - 2 * OV_PAD;
    if (clock) {
        op_rrect(l, x, y, cw, ch_clock, 28, G_BLACK);
        int ty = left ? y + 34 : y + (ch_clock - F_XL->line_h - 10 - F_S->line_h) / 2;
        int cw = clock_slots(l, x + 34, ty, F_XL, G_WHITE, ct);
        if (ap[0]) op_text(l, x + 44 + cw, ty + F_XL->line_h - F_M->line_h - 6, F_M, G_LIGHT, ap);
        if (date[0]) op_text(l, x + 34, ty + F_XL->line_h + 10, F_S, G_LIGHT, date);
        if (left) y += ch_clock + 26; else x += cw + 26;
    }
    for (int i = 0; i < n; i++) {
        op_rrect(l, x, y, cw, ch, 28, CARD_GREY);
        ov_item(l, x + 30, y + ch / 2, v[i].icon, v[i].val, v[i].tail, G_BLACK, G_DARK, F_L);
        if (left) y += ch + 26; else x += cw + 26;
    }
    if (links && !solo && left) {
        int cy = y + 50, xx = x + 30;
        op_text(l, xx, cy - F_IM->line_h / 2, F_IM, G_BLACK, wifi); xx += draw_font_w(F_IM, wifi) + 30;
        if (ha_connected()) op_text(l, xx, cy - F_IM->line_h / 2, F_IM, G_BLACK, ICON_HOUSE);
        if (!net_connected()) op_text(l, x + 30, cy + 40, F_S, G_BLACK, "Offline");
    } else if (links && !solo && !net_connected()) {
        op_text_r(l, W - OV_PAD - 20, oy + 14, F_S, G_BLACK, "Offline");
    }
}

/* ---- confirmations: a box over everything, then the screen as it was ------------
 * A key action or a volume change on the home screen says what it did for
 * 1.5 s; the dashboard underneath is repainted from the frame in PSRAM. */
static char s_toast_txt[24], s_toast_val[44];
static const char *s_toast_icon;

void toast(const char *icon, const char *txt, const char *val)
{
    s_toast_icon = icon;
    snprintf(s_toast_txt, sizeof s_toast_txt, "%s", txt);
    snprintf(s_toast_val, sizeof s_toast_val, "%s", val ? val : "");
    s_toast_until = (now_ms() + 3000u) | 1u;          /* long enough to read from the pillow */
    rebuild();
    mark(TOAST_X - 32, TOAST_Y - 32, TOAST_W + 64, TOAST_H + 64, crisp() ? IT8951_MODE_DU : IT8951_MODE_GL16);
}

static bool s_toast_loading;
void toast_loading(const char *title)
{
    toast(ICON_HOUSE, "Loading", title);
    s_toast_loading = true;
    s_toast_until = (now_ms() + 40000u) | 1u;            /* a first render can take a while */
}
/* The update in progress: a card with the percentage; a failure says why. The
 * overnight check: 03:10 local, auto-update on, no alarm within 90 minutes. */
void local_ota_service(void)
{
    static int last_pct = -1; static ota_state_t last = OTA_IDLE;
    ota_state_t st = ota_state();
    if (st == OTA_DOWNLOADING || st == OTA_VERIFYING) {
        int pct = ota_percent();
        if (pct != last_pct || st != last) {
            char v[40]; snprintf(v, sizeof v, "%s  %d%%", ota_target_version(), st == OTA_VERIFYING ? 100 : pct);
            toast(ICON_GEAR, st == OTA_VERIFYING ? "Checking the update" : "Updating", v);
            s_toast_loading = true; s_toast_until = (now_ms() + 120000u) | 1u;
            last_pct = pct;
        }
    } else if (st == OTA_FAILED && last != OTA_FAILED) {
        s_toast_loading = false; toast(ICON_GEAR, "Update failed", ota_error()); last_pct = -1;
    }
    last = st;
    if (!S.auto_update || ota_busy()) return;
    static int done_day = -1;
    int wd, sec; int64_t mn;
    if (!clock_now(&wd, &sec, &mn) || sec / 60 != 3 * 60 + 10 || done_day == (int)(mn / 1440)) return;
    done_day = (int)(mn / 1440);
    int ai; int32_t dl;
    if (next_alarm(&ai, &dl) && dl >= 0 && dl < 90 * 60) { printf("ota: auto-update skipped, an alarm is due\n"); return; }
    ota_auto_update();
}

/* main.c: a new frame arrived, so the loading card goes with the repaint. */
void local_frame_arrived(void)
{
    if (!s_toast_loading) return;
    s_toast_loading = false; s_toast_until = 0; rebuild();
}

void build_toast(layer_t *l)
{
    /* A rounded card: big icon on the left, the label above, the state large. */
    layer_reset(l, TOAST_X - 32, TOAST_Y - 32, TOAST_W + 64, TOAST_H + 64, G_WHITE);
    op_rrect(l, TOAST_X, TOAST_Y, TOAST_W, TOAST_H, 44, G_BLACK);
    op_rrect(l, TOAST_X + 6, TOAST_Y + 6, TOAST_W - 12, TOAST_H - 12, 38, G_WHITE);
    int cy = TOAST_Y + TOAST_H / 2, x = TOAST_X + 72;
    if (s_toast_icon) {
        op_circle(l, x + 105, cy, 118, CARD_GREY);
        op_text_c(l, x + 105, cy - icon170.line_h / 2, &icon170, G_BLACK, s_toast_icon);
        x += 210 + 64;
    }
    int ty = cy - (F_M->line_h + 8 + F_XL->line_h) / 2;
    op_text(l, x, ty, F_M, DIM_G, s_toast_txt);
    const char *v = s_toast_val[0] ? s_toast_val : s_toast_txt;
    int room = TOAST_X + TOAST_W - 56 - x;
    const bfont_t *vf = draw_font_w(F_XL, v) <= room ? F_XL : draw_font_w(F_L, v) <= room ? F_L : F_M;   /* long titles */
    op_text(l, x, ty + F_M->line_h + 8, vf, G_BLACK, v);
}

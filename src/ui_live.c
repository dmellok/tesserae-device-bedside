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
    bool live = S.dash_page == 0 && s_mode == M_HOME && S.phase != PHASE_RINGING;
    layer_reset(l, 0, SB_Y, W, SB_H, bg);
    op_fill(l, 0, SB_Y, W, 3, inv ? G_DARK : G_BLACK);
    int cy = SB_Y + 3 + (SB_H - 3) / 2;
    char b[40];
    /* left: volume, then one now-playing value */
    int x = MARGIN;
    op_text(l, x, cy - F_I->line_h / 2, F_I, fg, S.vol ? ICON_SPEAKER_HIGH : ICON_SPEAKER_SLASH);
    x += draw_font_w(F_I, ICON_SPEAKER_HIGH) + 16;
    op_rrect(l, x, cy - 4, 220, 8, 4, track);
    if (S.vol) op_rrect(l, x, cy - 4, 220 * S.vol / 10 < 8 ? 8 : 220 * S.vol / 10, 8, 4, fg);
    x += 220 + 16;
    snprintf(b, sizeof b, "%d", S.vol);
    op_text(l, x, cy - F_LBL->line_h / 2, F_LBL, fg, b);
    x += draw_font_w(F_LBL, b) + 48;
    bool playing = S.rain_on || (S.live_demo && live);
    if (playing) {
        const char *icon = S.rain_kind == RS_RADIO || (S.live_demo && live) ? ICON_SPEAKER_HIGH : ICON_CLOUD_RAIN;
        op_text(l, x, cy - F_I->line_h / 2, F_I, fg, icon); x += draw_font_w(F_I, icon) + 14;
        if (S.live_demo && live) snprintf(b, sizeof b, "Lush");
        else if (S.rain_kind == RS_RADIO) snprintf(b, sizeof b, "%s", radio_station());
        else snprintf(b, sizeof b, "%s %d", KINDS[S.rain_kind], S.rain_lvl);
    } else {
        op_text(l, x, cy - F_I->line_h / 2, F_I, dim, ICON_PAUSE); x += draw_font_w(F_I, ICON_PAUSE) + 14;
        snprintf(b, sizeof b, "Paused");
    }
    op_text(l, x, cy - F_LBL->line_h / 2, F_LBL, playing ? fg : dim, b);

    /* right, from the edge inwards; the Live tiles already carry alarm and climate */
    int xr = W - MARGIN;
    if (!net_connected()) xr = sb_item_r(l, xr, cy, ICON_WIFI_SLASH, "Offline", fg) - 30;
    else xr = sb_item_r(l, xr, cy, ICON_WIFI_HIGH, NULL, fg) - 30;
    if (ha_connected()) xr = sb_item_r(l, xr, cy, ICON_HOUSE, NULL, fg) - 30;
    op_fill(l, xr - 2, cy - 28, 2, 56, inv ? G_DARK : 0xC);
    xr -= 2 + 48;
    if (S.on_battery) { snprintf(b, sizeof b, "%d%%", S.batt_pct); xr = sb_item_r(l, xr, cy, ICON_BATTERY_CHARGING, b, fg) - 48; }
    if (S.night) xr = sb_item_r(l, xr, cy, ICON_MOON, NULL, dim) - 48;
    if (!live && S.ac_mode) { snprintf(b, sizeof b, "%s %d" DEG, AC_MODES[S.ac_mode], S.sp_x2 / 2); xr = sb_item_r(l, xr, cy, ICON_SNOWFLAKE, b, fg) - 48; }
    if (!live && S.alarm_on) { alarm_txt(b, sizeof b); xr = sb_item_r(l, xr, cy, ICON_ALARM, b, fg) - 48; }
    int left = sleep_left_s(), wd, sec; int64_t mn;
    if (!live && left > 0 && clock_now(&wd, &sec, &mn)) {
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

const bfont_t *lv_fit(const char *t, int w, const bfont_t *a, const bfont_t *b, const bfont_t *c)
{
    if (draw_font_w(a, t) <= w) return a;
    if (draw_font_w(b, t) <= w) return b;
    return c;
}

static const char *const DEMO_VALUES[LV_COUNT] = {
    [LV_OUT_TEMP] = "21.8", [LV_OUT_COND] = "Partly cloudy", [LV_OUT_HI] = "24.0", [LV_OUT_LO] = "16.5",
    [LV_RAIN] = "10,20,35,60,80,55,30,15", [LV_BED_TEMP] = "22.4", [LV_BED_HUM] = "47",
    [LV_WASHER] = "Ready", [LV_FRONT] = "locked", [LV_BACK] = "locked", [LV_GARAGE] = "closed",
    [LV_BINS] = "Recycling", [LV_BINS_WHEN] = "Out tonight", [LV_EVENT] = "09:00 Dentist",
};

const char *live_val(int slot)
{
    return S.live_demo ? DEMO_VALUES[slot] : live_value(slot);
}

bool live_is_known(int slot)
{
    return S.live_demo || live_known(slot);
}

static bool is_num(const char *v)
{
    if (!*v) return false;
    char *e; strtod(v, &e);
    return e != v && (*e == 0 || *e == ' ');
}

/* "21.43" -> "21.4°", anything else as it came; "--" when unknown. */
void fmt_temp(int slot, char *b, size_t n)
{
    const char *v = live_val(slot);
    if (!v[0]) { snprintf(b, n, "--"); return; }
    if (is_num(v)) { char d[16]; live_fmt_dec(d, sizeof d, strtod(v, NULL), strchr(v, '.') ? 1 : 0); snprintf(b, n, "%s" DEG, d); }
    else snprintf(b, n, "%s", v);
}

/* "unlocked" -> "Unlocked"; underscores to spaces. */
void fmt_word(const char *v, char *b, size_t n)
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

void card_title(layer_t *l, int x, int y, const char *icon, const char *label, bool dark)
{
    uint8_t fg = dark ? G_WHITE : G_BLACK;
    op_text(l, x + 32, y + 28, F_IM, fg, icon);
    op_text(l, x + 32 + draw_font_w(F_IM, icon) + 18, y + 26, F_M, fg, label);
}

bool live_dark_now(void) { return S.live_dark == 1 || (S.live_dark == 2 && S.night); }
void live_layout_changed(void) { if (s_mode == M_HOME && S.dash_page == 0) mark_screen(); }

/* The next alarm: time, early window (10 min when not set) and seconds away; demo: 06:30. */
bool live_alarm(int *h, int *m, int *early, int32_t *delta)
{
    int ai, on, mask, e = 15; *h = 6; *m = 30; *delta = 12 * 3600;
    if (S.live_demo) { *early = 15; return true; }
    if (!next_alarm(&ai, delta)) return false;
    local_alarm_get(ai, h, m, &on, &mask, &e);
    *early = e > 0 ? e : 10;
    return true;
}
/* Sunrise, minutes after local midnight, for the morning the alarm at alarm_min is on; -1 unknown. Demo: 06:42. */
int live_sunrise_today(int alarm_min)
{
    if (S.live_demo) return 6 * 60 + 42;
    int wd, sec; int64_t mn; int y = 0, mo = 0, d = 0;
    if (!clock_now(&wd, &sec, &mn) || sscanf(tesserae_local_time(), "%d-%d-%d", &y, &mo, &d) != 3) return -1;
    return live_sunrise_min(y, mo, d + (sec / 60 > alarm_min ? 1 : 0), tesserae_tz_offset_s());
}
/* "Rain 3 · stops 23:40", "Drone Zone", "Paused"; demo: "Lush". */
void live_sound_line(char *b, size_t n)
{
    int wd, sec; int64_t mn; int left = S.live_demo ? 5 * 3600 : sleep_left_s();
    char stops[24] = "";
    if (left > 0 && clock_now(&wd, &sec, &mn)) snprintf(stops, sizeof stops, " " DOT " stops %02d:%02d", (sec + left) / 60 % 1440 / 60, (sec + left) / 60 % 60);
    if (S.live_demo) snprintf(b, n, "Lush%s", stops);
    else if (!S.rain_on) snprintf(b, n, "Paused");
    else if (S.rain_kind == RS_RADIO) snprintf(b, n, "%.30s%s", radio_station(), stops);
    else snprintf(b, n, "%s %d%s", KINDS[S.rain_kind], S.rain_lvl, stops);
}
/* "All clear", "Front door open", "Washing done"; attention set when something needs you. */
void live_home_line(char *b, size_t n, bool *attention)
{
    static const char *const SHUT[] = { "locked", "closed", "off", "shut", NULL };
    static const char *const DONE[] = { "done", "finished", "complete", "completed", "end", NULL };
    static const int SL[3] = { LV_FRONT, LV_BACK, LV_GARAGE };
    static const char *const SN[3] = { "Front door", "Back door", "Garage" };
    int known = 0; *attention = false;
    for (int i = 0; i < 3; i++) if (live_is_known(SL[i])) { known++; if (!word_in(live_val(SL[i]), SHUT)) {
        char c[40]; fmt_word(live_val(SL[i]), c, sizeof c); c[0] = (char)tolower((unsigned char)c[0]);
        snprintf(b, n, "%s %.20s", SN[i], c); *attention = true; return; } }
    if (word_in(live_val(LV_WASHER), DONE)) { snprintf(b, n, "Washing done"); *attention = true; return; }
    if (known || live_is_known(LV_WASHER)) snprintf(b, n, "All clear"); else snprintf(b, n, "No reports yet");
}

static void build_live_bento(layer_t *l);
void build_live(layer_t *l)
{
    switch (S.live_layout) {
    case 1:  build_live_clock(l); break;
    case 2:  build_live_weather(l); break;
    case 3:  build_live_tonight(l); break;
    default: build_live_bento(l); break;
    }
    if (live_dark_now()) layer_invert(l);
}

static void build_live_bento(layer_t *l)
{
    layer_reset(l, 0, TB_H, W, SB_Y - TB_H, G_WHITE);
    char b[96], c[96];
    int wday = 0, sec = 0; int64_t minute = 0;
    bool have_clock = clock_now(&wday, &sec, &minute);

    if (!have_clock) snprintf(b, sizeof b, "--:--");
    else if (S.clock12) { int hh = sec / 3600 % 12; snprintf(b, sizeof b, "%d:%02d", hh ? hh : 12, sec / 60 % 60); }
    else snprintf(b, sizeof b, "%02d:%02d", sec / 3600, sec / 60 % 60);
    clock_slots(l, 68, LV_HEAD_Y + 250 - clock214.ascent, &clock214, G_BLACK, b);

    int ai; int32_t delta;
    bool alarm = S.live_demo || next_alarm(&ai, &delta);
    int ah = 6, am = 30, aon = 0, amask = 0, early = 15;
    if (S.live_demo) delta = 12 * 3600;
    else if (alarm) local_alarm_get(ai, &ah, &am, &aon, &amask, &early);
    bool dk = live_dark_now();                      /* inverted after building: a light card here reads dark grey on the glass */
    uint8_t tn_bg = dk ? 0xD : G_BLACK, tn_fg = dk ? G_BLACK : G_WHITE;
    op_rrect(l, LV_RIGHT_X, LV_HEAD_Y, LV_RIGHT_W, LV_HEAD_H, 36, tn_bg);
    card_title(l, LV_RIGHT_X, LV_HEAD_Y, ICON_ALARM, "Tonight", !dk);
    if (alarm) snprintf(b, sizeof b, "%02d:%02d", ah, am); else snprintf(b, sizeof b, "No alarm");
    op_text(l, LV_RIGHT_X + 40, LV_HEAD_Y + 96, F_XL, tn_fg, b);
    if (alarm) {
        int mins = (delta + 59) / 60;
        static const char *const DAYF[7] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
        if (delta < 0) c[0] = 0;
        else if (mins >= 24 * 60) snprintf(c, sizeof c, "on %s", DAYF[(wday + (sec + delta) / 86400) % 7]);
        else if (mins >= 60) {
            int step = mins >= 180 ? 15 : 5, r = (mins + step / 2) / step * step;
            if (r % 60) snprintf(c, sizeof c, "in %d h %d min", r / 60, r % 60); else snprintf(c, sizeof c, "in %d h", r / 60);
        } else snprintf(c, sizeof c, "in %d min", mins);
    } else snprintf(c, sizeof c, "Set one from Menu, Alarms");
    {   /* beside the value, however wide it is: "No alarm" in F_XL is 398 px */
        int hx = LV_RIGHT_X + 40 + draw_font_w(F_XL, b) + 30, hw = LV_RIGHT_X + LV_RIGHT_W - 40 - hx;
        op_text(l, hx, LV_HEAD_Y + 142, lv_fit(c, hw, F_M, F_LBL, F_LBL), tn_fg, c);
    }
    int win = early > 0 ? early : 10, cm = ((ah * 60 + am - win) % 1440 + 1440) % 1440;   /* alarm_window_s(): 10 min when not set */
    if (alarm) snprintf(b, sizeof b, "%s %02d:%02d", S.live_demo || S.birds ? "Dawn chorus" : "Wake window", cm / 60, cm % 60);
    else snprintf(b, sizeof b, "No wake window");
    op_text(l, LV_RIGHT_X + 40, LV_HEAD_Y + 226, F_M, tn_fg, b);
    int left = S.live_demo ? 5 * 3600 : sleep_left_s();
    if (left > 0 && have_clock) snprintf(b, sizeof b, "%02d:%02d", (sec + left) / 60 % 1440 / 60, (sec + left) / 60 % 60);
    else snprintf(b, sizeof b, "--");
    int sunrise = -1;
    if (S.live_demo) sunrise = 6 * 60 + 42;
    else if (alarm) {
        int y = 0, mo = 0, d = 0;
        if (sscanf(tesserae_local_time(), "%d-%d-%d", &y, &mo, &d) == 3)
            sunrise = live_sunrise_min(y, mo, d + (sec / 60 > ah * 60 + am ? 1 : 0), tesserae_tz_offset_s());
    }
    if (sunrise >= 0) snprintf(c, sizeof c, "Sound %.5s " DOT " sunrise %02d:%02d", b, sunrise / 60, sunrise % 60);
    else snprintf(c, sizeof c, "Sound %.5s", b);
    op_text(l, LV_RIGHT_X + 40, LV_HEAD_Y + 288, lv_fit(c, LV_RIGHT_W - 80, F_LBL, F_LBL, F_S), tn_fg, c);

    op_rrect(l, 36, LV_ROW1, LV_LEFT_W, LV_TILE_H, 36, 0xE);
    card_title(l, 36, LV_ROW1, ICON_CLOUD_SUN, "Outside", false);
    fmt_temp(LV_OUT_TEMP, b, sizeof b);
    op_text(l, 68, LV_ROW1 + 110, F_XL, G_BLACK, b);
    fmt_word(live_val(LV_OUT_COND), c, sizeof c);
    op_text(l, 68, LV_ROW1 + 232, lv_fit(c, LV_LEFT_W - 64, F_M, F_M, F_LBL), G_BLACK, c);
    char hi[16], lo[16]; fmt_temp(LV_OUT_HI, hi, sizeof hi); fmt_temp(LV_OUT_LO, lo, sizeof lo);
    snprintf(c, sizeof c, "High %s " DOT " Low %s", hi, lo);
    op_text(l, 68, LV_ROW1 + 294, F_M, G_BLACK, c);
    op_text(l, 68, LV_ROW1 + 382, F_LBL, G_BLACK, "Chance of rain");
    int vals[8], nv = 0;
    for (const char *p = live_val(LV_RAIN); *p && nv < 8; ) {
        while (*p == ' ' || *p == ',' || *p == '[') p++;
        if (!*p || *p == ']') break;
        vals[nv++] = atoi(p);
        while (*p && *p != ',') p++;
    }
    if (!nv) op_text(l, 68, LV_ROW1 + 452, F_M, G_BLACK, "No forecast yet");
    int cw = 58, bottom = LV_ROW1 + 574;
    for (int i = 0; i < nv; i++) {
        int v = vals[i] < 0 ? 0 : vals[i] > 100 ? 100 : vals[i];
        int bh = 20 + v * 110 / 100;
        int bx = 68 + i * (cw + 20);
        op_rrect(l, bx, bottom - bh, cw, bh, 10, v >= 60 ? G_BLACK : v >= 30 ? 0x6 : 0xA);
        const char *lab = i == 0 ? "Now" : (snprintf(c, sizeof c, "+%dh", i), c);
        op_text(l, bx + (cw - draw_font_w(F_S, lab)) / 2, bottom + 12, F_S, G_BLACK, lab);
    }

    op_rrect(l, LV_RIGHT_X, LV_ROW1, LV_SMALL_W, LV_SMALL_H, 36, 0xE);
    card_title(l, LV_RIGHT_X, LV_ROW1, ICON_THERMOMETER, "Bedroom", false);
    fmt_temp(LV_BED_TEMP, b, sizeof b);
    op_text(l, LV_RIGHT_X + 32, LV_ROW1 + 100, F_XL, G_BLACK, b);
    if (live_is_known(LV_BED_HUM)) snprintf(c, sizeof c, "%s%% humidity", live_val(LV_BED_HUM)); else snprintf(c, sizeof c, "Humidity --");
    op_text(l, LV_RIGHT_X + 32, LV_ROW1 + 214, F_M, G_BLACK, c);
    if (S.live_demo) snprintf(c, sizeof c, "Cool 22" DEG " " DOT " fan auto");
    else if (S.ac_mode) snprintf(c, sizeof c, "%s %d" DEG, AC_MODES[S.ac_mode], S.sp_x2 / 2); else snprintf(c, sizeof c, "A/C off");
    op_text(l, LV_RIGHT_X + 32, LV_ROW1 + 264, F_LBL, G_BLACK, c);

    static const char *const SHUT[] = { "locked", "closed", "off", "shut", NULL };
    static const char *const DONE[] = { "done", "finished", "complete", "completed", "end", NULL };
    static const int SL[3] = { LV_FRONT, LV_BACK, LV_GARAGE };
    static const char *const SN[3] = { "Front door", "Back door", "Garage" };
    int open = -1, known = 0;
    for (int i = 0; i < 3; i++) if (live_is_known(SL[i])) { known++; if (!word_in(live_val(SL[i]), SHUT) && open < 0) open = i; }
    bool washing_done = word_in(live_val(LV_WASHER), DONE), dark = open >= 0 || washing_done;
    op_rrect(l, LV_HOME_X, LV_ROW1, W - 36 - LV_HOME_X, LV_SMALL_H, 36, dark ? G_BLACK : 0xE);
    card_title(l, LV_HOME_X, LV_ROW1, dark ? ICON_BELL_RINGING : ICON_HOUSE, "Home", dark);
    if (dark) snprintf(b, sizeof b, "Attention");
    else if (known || live_is_known(LV_WASHER)) snprintf(b, sizeof b, "All clear");
    else snprintf(b, sizeof b, "--");
    op_text(l, LV_HOME_X + 32, LV_ROW1 + 108, lv_fit(b, LV_SMALL_W - 64, &ui72, F_M, F_M), dark ? G_WHITE : G_BLACK, b);
    if (open >= 0) {
        fmt_word(live_val(SL[open]), c, sizeof c); c[0] = (char)tolower((unsigned char)c[0]);
        char detail[64]; snprintf(detail, sizeof detail, "%.20s %.30s", SN[open], c);
        op_text(l, LV_HOME_X + 32, LV_ROW1 + 214, lv_fit(detail, LV_SMALL_W - 64, F_M, F_LBL, F_LBL), G_WHITE, detail);
    } else {
        fmt_word(live_val(LV_WASHER), c, sizeof c);
        op_text(l, LV_HOME_X + 32, LV_ROW1 + 214, lv_fit(c, LV_SMALL_W - 64, F_M, F_LBL, F_LBL), dark ? G_WHITE : G_BLACK, c);
        if (known) snprintf(c, sizeof c, "%d of 3 doors shut", known); else snprintf(c, sizeof c, "No door reports");
        op_text(l, LV_HOME_X + 32, LV_ROW1 + 264, F_LBL, dark ? G_WHITE : G_BLACK, c);
    }

    op_rrect(l, LV_RIGHT_X, LV_ROW2, LV_RIGHT_W, LV_REM_H, 36, 0xE);
    card_title(l, LV_RIGHT_X, LV_ROW2, ICON_CALENDAR_BLANK, "Reminders", false);
    op_text(l, LV_RIGHT_X + 40, LV_ROW2 + 100, F_M, G_BLACK, "Bins");
    fmt_word(live_val(LV_BINS), b, sizeof b);
    op_text(l, LV_RIGHT_X + 40, LV_ROW2 + 158, lv_fit(b, 440, &ui72, F_M, F_LBL), G_BLACK, b);
    op_text(l, LV_RIGHT_X + 40, LV_ROW2 + 254, F_LBL, G_BLACK, live_val(LV_BINS_WHEN));
    op_fill(l, 1290, LV_ROW2 + 36, 2, LV_REM_H - 72, 0xA);
    op_text(l, 1340, LV_ROW2 + 100, F_M, G_BLACK, "Next event");
    const char *event = live_val(LV_EVENT); if (!event[0]) event = "--";
    int eh, em;
    if (sscanf(event, "%d:%d", &eh, &em) == 2 && strlen(event) >= 5) {
        const char *rest = event + 5; while (*rest == ' ' || *rest == '-') rest++;
        snprintf(b, sizeof b, "%02d:%02d", eh, em);
        op_text(l, 1340, LV_ROW2 + 158, &ui72, G_BLACK, b);
        op_text(l, 1340, LV_ROW2 + 254, lv_fit(rest, 450, F_LBL, F_LBL, F_S), G_BLACK, rest);
    } else op_text(l, 1340, LV_ROW2 + 158, lv_fit(event, 450, &ui72, F_M, F_LBL), G_BLACK, event);
}

/* Which regions to repaint for a change of live slot s. */
static void live_mark_slot(int s)
{
    switch (s) {
    case LV_OUT_TEMP: case LV_OUT_COND: case LV_OUT_HI: case LV_OUT_LO: case LV_RAIN:
        mark(36, LV_ROW1, LV_LEFT_W, LV_TILE_H, IT8951_MODE_GL16); break;
    case LV_BED_TEMP: case LV_BED_HUM:
        mark(LV_RIGHT_X, LV_ROW1, LV_SMALL_W, LV_SMALL_H, IT8951_MODE_GL16); break;
    case LV_BINS: case LV_BINS_WHEN: case LV_EVENT:
        mark(LV_RIGHT_X, LV_ROW2, LV_RIGHT_W, LV_REM_H, IT8951_MODE_GL16); break;
    default:
        mark(LV_HOME_X, LV_ROW1, W - 36 - LV_HOME_X, LV_SMALL_H, IT8951_MODE_GL16); break;
    }
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
        if (S.live_layout) mark_changed(&s_prev_body, &L_body, 0, TB_H, W, SB_Y - TB_H, clock_du_active() ? IT8951_MODE_DU : IT8951_MODE_GL16);
        else mark_changed(&s_prev_body, &L_body, 0, LV_HEAD_Y, W, LV_HEAD_H, clock_du_active() ? IT8951_MODE_DU : IT8951_MODE_GL16);   /* the digits that changed, not the whole header */
        any = true;
    }
    if (sig != last_sig) {
        last_sig = sig;
        rebuild();
        if (S.live_layout) mark_changed(&s_prev_body, &L_body, 0, TB_H, W, SB_Y - TB_H, IT8951_MODE_GL16);
        else { mark_changed(&s_prev_body, &L_body, 0, LV_HEAD_Y, W, LV_HEAD_H, IT8951_MODE_GL16);
               mark(LV_RIGHT_X, LV_ROW1, LV_SMALL_W, LV_SMALL_H, IT8951_MODE_GL16); }
        any = true;
    }
    if (changed) {
        rebuild();
        if (S.live_layout) mark_changed(&s_prev_body, &L_body, 0, TB_H, W, SB_Y - TB_H, IT8951_MODE_GL16);
        else for (int i = 0; i < LV_COUNT; i++) if (changed & (1u << i)) live_mark_slot(i);
        any = true;
    }
    return any;
}

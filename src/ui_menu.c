/*
 * ui_menu.c: the click-wheel menus: the top bar, the pages, navigation and every input (the tree is ui_tree.c)
 */
#include "ui_internal.h"

/* ---- the click-wheel UI ---------------------------------------------------------
 * After the iPod. Every control means one thing everywhere:
 *   knob turn      scroll a list one row a detent, move a value (faster turns take bigger steps);
 *                  on the dashboard: volume
 *   knob press     choose (the wheel's centre button)
 *   knob hold      back, the same as MENU
 *   keys 1-3       on the dashboard: their bindings (rain, lights, A/C; holds change
 *                  dashboard and night mode); in a list or a choice: up, down, select;
 *                  on a slider: less, more, done. The top bar labels them for the screen.
 *   key 4  MENU    (orange) menu, then back one level; hold: straight home
 * When the alarm rings the centre stops it and anything else snoozes.
 * The words follow Apple's guidance for a crown-and-remote device: the top bar
 * labels the keys, so no row explains how to press; the line under the
 * preview says what the row does; a list carries icons only when every row
 * has one of its own; a toggle draws a switch; irreversible actions ask twice. */

mode_t_ s_mode = M_HOME;
/* Large text (Settings > Display): the menu type one size up, rows 137 px, eight to a page. */
const bfont_t *mf_s(void)   { return S.large_text ? &ui34 : F_S; }
const bfont_t *mf_lbl(void) { return S.large_text ? &ui46 : F_LBL; }
const bfont_t *mf_m(void)   { return S.large_text ? &ui60 : F_M; }
/* Crisp menus: everything the menus draw is black or white, so the panel's
 * two-level mode (DU: no flash, ~0.45 s) can paint them exactly: opening,
 * stepping, choices, sliders. GC16 (which flashes its whole rectangle and
 * wears the picture on this panel) is then only for the dashboards. */
bool crisp(void) { return S.quick_menus && s_mode != M_HOME; }

static struct { node_t node; int sel, top; } s_stack[6];   /* top: the first row shown, for lists that scroll */
/* How many rows a list of n shows at once: all of them (rows shrink past
 * eight) up to LIST_SCROLL, then a window of LIST_FIT; with large text a
 * window of eight whenever there are more. */
static int list_fit(int n)
{
    if (S.large_text) return n > 8 ? 8 : n;
    return n < LIST_SCROLL ? n : LIST_FIT;
}
static int list_top(int n, int fit, int sel, int *top)
{
    if (n <= fit) { *top = 0; return 0; }
    if (sel < *top) *top = sel;
    if (sel >= *top + fit) *top = sel - fit + 1;
    if (*top > n - fit) *top = n - fit;
    if (*top < 0) *top = 0;
    return *top;
}
int  s_depth;
int  s_rel_armed = -1;                 /* the release row pressed once (Software) */
int  s_armed = -1;                     /* Restart or Wi-Fi setup pressed once: the next press does it */
static item_t s_edit;
static row_def_t s_edit_row;           /* the row being edited (its description goes under the preview) */
static int  s_csel, s_ctop;            /* a choice: the option highlighted, the first shown */
static char s_edit_label[24];

screen_t local_screen(void)
{
    if (s_mode == M_HOME) return SCR_HOME;
    switch (s_stack[s_depth - 1].node) {
    case N_RAIN: case N_RADIO: case N_STATIONS: return SCR_RAIN; case N_LIGHTS: return SCR_LIGHTS; case N_AC: return SCR_AC;
    case N_ALARMS: case N_ALARM: case N_DAYS: case N_CHORUS: return SCR_ALARM; case N_SETTINGS: case N_HOMESCR: case N_NIGHTG: case N_KEYS: case N_KEY: case N_TIMEG: case N_DEVICE: case N_SOFTWARE: case N_SPEAKER:
    case N_DISPLAY: case N_SHOWS: case N_KEYLIGHTS: case N_MENUS: case N_EQ: case N_PANEL: return SCR_SETTINGS;
    default: return SCR_MENU;
    }
}

/* ---- the keys follow the page: what each does on the screen that is showing ---- */
static void key_role(int i, const char **icon, const char **label)
{
    *icon = NULL; *label = "";
    if (i < 0 || i > 3) return;
    if (S.phase == PHASE_RINGING) { *icon = ICON_ALARM; *label = "Snooze"; return; }
    if (s_mode == M_HOME) {
        if (i == 3) { *icon = ICON_LIST; *label = "Menu"; return; }
        const char *b = config_get()->bind[CONFIG_BIND_KEY1 + i];
        *icon = action_icon(b); *label = local_label_for_action(b); return;
    }
    static const char *const NAV[4] = { "Up", "Down", "Select", "Back" }, *const VAL[4] = { "Less", "More", "Done", "Back" };
    *label = s_mode == M_VALUE ? VAL[i] : NAV[i];
    *icon = i == 2 ? ICON_CHECK : i == 3 ? ICON_ARROW_U_UP_LEFT : NULL;     /* up, down, less and more are drawn */
}
const char *local_key_label(int i)
{
    const char *icon, *label; key_role(i, &icon, &label); return label;
}
/* 0 up, 1 down, 2 minus, 3 plus: the icon font has none of these. */
static void key_glyph(layer_t *l, int cx, int cy, int kind, uint8_t g)
{
    if (kind >= 2) { op_fill(l, cx - 18, cy - 3, 36, 6, g); if (kind == 3) op_fill(l, cx - 3, cy - 18, 6, 36, g); return; }
    int d = kind == 0 ? -1 : 1;
    op_line(l, cx - 20, cy - d * 10, cx, cy + d * 10, 6, g);
    op_line(l, cx, cy + d * 10, cx + 20, cy - d * 10, 6, g);
}

/* ---- building ---- */
int s_row_h = 116;              /* shrinks for long lists (the days) */
int s_list_n;                   /* rows in the list on screen */

void build_top(layer_t *l, bool inv)
{
    uint8_t fg = inv ? G_WHITE : G_BLACK, bg = inv ? G_BLACK : G_WHITE, dim = inv ? G_LIGHT : DIM_G;
    (void)dim;
    layer_reset(l, 0, 0, W, TB_H, bg);
    op_fill(l, 0, TB_H - 4, W, 4, inv ? G_DARK : G_BLACK);
    const char *title = "";
    if (S.phase == PHASE_RINGING) title = s_date;
    else if (s_mode == M_HOME) title = s_date;     /* which rotation page is showing is the server's to say */
    else if (s_mode == M_LIST) title = node_title(s_stack[s_depth - 1].node);
    else title = s_edit_label;
    const bfont_t *tf = s_mode == M_HOME ? F_M : mf_m(), *lf = s_mode == M_HOME ? F_LBL : mf_lbl(), *sf = s_mode == M_HOME ? F_S : mf_s();
    op_text(l, MARGIN, 50, tf, fg, title);
    /* the four keys: what each does on this screen, an icon and a word under the key; the orange MENU key framed */
    for (int i = 0; i < 4; i++) {
        int cx = KEY_CX[i];
        const char *ic, *lab; key_role(i, &ic, &lab);
        uint8_t g = i == 3 ? fg : dim;
        op_fill(l, cx - 2, 0, 4, 16, g);
        if (i == 3) op_frame(l, cx - 82, 20, 164, 112, 3, fg);
        if (ic) op_text_c(l, cx, 26, F_IM, g, ic);
        else if (lab[0] && s_mode != M_HOME) key_glyph(l, cx, 49, s_mode == M_VALUE ? 2 + i : i, g);
        if (lab[0]) op_text_c(l, cx, 82, draw_font_w(lf, lab) <= 150 ? lf : sf, g, lab);
    }
}

/* One line, cut at a word with an ellipsis to fit maxw in f. */
static void fit_line(char *s, size_t cap, const bfont_t *f, int maxw)
{
    while (s[0] && draw_font_w(f, s) > maxw) {
        char *sp = strrchr(s, ' ');
        if (!sp) { s[strlen(s) - 1] = 0; continue; }
        *sp = 0; size_t k = strlen(s);
        while (k && (s[k - 1] == ',' || s[k - 1] == '.' || s[k - 1] == ';' || s[k - 1] == ' ')) s[--k] = 0;
        if (draw_font_w(f, s) + draw_font_w(f, "...") <= maxw) { snprintf(s + k, cap - k, "..."); break; }
    }
}

/* A toggle's switch at the row's trailing edge: a filled pill with the knob
 * on the right for on, an outline with the knob on the left for off. Shape
 * carries the state, so it reads the same in two levels and on the black bar. */
static void switch_glyph(layer_t *l, int xr, int cy, bool on, bool selected)
{
    const int sw = 92, sh = 52, r = 26;
    int sx = xr - sw, sy = cy - sh / 2;
    uint8_t ink = selected ? G_WHITE : G_BLACK, paper = selected ? G_BLACK : G_WHITE, line = selected ? G_WHITE : DIM_G;
    if (on) { op_rrect(l, sx, sy, sw, sh, r, ink); op_circle(l, sx + sw - r, cy, 17, paper); }
    else {
        op_rrect(l, sx, sy, sw, sh, r, line); op_rrect(l, sx + 4, sy + 4, sw - 8, sh - 8, r - 4, paper);
        op_circle(l, sx + r, cy, 15, line);
    }
}

/* Where a list is in its rows when it shows a window of them: a thin track at
 * the list's edge with the window's share of it, so a long list looks long. */
static void scroll_bar(layer_t *l, int total, int top, int shown)
{
    if (total <= shown) return;
    int x = LIST_W - 12, y0 = LIST_Y + 8, h = shown * s_row_h - 16;
    op_fill(l, x + 3, y0, 2, h, crisp() ? G_BLACK : G_LIGHT);
    int th = h * shown / total; if (th < 48) th = 48;
    int ty = y0 + (h - th) * top / (total - shown);
    op_rrect(l, x, ty, 8, th, 4, G_BLACK);
}

/* A split list: rows on the left, the highlighted row large on the right.
 * sw: a toggle's state per row (-1 none); total / top: the whole list, for the scroll bar. */
static void build_split(layer_t *l, const char *const *labels, const char *const *icons, char vals[][24],
                        const bool *chev, const bool *check, const int8_t *sw, int n, int sel, int total, int top,
                        const char *pv_icon, const char *pv_big, const char *pv_small)
{
    layer_reset(l, 0, TB_H, W, SB_Y - TB_H, G_WHITE);
    op_fill(l, LIST_W, TB_H, crisp() ? 2 : 3, SB_Y - TB_H, LIGHT_G);
    const bfont_t *fm = mf_m(), *fl = mf_lbl(), *fs = mf_s();
    s_row_h = S.large_text ? (SB_Y - LIST_Y - 10) / 8 : n > 8 ? (SB_Y - LIST_Y - 10) / n : 116;
    for (int i = 0; i < n; i++) {
        int y = LIST_Y + i * s_row_h, cy = y + s_row_h / 2;
        bool s = (i == sel);
        uint8_t fg = s ? G_WHITE : G_BLACK, dim = s ? (crisp() ? G_WHITE : G_LIGHT) : DIM_G;
        if (s) op_rrect(l, 16, y + 3, LIST_W - 32, s_row_h - 6, 28, G_BLACK);      /* a rounded bar with a margin */
        else op_fill(l, MARGIN, y + s_row_h - 2, LIST_W - MARGIN - 30, crisp() ? 1 : 2, LIGHT_G);
        int x = MARGIN;
        if (icons && icons[i]) { op_text(l, x, cy - F_IR->line_h / 2, F_IR, dim, icons[i]); x += 80; }
        /* the trailing side first, so the label knows how much room it has */
        int xr = LIST_W - 40;
        if (chev && chev[i]) { xr -= draw_font_w(F_I, ICON_CARET_RIGHT); op_text(l, xr, cy - F_I->line_h / 2, F_I, dim, ICON_CARET_RIGHT); xr -= 18; }
        if (check && check[i]) { xr -= draw_font_w(F_IM, ICON_CHECK); op_text(l, xr, cy - F_IM->line_h / 2, F_IM, fg, ICON_CHECK); xr -= 18; }
        bool is_sw = sw && sw[i] >= 0;
        if (is_sw) { switch_glyph(l, xr, cy, sw[i] == 1, s); xr -= 92 + 18; }
        int vw = (!is_sw && vals && vals[i][0]) ? draw_font_w(fl, vals[i]) : 0;
        const bfont_t *lf = fm;
        if (x + draw_font_w(lf, labels[i]) + 24 > xr - vw) lf = F_M;        /* large text: a long label drops a size rather than run into its value */
        op_text(l, x, cy - lf->line_h / 2, lf, fg, labels[i]);
        if (vw) op_text_r(l, xr, cy - fl->line_h / 2, fl, dim, vals[i]);
        if (S.turn_hints && !s) {           /* turns from the highlighted row: clockwise is down */
            char t[12]; snprintf(t, sizeof t, "%+d", i - sel);
            op_text_r(l, MARGIN - 12, cy - fs->line_h / 2, fs, DIM_G, t);
        }
    }
    s_list_n = n;
    scroll_bar(l, total, top, n);
    int cx = LIST_W + (W - LIST_W) / 2, y = TB_H + 150;
    if (pv_icon) { op_text_c(l, cx, y, &icon170, G_BLACK, pv_icon); y += icon170.line_h + 30; }
    const bfont_t *bf = numeric(pv_big) && draw_font_w(F_NUM, pv_big) < W - LIST_W - 80 ? F_NUM : F_XL;
    if (draw_font_w(bf, pv_big) > W - LIST_W - 80) bf = fm;
    op_text_c(l, cx, y, bf, G_BLACK, pv_big); y += bf->line_h + 18;
    op_text_c(l, cx, y, fl, DIM_G, pv_small);
}

void build_list(layer_t *l)
{
    node_t nd = s_stack[s_depth - 1].node;
    int n = node_rows(nd, s_rows), sel = s_stack[s_depth - 1].sel;
    if (sel >= n) sel = s_stack[s_depth - 1].sel = n - 1;
    /* a long list shows a window of rows around the selection */
    int fit = list_fit(n), top = list_top(n, fit, sel, &s_stack[s_depth - 1].top), shown = n < fit ? n : fit;
    const char *labels[16], *icons[16]; char vals[16][24]; bool chev[16]; int8_t sw[16];
    for (int i = 0; i < shown; i++) {
        const row_def_t *r = &s_rows[top + i];
        labels[i] = r->label; icons[i] = r->icon; row_value(r, vals[i], 24);
        chev[i] = r->kind == K_SUB || r->kind == K_VALUE || r->kind == K_CHOICE;
        sw[i] = r->kind == K_TOGGLE ? (toggle_get(r) ? 1 : 0) : -1;
    }
    const row_def_t *r = &s_rows[sel];
    char big[24]; snprintf(big, sizeof big, "%s", vals[sel - top][0] ? vals[sel - top] : r->label);
    char small[96]; row_desc(r, small, sizeof small);
    if (r->kind == K_SUB && r->id == S_ALARM_ITEM) snprintf(big, sizeof big, "%s", r->label);
    if (r->id == I_STATUS) { if (S.on_battery) snprintf(big, sizeof big, "Battery %d%%", S.batt_pct); else snprintf(big, sizeof big, "Mains"); }
    if (r->id == A_SOMA) snprintf(big, sizeof big, "%.22s", radio_soma(r->arg)->title);
    fit_line(small, sizeof small, mf_lbl(), W - LIST_W - 60);
    build_split(l, labels, icons, vals, chev, NULL, sw, shown, sel - top, n, top, r->icon, big, small);
}

void build_choice(layer_t *l)
{
    const char *const *o; int n = choice_opts(s_edit, &o), cur = choice_get(s_edit);
    int fit = list_fit(n), top = list_top(n, fit, s_csel, &s_ctop), shown = n < fit ? n : fit;
    const char *labels[16]; bool check[16];
    for (int i = 0; i < shown; i++) { labels[i] = o[top + i]; check[i] = (top + i == cur); }
    char small[96]; row_desc(&s_edit_row, small, sizeof small);
    fit_line(small, sizeof small, mf_lbl(), W - LIST_W - 60);
    build_split(l, labels, NULL, NULL, NULL, check, NULL, shown, s_csel - top, n, top, NULL, o[s_csel], small);
}

void build_value(layer_t *l)
{
    layer_reset(l, 0, TB_H, W, SB_Y - TB_H, G_WHITE);
    vmeta_t m = value_meta(s_edit);
    char t[24]; value_text(s_edit, t, sizeof t);
    /* the value: its number as large as the alarm clock, the unit beside it on the
     * same baseline ("2 of 5", "+3 dB", "60%"); a word ("Natural", a date) as it is */
    int y = TB_H + 100;
    char head[24]; const char *tail = NULL;
    snprintf(head, sizeof head, "%s", t);
    char *sp = strchr(head, ' ');
    if (sp) { *sp = 0; tail = sp + 1; }
    bool plus = head[0] == '+';
    const char *digits = plus ? head + 1 : head;
    if (numeric(digits)) {
        int pw = plus ? draw_font_w(F_XL, "+") + 8 : 0, dw = draw_font_w(F_NUM, digits), tw = tail ? 24 + draw_font_w(F_M, tail) : 0;
        int x = (W - pw - dw - tw) / 2;
        if (plus) { op_text(l, x, y + F_NUM->ascent - F_XL->ascent, F_XL, G_BLACK, "+"); x += pw; }
        op_text(l, x, y, F_NUM, G_BLACK, digits); x += dw;
        if (tail) op_text(l, x + 24, y + F_NUM->ascent - mf_m()->ascent, mf_m(), DIM_G, tail);
    } else op_text_c(l, W / 2, y + F_NUM->ascent - F_XL->ascent, F_XL, G_BLACK, t);
    /* the bar right under it, a tick per step when there are few, the ends named */
    int bx = MARGIN + 60, bw = W - 2 * bx, by = TB_H + 440, bh = 40, span = m.max - m.min ? m.max - m.min : 1;
    int fx = (int)((long)bw * (m.v - m.min) / span);
    if (crisp()) { op_rrect(l, bx, by, bw, bh, 20, G_BLACK); op_rrect(l, bx + 3, by + 3, bw - 6, bh - 6, 17, G_WHITE); }   /* a rounded outline: black and white only */
    else op_rrect(l, bx, by, bw, bh, 20, G_LIGHT);
    if (fx > 0) op_rrect(l, bx, by, fx < bh ? bh : fx, bh, 20, G_BLACK);
    int steps = span / (m.step > 0 ? m.step : 1), nt = steps <= 12 ? steps : 4;
    for (int i = 0; i <= nt; i++) op_fill(l, bx + bw * i / nt - 1, by + bh + 14, 3, 18, DIM_G);
    op_text(l, bx, by + bh + 50, mf_m(), DIM_G, m.lo);
    op_text_r(l, bx + bw, by + bh + 50, mf_m(), DIM_G, m.hi);
    /* what the value does, not how to turn the knob: the top bar labels the keys */
    char d[96]; row_desc(&s_edit_row, d, sizeof d);
    fit_line(d, sizeof d, mf_lbl(), W - 2 * MARGIN);
    op_text_c(l, W / 2, TB_H + 760, mf_lbl(), DIM_G, d);
}

static void disarm(void) { s_armed = -1; s_rel_armed = -1; }
void go_home(void) { s_mode = M_HOME; s_depth = 0; disarm(); printf("local: home\n"); mark_screen(); }
static void push_node(node_t n)
{
    disarm();
    if (s_mode != M_LIST) s_depth = 0;
    if (s_depth == 0 && n != N_ROOT) { s_stack[0].node = N_ROOT; s_stack[0].sel = 0; s_depth = 1; }
    if (s_depth >= 6) return;
    s_stack[s_depth].node = n; s_stack[s_depth].sel = 0; s_stack[s_depth].top = 0; s_depth++;
    s_mode = M_LIST;
    printf("local: %s\n", node_title(n));
    mark_screen();
}
static void list_back(void)
{
    if (s_depth <= 1) { go_home(); return; }
    disarm();
    s_depth--;
    s_mode = M_LIST;
    printf("local: back to %s\n", node_title(s_stack[s_depth - 1].node));
    mark_screen();
}

bool local_open(screen_t s)
{
    s_depth = 0;
    switch (s) {
    case SCR_HOME: go_home(); return true;
    case SCR_MENU: push_node(N_ROOT); return true;
    case SCR_RAIN: push_node(N_RAIN); return true;
    case SCR_LIGHTS: push_node(N_LIGHTS); return true;
    case SCR_AC: push_node(N_AC); return true;
    case SCR_ALARM: push_node(N_ALARMS); return true;
    case SCR_SETTINGS: push_node(N_SETTINGS); return true;
    default: return false;
    }
}

void volume(int d)
{
    int v = clampi(S.vol + d, 0, 10);
    if (v == S.vol) return;
    S.vol = v;
    publish("config/volume", "%d", S.vol);
    rebuild();
    R_VOL();
    if (s_mode == M_HOME && S.dash_page > 0) { char b[4]; snprintf(b, sizeof b, "%d", S.vol); confirm(S.vol ? ICON_SPEAKER_HIGH : ICON_SPEAKER_SLASH, "Volume", b, 4); }
}
/* Speed: detents arriving close together count for more. A slow click moves
 * one step; a quick flick takes steps of 3x / 6x. Values only: in lists and
 * choices every detent is exactly one row. */
static uint32_t s_last_turn_ms;
static int accel_for(int d)
{
    uint32_t now = now_ms(), dt = now - s_last_turn_ms;
    s_last_turn_ms = now;
    int ad = d < 0 ? -d : d;
    if (ad >= 4 || dt < 60)  return 6;
    if (ad >= 2 || dt < 140) return 3;
    return 1;
}

/* Knob turn; d is the signed number of detents since the last call. */
bool local_turn(int d)
{
    if (!d) return true;
    if (S.phase == PHASE_RINGING) { if (!inputs_knob_down()) snooze(); return true; }   /* a detent while the knob is pushed is wobble, not a snooze */
    int acc = accel_for(d);
    switch (s_mode) {
    case M_HOME: volume(d > 0 ? 1 : -1); return true;
    case M_LIST: {
        int *sel = &s_stack[s_depth - 1].sel, n = node_rows(s_stack[s_depth - 1].node, s_rows), old = *sel;
        *sel = clampi(*sel + d, 0, n - 1);          /* one detent, one row: no speed-up in lists */
        if (*sel != old) {
            disarm();                                /* moving off a row pressed once forgets the press */
            int before = s_stack[s_depth - 1].top, top = list_top(n, list_fit(n), *sel, &s_stack[s_depth - 1].top);
            if (top != before) mark_list();          /* the window moved: every row changed */
            else mark_rows(old - top, *sel - top);
        }
        return true; }
    case M_CHOICE: {
        const char *const *o; int n = choice_opts(s_edit, &o), old = s_csel;
        s_csel = clampi(s_csel + d, 0, n - 1);
        if (s_csel != old) {
            if (s_edit == C_ALARM_TONE) tone_preview_soon(s_csel);     /* the sound the highlight rests on plays */
            int before = s_ctop, top = list_top(n, list_fit(n), s_csel, &s_ctop);
            if (top != before) mark_list();
            else mark_rows(old - top, s_csel - top);
        }
        return true; }
    case M_VALUE: value_step(s_edit, d, acc); mark_value(); return true;
    }
    return true;
}

/* The centre button: choose. */
bool local_press(void)
{
    if (S.phase == PHASE_RINGING) { stop_alarm(); return true; }
    switch (s_mode) {
    case M_HOME: push_node(N_ROOT); return true;
    case M_VALUE:
        if (s_edit == V_ALARM_TIME) publish_alarm(s_ai);
        s_mode = M_LIST; mark_screen(); return true;
    case M_CHOICE: {
        const char *const *o; int n = choice_opts(s_edit, &o);
        if (s_csel < n) choice_set(s_edit, s_csel);
        if (s_edit == C_ALARM_TONE) tone_preview(0, 0);
        s_mode = M_LIST; mark_screen(); return true; }
    case M_LIST: {
        int n = node_rows(s_stack[s_depth - 1].node, s_rows), sel = s_stack[s_depth - 1].sel, sl = sel - s_stack[s_depth - 1].top;
        if (sel >= n) return true;
        row_def_t r = s_rows[sel];
        switch (r.kind) {
        case K_TOGGLE: toggle_flip(&r); if (r.id == T_LARGE) mark_screen(); else mark_rows(sl, sl); return true;
        case K_SUB:    if (r.id == S_ALARM_ITEM) s_ai = r.arg; if (r.id == S_KEY) s_ki = r.arg; push_node(sub_node(r.id)); return true;
        case K_VALUE:  if ((r.id == V_TIME || r.id == V_DATE) && S.auto_time) return true;
                       if (r.id == V_SPK_EQ) s_eqb = r.arg;
                       s_edit = r.id; s_edit_row = r; snprintf(s_edit_label, sizeof s_edit_label, "%s", r.label); s_mode = M_VALUE; mark_screen(); return true;
        case K_CHOICE: if (r.id == C_DASH) tesserae_picks_fetch();     /* fresh: dashboards can be bound any time */
                       s_edit = r.id; s_edit_row = r; snprintf(s_edit_label, sizeof s_edit_label, "%s", r.label); s_csel = clampi(choice_get(r.id), 0, 99); s_ctop = 0; s_mode = M_CHOICE; mark_screen();
                       if (r.id == C_ALARM_TONE) tone_preview_soon(s_csel);   /* opening the list plays the current sound */
                       return true;
        case K_BACK:   list_back(); return true;
        case K_ACTION:
            if (r.id == A_ADD_ALARM && s_nalarms < MAX_ALARMS) {
                s_alarms[s_nalarms] = (alarm_t){ 7, 0, true, {1,1,1,1,1,0,0}, 15 };
                s_ai = s_nalarms++; publish_alarm(s_ai); sync_alarm_summary();
                s_stack[s_depth - 1].sel = s_ai;
                push_node(N_ALARM);
            } else if (r.id == A_REFRESH) {
                s_req = LREQ_REFRESH;
            } else if (r.id == A_RESTART || r.id == A_WIFISETUP) {
                /* an interruption you cannot take back: the first press arms it, the second does it */
                if (s_armed != (int)r.id) { s_armed = r.id; mark_rows(sl, sl); }
                else { s_armed = -1; s_req = r.id == A_RESTART ? LREQ_RESTART : LREQ_SETUP; }
            } else if (r.id == A_OTACHECK) {
                ota_releases_fetch(); s_rel_armed = -1; mark_screen();
            } else if (r.id == A_RELEASE) {
                if (s_rel_armed != r.arg) { s_rel_armed = r.arg; int sl = s_stack[s_depth - 1].sel - s_stack[s_depth - 1].top; mark_rows(sl, sl); }
                else { s_rel_armed = -1; if (!ota_install(r.arg)) toast(ICON_GEAR, "Update failed", ota_error()); }
            } else if (r.id == A_PREVIEW) {
                chorus_preview(s_preview_len_ms ? 0 : 90);
                rebuild(); { int sl = s_stack[s_depth - 1].sel - s_stack[s_depth - 1].top; mark_rows(sl, sl); }
            } else if (r.id == A_SOMA || r.id == A_STATION) {
                if (r.id == A_SOMA) { S.station = -1; snprintf(S.soma, sizeof S.soma, "%s", radio_soma(r.arg)->id); }
                else S.station = r.arg;
                S.rain_kind = RS_RADIO; S.rain_on = true;
                publish("set/rain", "radio %s", r.label);
                mark_list();                                    /* "Playing" moves to this row */
            } else if (r.id == A_DELETE_ALARM) {
                publish("set/alarm", "%d delete", r.arg + 1);
                for (int i = r.arg; i < s_nalarms - 1; i++) s_alarms[i] = s_alarms[i + 1];
                s_nalarms--; s_ai = 0; alarms_save(); sync_alarm_summary();
                list_back();
            }
            return true;
        case K_INFO: return true;
        }
    } }
    return true;
}

/* MENU (and knob hold): back one level. */
bool local_back(void)
{
    if (S.phase == PHASE_RINGING) { snooze(); return true; }
    switch (s_mode) {
    case M_HOME: push_node(N_ROOT); return true;
    case M_VALUE: if (s_edit == V_ALARM_TIME) publish_alarm(s_ai); s_mode = M_LIST; mark_screen(); return true;
    case M_CHOICE: if (s_edit == C_ALARM_TONE) tone_preview(0, 0); s_mode = M_LIST; mark_screen(); return true;
    case M_LIST: list_back(); return true;
    }
    return true;
}

void state_changed(void)
{
    rebuild();
    if (s_mode == M_HOME) R_SBMID();
    else if (s_mode == M_LIST) { int s = s_stack[s_depth - 1].sel - s_stack[s_depth - 1].top; mark_rows(s, s); }
}

static void dash_step(int d)
{
    dash_go((S.dash_page + d + 3) % 3);
    if (s_mode != M_HOME) go_home(); else rebuild();
}

/* Keys 1-3 wait to tell a press from a hold only when they have a hold
 * action; MENU always does (hold: home). */
bool local_key_has_hold(int i)
{
    if (S.phase == PHASE_RINGING || i < 0 || i > 3) return false;
    if (i == 3) return true;
    if (s_mode != M_HOME) return false;                   /* in the menus a key acts as soon as it goes down */
    const char *b = config_get()->bind[CONFIG_BIND_KEY1_HOLD + i];
    return b[0] && strcmp(b, "none") != 0;
}

/* True when the UI handled the key itself: the alarm ringing, MENU, or keys
 * 1-3 inside the menus (up / down / select, or less / more / done, as the top
 * bar says). On the dashboard keys 1-3 run their press / hold bindings (main.c). */
bool local_key(int i, bool hold)
{
    if (i < 0 || i > 3) return false;
    if (S.phase == PHASE_RINGING) { snooze(); return true; }
    if (i == 3) { if (hold) { if (s_mode != M_HOME) go_home(); return true; } return local_back(); }
    if (s_mode == M_HOME) return false;
    if (hold) return true;
    if (i == 2) return local_press();
    return local_turn(i == 0 ? -1 : 1);
}

bool local_back_up(void)
{
    if (S.phase == PHASE_RINGING) { stop_alarm(); return true; }     /* held past 600 ms: still the knob, still stop */
    if (s_mode == M_HOME) return false;
    return local_back();
}

bool local_action(const char *id)
{
    if (!id) return false;
    if (!strcmp(id, "menu"))          { if (s_mode == M_HOME) push_node(N_ROOT); else go_home(); return true; }
    if (!strcmp(id, "menu_select"))   return local_press();
    if (!strcmp(id, "back"))          return local_back();
    if (!strcmp(id, "vol.up"))        { volume(+1); return true; }
    if (!strcmp(id, "vol.down"))      { volume(-1); return true; }
    /* The confirmation: the overlay's own item, flashed, when it shows the
     * result (sound bit 4, alarm 2, room 8); a card over the page otherwise. */
    if (!strcmp(id, "rain.toggle"))   { toggle_flip(&R_T_RAIN); state_changed(); confirm(ICON_CLOUD_RAIN, KINDS[S.rain_kind], S.rain_on ? "On" : "Off", 4); return true; }
    if (!strcmp(id, "lights.toggle")) { toggle_flip(&R_T_LIGHTS); state_changed(); confirm(ICON_LIGHTBULB, "Lights", S.lights_on ? "On" : "Off", 8); return true; }
    if (!strcmp(id, "ac.toggle"))     { toggle_flip(&R_T_AC); state_changed();
                                        char v[16]; if (S.ac_mode) snprintf(v, sizeof v, "%s %d" DEG, AC_MODES[S.ac_mode], S.sp_x2 / 2); else snprintf(v, sizeof v, "Off");
                                        confirm(ICON_SNOWFLAKE, "AC", v, 8); return true; }
    if (!strcmp(id, "alarm.toggle"))  { if (s_nalarms) { s_alarms[0].on = !s_alarms[0].on; publish_alarm(0); sync_alarm_summary(); } state_changed();
                                        char v[8]; if (S.alarm_on) alarm_txt(v, sizeof v); else snprintf(v, sizeof v, "Off");
                                        confirm(ICON_ALARM, "Alarm", v, 2); return true; }
    if (!strcmp(id, "night"))         { toggle_flip(&R_T_NIGHT); state_changed(); toast(ICON_MOON, "Night mode", S.night ? "On" : "Off"); return true; }
    if (!strcmp(id, "light.night"))   { S.lights_on = true; S.light_mode = 1; S.bright = 10; publish("set/lights", "night 10%%"); state_changed(); confirm(ICON_LIGHTBULB, "Night light", "10%", 8); return true; }
    if (!strcmp(id, "dash.next"))     { dash_step(+1); return true; }
    if (!strcmp(id, "dash.prev"))     { dash_step(-1); return true; }
    if (!strcmp(id, "live.next"))     { S.live_layout = (S.live_layout + 1) % 4; if (S.dash_page) dash_go(0);
                                        if (s_mode != M_HOME) go_home(); else mark_screen(); return true; }
    if (!strncmp(id, "open.", 5)) {
        const char *n = id + 5;
        node_t nd = !strcmp(n, "ac") ? N_AC : !strcmp(n, "lights") ? N_LIGHTS : !strcmp(n, "sound") || !strcmp(n, "rain") ? N_RAIN :
                    !strcmp(n, "alarm") ? N_ALARMS : !strcmp(n, "settings") || !strcmp(n, "status") || !strcmp(n, "keys") ? N_SETTINGS :
                    !strcmp(n, "menu") ? N_ROOT : N_COUNT;
        if (nd == N_COUNT) return false;
        s_depth = 0; push_node(nd); return true;
    }
    return false;
}

bool local_scenario(const char *name)
{
    if (!strcmp(name, "ring"))    { sync_alarm_summary(); ring(-1); return true; }
    if (!strcmp(name, "prewake")) { S.phase = PHASE_PREWAKE; S.rain_on = false; S.lights_on = true; S.bright = 30; publish("event", "prewake"); state_changed(); return true; }
    if (!strcmp(name, "stop"))    { if (S.phase == PHASE_RINGING) stop_alarm(); else { S.phase = PHASE_IDLE; s_snooze_until_ms = 0; go_home(); } return true; }
    if (!strcmp(name, "power"))   { S.on_battery = true; publish("state/power", "battery %d%%", S.batt_pct); state_changed(); return true; }
    if (!strcmp(name, "mains"))   { S.on_battery = false; publish("state/power", "mains"); state_changed(); return true; }
    return false;
}

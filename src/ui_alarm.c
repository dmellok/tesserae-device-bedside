/*
 * ui_alarm.c: the alarm clock, the dawn chorus and the sleep timer
 */
#include "ui_internal.h"

/* A key cap and a knob seen from above: the two controls the alarm screen names. */
static void key_cap(layer_t *l, int x, int cy, uint8_t ink, uint8_t paper)
{
    op_rrect(l, x, cy - 36, 72, 72, 14, ink);
    op_rrect(l, x + 6, cy - 30, 60, 54, 10, paper);
}
static void knob_glyph(layer_t *l, int cx, int cy, uint8_t ink, uint8_t paper)
{
    op_ring(l, cx, cy, 36, 7, ink, paper);
    op_circle(l, cx, cy, 10, ink);
}

/* The alarm screen: the time, huge, then the two ways out named as Apple's
 * alarm names them: snooze on the prominent block (any key or a turn, what a
 * half-asleep hand finds first) and stop, deliberate, on the knob. */
void build_ring(layer_t *l)
{
    layer_reset(l, 0, TB_H, W, SB_Y - TB_H, G_BLACK);
    op_text_c(l, W / 2, TB_H + 70, F_M, G_LIGHT, "Good morning");
    char t[8]; alarm_txt(t, sizeof t);
    op_text_c(l, W / 2, TB_H + 170, F_HUGE, G_WHITE, t);
    const int bw = 800, bh = 200, gap = 64, by = TB_H + 700;
    int bx1 = (W - 2 * bw - gap) / 2, bx2 = bx1 + bw + gap, ty = by + (bh - F_L->line_h) / 2;
    char s[24]; snprintf(s, sizeof s, "Snooze %d min", S.snooze_min);
    op_rrect(l, bx1, by, bw, bh, 44, G_WHITE);
    key_cap(l, bx1 + 70, by + bh / 2, G_BLACK, G_WHITE);
    op_text(l, bx1 + 180, ty, F_L, G_BLACK, s);
    op_rrect(l, bx2, by, bw, bh, 44, G_WHITE);
    op_rrect(l, bx2 + 5, by + 5, bw - 10, bh - 10, 39, G_BLACK);
    knob_glyph(l, bx2 + 106, by + bh / 2, G_WHITE, G_BLACK);
    op_text(l, bx2 + 180, ty, F_L, G_WHITE, "Stop");
}

/* ---- the alarm clock and the dawn chorus ------------------------------------
 * Runs on the device from the server's clock (Date header + its UTC offset), so
 * the chorus still comes if Home Assistant is down. Before each alarm the wake
 * window (the alarm's "wake up early", else 10 min) builds the chorus from one
 * distant bird to a full one while the sleep sound fades; at the alarm time the
 * panel rings with the chorus at full. A knob press or hold stops; any key or
 * a turn snoozes for 9 min (not a detent while the knob is pushed: that is
 * wobble); it gives up after 30 min. A "Once" alarm (no days)
 * switches itself off after it has rung. */
uint32_t s_ring_ms, s_snooze_until_ms;
int      s_ring_alarm = -1;
static int64_t  s_fired_min = -10;
/* The dawn chorus (pre-wake) has its own stop: the knob, as when it rings.
 * Stopping it skips that ring too (you are up); the minute it would ring: */
static int64_t  s_prewake_ring_min = -10, s_skip_min = -10;
static int      s_prewake_alarm = -1;

bool clock_now(int *wday, int *sec, int64_t *minute)
{
    int64_t e = utc_now();
    if (!e) return false;
    alarm_split_time(e + tz_offset_at(e), wday, sec, minute);
    return true;
}

/* The next enabled alarm and the seconds until it; without a clock yet, the
 * first enabled alarm and -1. */
bool next_alarm(int *idx, int32_t *delta_s)
{
    int wday, sec; int64_t minute;
    *idx = -1; *delta_s = -1;
    if (!clock_now(&wday, &sec, &minute)) {
        for (int i = 0; i < s_nalarms; i++) if (s_alarms[i].on) { *idx = i; return true; }
        return false;
    }
    *idx = alarm_next(s_alarms, s_nalarms, wday * 86400 + sec, false, delta_s);
    return *idx >= 0;
}

/* The alarm sound: it starts with the ring at a third of its volume and is at
 * full after half a minute (RING_RAMP_MS); a preview plays at full and stops itself. */
#define RING_RAMP_MS 30000u
static uint32_t s_tone_due;          /* a preview asked for by the menu, once the knob has settled */
static int      s_tone_want;
void tone_preview(int tone, int seconds)
{
    s_tone_due = 0;
    if (tone <= 0 || tone >= RS_TONES || seconds <= 0) { if (S.phase != PHASE_RINGING) { S.tone_playing = 0; S.tone_until = 0; } return; }
    S.tone_playing = tone; S.tone_level = 1.0f; S.tone_until = (now_ms() + (uint32_t)seconds * 1000u) | 1u;
}
/* Moving the highlight over the sounds plays the one it rests on, 350 ms later (one, not every one passed). */
void tone_preview_soon(int tone)
{
    s_tone_want = tone; s_tone_due = (now_ms() + 350u) | 1u;
}

void ring(int i)
{
    s_ring_alarm = i; s_ring_ms = now_ms(); s_snooze_until_ms = 0; s_preview_len_ms = 0;
    if (i >= 0 && i < s_nalarms) { S.alarm_h = s_alarms[i].h; S.alarm_m = s_alarms[i].m; }
    S.phase = PHASE_RINGING; S.rain_on = false; S.lights_on = true;
    s_tone_due = 0; S.tone_playing = S.alarm_tone; S.tone_level = 0.35f; S.tone_until = 0;   /* the sound, after the chorus has built: gently at first */
    publish("event", "alarm_ringing");
    s_mode = M_HOME; s_depth = 0; mark_screen();
}

void snooze(void)
{
    S.phase = PHASE_IDLE; s_snooze_until_ms = now_ms() + SNOOZE_MS; S.tone_playing = 0; S.tone_until = 0;
    publish("event", "alarm_snoozed %d min", S.snooze_min); go_home();
    /* when it comes back, on the way out: the one fact worth knowing after a snooze */
    int wd, sec; int64_t mn; char v[32];
    if (clock_now(&wd, &sec, &mn)) { char hm[8]; fmt_hm(hm, sizeof hm, (sec / 60 + S.snooze_min) % 1440); snprintf(v, sizeof v, "Rings again %s", hm); }
    else snprintf(v, sizeof v, "Rings again in %d min", S.snooze_min);
    toast(ICON_ALARM, "Snoozed", v);
}

void stop_alarm(void)
{
    S.phase = PHASE_IDLE; s_snooze_until_ms = 0; S.tone_playing = 0; S.tone_until = 0;
    int i = s_ring_alarm; s_ring_alarm = -1;
    if (i >= 0 && i < s_nalarms) {
        if (alarm_is_once(&s_alarms[i])) { s_alarms[i].on = false; alarms_save(); sync_alarm_summary(); }
    }
    publish("event", "alarm_stopped"); go_home();
}

void stop_prewake(void)
{
    S.phase = PHASE_IDLE;
    s_skip_min = s_fired_min = s_prewake_ring_min;
    int i = s_prewake_alarm;
    if (i >= 0 && i < s_nalarms && alarm_is_once(&s_alarms[i])) { s_alarms[i].on = false; alarms_save(); sync_alarm_summary(); }
    publish("event", "alarm_stopped");
    go_home();
    char v[32], hm[8];
    if (i >= 0 && i < s_nalarms) { fmt_hm(hm, sizeof hm, s_alarms[i].h * 60 + s_alarms[i].m); snprintf(v, sizeof v, "No alarm at %s", hm); }
    else snprintf(v, sizeof v, "Alarm skipped");
    toast(ICON_ALARM, "Dawn chorus off", v);
}

void chorus_preview(int seconds)
{
    if (seconds <= 0) { s_preview_len_ms = 0; publish("event", "chorus_preview_stop"); return; }
    s_preview_start_ms = now_ms(); s_preview_len_ms = (uint32_t)seconds * 1000u;
    publish("event", "chorus_preview %d s", seconds);
}

void local_alarm_tick(bool *changed)
{
    uint32_t t = now_ms();
    {   /* auto night: switch at the window's edges only, so a manual change holds until the next one */
        static int last_in = -1;
        int wd, sc; int64_t mn;
        if (S.auto_night && clock_now(&wd, &sc, &mn)) {
            int in = alarm_auto_night_in(S.auto_night, sc / 60) ? 1 : 0;
            if (in != last_in) {
                last_in = in;
                if ((bool)in != S.night) {
                    S.night = in; inputs_set_night(S.night); config_set_lights(inputs_backlight(), S.night); config_save();
                    publish("config/night", S.night ? "on" : "off");
                    rebuild(); R_SBMID(); if (changed) *changed = true;
                }
            }
        } else last_in = -1;
    }
    float c = 0;
    if (s_preview_len_ms) {                         /* preview: build, then 20 s at full */
        uint32_t el = t - s_preview_start_ms;
        if (el < s_preview_len_ms) c = 0.05f + 0.95f * (float)el / (float)s_preview_len_ms;
        else if (el < s_preview_len_ms + 20000u) c = 1.0f;
        else { s_preview_len_ms = 0; if (changed) *changed = true; rebuild(); }
    }
    if (s_tone_due && (int32_t)(t - s_tone_due) >= 0) tone_preview(s_tone_want, 6);   /* the menu's highlight came to rest on a sound */
    if (S.phase == PHASE_RINGING) {
        c = 1.0f;
        uint32_t el = t - s_ring_ms;
        S.tone_level = el >= RING_RAMP_MS ? 1.0f : 0.35f + 0.65f * (float)el / (float)RING_RAMP_MS;
        if (el > RING_MAX_MS) { stop_alarm(); if (changed) *changed = true; }
        S.chorus = c; return;
    }
    if (s_snooze_until_ms) {
        if ((int32_t)(t - s_snooze_until_ms) >= 0) { ring(s_ring_alarm); if (changed) *changed = true; S.chorus = 1.0f; return; }
        S.chorus = c; return;
    }
    int wday, sec; int64_t minute;
    if (!clock_now(&wday, &sec, &minute)) { S.chorus = c; return; }
    int32_t best_delta;
    int best = alarm_next(s_alarms, s_nalarms, wday * 86400 + sec, true, &best_delta);
    if (best >= 0 && best_delta <= 0 && minute - s_fired_min > 1) {
        s_fired_min = minute;
        ring(best); if (changed) *changed = true; S.chorus = 1.0f; return;
    }
    if (best >= 0 && best_delta > 0) {
        int win = alarm_window_s(&s_alarms[best]);
        int64_t ring_min = (minute * 60 + sec % 60 + best_delta) / 60;
        if (best_delta <= win && ring_min == s_skip_min) { if (S.phase == PHASE_PREWAKE) S.phase = PHASE_IDLE; S.chorus = c; return; }   /* stopped this morning */
        if (best_delta <= win) {
            s_prewake_ring_min = ring_min; s_prewake_alarm = best;
            float w = alarm_chorus_level(best_delta, win);
            if (w > c) c = w;
            if (S.phase == PHASE_IDLE) {
                S.phase = PHASE_PREWAKE; S.lights_on = true; S.bright = 30;
                publish("event", "prewake %02d:%02d", s_alarms[best].h, s_alarms[best].m);
                rebuild(); if (s_mode == M_HOME) R_SBMID(); if (changed) *changed = true;
            }
            S.chorus = c; return;
        }
    }
    if (S.phase == PHASE_PREWAKE) S.phase = PHASE_IDLE;   /* the alarm was moved or switched off */
    S.chorus = c;
}

/* Sleep timer: armed when sleep sounds turn on (or the timer is changed while
 * they play); fades the sound over the last minute, then turns it off. */
static uint32_t s_sleep_end;
static bool     s_prev_rain_on;
static int      s_prev_sleep = -1;

float local_sleep_tick(bool *changed)
{
    uint32_t t = now_ms();
    if (S.rain_on != s_prev_rain_on || S.sleep_min != s_prev_sleep) {
        s_sleep_end = (S.rain_on && S.sleep_min) ? t + (uint32_t)S.sleep_min * 60000u : 0;
        s_prev_rain_on = S.rain_on; s_prev_sleep = S.sleep_min;
    }
    if (!s_sleep_end) return 1.0f;
    int32_t left = (int32_t)(s_sleep_end - t);
    if (left <= 0) {
        printf("local: sleep timer ended\n");
        s_sleep_end = 0;
        local_set("rain_on", "0");
        s_prev_rain_on = false;
        if (changed) *changed = true;
        return 0.0f;
    }
    return left < 60000 ? (float)left / 60000.0f : 1.0f;
}

int sleep_left_s(void) { return s_sleep_end ? (int)((int32_t)(s_sleep_end - now_ms()) / 1000) : 0; }

/* The device-side state for the web app, as a JSON object (no braces around
 * the whole: the caller embeds it). Returns bytes written. */

/* ---- the alarms: the list, its summary, flash and MQTT ---- */
alarm_t s_alarms[MAX_ALARMS] = { { 6, 30, true, {1,1,1,1,1,0,0}, 15 } };
int s_nalarms = 1, s_ai;       /* count; the alarm being edited */
const char *const DAY_NAMES[7] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };

void alarm_days_txt(const alarm_t *a, char *b, size_t n)
{
    int c = 0; for (int i = 0; i < 7; i++) c += a->days[i];
    bool wk = a->days[0] && a->days[1] && a->days[2] && a->days[3] && a->days[4];
    if (c == 7) { snprintf(b, n, "Every day"); return; }
    if (c == 5 && wk) { snprintf(b, n, "Weekdays"); return; }
    if (c == 2 && a->days[5] && a->days[6]) { snprintf(b, n, "Weekends"); return; }
    if (c == 0) { snprintf(b, n, "Once"); return; }
    size_t k = 0; b[0] = 0;
    for (int i = 0; i < 7 && k + 4 < n; i++) if (a->days[i]) k += (size_t)snprintf(b + k, n - k, "%s%.2s", k ? " " : "", DAY_NAMES[i]);
}
/* The summary the status bar, the alarm screen and the web page show: the
 * first alarm that is on (or the first alarm). */
void sync_alarm_summary(void)
{
    int k = 0;
    for (int i = 0; i < s_nalarms; i++) if (s_alarms[i].on) { k = i; break; }
    if (!s_nalarms) { S.alarm_on = false; return; }
    S.alarm_h = s_alarms[k].h; S.alarm_m = s_alarms[k].m; S.alarm_on = s_alarms[k].on;
    memcpy(S.days, s_alarms[k].days, 7); S.offset_min = s_alarms[k].early;
}
/* Alarms persist in the flash config. */
void alarms_load(void)
{
    const config_t *c = config_get();
    s_nalarms = c->n_alarms > MAX_ALARMS ? MAX_ALARMS : c->n_alarms;
    for (int i = 0; i < s_nalarms; i++) {
        s_alarms[i].h = c->alarms[i].h % 24; s_alarms[i].m = c->alarms[i].m % 60; s_alarms[i].on = c->alarms[i].on;
        s_alarms[i].early = c->alarms[i].early;
        for (int d = 0; d < 7; d++) s_alarms[i].days[d] = (c->alarms[i].days >> d) & 1;
    }
}
void alarms_save(void)
{
    config_t *c = config_mut();
    c->n_alarms = (uint8_t)s_nalarms;
    for (int i = 0; i < s_nalarms; i++) {
        uint8_t m = 0; for (int d = 0; d < 7; d++) if (s_alarms[i].days[d]) m |= (uint8_t)(1u << d);
        c->alarms[i].h = (uint8_t)s_alarms[i].h; c->alarms[i].m = (uint8_t)s_alarms[i].m; c->alarms[i].on = s_alarms[i].on;
        c->alarms[i].days = m; c->alarms[i].early = (uint8_t)s_alarms[i].early;
    }
    config_save();
}
void publish_alarm(int i)
{
    alarms_save();
    char d[40]; alarm_days_txt(&s_alarms[i], d, sizeof d);
    publish("set/alarm", "%d %s %02d:%02d %s early %d", i + 1, s_alarms[i].on ? "on" : "off", s_alarms[i].h, s_alarms[i].m, d, s_alarms[i].early);
}


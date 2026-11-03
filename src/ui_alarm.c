/*
 * ui_alarm.c: the alarm clock, the dawn chorus and the sleep timer
 */
#include "ui_internal.h"

/* The alarm screen: the time, huge, and what the keys do while it rings. */
void build_ring(layer_t *l)
{
    layer_reset(l, 0, TB_H, W, SB_Y - TB_H, G_BLACK);
    op_text_c(l, W / 2, TB_H + 90, F_M, G_LIGHT, "Good morning");
    char t[8]; alarm_txt(t, sizeof t);
    op_text_c(l, W / 2, TB_H + 200, F_HUGE, G_WHITE, t);
    op_text_c(l, W / 2, TB_H + 620, F_LBL, G_LIGHT, "Press the knob to stop  " DOT "  any key or a turn snoozes");
}

/* ---- the alarm clock and the dawn chorus ------------------------------------
 * Runs on the device from the server's clock (Date header + its UTC offset), so
 * the chorus still comes if Home Assistant is down. Before each alarm the wake
 * window (the alarm's "wake up early", else 10 min) builds the chorus from one
 * distant bird to a full one while the sleep sound fades; at the alarm time the
 * panel rings with the chorus at full. Knob press stops; any key or a turn
 * snoozes for 9 min; it gives up after 30 min. A "Once" alarm (no days)
 * switches itself off after it has rung. */
uint32_t s_ring_ms, s_snooze_until_ms;
int      s_ring_alarm = -1;
static int64_t  s_fired_min = -10;

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

void ring(int i)
{
    s_ring_alarm = i; s_ring_ms = now_ms(); s_snooze_until_ms = 0; s_preview_len_ms = 0;
    if (i >= 0 && i < s_nalarms) { S.alarm_h = s_alarms[i].h; S.alarm_m = s_alarms[i].m; }
    S.phase = PHASE_RINGING; S.rain_on = false; S.lights_on = true;
    publish("event", "alarm_ringing");
    s_mode = M_HOME; s_depth = 0; mark_screen();
}

void snooze(void)
{
    S.phase = PHASE_IDLE; s_snooze_until_ms = now_ms() + SNOOZE_MS;
    publish("event", "alarm_snoozed %d min", S.snooze_min); go_home();
}

void stop_alarm(void)
{
    S.phase = PHASE_IDLE; s_snooze_until_ms = 0;
    int i = s_ring_alarm; s_ring_alarm = -1;
    if (i >= 0 && i < s_nalarms) {
        if (alarm_is_once(&s_alarms[i])) { s_alarms[i].on = false; alarms_save(); sync_alarm_summary(); }
    }
    publish("event", "alarm_stopped"); go_home();
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
    if (S.phase == PHASE_RINGING) {
        c = 1.0f;
        if (t - s_ring_ms > RING_MAX_MS) { stop_alarm(); if (changed) *changed = true; }
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
        if (best_delta <= win) {
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


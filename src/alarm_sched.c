/*
 * alarm_sched.c: the alarm decisions. See alarm_sched.h.
 */
#include "alarm_sched.h"

#include <math.h>

void alarm_split_time(int64_t l, int *wday, int *sec, int64_t *minute)
{
    *wday = (int)(((l / 86400) + 3) % 7);           /* 1 Jan 1970 was a Thursday; 0 = Monday */
    *sec = (int)(l % 86400);
    *minute = l / 60;
}

bool alarm_is_once(const alarm_t *a)
{
    for (int d = 0; d < 7; d++) if (a->days[d]) return false;
    return true;
}

int alarm_next(const alarm_t *a, int n, int now_w, bool in_minute, int32_t *delta_s)
{
    int best = -1;
    int32_t best_delta = 0x7FFFFFFF;
    for (int i = 0; i < n; i++) {
        if (!a[i].on) continue;
        bool once = alarm_is_once(&a[i]);
        int t = a[i].h * 3600 + a[i].m * 60;
        for (int d = 0; d < 7; d++) {
            if (!once && !a[i].days[d]) continue;
            int32_t delta = ((d * 86400 + t) - now_w + WEEK_S) % WEEK_S;   /* seconds until it */
            if (in_minute) { if (delta > WEEK_S - 60) delta -= WEEK_S; }    /* in its minute: due now */
            else if (delta == 0) delta = WEEK_S;
            if (delta < best_delta) { best_delta = delta; best = i; }
        }
    }
    if (delta_s) *delta_s = best >= 0 ? best_delta : -1;
    return best;
}

int alarm_window_s(const alarm_t *a) { return (a->early > 0 ? a->early : 10) * 60; }

float alarm_chorus_level(int32_t delta_s, int window_s)
{
    if (delta_s <= 0 || window_s <= 0 || delta_s > window_s) return 0.0f;
    float pr = 1.0f - (float)delta_s / (float)window_s;
    return 0.05f + 0.75f * powf(pr, 1.2f);
}

bool alarm_auto_night_in(int setting, int m)
{
    if (setting < 1 || setting > 3) return false;
    int from = (20 + setting) * 60, until = 7 * 60;
    return m >= from || m < until;
}

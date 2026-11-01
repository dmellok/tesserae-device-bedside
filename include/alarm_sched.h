/*
 * alarm_sched.h: the alarm decisions, as plain functions of the alarms and the
 * local time, with no clock, flash or screen of their own. local.c owns the
 * state and the side effects (ringing, publishing, drawing); tests/ checks
 * these on the host.
 *
 * Time is "seconds into the week", Monday 00:00 = 0, in local time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct { int h, m; bool on; uint8_t days[7]; int early; } alarm_t;   /* days[0] = Monday; none set = once */

#define WEEK_S (7 * 86400)

/* Local epoch seconds -> weekday (0 = Monday), seconds into the day, and
 * minutes since the epoch (for "already fired this minute"). */
void alarm_split_time(int64_t local_epoch, int *wday, int *sec, int64_t *minute);

bool alarm_is_once(const alarm_t *a);

/* The enabled alarm that comes next from now_w (seconds into the week), and
 * the seconds until it (1 .. WEEK_S). With in_minute, an alarm whose time is
 * this minute counts as due now (delta <= 0) rather than a week away: that is
 * how the tick decides to ring. Returns the index, or -1 if none is on. */
int alarm_next(const alarm_t *a, int n, int now_w, bool in_minute, int32_t *delta_s);

/* The wake-up window before an alarm, in seconds (its "early" minutes, or 10). */
int alarm_window_s(const alarm_t *a);

/* How built up the dawn chorus is, delta_s before the alarm: 0 outside the
 * window, rising from 0.05 at its start to 0.8 just before the alarm. */
float alarm_chorus_level(int32_t delta_s, int window_s);

/* Auto night: setting 0 off, 1..3 = from 21/22/23:00 until 07:00. */
bool alarm_auto_night_in(int setting, int minute_of_day);

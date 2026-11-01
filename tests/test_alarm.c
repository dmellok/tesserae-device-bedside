/*
 * test_alarm.c: the alarm decisions (src/alarm_sched.c): which alarm is next,
 * when it rings, the wake-up window, auto night.
 */
#include "test.h"
#include "alarm_sched.h"

/* Seconds into the week: day 0 = Monday. */
static int W(int day, int h, int m, int s) { return day * 86400 + h * 3600 + m * 60 + s; }
enum { MON, TUE, WED, THU, FRI, SAT, SUN };
static const alarm_t WEEKDAYS_0630 = { 6, 30, true, {1,1,1,1,1,0,0}, 15 };

static void weekday_alarm_next_morning(void)
{
    int32_t d;
    CHECK_EQ(alarm_next(&WEEKDAYS_0630, 1, W(MON, 22, 0, 0), false, &d), 0);
    CHECK_EQ(d, W(TUE, 6, 30, 0) - W(MON, 22, 0, 0));          /* 8 h 30 */
}

static void weekday_alarm_skips_the_weekend(void)
{
    int32_t d;
    alarm_next(&WEEKDAYS_0630, 1, W(FRI, 7, 0, 0), false, &d);   /* Friday, after it rang */
    CHECK_EQ(d, W(MON, 6, 30, 0) + 7 * 86400 - W(FRI, 7, 0, 0));
    alarm_next(&WEEKDAYS_0630, 1, W(SAT, 12, 0, 0), false, &d);
    CHECK_EQ(d, W(MON, 6, 30, 0) + 7 * 86400 - W(SAT, 12, 0, 0));
}

static void sunday_night_wraps_to_monday(void)
{
    int32_t d;
    alarm_next(&WEEKDAYS_0630, 1, W(SUN, 23, 59, 0), false, &d);
    CHECK_EQ(d, 6 * 3600 + 30 * 60 + 60);
}

static void once_alarm_fires_on_any_day(void)
{
    alarm_t once = { 9, 0, true, {0}, 10 };
    int32_t d;
    CHECK_EQ(alarm_next(&once, 1, W(SAT, 8, 0, 0), false, &d), 0);
    CHECK_EQ(d, 3600);
    CHECK(alarm_is_once(&once));
    CHECK(!alarm_is_once(&WEEKDAYS_0630));
}

static void disabled_alarms_are_ignored(void)
{
    alarm_t a[2] = { { 6, 0, false, {1,1,1,1,1,1,1}, 15 }, { 7, 0, false, {0}, 15 } };
    int32_t d = 123;
    CHECK_EQ(alarm_next(a, 2, W(MON, 5, 0, 0), false, &d), -1);
    CHECK_EQ(d, -1);
}

static void picks_the_soonest_of_several(void)
{
    alarm_t a[3] = { { 7, 0, true, {1,1,1,1,1,0,0}, 15 },
                     { 9, 30, true, {0,0,0,0,0,1,1}, 15 },        /* weekends */
                     { 6, 45, true, {0,0,0,1,0,0,0}, 15 } };      /* Thursdays */
    int32_t d;
    CHECK_EQ(alarm_next(a, 3, W(WED, 21, 0, 0), false, &d), 2);    /* Thursday 06:45 beats 07:00 */
    CHECK_EQ(alarm_next(a, 3, W(FRI, 21, 0, 0), false, &d), 1);    /* Saturday 09:30 */
    CHECK_EQ(alarm_next(a, 3, W(SUN, 12, 0, 0), false, &d), 0);    /* Monday 07:00 */
}

/* The tick asks with in_minute: it must ring during the alarm's minute, and
 * only then. */
static void rings_in_its_minute(void)
{
    int32_t d;
    alarm_next(&WEEKDAYS_0630, 1, W(TUE, 6, 30, 0), true, &d);
    CHECK(d <= 0);
    alarm_next(&WEEKDAYS_0630, 1, W(TUE, 6, 30, 59), true, &d);
    CHECK(d <= 0);
    alarm_next(&WEEKDAYS_0630, 1, W(TUE, 6, 29, 59), true, &d);
    CHECK_EQ(d, 1);
    alarm_next(&WEEKDAYS_0630, 1, W(TUE, 6, 31, 0), true, &d);      /* a minute late: next day */
    CHECK(d > 0);
    CHECK_EQ(d, 86400 - 60);
}

static void exact_time_without_in_minute_is_a_week_away(void)
{
    alarm_t daily = { 6, 30, true, {1,1,1,1,1,1,1}, 15 };
    int32_t d;
    alarm_next(&daily, 1, W(TUE, 6, 30, 0), false, &d);
    CHECK_EQ(d, 86400);                                              /* tomorrow's, not "now" */
}

static void wake_window_uses_early_minutes(void)
{
    alarm_t a = WEEKDAYS_0630;
    CHECK_EQ(alarm_window_s(&a), 15 * 60);
    a.early = 0;
    CHECK_EQ(alarm_window_s(&a), 10 * 60);                           /* "Off" still builds over 10 min */
    a.early = 30;
    CHECK_EQ(alarm_window_s(&a), 30 * 60);
}

static void chorus_builds_through_the_window(void)
{
    int win = 15 * 60;
    CHECK_NEAR(alarm_chorus_level(win + 1, win), 0.0, 1e-6);         /* before the window */
    CHECK_NEAR(alarm_chorus_level(win, win), 0.05, 1e-4);            /* its start */
    CHECK_NEAR(alarm_chorus_level(1, win), 0.80, 0.01);              /* just before the alarm */
    CHECK_NEAR(alarm_chorus_level(0, win), 0.0, 1e-6);               /* the alarm itself takes over */
    float prev = 0;
    for (int s = win; s >= 1; s -= 30) {                             /* never falls back as it nears */
        float v = alarm_chorus_level(s, win);
        CHECK(v >= prev);
        prev = v;
    }
}

static void splits_local_time(void)
{
    int wd, sec; int64_t minute;
    alarm_split_time(0, &wd, &sec, &minute);                         /* 1 Jan 1970, a Thursday */
    CHECK_EQ(wd, THU); CHECK_EQ(sec, 0); CHECK_EQ(minute, 0);
    alarm_split_time(1793491200, &wd, &sec, &minute);                /* Sun 1 Nov 2026 00:00 */
    CHECK_EQ(wd, SUN); CHECK_EQ(sec, 0);
    alarm_split_time(1793491200 + 6 * 3600 + 30 * 60 + 15, &wd, &sec, &minute);
    CHECK_EQ(sec, 6 * 3600 + 30 * 60 + 15);
    CHECK_EQ(minute, (1793491200 + 6 * 3600 + 30 * 60) / 60);
}

static void auto_night_window(void)
{
    CHECK(!alarm_auto_night_in(0, 23 * 60));                         /* off */
    CHECK(alarm_auto_night_in(1, 21 * 60));                          /* 21:00 to 07:00 */
    CHECK(!alarm_auto_night_in(1, 20 * 60 + 59));
    CHECK(alarm_auto_night_in(1, 6 * 60 + 59));
    CHECK(!alarm_auto_night_in(1, 7 * 60));
    CHECK(alarm_auto_night_in(3, 23 * 60));                          /* 23:00 to 07:00 */
    CHECK(!alarm_auto_night_in(3, 22 * 60 + 30));
    CHECK(!alarm_auto_night_in(4, 23 * 60));                         /* out of range: off */
}

int main(void)
{
    RUN(weekday_alarm_next_morning);
    RUN(weekday_alarm_skips_the_weekend);
    RUN(sunday_night_wraps_to_monday);
    RUN(once_alarm_fires_on_any_day);
    RUN(disabled_alarms_are_ignored);
    RUN(picks_the_soonest_of_several);
    RUN(rings_in_its_minute);
    RUN(exact_time_without_in_minute_is_a_week_away);
    RUN(wake_window_uses_early_minutes);
    RUN(chorus_builds_through_the_window);
    RUN(splits_local_time);
    RUN(auto_night_window);
    return test_summary("alarm");
}

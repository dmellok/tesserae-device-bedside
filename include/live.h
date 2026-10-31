/*
 * live.h: the values the live dashboard shows (Home Assistant, over MQTT).
 *
 * Each slot reads one MQTT topic. A slot's source is either a Home Assistant
 * entity id ("sensor.bedroom_temperature"), read from MQTT Statestream's
 * <statestream base>/<domain>/<object>/state, or a topic of its own (anything
 * with a '/'). An empty source falls back to <base>/live/<slot key>, so an HA
 * automation can publish there directly (the hourly rain list has to arrive
 * that way: "70,60,45,30,20,10,5,5").
 *
 * Messages land from the lwIP context into a pending copy; live_service()
 * takes them on the main loop, so the UI never reads a half-written value.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LV_OUT_TEMP, LV_OUT_COND, LV_OUT_HI, LV_OUT_LO, LV_RAIN,
    LV_BED_TEMP, LV_BED_HUM, LV_WASHER,
    LV_FRONT, LV_BACK, LV_GARAGE,
    LV_BINS, LV_BINS_WHEN, LV_EVENT,
    LV_COUNT
} lv_slot_t;

void        live_init(void);
const char *live_key(int s);              /* "outside_temp" ... */
const char *live_name(int s);             /* "Outside temperature" ... */
const char *live_source(int s);           /* as configured ("" = default topic) */
/* The MQTT topic for slot s (base = the device's MQTT base topic). */
void        live_topic(int s, const char *base, char *out, size_t n);

/* lwIP context: an incoming publish; returns true when it was a live topic. */
bool        live_on_message(const char *topic, const char *payload, const char *base);
/* Main loop: take pending values; returns a mask of the slots that changed. */
uint32_t    live_service(void);

const char *live_value(int s);            /* "" until known */
bool        live_known(int s);
uint32_t    live_age_s(int s);            /* since the value last changed */
/* Bedroom temperature, one sample per 30 min, oldest first; returns count. */
int         live_history(float *out, int max);

/* Settings (web app): source for one slot, the Statestream base, location. */
bool        live_set_source(int s, const char *src);
void        live_set_ss_base(const char *b);
void        live_set_location(float lat, float lon);
const char *live_ss_base(void);
void        live_location(float *lat, float *lon);
/* Bumped whenever the sources change (ha.c resubscribes). */
uint32_t    live_sources_gen(void);

/* Sunrise for local date y-m-d at the configured place, as minutes after
 * local midnight (tz_s = UTC offset); -1 when the sun does not rise. */
int         live_sunrise_min(int y, int m, int d, int32_t tz_s);

size_t      live_json(char *b, size_t cap);

/* v with dec decimals ("-37.8000", "21.4"): this build's printf has no floats. */
void        live_fmt_dec(char *b, size_t n, double v, int dec);

/*
 * live.c: live dashboard values from Home Assistant over MQTT. See live.h.
 */
#include "live.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "config.h"

static const char *const KEYS[LV_COUNT] = {
    "outside_temp", "outside_cond", "outside_high", "outside_low", "rain_hourly",
    "bedroom_temp", "bedroom_humidity", "washer",
    "front_door", "back_door", "garage",
    "bins", "bins_when", "next_event",
};
static const char *const NAMES[LV_COUNT] = {
    "Outside temperature", "Outside conditions", "Today's high", "Today's low", "Chance of rain, hourly",
    "Bedroom temperature", "Bedroom humidity", "Washing machine",
    "Front door", "Back door", "Garage",
    "Bins", "Bins: when", "Next calendar event",
};
const char *live_key(int s)  { return (s >= 0 && s < LV_COUNT) ? KEYS[s] : ""; }
const char *live_name(int s) { return (s >= 0 && s < LV_COUNT) ? NAMES[s] : ""; }

/* ---- settings (flash blob slot 3) ---- */
#define LV_MAGIC 0x4C495631u   /* "LIV1" */
typedef struct {
    char  ss_base[32];
    char  src[LV_COUNT][56];
    float lat, lon;
} lv_cfg_t;
static lv_cfg_t C;
static uint32_t s_gen = 1;

static void save(void) { config_blob_save(3, LV_MAGIC, &C, sizeof C); s_gen++; }

void live_init(void)
{
    if (!config_blob_load(3, LV_MAGIC, &C, sizeof C)) {
        memset(&C, 0, sizeof C);
        snprintf(C.ss_base, sizeof C.ss_base, "homeassistant_states");
        C.lat = -37.80f; C.lon = 144.97f;            /* Melbourne, as the dashboards' weather */
    }
    C.ss_base[sizeof C.ss_base - 1] = 0;
    for (int i = 0; i < LV_COUNT; i++) C.src[i][sizeof C.src[i] - 1] = 0;
}

const char *live_source(int s) { return (s >= 0 && s < LV_COUNT) ? C.src[s] : ""; }
const char *live_ss_base(void) { return C.ss_base; }
void live_location(float *lat, float *lon) { *lat = C.lat; *lon = C.lon; }
uint32_t live_sources_gen(void) { return s_gen; }

void live_topic(int s, const char *base, char *out, size_t n)
{
    const char *src = live_source(s);
    if (!src[0]) { snprintf(out, n, "%s/live/%s", base, KEYS[s]); return; }
    if (strchr(src, '/')) { snprintf(out, n, "%s", src); return; }
    const char *dot = strchr(src, '.');                 /* entity id -> Statestream topic */
    if (dot) snprintf(out, n, "%s/%.*s/%s/state", C.ss_base, (int)(dot - src), src, dot + 1);
    else snprintf(out, n, "%s/live/%s", base, KEYS[s]);
}

bool live_set_source(int s, const char *src)
{
    if (s < 0 || s >= LV_COUNT) return false;
    while (*src == ' ') src++;
    snprintf(C.src[s], sizeof C.src[s], "%s", src);
    save();
    return true;
}
void live_set_ss_base(const char *b) { snprintf(C.ss_base, sizeof C.ss_base, "%s", b[0] ? b : "homeassistant_states"); save(); }
void live_set_location(float lat, float lon) { C.lat = lat; C.lon = lon; save(); }

/* ---- values ---- */
typedef struct { char v[48]; bool known; uint32_t changed_ms; } val_t;
static val_t V[LV_COUNT];
static char           s_pend[LV_COUNT][48];
static volatile uint32_t s_pend_mask;

#define HIST 25                                   /* 12 h at 30 min */
static float    s_hist[HIST];
static int      s_hn;
static uint32_t s_hist_ms;

bool live_on_message(const char *topic, const char *payload, const char *base)
{
    char t[112];
    bool hit = false;
    for (int s = 0; s < LV_COUNT; s++) {
        live_topic(s, base, t, sizeof t);
        if (strcmp(t, topic)) continue;
        snprintf(s_pend[s], sizeof s_pend[s], "%s", payload);
        s_pend_mask |= 1u << s;
        hit = true;
    }
    return hit;
}

uint32_t live_service(void)
{
    uint32_t changed = 0, now = to_ms_since_boot(get_absolute_time());
    if (s_pend_mask) {
        uint32_t ints = save_and_disable_interrupts();   /* the lwIP IRQ writes these */
        uint32_t m = s_pend_mask; s_pend_mask = 0;
        char tmp[LV_COUNT][48];
        for (int s = 0; s < LV_COUNT; s++) if (m & (1u << s)) memcpy(tmp[s], s_pend[s], sizeof tmp[s]);
        restore_interrupts(ints);
        for (int s = 0; s < LV_COUNT; s++) {
            if (!(m & (1u << s))) continue;
            if (V[s].known && !strcmp(V[s].v, tmp[s])) continue;
            memcpy(V[s].v, tmp[s], sizeof V[s].v);
            V[s].known = true; V[s].changed_ms = now;
            changed |= 1u << s;
        }
    }
    /* bedroom history: a sample every 30 min (and the first one straight away) */
    if (V[LV_BED_TEMP].known && (!s_hn || now - s_hist_ms >= 30u * 60u * 1000u)) {
        float t = (float)atof(V[LV_BED_TEMP].v);
        if (s_hn == HIST) { memmove(s_hist, s_hist + 1, sizeof s_hist[0] * (HIST - 1)); s_hn--; }
        s_hist[s_hn++] = t; s_hist_ms = now;
        changed |= 1u << LV_BED_TEMP;
    }
    return changed;
}

const char *live_value(int s) { return (s >= 0 && s < LV_COUNT && V[s].known) ? V[s].v : ""; }
bool live_known(int s) { return s >= 0 && s < LV_COUNT && V[s].known; }
uint32_t live_age_s(int s) { return (s >= 0 && s < LV_COUNT && V[s].known) ? (to_ms_since_boot(get_absolute_time()) - V[s].changed_ms) / 1000u : 0; }
int live_history(float *out, int max) { int n = s_hn < max ? s_hn : max; memcpy(out, s_hist + (s_hn - n), sizeof(float) * (size_t)n); return n; }

/* ---- sunrise (NOAA's simplified equations) ---- */
int live_sunrise_min(int y, int m, int d, int32_t tz_s)
{
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int doy = cum[(m - 1) % 12] + d + ((m > 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0)) ? 1 : 0);
    const float pi = 3.14159265f, rad = pi / 180.0f;
    float g = 2.0f * pi / 365.0f * ((float)doy - 1.0f);
    float eqt = 229.18f * (0.000075f + 0.001868f * cosf(g) - 0.032077f * sinf(g) - 0.014615f * cosf(2 * g) - 0.040849f * sinf(2 * g));
    float decl = 0.006918f - 0.399912f * cosf(g) + 0.070257f * sinf(g) - 0.006758f * cosf(2 * g) + 0.000907f * sinf(2 * g)
               - 0.002697f * cosf(3 * g) + 0.00148f * sinf(3 * g);
    float lat = C.lat * rad;
    float c = cosf(90.833f * rad) / (cosf(lat) * cosf(decl)) - tanf(lat) * tanf(decl);
    if (c < -1.0f || c > 1.0f) return -1;
    float ha = acosf(c) / rad;
    float utc_min = 720.0f - 4.0f * (C.lon + ha) - eqt;
    int local = (int)lroundf(utc_min + (float)tz_s / 60.0f);
    return ((local % 1440) + 1440) % 1440;
}

void live_fmt_dec(char *b, size_t n, double v, int dec)
{
    long scale = 1; for (int i = 0; i < dec; i++) scale *= 10;
    long q = lround(v * (double)scale);
    const char *sign = q < 0 ? "-" : "";
    if (q < 0) q = -q;
    if (dec) snprintf(b, n, "%s%ld.%0*ld", sign, q / scale, dec, q % scale);
    else snprintf(b, n, "%s%ld", sign, q);
}

size_t live_json(char *b, size_t cap)
{
    size_t n = 0;
#define J(...) do { int k = snprintf(b + n, cap > n ? cap - n : 0, __VA_ARGS__); if (k > 0) n += (size_t)k; } while (0)
    char la[16], lo[16]; live_fmt_dec(la, sizeof la, C.lat, 4); live_fmt_dec(lo, sizeof lo, C.lon, 4);
    J("{\"ss_base\":\"%s\",\"lat\":%s,\"lon\":%s,\"slots\":[", C.ss_base, la, lo);
    for (int s = 0; s < LV_COUNT; s++) {
        char v[100]; size_t k = 0;
        for (const char *p = live_value(s); *p && k + 2 < sizeof v; p++) { if (*p == '"' || *p == '\\') v[k++] = '\\'; v[k++] = (unsigned char)*p < 0x20 ? ' ' : *p; }
        v[k] = 0;
        J("%s{\"key\":\"%s\",\"name\":\"%s\",\"src\":\"%s\",\"value\":\"%s\",\"known\":%d,\"age_s\":%lu}", s ? "," : "",
          KEYS[s], NAMES[s], C.src[s], v, live_known(s), (unsigned long)live_age_s(s));
    }
    J("]}");
#undef J
    return n < cap ? n : cap - 1;
}

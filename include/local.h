/*
 * local.h: the device-drawn UI, after the proposal's interactive mock-up.
 *
 * Layers over the server frame, all composited per row with no frame buffer:
 *   top bar   date (from the server's local_time), page context, battery /
 *             night chips, and the four key labels hanging under the keys
 *   sheet     the menu and the Dashboards picker, on the right
 *   pages     AC, Lights, Sound, Alarm, Weather, Status, Keys: the body below
 *             the top bar, drawn by the device so they respond instantly
 *   volume    a bar along the bottom while the knob sets the volume
 *   ring      the alarm screen, inverted, over everything
 *
 * Input grammar (mock-up): knob turn moves / adjusts, press selects or opens
 * the menu, hold goes back. Keys on the home screen run their bindings (main.c);
 * on a page they become that page's own actions; while the alarm rings they
 * are Snooze / Snooze / Snooze / Stop.
 *
 * Local state (alarm, rain, lights, AC, volume, night, power) lives here with
 * sample values until the MQTT topics land (Phase 3); every change is logged
 * as the topic it will publish.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "it8951.h"

typedef enum {
    SCR_HOME = 0, SCR_MENU, SCR_RAIN, SCR_LIGHTS, SCR_AC, SCR_ALARM, SCR_SETTINGS, SCR_COUNT
} screen_t;

typedef enum { PHASE_IDLE = 0, PHASE_PREWAKE, PHASE_RINGING } alarm_phase_t;

typedef struct {
    int  alarm_h, alarm_m; bool alarm_on; uint8_t days[7]; int offset_min; alarm_phase_t phase;
    bool rain_on; int rain_lvl; int rain_kind; int sleep_min;        /* kind 0 rain 1 ocean 2 brown */
    bool lights_on; int bright; int light_mode;                       /* mode 0 warm 1 night */
    int  ac_mode; int sp_x2; int room_x10; int fan;                   /* mode 0 off 1 heat 2 fan 3 cool 4 dry */
    int  vol;
    bool night, on_battery; int batt_pct;
    int  dash_page; int dash_count;                                   /* server dashboards (rotation steps) */
    /* rain shaping (rainsynth.h), 0..100 unless noted */
    int  patter, wash, tone, wind, surface, thunder, thunder_near;     /* surface 0 open 1 leaves 2 roof 3 window */
    uint32_t strike;                                                  /* bumped to roll thunder now */
    /* the dawn chorus (birds.h): which species, its own volume, how built up it is */
    uint16_t birds; int bird_vol; float chorus; uint32_t audition; int audition_bird;
    int  bird_soft;                                                   /* 0 natural .. 100 soft (rainsynth.h) */
    int  station;                                                     /* the radio preset playing when the kind is Radio (radio.h) */
    /* panel settings: key colours (palette index), knob light, auto night (0 off,
     * 1..3: from 21/22/23:00 to 07:00), 12-hour clock, snooze length */
    int  key_col, menu_col, night_col; bool knob_light; int auto_night; bool clock12; int snooze_min;
    /* Mk II: keys at night (0 dim, 1 off); the home overlay (0 strip, 1 cards
     * left, 2 cards bottom, 3 off), which items it shows (bit 0 clock, 1 alarm,
     * 2 sound, 3 room, 4 connection), at night (0 same, 1 clock only); the
     * time zone (0 = from Tesserae, else a TZONES index) and automatic time */
    int  night_keys, overlay, show, at_night, tz; bool auto_time;
    int  clock_du;                     /* the strip's clock: 0 full quality (GC16), 1 two-level at night, 2 two-level always (DU: no flash, no partial-refresh fade) */
    /* screen updates: 0 fast (partial, a full refresh once input settles),
     * 1 clean (every change with GC16, just its area), 2 full (every change
     * repaints the whole screen with GC16) */
    int  refresh_mode;
    int  wf_temp;                      /* waveform temperature forced on the controller, C; 0 = the firmware default (20) */
    bool turn_hints;                   /* menus: the turns needed to reach each row, beside it */
    bool quick_menus;                  /* menus: knob steps paint black-and-white (fast), full quality once the knob rests */
    int  clean_min;                    /* a full clean refresh this many minutes after partials while idle (default 10) */
    bool settle_full;                  /* ...and that full-quality pass repaints the whole screen (INIT + GC16) rather than the list */
    bool auto_update;                  /* install the newest stable release overnight */
    /* hardware: play through the speaker; read the PowerBoost (battery backup
     * fitted); and which stored file the Recording sound plays ("" = the
     * first sound file) */
    bool speaker, power_mon;
    char rec[40];
    /* speaker tuning (spk_dsp.h): level trim and shelves in dB, soften and
     * low-cut as table indexes, how loud moments are held; and the test sound
     * (0 none, 1 pink, 2 1 kHz, 3 100 Hz, 4 sweep), which stops itself */
    int  spk_gain, spk_bass, spk_treble, spk_soft, spk_hpf, spk_limit;
    int  test; uint32_t test_until;
} local_state_t;

/* How much of the panel a repaint needs, and which waveform suits it. */
typedef struct { int x, y, w, h; uint8_t mode; bool full; } local_dirty_t;

void local_init(void);
/* The PowerBoost's readings (power.h), when battery backup is fitted. */
void local_power(bool on_battery, int pct, bool low);
local_state_t *local_state(void);
screen_t       local_screen(void);
const char    *local_screen_name(screen_t s);

/* Composite the local layers onto one panel row (call after the content). */
void local_overlay_row(int y, uint8_t row[IT8951_ROW_BYTES]);

/* Inputs. Each returns true when the local UI consumed the event; the caller
 * then calls local_take_dirty() and repaints that region. */
bool local_turn(int dir);
bool local_press(void);
bool local_back(void);
/* Back one level, but nothing on the home screen (the knob's hold). */
bool local_back_up(void);
/* A new dashboard frame arrived: the "Loading" card of a pick goes. */
void local_frame_arrived(void);
/* main.c, every loop: the update's progress card, and the overnight check. */
void local_ota_service(void);
/* Screen updates setting (see local_state_t.refresh_mode). */
int  local_refresh_mode(void);
bool local_key(int i, bool hold);
/* True when key i also has a hold action on the current screen. */
bool local_key_has_hold(int i);
bool local_open(screen_t s);
bool local_action(const char *id);     /* "open.ac", "rain.toggle", "vol.up" ... */
void local_set_dirty_full(void);       /* after the content underneath changed */

/* Timers (volume banner expiry). Returns true when a repaint is due. */
bool local_tick(void);
/* The live dashboard (dashboard 0, drawn here): true while it is the chosen
 * dashboard; the tick takes new values and marks what to repaint (true when
 * a repaint is due). */
bool local_live_active(void);
/* Things only main.c can do, asked for from the settings: */
typedef enum { LREQ_NONE, LREQ_REFRESH, LREQ_RESTART, LREQ_RESET, LREQ_SETUP } local_req_t;
local_req_t local_take_request(void);
/* Palette names for the key colours (for the web app / HA). */
int         local_palette_count(void);
const char *local_palette_name(int i);
bool local_live_tick(void);

/* Pop the next dirty rectangle (several small ones per change, not a union).
 * Returns false when none are left. */
bool local_take_dirty(local_dirty_t *out);
/* Bumped on every screen change, so input made while the screen was changing
 * can be told apart and dropped (main.c). */
uint32_t local_epoch(void);

/* The action table (ids + labels) the pages and the web UI pick from. */
int         local_action_count(void);
const char *local_action_id(int i);
const char *local_action_name(int i);

/* Label for key i as the top bar shows it right now ("" = none). */
const char *local_key_label(int i);
/* Label derived from a binding spec on the home screen ("Rain off" ...). */
const char *local_label_for_action(const char *spec);

/* Server local time, ISO 8601 ("2026-10-29T15:37:34+11:00"), for the date. */
void local_set_time(const char *iso);
/* Scenario hooks (serial / web): "ring", "prewake", "stop", "power", "mains". */
bool local_scenario(const char *name);

/* Direct control for the web app: set=<key>|<value> (see local.c). */
bool   local_set(const char *key, const char *val);
/* Device-side state as a JSON object, for /api/state. */
size_t local_state_json(char *b, size_t cap);

/* Panel painting from core 1: freeze the layers, then render rows from the
 * frozen copy (see painter.h). */
/* Run the sleep timer; returns the sound's fade (1 = full, falls to 0 over
 * the last minute). Sets *changed when it turned the sound off. */
float local_sleep_tick(bool *changed);
/* The alarm clock: runs the wake window (the chorus builds), rings, snoozes
 * and previews. Updates S.chorus; sets *changed when the panel needs a repaint. */
void  local_alarm_tick(bool *changed);
/* The knob has rested on a menu row: repaint the list and preview in full quality. True = flush. */
bool  local_settle_tick(void);
/* Save the sound settings a few seconds after they stop changing. */
void  local_persist_tick(void);
/* Read-only views (ha.c). */
int   local_alarm_count(void);
bool  local_alarm_get(int i, int *h, int *m, int *on, int *mask, int *early);
const char *local_dash_name(int i);
int   local_dash_count(void);


void local_snapshot(void);
void local_snapshot_row(int y, uint8_t row[IT8951_ROW_BYTES]);
void local_mark_area(int x, int y, int w, int h, uint8_t mode);

/*
 * ui_api.c: the outside faces: the web app's direct control, ha.c views, settings persistence, power and the state JSON
 */
#include "ui_internal.h"

/* ---- direct control (the phone web app) -------------------------------------- */
static void refresh_after_set(void)
{
    rebuild();
    if (s_mode == M_HOME) R_SBMID();
    else mark(0, TB_H, W, SB_Y - TB_H, IT8951_MODE_GL16);
}

/* set=<key>|<value>. Returns false for an unknown key or a bad value. */
bool local_set(const char *key, const char *val)
{
    int v = atoi(val);
    if (!strcmp(key, "vol"))            { if (v < 0 || v > 10) return false; S.vol = v; publish("config/volume", "%d", v); rebuild(); R_VOL(); return true; }
    else if (!strcmp(key, "rain_on"))   { S.rain_on = v != 0; publish("set/rain", S.rain_on ? "play" : "pause"); }
    else if (!strcmp(key, "rain_lvl"))  { if (v < 1 || v > 5) return false; S.rain_lvl = v; publish("set/rain", "level %d", v); }
    else if (!strcmp(key, "rain_kind")) { if (v < 0 || v > 4) return false; S.rain_kind = v; publish("set/rain", "%s", KINDS[v]); }
    else if (!strcmp(key, "sleep"))     { if (v != 0 && v != 15 && v != 30 && v != 60) return false; S.sleep_min = v; publish("set/rain", "sleep %d", v); }
    else if (!strcmp(key, "lights_on")) { S.lights_on = v != 0; publish("set/lights", S.lights_on ? "on %d%%" : "off", S.bright); }
    else if (!strcmp(key, "bright"))    { if (v < 5 || v > 100) return false; S.bright = v; S.lights_on = true; publish("set/lights", "on %d%%", v); }
    else if (!strcmp(key, "colour"))    { if (v < 0 || v > 1) return false; S.light_mode = v; publish("set/lights", "%s", v ? "night" : "warm"); }
    else if (!strcmp(key, "ac_mode"))   { if (v < 0 || v > 4) return false; S.ac_mode = v; publish("set/ac", "%s", AC_MODES[v]); }
    else if (!strcmp(key, "setpoint"))  { int x2 = (int)(atof(val) * 2 + 0.5); if (x2 < 32 || x2 > 60) return false; S.sp_x2 = x2; if (!S.ac_mode) S.ac_mode = 3; publish("set/ac", "%s", val); }
    else if (!strcmp(key, "fan"))       { if (v < 0 || v > 3) return false; S.fan = v; publish("set/ac", "fan %s", FANS[v]); }
    else if (!strcmp(key, "night"))     { if ((v != 0) != S.night) toggle_flip(&R_T_NIGHT); }
    else if (!strcmp(key, "keylights")) { if (v < 0 || v > 100) return false; uint8_t lv = (uint8_t)(v * 255 / 100); inputs_set_backlight(lv); config_set_lights(lv, S.night); config_save(); return true; }
    else if (!strcmp(key, "dash"))      { if (v < 0 || v > 2) return false; dash_go(v); if (s_mode == M_HOME) mark_screen(); }
    else if (!strcmp(key, "pick"))      { if (v < -1 || v >= tesserae_pick_count()) return false; if (v < 0) { dash_go(0); s_dash_sel = 0; } else dash_pick(v + 1); }
    else if (!strcmp(key, "patter") || !strcmp(key, "wash") || !strcmp(key, "tone") || !strcmp(key, "wind") ||
             !strcmp(key, "thunder") || !strcmp(key, "thunder_near")) {
        if (v < 0 || v > 100) return false;
        int *f = !strcmp(key, "patter") ? &S.patter : !strcmp(key, "wash") ? &S.wash : !strcmp(key, "tone") ? &S.tone :
                 !strcmp(key, "wind") ? &S.wind : !strcmp(key, "thunder") ? &S.thunder : &S.thunder_near;
        *f = v; publish("set/sound", "%s %d", key, v);
    }
    else if (!strcmp(key, "surface"))   { if (v < 0 || v > 3) return false; S.surface = v; publish("set/rain", "surface %s", SURFACES[v]); }
    else if (!strcmp(key, "strike"))    { S.strike++; publish("event", "thunder"); return true; }
    else if (!strcmp(key, "key_col") || !strcmp(key, "menu_col") || !strcmp(key, "night_col")) {
        if (v < 0 || v >= N_PAL) return false;
        *(!strcmp(key, "key_col") ? &S.key_col : !strcmp(key, "menu_col") ? &S.menu_col : &S.night_col) = v;
        apply_colours(); return true;
    }
    else if (!strcmp(key, "knob_light")) { S.knob_light = v != 0; apply_colours(); return true; }
    else if (!strcmp(key, "auto_night")) { if (v < 0 || v > 3) return false; S.auto_night = v; return true; }
    else if (!strcmp(key, "clock12"))    { S.clock12 = v != 0; rebuild(); if (s_mode == M_HOME && S.dash_page == 0) mark(0, LV_HEAD_Y, W, LV_HEAD_H, IT8951_MODE_GL16); return true; }
    else if (!strcmp(key, "snooze"))     { if (v < 1 || v > 30) return false; S.snooze_min = v; return true; }
    else if (!strcmp(key, "night_keys")) { if (v < 0 || v > 1) return false; S.night_keys = v; apply_colours(); return true; }
    else if (!strcmp(key, "overlay"))    { if (v < 0 || v > 3) return false; S.overlay = v; overlay_apply(); if (s_mode == M_HOME) mark_screen(); return true; }
    else if (!strcmp(key, "show"))       { if (v < 0 || v > 31) return false; S.show = v; overlay_apply(); if (s_mode == M_HOME) mark_screen(); return true; }
    else if (!strcmp(key, "at_night"))   { if (v < 0 || v > 1) return false; S.at_night = v; rebuild(); if (ov_active()) mark_ov(); return true; }
    else if (!strcmp(key, "tz"))         { if (v < 0 || v >= N_TZ) return false; S.tz = v; rebuild(); if (ov_active()) mark_ov(); return true; }
    else if (!strcmp(key, "autoupdate"))   { S.auto_update = v != 0; return true; }
    else if (!strcmp(key, "speaker"))      { S.speaker = v != 0; publish("config/speaker", S.speaker ? "on" : "off"); return true; }
    /* speaker tuning (spk_dsp.h) and the birds' softness: nothing to repaint */
    else if (!strcmp(key, "bird_soft"))    { if (v < 0 || v > 100) return false; S.bird_soft = v; return true; }
    else if (!strcmp(key, "spk_gain"))     { if (v < -12 || v > 6) return false; S.spk_gain = v; return true; }
    else if (!strcmp(key, "spk_bass"))     { if (v < -8 || v > 8) return false; S.spk_bass = v; return true; }
    else if (!strcmp(key, "spk_treble"))   { if (v < -12 || v > 6) return false; S.spk_treble = v; return true; }
    else if (!strcmp(key, "spk_soft"))     { if (v < 0 || v > 5) return false; S.spk_soft = v; return true; }
    else if (!strcmp(key, "spk_hpf"))      { if (v < 0 || v > 4) return false; S.spk_hpf = v; return true; }
    else if (!strcmp(key, "spk_limit"))    { if (v < 0 || v > 2) return false; S.spk_limit = v; return true; }
    else if (!strcmp(key, "test"))         { if (v < 0 || v > 4) return false; S.test = v; S.test_until = v ? now_ms() + 20000u : 0; return true; }
    else if (!strcmp(key, "power_mon"))    { S.power_mon = v != 0; if (!S.power_mon && S.on_battery) { S.on_battery = false; state_changed(); } return true; }
    else if (!strcmp(key, "rec"))          { if (strlen(val) >= sizeof S.rec) return false; snprintf(S.rec, sizeof S.rec, "%s", val); }
    else if (!strcmp(key, "refresh_mode")) { if (v < 0 || v > 2) return false; S.refresh_mode = v; s_req = LREQ_REFRESH; return true; }
    else if (!strcmp(key, "turn_hints"))   { S.turn_hints = v != 0; if (s_mode == M_LIST || s_mode == M_CHOICE) mark_screen(); return true; }
    else if (!strcmp(key, "quick_menus"))  { S.quick_menus = v != 0; return true; }
    else if (!strcmp(key, "settle_full"))  { S.settle_full = v != 0; return true; }
    else if (!strcmp(key, "clean_min"))    { if (v < 1 || v > 240) return false; S.clean_min = v; return true; }
    else if (!strcmp(key, "wf_temp"))      { if (v < 0 || v > 50) return false; S.wf_temp = v; s_req = LREQ_REFRESH; return true; }
    else if (!strcmp(key, "auto_time"))  { S.auto_time = v != 0; if (!S.auto_time) manual_start(); rebuild(); return true; }
    else if (!strcmp(key, "birds"))     { if (v < 0 || v > BIRDS_ALL) return false; S.birds = (uint16_t)v; }
    else if (!strcmp(key, "bird_vol"))  { if (v < 1 || v > 10) return false; S.bird_vol = v; return true; }
    else if (!strcmp(key, "audition"))  { if (v < 0 || v >= BIRD_COUNT) return false; S.audition_bird = v; S.audition++; return true; }
    else if (!strcmp(key, "chorus_preview")) { if (v < 0 || v > 900) return false; chorus_preview(v); }
    else if (!strcmp(key, "alarm_stop"))  { if (S.phase == PHASE_RINGING) stop_alarm(); else if (s_snooze_until_ms) { s_snooze_until_ms = 0; s_ring_alarm = -1; } else return false; }
    else if (!strcmp(key, "alarm_snooze")) { if (S.phase != PHASE_RINGING) return false; snooze(); }
    else if (!strcmp(key, "alarm")) {
        /* i,h,m,on,daysmask,early */
        int i, h, m, on, mask, early;
        if (sscanf(val, "%d,%d,%d,%d,%d,%d", &i, &h, &m, &on, &mask, &early) != 6) return false;
        if (i < 0 || i >= s_nalarms || h < 0 || h > 23 || m < 0 || m > 59 || mask < 0 || mask > 127 || early < 0 || early > 60) return false;
        s_alarms[i].h = h; s_alarms[i].m = m; s_alarms[i].on = on != 0; s_alarms[i].early = early;
        for (int d = 0; d < 7; d++) s_alarms[i].days[d] = (mask >> d) & 1;
        publish_alarm(i); sync_alarm_summary();
    }
    else if (!strcmp(key, "alarm_add")) {
        if (s_nalarms >= MAX_ALARMS) return false;
        s_alarms[s_nalarms] = (alarm_t){ 7, 0, true, {1,1,1,1,1,0,0}, 15 };
        publish_alarm(s_nalarms++); sync_alarm_summary();
    }
    else if (!strcmp(key, "alarm_del")) {
        if (v < 0 || v >= s_nalarms) return false;
        publish("set/alarm", "%d delete", v + 1);
        for (int i = v; i < s_nalarms - 1; i++) s_alarms[i] = s_alarms[i + 1];
        s_nalarms--; if (s_ai >= s_nalarms) s_ai = 0;
        alarms_save(); sync_alarm_summary();
        if (s_mode != M_HOME && local_screen() == SCR_ALARM) { s_depth = 0; go_home(); return true; }
    }
    else return false;
    refresh_after_set();
    return true;
}

/* ---- read-only views for ha.c ---- */
int  local_alarm_count(void) { return s_nalarms; }
bool local_alarm_get(int i, int *h, int *m, int *on, int *mask, int *early)
{
    if (i < 0 || i >= s_nalarms) return false;
    *h = s_alarms[i].h; *m = s_alarms[i].m; *on = s_alarms[i].on; *early = s_alarms[i].early;
    *mask = 0; for (int d = 0; d < 7; d++) if (s_alarms[i].days[d]) *mask |= 1 << d;
    return true;
}
const char *local_dash_name(int i) { return (i >= 0 && i < 3) ? DASHES[i] : ""; }
int  local_dash_count(void) { return 3; }

/* ---- sound settings persistence (flash blob slot 1) ---- */
static void snd_pack(snd_blob_t *b)
{
    memset(b, 0, sizeof *b);
    b->vol = (int8_t)S.vol; b->rain_kind = (int8_t)S.rain_kind; b->rain_lvl = (int8_t)S.rain_lvl; b->surface = (int8_t)S.surface;
    b->patter = (uint8_t)S.patter; b->wash = (uint8_t)S.wash; b->tone = (uint8_t)S.tone; b->wind = (uint8_t)S.wind;
    b->thunder = (uint8_t)S.thunder; b->thunder_near = (uint8_t)S.thunder_near;
    b->bird_vol = (uint8_t)S.bird_vol; b->birds = S.birds;
    b->pad = 1;                         /* 1: saved after the synthesised birds were added */
}
static snd_blob_t s_snd_saved;
static uint32_t   s_snd_changed_at;
void sound_load(void)
{
    snd_blob_t b;
    if (!config_blob_load(1, SND_MAGIC, &b, sizeof b)) { snd_pack(&s_snd_saved); return; }
    S.vol = clampi(b.vol, 0, 10); S.rain_kind = clampi(b.rain_kind, 0, 4); S.rain_lvl = clampi(b.rain_lvl, 1, 5); S.surface = clampi(b.surface, 0, 3);
    S.bird_vol = clampi(b.bird_vol, 1, 10); S.birds = b.birds & BIRDS_ALL;
    if (b.pad != 1) S.birds |= BIRDS_SYNTH;      /* settings from before: switch the new birds on once */
    S.patter = b.patter; S.wash = b.wash; S.tone = b.tone; S.wind = b.wind; S.thunder = b.thunder; S.thunder_near = b.thunder_near;
    s_snd_saved = b;
    printf("local: sound settings loaded\n");
}
/* Panel settings (blob slot 4). */
static set_blob_t s_set_saved;
static uint32_t   s_set_changed_at;
static void set_pack(set_blob_t *b)
{
    memset(b, 0, sizeof *b);
    b->key_col = (uint8_t)S.key_col; b->menu_col = (uint8_t)S.menu_col; b->night_col = (uint8_t)S.night_col;
    b->knob = S.knob_light; b->auto_night = (uint8_t)S.auto_night; b->clock12 = S.clock12; b->snooze = (uint8_t)S.snooze_min;
    b->night_keys = (uint8_t)S.night_keys; b->overlay = (uint8_t)S.overlay; b->show = (uint8_t)S.show; b->at_night = (uint8_t)S.at_night;
    b->tz = (uint8_t)S.tz; b->auto_time = S.auto_time; b->refresh_mode = (uint8_t)S.refresh_mode; b->auto_update = S.auto_update;
    b->pad[0] = (uint8_t)S.wf_temp;
}
void settings_load(void)
{
    set_blob_t b; set1_blob_t o;
    if (config_blob_load(4, SET2_MAGIC, &b, sizeof b)) {
        S.night_keys = b.night_keys % 2; S.overlay = b.overlay % 4; S.show = b.show & 0x1F; S.at_night = b.at_night % 2;
        S.tz = b.tz % N_TZ; S.auto_time = b.auto_time; S.refresh_mode = b.refresh_mode % 3; S.auto_update = b.auto_update == 1;
        S.wf_temp = b.pad[0] <= 50 ? b.pad[0] : 0;
    } else if (config_blob_load(4, SET_MAGIC, &o, sizeof o)) {
        memset(&b, 0, sizeof b); memcpy(&b, &o, 7);           /* the SET1 fields; Mk II ones stay at their defaults */
    } else b.snooze = 0, b.knob = 2;
    if (b.knob != 2) {
        S.key_col = b.key_col % N_PAL; S.menu_col = b.menu_col % N_PAL; S.night_col = b.night_col % N_PAL;
        S.knob_light = b.knob; S.auto_night = b.auto_night % 4; S.clock12 = b.clock12;
        S.snooze_min = (b.snooze >= 1 && b.snooze <= 30) ? b.snooze : 9;
    }
    set_pack(&s_set_saved);
    apply_colours();
    overlay_apply();
}

/* Hardware settings and the recording (blob slot 5). */
static ext_blob_t s_ext_saved;
static uint32_t   s_ext_changed_at;
static void ext_pack(ext_blob_t *b)
{
    memset(b, 0, sizeof *b);
    b->speaker = S.speaker; b->power_mon = S.power_mon;
    b->rsv0 = (uint8_t)((S.turn_hints ? 0 : 1) | (S.quick_menus ? 0 : 2) | (S.settle_full ? 4 : 0));   /* bits 0-1 set = off (older blobs read as on); bit 2 set = on */
    b->pad = (uint8_t)S.clean_min;
    b->spk_gain = (int8_t)S.spk_gain; b->spk_bass = (int8_t)S.spk_bass; b->spk_treble = (int8_t)S.spk_treble;
    b->spk_soft = (uint8_t)S.spk_soft; b->spk_hpf = (uint8_t)S.spk_hpf; b->spk_limit = (uint8_t)S.spk_limit;
    b->bird_soft_p1 = (uint8_t)(S.bird_soft + 1);
    memcpy(b->rec, S.rec, sizeof b->rec);
}
void ext_load(void)
{
    ext_blob_t b;
    if (config_blob_load(5, EXT_MAGIC, &b, sizeof b)) {
        S.speaker = b.speaker; S.power_mon = b.power_mon;
        S.turn_hints = !(b.rsv0 & 1); S.quick_menus = !(b.rsv0 & 2); S.settle_full = (b.rsv0 & 4) != 0;
        S.clean_min = (b.pad >= 1 && b.pad <= 240) ? b.pad : 10;
        S.spk_gain = clampi(b.spk_gain, -12, 6); S.spk_bass = clampi(b.spk_bass, -8, 8); S.spk_treble = clampi(b.spk_treble, -12, 6);
        S.spk_soft = clampi(b.spk_soft, 0, 5); S.spk_hpf = clampi(b.spk_hpf, 0, 4); S.spk_limit = clampi(b.spk_limit, 0, 2);
        if (b.bird_soft_p1) S.bird_soft = clampi(b.bird_soft_p1 - 1, 0, 100);
        b.rec[39] = 0; memcpy(S.rec, b.rec, sizeof S.rec);
    }
    ext_pack(&s_ext_saved);
}

void local_power(bool on_battery, int pct, bool low)
{
    bool was = S.on_battery;
    int before = S.batt_pct;
    S.on_battery = on_battery; S.batt_pct = pct;
    if (on_battery != was) {
        publish("state/power", on_battery ? "battery %d%%" : "mains", pct);
        if (on_battery) toast(ICON_BATTERY_CHARGING, "On battery", "");
        else            toast(ICON_PLUG, "Mains power", "");
        state_changed();
    } else if (on_battery && pct != before) {
        publish("state/power", "battery %d%%", pct);
        if (low && before >= 15 && pct < 15) toast(ICON_BATTERY_CHARGING, "Battery low", "");
        state_changed();
    }
}

void local_persist_tick(void)
{
    {   ext_blob_t now; ext_pack(&now);
        if (memcmp(&now, &s_ext_saved, sizeof now)) {
            uint32_t t = now_ms();
            if (!s_ext_changed_at) s_ext_changed_at = t;
            else if (t - s_ext_changed_at >= 3000) {
                s_ext_changed_at = 0;
                if (config_blob_save(5, EXT_MAGIC, &now, sizeof now)) { s_ext_saved = now; printf("local: hardware settings saved\n"); }
            }
        } else s_ext_changed_at = 0;
    }
    {   set_blob_t now; set_pack(&now);
        if (memcmp(&now, &s_set_saved, sizeof now)) {
            uint32_t t = now_ms();
            if (!s_set_changed_at) s_set_changed_at = t;
            else if (t - s_set_changed_at >= 3000) {
                s_set_changed_at = 0;
                if (config_blob_save(4, SET2_MAGIC, &now, sizeof now)) { s_set_saved = now; printf("local: settings saved\n"); }
            }
        } else s_set_changed_at = 0;
    }
    snd_blob_t now; snd_pack(&now);
    if (!memcmp(&now, &s_snd_saved, sizeof now)) { s_snd_changed_at = 0; return; }
    uint32_t t = now_ms();
    if (!s_snd_changed_at) { s_snd_changed_at = t; return; }
    if (t - s_snd_changed_at < 4000) return;               /* wait until the knob stops */
    s_snd_changed_at = 0;
    if (config_blob_save(1, SND_MAGIC, &now, sizeof now)) { s_snd_saved = now; printf("local: sound settings saved\n"); }
}

size_t local_state_json(char *b, size_t cap)
{
    size_t n = 0;
#define J(...) do { int k = snprintf(b + n, cap > n ? cap - n : 0, __VA_ARGS__); if (k > 0) n += (size_t)k; } while (0)
    J("{\"vol\":%d,\"rain\":{\"on\":%d,\"lvl\":%d,\"kind\":%d,\"sleep\":%d,\"sleep_left_s\":%d},", S.vol, S.rain_on, S.rain_lvl, S.rain_kind, S.sleep_min, sleep_left_s() > 0 ? sleep_left_s() : 0);
    {
        int left = s_preview_len_ms ? (int)((int32_t)(s_preview_start_ms + s_preview_len_ms + 20000u - now_ms()) / 1000) : 0;
        int snz = s_snooze_until_ms ? (int)((int32_t)(s_snooze_until_ms - now_ms()) / 1000) : 0;
        J("\"chorus\":{\"birds\":%u,\"vol\":%d,\"soft\":%d,\"level\":%d,\"preview_s\":%d,\"snooze_s\":%d,\"names\":[", S.birds, S.bird_vol, S.bird_soft, (int)(S.chorus * 100), left > 0 ? left : 0, snz > 0 ? snz : 0);
        for (int i = 0; i < BIRD_COUNT; i++) J("%s\"%s\"", i ? "," : "", bird_name(i));
        J("]},");
    }
    J("\"panel\":{\"key_col\":%d,\"menu_col\":%d,\"night_col\":%d,\"knob_light\":%d,\"auto_night\":%d,\"clock12\":%d,\"snooze\":%d,\"palette\":[",
      S.key_col, S.menu_col, S.night_col, S.knob_light, S.auto_night, S.clock12, S.snooze_min);
    for (int i = 0; i < N_PAL; i++) J("%s\"%s\"", i ? "," : "", PAL[i].name);
    {   int vx, vy, vw, vh; viewport(&vx, &vy, &vw, &vh);
        J("],\"auto_update\":%d,\"turn_hints\":%d,\"quick_menus\":%d,\"settle_full\":%d,\"clean_min\":%d,\"wf_temp\":%d,\"temp_sensed\":%d,\"temp_forced\":%d,\"refresh_mode\":%d,\"night_keys\":%d,\"overlay\":%d,\"show\":%d,\"at_night\":%d,\"tz\":%d,\"auto_time\":%d,\"vp\":[%d,%d,%d,%d],\"tzs\":[",
          S.auto_update, S.turn_hints, S.quick_menus, S.settle_full, S.clean_min, S.wf_temp, it8951_temp_sensed(), it8951_temp_forced(), S.refresh_mode, S.night_keys, S.overlay, S.show, S.at_night, S.tz, S.auto_time, vx, vy, vw, vh); }
    for (int i = 0; i < N_TZ; i++) J("%s\"%s\"", i ? "," : "", TZ_NAMES[i]);
    J("],\"keyacts\":[");
    for (int i = 0; i < N_KEYACT; i++) J("%s{\"id\":\"%s\",\"label\":\"%s\"}", i ? "," : "", KEYACT_ID[i], KEYACT_NAME[i]);
    J("],\"knobacts\":[");
    for (int i = 0; i < N_KNOBACT; i++) J("%s{\"id\":\"%s\",\"label\":\"%s\"}", i ? "," : "", KNOBACT_ID[i], KNOBACT_NAME[i]);
    J("]},");
    J("\"shape\":{\"patter\":%d,\"wash\":%d,\"tone\":%d,\"wind\":%d,\"surface\":%d,\"thunder\":%d,\"thunder_near\":%d},",
      S.patter, S.wash, S.tone, S.wind, S.surface, S.thunder, S.thunder_near);
    J("\"spk\":{\"gain\":%d,\"bass\":%d,\"treble\":%d,\"soft\":%d,\"hpf\":%d,\"limit\":%d,\"test\":%d,\"dropouts\":%lu},",
      S.spk_gain, S.spk_bass, S.spk_treble, S.spk_soft, S.spk_hpf, S.spk_limit, S.test, (unsigned long)audio_speaker_dropouts());
    J("\"hw\":{\"speaker\":%d,\"spk_on\":%d,\"i2s_fills\":%lu,\"i2s_diag\":%lu,\"power_mon\":%d,\"on_battery\":%d,\"pct\":%d,\"bat_mv\":%d,\"usb_mv\":%d,"
      "\"rec\":\"%s\"},",
      S.speaker, audio_speaker_playing(), (unsigned long)audio_speaker_fills(), (unsigned long)audio_i2s_diag(), S.power_mon, S.on_battery, S.batt_pct, (int)(power_battery_v() * 1000), (int)(power_usb_v() * 1000),
      S.rec);
    J("\"lights\":{\"on\":%d,\"bright\":%d,\"colour\":%d},", S.lights_on, S.bright, S.light_mode);
    J("\"ac\":{\"mode\":%d,\"sp\":%d.%d,\"fan\":%d,\"room\":%d.%d},", S.ac_mode, S.sp_x2 / 2, (S.sp_x2 % 2) * 5, S.fan, S.room_x10 / 10, S.room_x10 % 10);
    J("\"picks\":[");
    for (int i = 0; i < tesserae_pick_count(); i++) J("%s\"%s\"", i ? "," : "", tesserae_pick_title(i));
    J("],\"pick\":%d,", S.dash_page ? s_dash_sel - 1 : -1);
    J("\"night\":%d,\"keylights\":%d,\"dash\":%d,\"dashes\":[\"%s\",\"%s\",\"%s\"],\"phase\":%d,\"screen\":\"%s\",\"alarms\":[",
      S.night, inputs_backlight() * 100 / 255, S.dash_page, DASHES[0], DASHES[1], DASHES[2], (int)S.phase, local_screen_name(local_screen()));
    for (int i = 0; i < s_nalarms; i++) {
        int mask = 0; for (int d = 0; d < 7; d++) if (s_alarms[i].days[d]) mask |= 1 << d;
        char dt[40]; alarm_days_txt(&s_alarms[i], dt, sizeof dt);
        J("%s{\"h\":%d,\"m\":%d,\"on\":%d,\"days\":%d,\"early\":%d,\"dt\":\"%s\"}", i ? "," : "", s_alarms[i].h, s_alarms[i].m, s_alarms[i].on, mask, s_alarms[i].early, dt);
    }
    J("],\"max_alarms\":%d}", MAX_ALARMS);
#undef J
    return n < cap ? n : cap - 1;
}

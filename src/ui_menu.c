/*
 * ui_menu.c: the click-wheel menus: the tree, the rows' model, the top bar, navigation and every input
 */
#include "ui_internal.h"

/* ---- the click-wheel UI ---------------------------------------------------------
 * After the iPod. Every control means one thing everywhere:
 *   knob turn      scroll a list one row a detent, move a value (faster turns take bigger steps);
 *                  on the dashboard: volume
 *   knob press     choose (the wheel's centre button)
 *   knob hold      back, the same as MENU
 *   key 1  <<      previous dashboard        key 3  >>   next dashboard
 *   key 2  >||     rain play / pause; hold: bedroom lights on / off
 *   key 4  MENU    (orange) back one level; hold: straight home
 * Every list ends with a Back row. When the alarm rings the centre stops it
 * and anything else snoozes.                                                 */

mode_t_ s_mode = M_HOME;
/* Crisp menus: everything the menus draw is black or white, so the panel's
 * two-level mode (DU: no flash, ~0.45 s) can paint them exactly: opening,
 * stepping, choices, sliders. GC16 (which flashes its whole rectangle and
 * wears the picture on this panel) is then only for the dashboards. */
bool crisp(void) { return S.quick_menus && s_mode != M_HOME; }

/* ---- alarms ---- */
alarm_t s_alarms[MAX_ALARMS] = { { 6, 30, true, {1,1,1,1,1,0,0}, 15 } };
int s_nalarms = 1, s_ai;       /* count; the alarm being edited */
static int s_ki;                      /* the key being set (0..2) */
static int s_rel_armed = -1;           /* the release row pressed once (Software) */
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

/* ---- the menu tree ---- */
static row_def_t s_rows[16];
static char s_labels[16][24];
static int node_rows(node_t nd, row_def_t *r)
{
    int n = 0;
    switch (nd) {
    case N_ROOT:
        r[n++] = (row_def_t){ S_RAIN, K_SUB, "Sound", ICON_CLOUD_RAIN, 0 };
        r[n++] = (row_def_t){ S_LIGHTS, K_SUB, "Lights", ICON_LIGHTBULB, 0 };
        r[n++] = (row_def_t){ S_AC, K_SUB, "A/C", ICON_SNOWFLAKE, 0 };
        r[n++] = (row_def_t){ S_ALARMS, K_SUB, "Alarms", ICON_ALARM, 0 };
        r[n++] = (row_def_t){ C_DASH, K_CHOICE, "Dashboard", ICON_HOUSE, 0 };
        r[n++] = (row_def_t){ S_SETTINGS, K_SUB, "Settings", ICON_GEAR, 0 };
        break;      /* Back (appended below) goes home */
    case N_RAIN:
        r[n++] = (row_def_t){ T_RAIN, K_TOGGLE, "Playing", ICON_PLAY, 0 };
        r[n++] = (row_def_t){ V_RAIN_LVL, K_VALUE, "Intensity", ICON_CLOUD_RAIN, 0 };
        r[n++] = (row_def_t){ C_RAIN_KIND, K_CHOICE, "Sound", ICON_SPEAKER_HIGH, 0 };
        if (S.rain_kind == 0) r[n++] = (row_def_t){ C_SURFACE, K_CHOICE, "Falling on", ICON_CLOUD_RAIN, 0 };          /* rain only */
        if (S.rain_kind == 0 || S.rain_kind == 4) r[n++] = (row_def_t){ C_THUNDER, K_CHOICE, "Thunder", ICON_CLOUD_LIGHTNING, 0 };   /* over rain or a recording */
        r[n++] = (row_def_t){ C_SLEEP, K_CHOICE, "Sleep timer", ICON_MOON, 0 };
        break;
    case N_LIGHTS:
        r[n++] = (row_def_t){ T_LIGHTS, K_TOGGLE, "Bedroom lights", ICON_LIGHTBULB, 0 };
        r[n++] = (row_def_t){ V_BRIGHT, K_VALUE, "Brightness", ICON_SUN, 0 };
        r[n++] = (row_def_t){ C_COLOUR, K_CHOICE, "Colour", ICON_LIGHTBULB, 0 };
        break;
    case N_AC:
        r[n++] = (row_def_t){ T_AC, K_TOGGLE, "A/C", ICON_SNOWFLAKE, 0 };
        r[n++] = (row_def_t){ V_SETPOINT, K_VALUE, "Temperature", ICON_THERMOMETER, 0 };
        r[n++] = (row_def_t){ C_AC_MODE, K_CHOICE, "Mode", ICON_SNOWFLAKE, 0 };
        r[n++] = (row_def_t){ C_FAN, K_CHOICE, "Fan", ICON_SNOWFLAKE, 0 };
        break;
    case N_ALARMS:
        for (int i = 0; i < s_nalarms; i++) {
            snprintf(s_labels[i], sizeof s_labels[i], "%02d:%02d", s_alarms[i].h, s_alarms[i].m);
            r[n++] = (row_def_t){ S_ALARM_ITEM, K_SUB, s_labels[i], ICON_ALARM, i };
        }
        if (s_nalarms < MAX_ALARMS) r[n++] = (row_def_t){ A_ADD_ALARM, K_ACTION, "Add alarm", ICON_BELL_RINGING, 0 };
        r[n++] = (row_def_t){ S_CHORUS, K_SUB, "Dawn chorus", ICON_BIRD, 0 };
        break;
    case N_ALARM:
        r[n++] = (row_def_t){ T_ALARM_ON, K_TOGGLE, "Alarm", ICON_ALARM, s_ai };
        r[n++] = (row_def_t){ V_ALARM_TIME, K_VALUE, "Time", ICON_ALARM, s_ai };
        r[n++] = (row_def_t){ S_DAYS, K_SUB, "Days", ICON_CALENDAR_BLANK, s_ai };
        r[n++] = (row_def_t){ C_EARLY, K_CHOICE, "Wake up early", ICON_SUN, s_ai };
        r[n++] = (row_def_t){ A_DELETE_ALARM, K_ACTION, "Delete alarm", ICON_BELL_RINGING, s_ai };
        break;
    case N_DAYS:
        for (int i = 0; i < 7; i++) r[n++] = (row_def_t){ T_DAY, K_TOGGLE, DAY_NAMES[i], NULL, i };
        break;
    case N_CHORUS:
        for (int i = 0; i < BIRD_COUNT; i++) r[n++] = (row_def_t){ T_BIRD, K_TOGGLE, bird_name(i), ICON_BIRD, i };
        r[n++] = (row_def_t){ V_BIRD_VOL, K_VALUE, "Alarm volume", ICON_SPEAKER_HIGH, 0 };
        r[n++] = (row_def_t){ A_PREVIEW, K_ACTION, "Preview", ICON_PLAY, 0 };
        break;
    case N_SETTINGS:
        r[n++] = (row_def_t){ S_HOMESCR, K_SUB, "Home screen", ICON_HOUSE, 0 };
        r[n++] = (row_def_t){ S_NIGHTG, K_SUB, "Night", ICON_MOON, 0 };
        r[n++] = (row_def_t){ S_KEYS, K_SUB, "Keys and knob", ICON_KEYBOARD, 0 };
        r[n++] = (row_def_t){ S_TIMEG, K_SUB, "Time and clock", ICON_ALARM, 0 };
        r[n++] = (row_def_t){ S_DEVICE, K_SUB, "Device", ICON_GEAR, 0 };
        break;
    case N_HOMESCR: {
        static const char *const SHOW_L[5] = { "Show clock", "Show alarm", "Show sound", "Show room", "Show connection" };
        static const char *const SHOW_I[5] = { ICON_ALARM, ICON_BELL_RINGING, ICON_CLOUD_RAIN, ICON_SNOWFLAKE, ICON_WIFI_HIGH };
        r[n++] = (row_def_t){ C_OVERLAY, K_CHOICE, "Overlay", ICON_HOUSE, 0 };
        for (int i = 0; i < 5; i++) r[n++] = (row_def_t){ T_SHOW, K_TOGGLE, SHOW_L[i], SHOW_I[i], i };
        r[n++] = (row_def_t){ C_ATNIGHT, K_CHOICE, "At night", ICON_MOON, 0 };
        r[n++] = (row_def_t){ I_VIEWPORT, K_INFO, "Tesserae gets", ICON_WIFI_HIGH, 0 };
        break; }
    case N_NIGHTG:
        r[n++] = (row_def_t){ T_NIGHT, K_TOGGLE, "Night mode", ICON_MOON, 0 };
        r[n++] = (row_def_t){ C_AUTONIGHT, K_CHOICE, "Auto night", ICON_MOON, 0 };
        r[n++] = (row_def_t){ C_NIGHTKEYS, K_CHOICE, "Keys at night", ICON_KEYBOARD, 0 };
        break;
    case N_KEYS: {
        static const char *const KEY_L[3] = { "Key 1", "Key 2", "Key 3" };
        for (int i = 0; i < 3; i++) r[n++] = (row_def_t){ S_KEY, K_SUB, KEY_L[i], ICON_KEYBOARD, i };
        r[n++] = (row_def_t){ C_KNOB_HOLD, K_CHOICE, "Knob hold", ICON_ARROW_U_UP_LEFT, 0 };
        r[n++] = (row_def_t){ I_MENUKEY, K_INFO, "MENU key", ICON_KEYBOARD, 0 };
        r[n++] = (row_def_t){ T_TURNHINTS, K_TOGGLE, "Turn counts", ICON_LIST, 0 };
        r[n++] = (row_def_t){ T_QUICKMENU, K_TOGGLE, "Quick menu moves", ICON_PLAY, 0 };
        r[n++] = (row_def_t){ V_KEYS, K_VALUE, "Brightness", ICON_SUN, 0 };
        r[n++] = (row_def_t){ C_KEYCOL, K_CHOICE, "Key colour", ICON_KEYBOARD, 0 };
        r[n++] = (row_def_t){ C_MENUCOL, K_CHOICE, "Menu key colour", ICON_KEYBOARD, 0 };
        r[n++] = (row_def_t){ T_KNOB, K_TOGGLE, "Knob light", ICON_SUN, 0 };
        break; }
    case N_KEY:
        r[n++] = (row_def_t){ C_KEY_PRESS, K_CHOICE, "Press", ICON_KEYBOARD, s_ki };
        r[n++] = (row_def_t){ C_KEY_HOLD, K_CHOICE, "Hold", ICON_KEYBOARD, s_ki };
        break;
    case N_SOFTWARE: {
        static char lab[OTA_REL_MAX][32];
        r[n++] = (row_def_t){ I_FWVER, K_INFO, "Installed", ICON_GEAR, 0 };
        r[n++] = (row_def_t){ A_OTACHECK, K_ACTION, "Check for updates", ICON_WIFI_HIGH, 0 };
        r[n++] = (row_def_t){ T_AUTOUPD, K_TOGGLE, "Auto-update overnight", ICON_MOON, 0 };
        for (int i = 0; i < ota_release_count() && n < 15; i++) {
            const ota_release_t *e = ota_release(i);
            snprintf(lab[i], sizeof lab[i], "%s%s", e->tag, e->pre ? " (pre)" : "");
            r[n++] = (row_def_t){ A_RELEASE, K_ACTION, lab[i], ICON_HOUSE, i };
        }
        break; }
    case N_TIMEG:
        r[n++] = (row_def_t){ T_AUTOTIME, K_TOGGLE, "Set automatically", ICON_WIFI_HIGH, 0 };
        r[n++] = (row_def_t){ C_TZ, K_CHOICE, "Time zone", ICON_HOUSE, 0 };
        r[n++] = (row_def_t){ V_TIME, K_VALUE, "Time", ICON_ALARM, 0 };
        r[n++] = (row_def_t){ V_DATE, K_VALUE, "Date", ICON_CALENDAR_BLANK, 0 };
        r[n++] = (row_def_t){ C_CLOCK, K_CHOICE, "Clock", ICON_ALARM, 0 };
        r[n++] = (row_def_t){ C_SNOOZE, K_CHOICE, "Snooze", ICON_BELL_RINGING, 0 };
        break;
    case N_DEVICE:
        r[n++] = (row_def_t){ S_SOFTWARE, K_SUB, "Software", ICON_GEAR, 0 };
        r[n++] = (row_def_t){ C_REFRESHMODE, K_CHOICE, "Screen updates", ICON_SUN, 0 };
        r[n++] = (row_def_t){ C_WFTEMP, K_CHOICE, "Waveform temp", ICON_THERMOMETER, 0 };
        r[n++] = (row_def_t){ C_CLEANMIN, K_CHOICE, "Clean refresh every", ICON_SUN, 0 };
        r[n++] = (row_def_t){ A_REFRESH, K_ACTION, "Refresh screen", ICON_HOUSE, 0 };
        r[n++] = (row_def_t){ I_STATUS, K_INFO, "Status", ICON_PLUG, 0 };
        r[n++] = (row_def_t){ T_SPEAKER, K_TOGGLE, "Speaker", ICON_SPEAKER_HIGH, 0 };
        r[n++] = (row_def_t){ T_POWERMON, K_TOGGLE, "Battery backup", ICON_BATTERY_CHARGING, 0 };
        r[n++] = (row_def_t){ A_WIFISETUP, K_ACTION, "Wi-Fi setup", ICON_WIFI_HIGH, 0 };
        r[n++] = (row_def_t){ A_RESTART, K_ACTION, "Restart", ICON_PLUG, 0 };
        r[n++] = (row_def_t){ C_RESET, K_CHOICE, "Reset everything", ICON_TRASH, 0 };
        break;
    default: break;
    }
    r[n++] = (row_def_t){ R_BACK, K_BACK, "Back", ICON_ARROW_U_UP_LEFT, 0 };
    return n;
}
static const char *node_title(node_t nd)
{
    static char b[24];
    switch (nd) {
    case N_ROOT: return "Menu"; case N_RAIN: return "Sound"; case N_LIGHTS: return "Lights"; case N_AC: return "A/C";
    case N_ALARMS: return "Alarms"; case N_SETTINGS: return "Settings"; case N_CHORUS: return "Dawn chorus";
    case N_ALARM: snprintf(b, sizeof b, "Alarm %d", s_ai + 1); return b;
    case N_DAYS:  snprintf(b, sizeof b, "Alarm %d days", s_ai + 1); return b;
    case N_HOMESCR: return "Home screen"; case N_NIGHTG: return "Night"; case N_KEYS: return "Keys and knob";
    case N_KEY:   snprintf(b, sizeof b, "Key %d", s_ki + 1); return b;
    case N_TIMEG: return "Time and clock"; case N_DEVICE: return "Device"; case N_SOFTWARE: return "Software";
    default: return "";
    }
}

static struct { node_t node; int sel; } s_stack[6];
int  s_depth;
static item_t s_edit;
static int  s_csel;
static char s_edit_label[24];

screen_t local_screen(void)
{
    if (s_mode == M_HOME) return SCR_HOME;
    switch (s_stack[s_depth - 1].node) {
    case N_RAIN: return SCR_RAIN; case N_LIGHTS: return SCR_LIGHTS; case N_AC: return SCR_AC;
    case N_ALARMS: case N_ALARM: case N_DAYS: case N_CHORUS: return SCR_ALARM; case N_SETTINGS: case N_HOMESCR: case N_NIGHTG: case N_KEYS: case N_KEY: case N_TIMEG: case N_DEVICE: case N_SOFTWARE: return SCR_SETTINGS;
    default: return SCR_MENU;
    }
}

/* ---- the model behind the rows ---- */
static const char *const SLEEPS[4]  = { "Off", "15 min", "30 min", "60 min" };
static const int SLEEP_V[4] = { 0, 15, 30, 60 };
static const char *const COLOURS[2] = { "Warm", "Night" };
static const char *const MODES4[4]  = { "Cool", "Heat", "Fan", "Dry" };
static const int MODE_V[4] = { 3, 1, 2, 4 };
static const char *const EARLY[5]   = { "Off", "5 min", "10 min", "15 min", "30 min" };
static const int EARLY_V[5] = { 0, 5, 10, 15, 30 };

static bool toggle_get(const row_def_t *r)
{
    switch (r->id) {
    case T_RAIN: return S.rain_on; case T_LIGHTS: return S.lights_on; case T_AC: return S.ac_mode != 0;
    case T_ALARM_ON: return s_alarms[r->arg].on; case T_DAY: return s_alarms[s_ai].days[r->arg];
    case T_BIRD: return (S.birds >> r->arg) & 1;
    case T_KNOB: return S.knob_light;
    case T_SHOW: return (S.show >> r->arg) & 1;
    case T_AUTOTIME: return S.auto_time;
    case T_AUTOUPD: return S.auto_update;
    case T_SPEAKER: return S.speaker; case T_POWERMON: return S.power_mon;
    case T_TURNHINTS: return S.turn_hints; case T_QUICKMENU: return S.quick_menus; case T_SETTLEFULL: return S.settle_full;
    case T_NIGHT: return S.night; default: return false;
    }
}
void toggle_flip(const row_def_t *r)
{
    switch (r->id) {
    case T_RAIN:   S.rain_on = !S.rain_on; publish("set/rain", S.rain_on ? "play" : "pause"); break;
    case T_LIGHTS: S.lights_on = !S.lights_on; publish("set/lights", S.lights_on ? "on %d%%" : "off", S.bright); break;
    case T_AC:     S.ac_mode = S.ac_mode ? 0 : 3; publish("set/ac", "%s", AC_MODES[S.ac_mode]); break;
    case T_ALARM_ON: s_alarms[r->arg].on = !s_alarms[r->arg].on; publish_alarm(r->arg); sync_alarm_summary(); break;
    case T_DAY:    s_alarms[s_ai].days[r->arg] ^= 1; publish_alarm(s_ai); sync_alarm_summary(); break;
    case T_KNOB:   S.knob_light = !S.knob_light; apply_colours(); break;
    case T_SHOW:   S.show ^= 1 << r->arg; overlay_apply(); break;
    case T_AUTOUPD: S.auto_update = !S.auto_update; break;
    case T_SPEAKER: S.speaker = !S.speaker; publish("config/speaker", S.speaker ? "on" : "off"); break;
    case T_TURNHINTS: S.turn_hints = !S.turn_hints; break;
    case T_QUICKMENU: S.quick_menus = !S.quick_menus; break;
    case T_SETTLEFULL: S.settle_full = !S.settle_full; break;
    case T_POWERMON: S.power_mon = !S.power_mon; if (!S.power_mon) S.on_battery = false; break;
    case T_AUTOTIME: S.auto_time = !S.auto_time; if (!S.auto_time) manual_start(); rebuild(); break;
    case T_BIRD:   S.birds ^= (uint16_t)(1u << r->arg);
                   if ((S.birds >> r->arg) & 1) { S.audition_bird = r->arg; S.audition++; }   /* let you hear who you added */
                   break;
    case T_NIGHT:  S.night = !S.night; inputs_set_night(S.night); config_set_lights(inputs_backlight(), S.night); config_save();
                   publish("config/night", S.night ? "on" : "off"); break;
    default: break;
    }
}
const row_def_t R_T_RAIN = { T_RAIN, K_TOGGLE, "Rain", NULL, 0 }, R_T_LIGHTS = { T_LIGHTS, K_TOGGLE, "Lights", NULL, 0 },
                       R_T_AC = { T_AC, K_TOGGLE, "A/C", NULL, 0 }, R_T_NIGHT = { T_NIGHT, K_TOGGLE, "Night", NULL, 0 };

static vmeta_t value_meta(item_t id)
{
    switch (id) {
    case V_RAIN_LVL:   return (vmeta_t){ S.rain_lvl, 1, 5, 1, "Drizzle", "Downpour", false };
    case V_BRIGHT:     return (vmeta_t){ S.bright, 5, 100, 5, "Dim", "Bright", false };
    case V_SETPOINT:   return (vmeta_t){ S.sp_x2, 32, 60, 1, "16" DEG, "30" DEG, false };
    case V_ALARM_TIME: return (vmeta_t){ s_alarms[s_ai].h * 60 + s_alarms[s_ai].m, 0, 1439, 5, "00:00", "23:59", true };
    case V_KEYS:       return (vmeta_t){ inputs_backlight() * 100 / 255, 0, 100, 10, "Off", "Bright", false };
    case V_BIRD_VOL:   return (vmeta_t){ S.bird_vol, 1, 10, 1, "Soft", "Loud", false };
    case V_TIME:       { int wd, sec; int64_t mn; int v = clock_now(&wd, &sec, &mn) ? sec / 60 : 0;
                         return (vmeta_t){ v, 0, 1439, 1, "00:00", "23:59", true }; }
    case V_DATE:       return (vmeta_t){ s_man_days, -3650, 3650, 1, "", "", false };
    default:           return (vmeta_t){ 0, 0, 1, 1, "", "", false };
    }
}
static void value_text(item_t id, char *b, size_t n)
{
    switch (id) {
    case V_RAIN_LVL:   snprintf(b, n, "%d of 5", S.rain_lvl); break;
    case V_BRIGHT:     snprintf(b, n, "%d%%", S.bright); break;
    case V_SETPOINT:   snprintf(b, n, "%d.%d" DEG, S.sp_x2 / 2, (S.sp_x2 % 2) * 5); break;
    case V_ALARM_TIME: snprintf(b, n, "%02d:%02d", s_alarms[s_ai].h, s_alarms[s_ai].m); break;
    case V_KEYS:       snprintf(b, n, "%d%%", inputs_backlight() * 100 / 255); break;
    case V_BIRD_VOL:   snprintf(b, n, "%d of 10", S.bird_vol); break;
    case V_TIME:       { int wd, sec; int64_t mn; if (clock_now(&wd, &sec, &mn)) fmt_hm(b, n, sec / 60); else snprintf(b, n, "--:--"); } break;
    case V_DATE:       { int y, m, d, wd; if (local_ymd(&y, &m, &d, &wd)) snprintf(b, n, "%.3s %d %.3s", DAY_NAMES[wd], d, MONTHS[m - 1]); else snprintf(b, n, "--"); } break;
    default:           b[0] = 0; break;
    }
}
/* d is the number of detents (signed); accel scales the step for fast turns. */
static void value_step(item_t id, int d, int accel)
{
    vmeta_t m = value_meta(id);
    int v = m.v + d * m.step * accel;
    if (m.wrap) { int span = m.max - m.min + 1; v = ((v - m.min) % span + span) % span + m.min; v -= v % m.step; }
    else v = clampi(v, m.min, m.max);
    switch (id) {
    case V_RAIN_LVL:   S.rain_lvl = v; publish("set/rain", "level %d", v); break;
    case V_BRIGHT:     S.bright = v; S.lights_on = true; publish("set/lights", "on %d%%", v); break;
    case V_SETPOINT:   S.sp_x2 = v; if (!S.ac_mode) S.ac_mode = 3; publish("set/ac", "%d.%d", v / 2, (v % 2) * 5); break;
    case V_ALARM_TIME: s_alarms[s_ai].h = v / 60; s_alarms[s_ai].m = v % 60; sync_alarm_summary(); break;
    case V_BIRD_VOL:   S.bird_vol = v; S.audition_bird = BIRD_MAGPIE; S.audition++; break;   /* a call at the new level */
    case V_KEYS:       { uint8_t lv = (uint8_t)(v * 255 / 100); inputs_set_backlight(lv); config_set_lights(lv, S.night); config_save(); } break;
    case V_TIME:       { int delta = v - m.v; if (delta > 720) delta -= 1440; if (delta < -720) delta += 1440;
                         s_man_base += (int64_t)delta * 60; rebuild(); } break;
    case V_DATE:       s_man_base += (int64_t)(v - s_man_days) * 86400; s_man_days = v; rebuild(); break;
    default: break;
    }
}
static int choice_opts(item_t id, const char *const **o)
{
    switch (id) {
    case C_RAIN_KIND: *o = KINDS; return 5;   case C_SLEEP: *o = SLEEPS; return 4;
    case C_COLOUR:    *o = COLOURS; return 2; case C_AC_MODE: *o = MODES4; return 4;
    case C_FAN:       *o = FANS; return 4;    case C_EARLY: *o = EARLY; return 5;
    case C_DASH:      if (!tesserae_pick_count()) { *o = DASHES; return 3; }      /* no list (older server): the rotation */
                      *o = dash_opts(); return 1 + tesserae_pick_count();
    case C_SURFACE:   *o = SURFACES; return 4;
    case C_KEYCOL: case C_MENUCOL: case C_NIGHTCOL: *o = PAL_NAMES; return N_PAL;
    case C_AUTONIGHT: *o = AUTONIGHT; return 4;
    case C_CLOCK:     *o = CLOCKF; return 2;
    case C_SNOOZE:    *o = SNOOZES; return 4;
    case C_RESET:     *o = RESETS; return 2;
    case C_THUNDER:   *o = THUNDERS; return 3;
    case C_OVERLAY:   *o = OVERLAYS; return 4;
    case C_ATNIGHT:   *o = ATNIGHT; return 2;
    case C_NIGHTKEYS: *o = NIGHTKEYS; return 2;
    case C_KEY_PRESS: case C_KEY_HOLD: *o = KEYACT_NAME; return N_KEYACT;
    case C_KNOB_HOLD: *o = KNOBACT_NAME; return N_KNOBACT;
    case C_TZ:        *o = TZ_NAMES; return N_TZ;
    case C_REFRESHMODE: *o = REFRESHES; return 3;
    case C_WFTEMP:    *o = WFTEMPS; return N_WFTEMP;
    case C_CLEANMIN:  *o = CLEANS; return N_CLEAN;
    default: *o = NULL; return 0;
    }
}
static int choice_get(item_t id)
{
    switch (id) {
    case C_RAIN_KIND: return S.rain_kind;
    case C_SLEEP:     for (int i = 0; i < 4; i++) if (SLEEP_V[i] == S.sleep_min) return i; return 0;
    case C_COLOUR:    return S.light_mode;
    case C_AC_MODE:   for (int i = 0; i < 4; i++) if (MODE_V[i] == S.ac_mode) return i; return 0;
    case C_FAN:       return S.fan;
    case C_EARLY:     for (int i = 0; i < 5; i++) if (EARLY_V[i] == s_alarms[s_ai].early) return i; return 3;
    case C_DASH:      return tesserae_pick_count() ? (S.dash_page == 0 ? 0 : s_dash_sel) : S.dash_page;
    case C_SURFACE:   return S.surface;
    case C_KEYCOL:    return S.key_col;
    case C_MENUCOL:   return S.menu_col;
    case C_NIGHTCOL:  return S.night_col;
    case C_AUTONIGHT: return S.auto_night;
    case C_CLOCK:     return S.clock12 ? 1 : 0;
    case C_SNOOZE:    for (int i = 0; i < 4; i++) if (SNOOZE_V[i] == S.snooze_min) return i; return 1;
    case C_RESET:     return 0;
    case C_THUNDER:   return !S.thunder ? 0 : S.thunder_near < 50 ? 1 : 2;
    case C_OVERLAY:   return S.overlay;
    case C_ATNIGHT:   return S.at_night;
    case C_NIGHTKEYS: return S.night_keys;
    case C_KEY_PRESS: case C_KEY_HOLD: {
        const char *b = config_get()->bind[(id == C_KEY_HOLD ? CONFIG_BIND_KEY1_HOLD : CONFIG_BIND_KEY1) + s_ki];
        for (int i = 0; i < N_KEYACT; i++) if (!strcmp(b, KEYACT_ID[i])) return i;
        return -1; }                                        /* something set from the web app */
    case C_KNOB_HOLD: {
        const char *b = config_get()->bind[CONFIG_BIND_KNOB_HOLD];
        for (int i = 0; i < N_KNOBACT; i++) if (!strcmp(b, KNOBACT_ID[i])) return i;
        return b[0] ? -1 : 0; }
    case C_TZ:        return S.tz;
    case C_REFRESHMODE: return S.refresh_mode;
    case C_WFTEMP: { for (int i = 0; i < N_WFTEMP; i++) if (WFTEMP_V[i] == S.wf_temp) return i; return 0; }
    case C_CLEANMIN: { for (int i = 0; i < N_CLEAN; i++) if (CLEAN_V[i] == S.clean_min) return i; return 1; }
    default:          return 0;
    }
}
static void choice_set(item_t id, int k)
{
    switch (id) {
    case C_RAIN_KIND: S.rain_kind = k; S.rain_on = true; publish("set/rain", "%s", KINDS[k]); break;
    case C_SLEEP:     S.sleep_min = SLEEP_V[k]; publish("set/rain", "sleep %d", S.sleep_min); break;
    case C_COLOUR:    S.light_mode = k; S.lights_on = true; publish("set/lights", "%s", k ? "night" : "warm"); break;
    case C_AC_MODE:   S.ac_mode = MODE_V[k]; publish("set/ac", "%s", AC_MODES[S.ac_mode]); break;
    case C_FAN:       S.fan = k; publish("set/ac", "fan %s", FANS[k]); break;
    case C_EARLY:     s_alarms[s_ai].early = EARLY_V[k]; publish_alarm(s_ai); sync_alarm_summary(); break;
    case C_DASH:      if (!tesserae_pick_count()) { dash_go(k); break; }
                      if (k == 0) { dash_go(0); s_dash_sel = 0; break; }
                      dash_pick(k); break;
    case C_SURFACE:   S.surface = k; S.rain_on = true; publish("set/rain", "surface %s", SURFACES[k]); break;
    case C_KEYCOL:    S.key_col = k; apply_colours(); break;
    case C_MENUCOL:   S.menu_col = k; apply_colours(); break;
    case C_NIGHTCOL:  S.night_col = k; apply_colours(); break;
    case C_AUTONIGHT: S.auto_night = k; break;
    case C_CLOCK:     S.clock12 = k == 1; break;
    case C_SNOOZE:    S.snooze_min = SNOOZE_V[k]; break;
    case C_RESET:     if (k == 1) { printf("local: master reset asked for\n"); s_req = LREQ_RESET; } break;
    case C_OVERLAY:   S.overlay = k; overlay_apply(); break;
    case C_ATNIGHT:   S.at_night = k; break;
    case C_NIGHTKEYS: S.night_keys = k; apply_colours(); break;
    case C_KEY_PRESS: case C_KEY_HOLD:
        config_set_bind((id == C_KEY_HOLD ? CONFIG_BIND_KEY1_HOLD : CONFIG_BIND_KEY1) + s_ki, KEYACT_ID[k]); config_save(); break;
    case C_KNOB_HOLD: config_set_bind(CONFIG_BIND_KNOB_HOLD, KNOBACT_ID[k]); config_save(); break;
    case C_TZ:        S.tz = k; rebuild(); break;
    case C_REFRESHMODE: S.refresh_mode = k; s_req = LREQ_REFRESH; break;   /* one clean refresh now */
    case C_WFTEMP:      S.wf_temp = WFTEMP_V[k]; s_req = LREQ_REFRESH; break;
    case C_CLEANMIN:    S.clean_min = CLEAN_V[k]; break;
    case C_THUNDER:   if (k == 0) S.thunder = 0;
                      else if (k == 1) { if (!S.thunder) S.thunder = 35; S.thunder_near = 20; }
                      else { if (S.thunder < 60) S.thunder = 70; S.thunder_near = 75; }
                      publish("set/rain", "thunder %s", THUNDERS[k]); break;
    default: break;
    }
}
static void row_value(const row_def_t *r, char *b, size_t n)
{
    const char *const *o;
    b[0] = 0;
    switch (r->kind) {
    case K_TOGGLE: snprintf(b, n, "%s", toggle_get(r) ? "On" : "Off"); break;
    case K_VALUE:  value_text(r->id, b, n); break;
    case K_CHOICE: { if (r->id == C_RESET) break;
                     if (r->id == C_DASH && tesserae_pick_count() && S.dash_page) { snprintf(b, n, "%s", tesserae_pick_title(s_dash_sel - 1 >= 0 ? s_dash_sel - 1 : 0)); break; }
                     if (r->id == C_KEY_PRESS || r->id == C_KEY_HOLD) s_ki = r->arg;
                     int k = choice_opts(r->id, &o), c = choice_get(r->id);
                     if (k && c >= 0) snprintf(b, n, "%s", o[c]);
                     else if (r->id == C_KEY_PRESS || r->id == C_KEY_HOLD || r->id == C_KNOB_HOLD)
                         snprintf(b, n, "%s", action_name(config_get()->bind[r->id == C_KNOB_HOLD ? CONFIG_BIND_KNOB_HOLD
                                  : (r->id == C_KEY_HOLD ? CONFIG_BIND_KEY1_HOLD : CONFIG_BIND_KEY1) + r->arg])); } break;
    case K_INFO:
        if (r->id == I_VIEWPORT) { int x, y, w, h; viewport(&x, &y, &w, &h); snprintf(b, n, "%d x %d", w, h); }
        else if (r->id == I_FWVER) snprintf(b, n, "%s", FW_VERSION);
        else if (r->id == I_MENUKEY) snprintf(b, n, "Fixed");
        else if (S.on_battery) snprintf(b, n, "Battery %d%%", S.batt_pct);
        else snprintf(b, n, "Mains");
        break;
    case K_ACTION: if (r->id == A_PREVIEW && s_preview_len_ms) snprintf(b, n, "Playing");
                   if (r->id == A_OTACHECK && ota_checked_ms()) { int m = (int)((now_ms() - ota_checked_ms()) / 60000); if (m < 1) snprintf(b, n, "just now"); else snprintf(b, n, "%d min ago", m); }
                   if (r->id == A_RELEASE) { const ota_release_t *e = ota_release(r->arg); int c = e ? ota_version_cmp(e->ver, FW_VERSION) : 0;
                                             snprintf(b, n, "%s", !e ? "" : s_rel_armed == r->arg ? "Press again" : !strcmp(e->ver, FW_VERSION) ? "Installed" : c > 0 ? "Newer" : "Older"); }
                   break;
    case K_SUB:
        switch (r->id) {
        case S_RAIN:   if (S.rain_on) snprintf(b, n, "%s %d", KINDS[S.rain_kind], S.rain_lvl); else snprintf(b, n, "Off"); break;
        case S_LIGHTS: if (S.lights_on) snprintf(b, n, "%d%%", S.bright); else snprintf(b, n, "Off"); break;
        case S_AC:     if (S.ac_mode) snprintf(b, n, "%s %d" DEG, AC_MODES[S.ac_mode], S.sp_x2 / 2); else snprintf(b, n, "Off"); break;
        case S_ALARMS: { int on = 0; for (int i = 0; i < s_nalarms; i++) on += s_alarms[i].on;
                         if (on) snprintf(b, n, "%d on", on); else snprintf(b, n, "Off"); } break;
        case S_ALARM_ITEM: { const alarm_t *a = &s_alarms[r->arg]; char d[32]; alarm_days_txt(a, d, sizeof d);
                         if (a->on) snprintf(b, n, "%.23s", d); else snprintf(b, n, "Off"); } break;
        case S_DAYS:   alarm_days_txt(&s_alarms[s_ai], b, n); break;
        case S_CHORUS: { int k = 0; for (int i = 0; i < BIRD_COUNT; i++) k += (S.birds >> i) & 1;
                         if (k) snprintf(b, n, "%d birds", k); else snprintf(b, n, "Off"); } break;
        case S_HOMESCR: snprintf(b, n, "%s", OVERLAYS[ov_kind()]); break;
        case S_NIGHTG:  snprintf(b, n, "%s", S.night ? "On" : S.auto_night ? AUTONIGHT[S.auto_night] : "Off"); break;
        case S_KEY:     snprintf(b, n, "%s", local_label_for_action(config_get()->bind[CONFIG_BIND_KEY1 + r->arg])); break;
        case S_TIMEG:   snprintf(b, n, "%s", S.auto_time ? (S.tz ? TZ_NAMES[S.tz] : "Automatic") : "Set by hand"); break;
        default: break;
        }
        break;
    default: break;
    }
}
static node_t sub_node(item_t id)
{
    switch (id) {
    case S_CHORUS: return N_CHORUS;
    case S_RAIN: return N_RAIN; case S_LIGHTS: return N_LIGHTS; case S_AC: return N_AC; case S_ALARMS: return N_ALARMS;
    case S_ALARM_ITEM: return N_ALARM; case S_DAYS: return N_DAYS;
    case S_HOMESCR: return N_HOMESCR; case S_NIGHTG: return N_NIGHTG; case S_KEYS: return N_KEYS; case S_KEY: return N_KEY;
    case S_TIMEG: return N_TIMEG; case S_DEVICE: return N_DEVICE; case S_SOFTWARE: return N_SOFTWARE;
    default: return N_SETTINGS;
    }
}

/* ---- key labels (fixed: the keys mean the same everywhere) ---- */
const char *local_key_label(int i)
{
    if (i < 0 || i > 3) return "";
    if (S.phase == PHASE_RINGING) return "Snooze";
    if (i == 3) return "Menu";
    return local_label_for_action(config_get()->bind[CONFIG_BIND_KEY1 + i]);
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
    op_text(l, MARGIN, 50, F_M, fg, title);
    /* the four keys: icons under the keys, MENU framed on the orange one */
    for (int i = 0; i < 4; i++) {
        int cx = KEY_CX[i];
        op_fill(l, cx - 2, 0, 4, 16, i == 3 ? fg : dim);
        if (i < 3) { const char *ic = action_icon(config_get()->bind[CONFIG_BIND_KEY1 + i]); if (ic) op_text_c(l, cx, 44, F_IM, dim, ic); }
        else { int tw = draw_font_w(F_LBL, "MENU"); op_frame(l, cx - tw / 2 - 18, 36, tw + 36, 60, 3, fg); op_text_c(l, cx, 45, F_LBL, fg, "MENU"); }
    }
}

/* A split list: rows on the left, the highlighted row large on the right. */
static void build_split(layer_t *l, const char *const *labels, const char *const *icons, char vals[][24],
                        const bool *chev, const bool *check, int n, int sel, const char *pv_icon, const char *pv_big, const char *pv_small)
{
    layer_reset(l, 0, TB_H, W, SB_Y - TB_H, G_WHITE);
    op_fill(l, LIST_W, TB_H, crisp() ? 2 : 3, SB_Y - TB_H, LIGHT_G);
    s_row_h = n > 8 ? (SB_Y - LIST_Y - 10) / n : 116;
    for (int i = 0; i < n; i++) {
        int y = LIST_Y + i * s_row_h;
        bool s = (i == sel);
        uint8_t fg = s ? G_WHITE : G_BLACK, dim = s ? (crisp() ? G_WHITE : G_LIGHT) : DIM_G;
        if (s) op_fill(l, 0, y, LIST_W, s_row_h, G_BLACK);
        else op_fill(l, MARGIN, y + s_row_h - 2, LIST_W - MARGIN - 30, crisp() ? 1 : 2, LIGHT_G);
        int x = MARGIN;
        if (icons && icons[i]) { op_text(l, x, y + (s_row_h - F_IM->line_h) / 2, F_IM, dim, icons[i]); x += 80; }
        op_text(l, x, y + (s_row_h - F_M->line_h) / 2, F_M, fg, labels[i]);
        int xr = LIST_W - 40;
        if (chev && chev[i]) { xr -= draw_font_w(F_I, ICON_CARET_RIGHT); op_text(l, xr, y + (s_row_h - F_I->line_h) / 2, F_I, dim, ICON_CARET_RIGHT); xr -= 18; }
        if (check && check[i]) { xr -= draw_font_w(F_IM, ICON_CHECK); op_text(l, xr, y + (s_row_h - F_IM->line_h) / 2, F_IM, fg, ICON_CHECK); xr -= 18; }
        if (vals && vals[i][0]) op_text_r(l, xr, y + (s_row_h - F_LBL->line_h) / 2, F_LBL, dim, vals[i]);
        if (S.turn_hints && !s) {           /* turns from the highlighted row: clockwise is down */
            char t[12]; snprintf(t, sizeof t, "%+d", i - sel);
            op_text_r(l, MARGIN - 12, y + (s_row_h - F_S->line_h) / 2, F_S, DIM_G, t);
        }
    }
    s_list_n = n;
    int cx = LIST_W + (W - LIST_W) / 2, y = TB_H + 150;
    if (pv_icon) { op_text_c(l, cx, y, &icon170, G_BLACK, pv_icon); y += icon170.line_h + 30; }
    const bfont_t *bf = numeric(pv_big) && draw_font_w(F_NUM, pv_big) < W - LIST_W - 80 ? F_NUM : F_XL;
    if (draw_font_w(bf, pv_big) > W - LIST_W - 80) bf = F_M;
    op_text_c(l, cx, y, bf, G_BLACK, pv_big); y += bf->line_h + 18;
    op_text_c(l, cx, y, F_LBL, DIM_G, pv_small);
}

void build_list(layer_t *l)
{
    node_t nd = s_stack[s_depth - 1].node;
    int n = node_rows(nd, s_rows), sel = s_stack[s_depth - 1].sel;
    if (sel >= n) sel = s_stack[s_depth - 1].sel = n - 1;
    const char *labels[16], *icons[16]; char vals[16][24]; bool chev[16];
    for (int i = 0; i < n; i++) {
        const row_def_t *r = &s_rows[i];
        labels[i] = r->label; icons[i] = r->icon; row_value(r, vals[i], 24);
        chev[i] = r->kind == K_SUB || r->kind == K_VALUE || r->kind == K_CHOICE;
    }
    const row_def_t *r = &s_rows[sel];
    char big[24]; snprintf(big, sizeof big, "%s", vals[sel][0] ? vals[sel] : r->label);
    const char *hint = r->kind == K_TOGGLE ? "press to switch" : r->kind == K_SUB ? "press to open" : r->kind == K_BACK ? "press to go back"
                     : r->kind == K_ACTION ? "press" : r->kind == K_INFO ? "" : "press to change";
    char small[64]; snprintf(small, sizeof small, "%s%s%s", r->kind == K_BACK ? "" : r->label, (hint[0] && r->kind != K_BACK) ? "  " DOT "  " : "", hint);
    if (r->kind == K_SUB && r->id == S_ALARM_ITEM) snprintf(big, sizeof big, "%s", r->label);
    if (r->id == I_STATUS) { if (S.on_battery) snprintf(big, sizeof big, "Battery %d%%", S.batt_pct); else snprintf(big, sizeof big, "Mains"); snprintf(small, sizeof small, "Wi-Fi %s " DOT " %s", net_connected() ? "on" : "off", tesserae_state_name()); }
    if (r->id == I_VIEWPORT) { int x, y, w, h; viewport(&x, &y, &w, &h); snprintf(small, sizeof small, "at x %d, y %d " DOT " every heartbeat", x, y); }
    if (r->id == I_MENUKEY) { snprintf(big, sizeof big, "Menu"); snprintf(small, sizeof small, "Press: menu or back " DOT " hold: home"); }
    if ((r->id == V_TIME || r->id == V_DATE) && S.auto_time) snprintf(small, sizeof small, "Set automatically " DOT " switch it off to change");
    if (r->id == C_OVERLAY) { int x, y, w, h; viewport(&x, &y, &w, &h); snprintf(small, sizeof small, "Dashboard %d x %d " DOT " press to change", w, h); }
    if (r->id == I_FWVER) { int sl = ota_boot_partition(); snprintf(small, sizeof small, "slot %c " DOT " from github.com/%s", sl >= 0 ? 'A' + sl : '-', "dmellok"); }
    if (r->id == A_OTACHECK) snprintf(small, sizeof small, "%d release(s) listed " DOT " press to check GitHub", ota_release_count());
    if (r->id == T_AUTOUPD) snprintf(small, sizeof small, "Newest stable, 03:10, never near an alarm");
    if (r->id == A_RELEASE) { const ota_release_t *e = ota_release(r->arg);
        if (e) snprintf(small, sizeof small, "%s " DOT " %s", e->date, s_rel_armed == r->arg ? "press again to install" : "press to install"); }
    if (r->id == A_WIFISETUP) snprintf(small, sizeof small, "Restarts into setup: join its WiFi from a phone");
    if (r->id == T_TURNHINTS) snprintf(small, sizeof small, "Beside each row: turns to reach it, + clockwise");
    if (r->id == T_QUICKMENU) snprintf(small, sizeof small, "Steps in black and white, full quality when you stop");
    if (r->id == T_SETTLEFULL) snprintf(small, sizeof small, "When you stop: the whole screen, cleared first " DOT " flashes");
    if (r->id == C_CLEANMIN) snprintf(small, sizeof small, "Partial updates wear the picture; this resets it while idle");
    if (r->id == C_WFTEMP) { int s = it8951_temp_sensed(); if (s >= 0) snprintf(small, sizeof small, "Board sensor says %d C " DOT " lower = darker", s); else snprintf(small, sizeof small, "Lower = darker, higher = lighter"); }
    if (r->id == C_REFRESHMODE) snprintf(small, sizeof small, "%s", S.refresh_mode == 2 ? "Whole screen, every change " DOT " flashes"
                                                               : S.refresh_mode == 1 ? "Full quality, just what changed" : "Just what changed, a clean-up when you stop");
    if (r->id == C_KEY_PRESS || r->id == C_KEY_HOLD) snprintf(small, sizeof small, "Key %d, %s " DOT " press to change", s_ki + 1, r->id == C_KEY_HOLD ? "held" : "pressed");
    build_split(l, labels, icons, vals, chev, NULL, n, sel, r->icon, big, small);
}

void build_choice(layer_t *l)
{
    const char *const *o; int n = choice_opts(s_edit, &o), cur = choice_get(s_edit);
    const char *labels[16], *icons[16]; bool check[16];
    for (int i = 0; i < n; i++) { labels[i] = o[i]; check[i] = (i == cur); icons[i] = NULL; }
    labels[n] = "Back"; check[n] = false; icons[n] = ICON_ARROW_U_UP_LEFT;
    bool back = s_csel == n;
    build_split(l, labels, icons, NULL, NULL, check, n + 1, s_csel, back ? ICON_ARROW_U_UP_LEFT : NULL,
                back ? "Back" : o[s_csel], back ? "press to go back" : "press to choose");
}

void build_value(layer_t *l)
{
    layer_reset(l, 0, TB_H, W, SB_Y - TB_H, G_WHITE);
    vmeta_t m = value_meta(s_edit);
    char t[24]; value_text(s_edit, t, sizeof t);
    int y = TB_H + 60;
    const char *icon = s_edit == V_BIRD_VOL ? ICON_BIRD : s_edit == V_ALARM_TIME ? ICON_ALARM : s_edit == V_SETPOINT ? ICON_THERMOMETER : s_edit == V_RAIN_LVL ? ICON_CLOUD_RAIN : ICON_SUN;
    op_text_c(l, W / 2, y, &icon170, G_BLACK, icon);
    y += icon170.line_h + 20;
    op_text_c(l, W / 2, y, numeric(t) ? F_NUM : F_XL, G_BLACK, t);
    y = TB_H + 700;
    int fx = (int)((long)TRACK_W * (m.v - m.min) / (m.max - m.min ? m.max - m.min : 1));
    if (crisp()) op_frame(l, TRACK_X, y, TRACK_W, 22, 2, G_BLACK); else op_fill(l, TRACK_X, y, TRACK_W, 22, G_LIGHT);
    op_fill(l, TRACK_X, y, fx, 22, G_BLACK);
    op_circle(l, TRACK_X + fx, y + 11, 34, G_BLACK);
    op_circle(l, TRACK_X + fx, y + 11, 22, G_WHITE);
    op_text(l, TRACK_X, y + 60, F_LBL, DIM_G, m.lo);
    op_text_r(l, TRACK_X + TRACK_W, y + 60, F_LBL, DIM_G, m.hi);
    op_text_c(l, W / 2, y + 160, F_S, DIM_G, "turn to adjust, faster for bigger steps  " DOT "  press or MENU when done");
}

void go_home(void) { s_mode = M_HOME; s_depth = 0; printf("local: home\n"); mark_screen(); }
static void push_node(node_t n)
{
    if (s_mode != M_LIST) s_depth = 0;
    if (s_depth == 0 && n != N_ROOT) { s_stack[0].node = N_ROOT; s_stack[0].sel = 0; s_depth = 1; }
    if (s_depth >= 6) return;
    s_stack[s_depth].node = n; s_stack[s_depth].sel = 0; s_depth++;
    s_mode = M_LIST;
    printf("local: %s\n", node_title(n));
    mark_screen();
}
static void list_back(void)
{
    if (s_depth <= 1) { go_home(); return; }
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
    if (s_mode == M_HOME && S.dash_page > 0) { char b[4]; snprintf(b, sizeof b, "%d", S.vol); toast(S.vol ? ICON_SPEAKER_HIGH : ICON_SPEAKER_SLASH, "Volume", b); }
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
    if (S.phase == PHASE_RINGING) { snooze(); return true; }
    int acc = accel_for(d);
    switch (s_mode) {
    case M_HOME: volume(d > 0 ? 1 : -1); return true;
    case M_LIST: {
        int *sel = &s_stack[s_depth - 1].sel, n = node_rows(s_stack[s_depth - 1].node, s_rows), old = *sel;
        *sel = clampi(*sel + d, 0, n - 1);          /* one detent, one row: no speed-up in lists */
        if (*sel != old) mark_rows(old, *sel);
        return true; }
    case M_CHOICE: {
        const char *const *o; int n = choice_opts(s_edit, &o) + 1, old = s_csel;
        s_csel = clampi(s_csel + d, 0, n - 1);
        if (s_csel != old) mark_rows(old, s_csel);
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
        s_mode = M_LIST; mark_screen(); return true; }
    case M_LIST: {
        int n = node_rows(s_stack[s_depth - 1].node, s_rows), sel = s_stack[s_depth - 1].sel;
        if (sel >= n) return true;
        row_def_t r = s_rows[sel];
        switch (r.kind) {
        case K_TOGGLE: toggle_flip(&r); mark_rows(sel, sel); return true;
        case K_SUB:    if (r.id == S_ALARM_ITEM) s_ai = r.arg; if (r.id == S_KEY) s_ki = r.arg; push_node(sub_node(r.id)); return true;
        case K_VALUE:  if ((r.id == V_TIME || r.id == V_DATE) && S.auto_time) return true;
                       s_edit = r.id; snprintf(s_edit_label, sizeof s_edit_label, "%s", r.label); s_mode = M_VALUE; mark_screen(); return true;
        case K_CHOICE: if (r.id == C_DASH) tesserae_picks_fetch();     /* fresh: dashboards can be bound any time */
                       s_edit = r.id; snprintf(s_edit_label, sizeof s_edit_label, "%s", r.label); s_csel = clampi(choice_get(r.id), 0, 99); s_mode = M_CHOICE; mark_screen(); return true;
        case K_BACK:   list_back(); return true;
        case K_ACTION:
            if (r.id == A_ADD_ALARM && s_nalarms < MAX_ALARMS) {
                s_alarms[s_nalarms] = (alarm_t){ 7, 0, true, {1,1,1,1,1,0,0}, 15 };
                s_ai = s_nalarms++; publish_alarm(s_ai); sync_alarm_summary();
                s_stack[s_depth - 1].sel = s_ai;
                push_node(N_ALARM);
            } else if (r.id == A_REFRESH) {
                s_req = LREQ_REFRESH;
            } else if (r.id == A_RESTART) {
                s_req = LREQ_RESTART;
            } else if (r.id == A_WIFISETUP) {
                s_req = LREQ_SETUP;
            } else if (r.id == A_OTACHECK) {
                ota_releases_fetch(); s_rel_armed = -1; mark_screen();
            } else if (r.id == A_RELEASE) {
                if (s_rel_armed != r.arg) { s_rel_armed = r.arg; int sl = s_stack[s_depth - 1].sel; mark_rows(sl, sl); }
                else { s_rel_armed = -1; if (!ota_install(r.arg)) toast(ICON_GEAR, "Update failed", ota_error()); }
            } else if (r.id == A_PREVIEW) {
                chorus_preview(s_preview_len_ms ? 0 : 90);
                rebuild(); { int sl = s_stack[s_depth - 1].sel; mark_rows(sl, sl); }
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
    case M_CHOICE: s_mode = M_LIST; mark_screen(); return true;
    case M_LIST: list_back(); return true;
    }
    return true;
}

void state_changed(void)
{
    rebuild();
    if (s_mode == M_HOME) R_SBMID();
    else if (s_mode == M_LIST) { int s = s_stack[s_depth - 1].sel; mark_rows(s, s); }
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
    const char *b = config_get()->bind[CONFIG_BIND_KEY1_HOLD + i];
    return b[0] && strcmp(b, "none") != 0;
}

/* True when the UI handled the key itself: the alarm ringing, or MENU. Keys
 * 1-3 otherwise run their press / hold bindings (main.c). */
bool local_key(int i, bool hold)
{
    if (i < 0 || i > 3) return false;
    if (S.phase == PHASE_RINGING) { snooze(); return true; }
    if (i == 3) { if (hold) { if (s_mode != M_HOME) go_home(); return true; } return local_back(); }
    return false;
}

bool local_back_up(void)
{
    if (S.phase == PHASE_RINGING) { snooze(); return true; }
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
    if (!strcmp(id, "rain.toggle"))   { toggle_flip(&R_T_RAIN); state_changed(); toast(ICON_CLOUD_RAIN, KINDS[S.rain_kind], S.rain_on ? "On" : "Off"); return true; }
    if (!strcmp(id, "lights.toggle")) { toggle_flip(&R_T_LIGHTS); state_changed(); toast(ICON_LIGHTBULB, "Lights", S.lights_on ? "On" : "Off"); return true; }
    if (!strcmp(id, "ac.toggle"))     { toggle_flip(&R_T_AC); state_changed();
                                        char v[16]; if (S.ac_mode) snprintf(v, sizeof v, "%s %d" DEG, AC_MODES[S.ac_mode], S.sp_x2 / 2); else snprintf(v, sizeof v, "Off");
                                        toast(ICON_SNOWFLAKE, "AC", v); return true; }
    if (!strcmp(id, "alarm.toggle"))  { if (s_nalarms) { s_alarms[0].on = !s_alarms[0].on; publish_alarm(0); sync_alarm_summary(); } state_changed();
                                        char v[8]; if (S.alarm_on) alarm_txt(v, sizeof v); else snprintf(v, sizeof v, "Off");
                                        toast(ICON_ALARM, "Alarm", v); return true; }
    if (!strcmp(id, "night"))         { toggle_flip(&R_T_NIGHT); state_changed(); toast(ICON_MOON, "Night mode", S.night ? "On" : "Off"); return true; }
    if (!strcmp(id, "light.night"))   { S.lights_on = true; S.light_mode = 1; S.bright = 10; publish("set/lights", "night 10%%"); state_changed(); toast(ICON_LIGHTBULB, "Night light", "10%"); return true; }
    if (!strcmp(id, "dash.next"))     { dash_step(+1); return true; }
    if (!strcmp(id, "dash.prev"))     { dash_step(-1); return true; }
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

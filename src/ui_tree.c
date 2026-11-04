/*
 * ui_tree.c: the menu tree and the model behind its rows: which rows a node
 * has, their titles, and what a toggle, a value or a choice reads and sets.
 * ui_menu.c navigates and draws it.
 */
#include "ui_internal.h"

int s_ki;                             /* the key being set (0..2) */
int s_eqb;                            /* the equaliser band being set (0..SPK_EQ_BANDS-1) */

/* ---- the menu tree ---- */
row_def_t s_rows[ROWS_MAX];
static char s_labels[16][24];
int node_rows(node_t nd, row_def_t *r)
{
    int n = 0;
    switch (nd) {
    case N_ROOT:
        r[n++] = (row_def_t){ S_RAIN, K_SUB, "Sound", ICON_CLOUD_RAIN, 0 };
        r[n++] = (row_def_t){ S_RADIO, K_SUB, "Radio", ICON_SPEAKER_HIGH, 0 };
        r[n++] = (row_def_t){ S_LIGHTS, K_SUB, "Lights", ICON_LIGHTBULB, 0 };
        r[n++] = (row_def_t){ S_AC, K_SUB, "A/C", ICON_SNOWFLAKE, 0 };
        r[n++] = (row_def_t){ S_ALARMS, K_SUB, "Alarms", ICON_ALARM, 0 };
        r[n++] = (row_def_t){ C_DASH, K_CHOICE, "Dashboard", ICON_HOUSE, 0 };
        r[n++] = (row_def_t){ S_SETTINGS, K_SUB, "Settings", ICON_GEAR, 0 };
        break;
    /* Icons go on a list only when every row can have one of its own (the
     * menu, Settings, Sound, Shows); a list of facets of one thing has none. */
    case N_RAIN:
        r[n++] = (row_def_t){ T_RAIN, K_TOGGLE, "Playing", ICON_PLAY, 0 };
        r[n++] = (row_def_t){ V_RAIN_LVL, K_VALUE, "Intensity", ICON_DROP, 0 };
        r[n++] = (row_def_t){ C_RAIN_KIND, K_CHOICE, "Sound", ICON_SPEAKER_HIGH, 0 };
        if (S.rain_kind == RS_RADIO) r[n++] = (row_def_t){ C_STATION, K_CHOICE, "Station", ICON_LIST, 0 };  /* the radio's presets */
        if (S.rain_kind == 0) r[n++] = (row_def_t){ C_SURFACE, K_CHOICE, "Falling on", ICON_CLOUD_RAIN, 0 };          /* rain only */
        if (S.rain_kind == 0 || S.rain_kind == 4) r[n++] = (row_def_t){ C_THUNDER, K_CHOICE, "Thunder", ICON_CLOUD_LIGHTNING, 0 };   /* over rain or a recording */
        r[n++] = (row_def_t){ C_SLEEP, K_CHOICE, "Sleep timer", ICON_TIMER, 0 };
        break;
    case N_LIGHTS:
        r[n++] = (row_def_t){ T_LIGHTS, K_TOGGLE, "Bedroom lights", NULL, 0 };
        r[n++] = (row_def_t){ V_BRIGHT, K_VALUE, "Brightness", NULL, 0 };
        r[n++] = (row_def_t){ C_COLOUR, K_CHOICE, "Colour", NULL, 0 };
        break;
    case N_AC:
        r[n++] = (row_def_t){ T_AC, K_TOGGLE, "A/C", NULL, 0 };
        r[n++] = (row_def_t){ V_SETPOINT, K_VALUE, "Temperature", NULL, 0 };
        r[n++] = (row_def_t){ C_AC_MODE, K_CHOICE, "Mode", NULL, 0 };
        r[n++] = (row_def_t){ C_FAN, K_CHOICE, "Fan", NULL, 0 };
        break;
    case N_ALARMS:
        for (int i = 0; i < s_nalarms; i++) {
            snprintf(s_labels[i], sizeof s_labels[i], "%02d:%02d", s_alarms[i].h, s_alarms[i].m);
            r[n++] = (row_def_t){ S_ALARM_ITEM, K_SUB, s_labels[i], NULL, i };
        }
        if (s_nalarms < MAX_ALARMS) r[n++] = (row_def_t){ A_ADD_ALARM, K_ACTION, "Add alarm", NULL, 0 };
        r[n++] = (row_def_t){ S_CHORUS, K_SUB, "Dawn chorus", NULL, 0 };
        r[n++] = (row_def_t){ C_ALARM_TONE, K_CHOICE, "Alarm sound", NULL, 0 };
        r[n++] = (row_def_t){ C_SNOOZE, K_CHOICE, "Snooze", NULL, 0 };
        break;
    case N_ALARM:
        r[n++] = (row_def_t){ T_ALARM_ON, K_TOGGLE, "Alarm", NULL, s_ai };
        r[n++] = (row_def_t){ V_ALARM_TIME, K_VALUE, "Time", NULL, s_ai };
        r[n++] = (row_def_t){ S_DAYS, K_SUB, "Days", NULL, s_ai };
        r[n++] = (row_def_t){ C_EARLY, K_CHOICE, "Wake up early", NULL, s_ai };
        r[n++] = (row_def_t){ A_DELETE_ALARM, K_ACTION, "Delete alarm", NULL, s_ai };
        break;
    case N_DAYS:
        for (int i = 0; i < 7; i++) r[n++] = (row_def_t){ T_DAY, K_TOGGLE, DAY_NAMES[i], NULL, i };
        break;
    case N_CHORUS:
        for (int i = 0; i < BIRD_COUNT; i++) r[n++] = (row_def_t){ T_BIRD, K_TOGGLE, bird_name(i), NULL, i };
        r[n++] = (row_def_t){ V_BIRD_VOL, K_VALUE, "Alarm volume", NULL, 0 };
        r[n++] = (row_def_t){ V_BIRD_SOFT, K_VALUE, "Softness", NULL, 0 };
        r[n++] = (row_def_t){ A_PREVIEW, K_ACTION, "Preview", NULL, 0 };
        break;
    case N_SETTINGS:
        r[n++] = (row_def_t){ S_HOMESCR, K_SUB, "Home screen", ICON_HOUSE, 0 };
        r[n++] = (row_def_t){ S_DISPLAY, K_SUB, "Display", ICON_SUN, 0 };
        r[n++] = (row_def_t){ S_NIGHTG, K_SUB, "Night", ICON_MOON, 0 };
        r[n++] = (row_def_t){ S_KEYS, K_SUB, "Keys and knob", ICON_KEYBOARD, 0 };
        r[n++] = (row_def_t){ S_SPEAKER, K_SUB, "Speaker", ICON_SPEAKER_HIGH, 0 };
        r[n++] = (row_def_t){ S_TIMEG, K_SUB, "Time and clock", ICON_ALARM, 0 };
        r[n++] = (row_def_t){ S_DEVICE, K_SUB, "Device", ICON_GEAR, 0 };
        break;
    case N_HOMESCR:
        r[n++] = (row_def_t){ C_OVERLAY, K_CHOICE, "Overlay", NULL, 0 };
        r[n++] = (row_def_t){ S_SHOWS, K_SUB, "Shows", NULL, 0 };
        r[n++] = (row_def_t){ C_ATNIGHT, K_CHOICE, "At night", NULL, 0 };
        r[n++] = (row_def_t){ C_LIVE_LAYOUT, K_CHOICE, "Live layout", NULL, 0 };
        r[n++] = (row_def_t){ C_LIVE_DARK, K_CHOICE, "Live colours", NULL, 0 };
        r[n++] = (row_def_t){ T_DEMO, K_TOGGLE, "Demo data", NULL, 0 };
        break;
    case N_SHOWS: {
        static const char *const SHOW_L[5] = { "Clock", "Alarm", "Sound", "Room", "Connection" };
        static const char *const SHOW_I[5] = { ICON_ALARM, ICON_BELL_RINGING, ICON_CLOUD_RAIN, ICON_SNOWFLAKE, ICON_WIFI_HIGH };
        for (int i = 0; i < 5; i++) r[n++] = (row_def_t){ T_SHOW, K_TOGGLE, SHOW_L[i], SHOW_I[i], i };
        break; }
    case N_DISPLAY:
        r[n++] = (row_def_t){ C_CLOCKDU, K_CHOICE, "Clock updates", NULL, 0 };
        r[n++] = (row_def_t){ T_LARGE, K_TOGGLE, "Large text", NULL, 0 };
        r[n++] = (row_def_t){ A_REFRESH, K_ACTION, "Refresh now", NULL, 0 };
        break;
    case N_PANEL:                                   /* the panel's tuning: rarely touched, under Device */
        r[n++] = (row_def_t){ C_REFRESHMODE, K_CHOICE, "Screen updates", NULL, 0 };
        r[n++] = (row_def_t){ C_CLEANMIN, K_CHOICE, "Clean refresh every", NULL, 0 };
        r[n++] = (row_def_t){ C_WFTEMP, K_CHOICE, "Waveform temp", NULL, 0 };
        break;
    case N_NIGHTG:
        r[n++] = (row_def_t){ T_NIGHT, K_TOGGLE, "Night mode", NULL, 0 };
        r[n++] = (row_def_t){ C_AUTONIGHT, K_CHOICE, "Auto night", NULL, 0 };
        r[n++] = (row_def_t){ C_NIGHTKEYS, K_CHOICE, "Keys at night", NULL, 0 };
        break;
    case N_KEYS: {
        static const char *const KEY_L[3] = { "Key 1", "Key 2", "Key 3" };
        for (int i = 0; i < 3; i++) r[n++] = (row_def_t){ S_KEY, K_SUB, KEY_L[i], NULL, i };
        r[n++] = (row_def_t){ C_KNOB_HOLD, K_CHOICE, "Knob hold", NULL, 0 };
        r[n++] = (row_def_t){ S_KEYLIGHTS, K_SUB, "Key lights", NULL, 0 };
        r[n++] = (row_def_t){ S_MENUS, K_SUB, "Menus", NULL, 0 };
        break; }
    case N_KEYLIGHTS:
        r[n++] = (row_def_t){ V_KEYS, K_VALUE, "Brightness", NULL, 0 };
        r[n++] = (row_def_t){ C_KEYCOL, K_CHOICE, "Key colour", NULL, 0 };
        r[n++] = (row_def_t){ C_MENUCOL, K_CHOICE, "Menu key colour", NULL, 0 };
        r[n++] = (row_def_t){ T_KNOB, K_TOGGLE, "Knob light", NULL, 0 };
        break;
    case N_MENUS:
        r[n++] = (row_def_t){ T_TURNHINTS, K_TOGGLE, "Turn counts", NULL, 0 };
        r[n++] = (row_def_t){ T_QUICKMENU, K_TOGGLE, "Quick menu moves", NULL, 0 };
        break;
    case N_KEY:
        r[n++] = (row_def_t){ C_KEY_PRESS, K_CHOICE, "Press", NULL, s_ki };
        r[n++] = (row_def_t){ C_KEY_HOLD, K_CHOICE, "Hold", NULL, s_ki };
        break;
    case N_SOFTWARE: {
        static char lab[OTA_REL_MAX][32];
        r[n++] = (row_def_t){ I_FWVER, K_INFO, "Installed", NULL, 0 };
        r[n++] = (row_def_t){ A_OTACHECK, K_ACTION, "Check for updates", NULL, 0 };
        r[n++] = (row_def_t){ T_AUTOUPD, K_TOGGLE, "Auto-update overnight", NULL, 0 };
        for (int i = 0; i < ota_release_count() && n < 15; i++) {
            const ota_release_t *e = ota_release(i);
            snprintf(lab[i], sizeof lab[i], "%s%s", e->tag, e->pre ? " (pre)" : "");
            r[n++] = (row_def_t){ A_RELEASE, K_ACTION, lab[i], NULL, i };
        }
        break; }
    case N_TIMEG:
        r[n++] = (row_def_t){ T_AUTOTIME, K_TOGGLE, "Set automatically", NULL, 0 };
        r[n++] = (row_def_t){ C_TZ, K_CHOICE, "Time zone", NULL, 0 };
        r[n++] = (row_def_t){ V_TIME, K_VALUE, "Time", NULL, 0 };
        r[n++] = (row_def_t){ V_DATE, K_VALUE, "Date", NULL, 0 };
        r[n++] = (row_def_t){ C_CLOCK, K_CHOICE, "Clock", NULL, 0 };
        break;
    case N_DEVICE:
        r[n++] = (row_def_t){ S_SOFTWARE, K_SUB, "Software", NULL, 0 };
        r[n++] = (row_def_t){ S_PANEL, K_SUB, "Panel", NULL, 0 };
        r[n++] = (row_def_t){ I_STATUS, K_INFO, "Status", NULL, 0 };
        r[n++] = (row_def_t){ T_POWERMON, K_TOGGLE, "Battery backup", NULL, 0 };
        r[n++] = (row_def_t){ A_WIFISETUP, K_ACTION, "Wi-Fi setup", NULL, 0 };
        r[n++] = (row_def_t){ A_RESTART, K_ACTION, "Restart", NULL, 0 };
        r[n++] = (row_def_t){ C_RESET, K_CHOICE, "Reset everything", NULL, 0 };
        break;
    case N_SPEAKER:
        r[n++] = (row_def_t){ T_SPEAKER, K_TOGGLE, "Speaker", NULL, 0 };
        r[n++] = (row_def_t){ C_SPK_PRESET, K_CHOICE, "Preset", NULL, 0 };
        r[n++] = (row_def_t){ S_EQ, K_SUB, "Equaliser", NULL, 0 };
        r[n++] = (row_def_t){ C_SPK_SOFT, K_CHOICE, "Soften highs", NULL, 0 };
        r[n++] = (row_def_t){ C_SPK_HPF, K_CHOICE, "Low cut", NULL, 0 };
        r[n++] = (row_def_t){ C_SPK_LIMIT, K_CHOICE, "Loud moments", NULL, 0 };
        r[n++] = (row_def_t){ C_SPK_TEST, K_CHOICE, "Test sound", NULL, 0 };
        break;
    case N_EQ:
        for (int i = 0; i < SPK_EQ_BANDS; i++) r[n++] = (row_def_t){ V_SPK_EQ, K_VALUE, SPK_EQ_NAMES[i], NULL, i };
        r[n++] = (row_def_t){ V_SPK_GAIN, K_VALUE, "Level", NULL, 0 };
        break;
    case N_RADIO:
        r[n++] = (row_def_t){ T_RADIO, K_TOGGLE, "Playing", NULL, 0 };
        r[n++] = (row_def_t){ S_STATIONS, K_SUB, "My stations", NULL, 0 };
        for (int i = 0; i < radio_soma_count() && n < ROWS_MAX - 1; i++) r[n++] = (row_def_t){ A_SOMA, K_ACTION, radio_soma(i)->title, NULL, i };
        break;
    case N_STATIONS:
        for (int i = 0; i < radio_preset_count(); i++) {
            const radio_preset_t *p = radio_preset(i);
            if (p->url[0]) r[n++] = (row_def_t){ A_STATION, K_ACTION, p->name[0] ? p->name : p->url, NULL, i };
        }
        if (!n) r[n++] = (row_def_t){ I_STATUS, K_INFO, "Set stations in the web app", NULL, 0 };
        break;
    default: break;
    }
    /* No Back row: the MENU key is labelled Back on every page and a knob hold does the same. */
    return n;
}
const char *node_title(node_t nd)
{
    static char b[24];
    switch (nd) {
    case N_ROOT: return "Menu"; case N_RAIN: return "Sound"; case N_RADIO: return "Radio"; case N_STATIONS: return "My stations"; case N_LIGHTS: return "Lights"; case N_AC: return "A/C";
    case N_ALARMS: return "Alarms"; case N_SETTINGS: return "Settings"; case N_CHORUS: return "Dawn chorus";
    case N_ALARM: snprintf(b, sizeof b, "Alarm %d", s_ai + 1); return b;
    case N_DAYS:  snprintf(b, sizeof b, "Alarm %d days", s_ai + 1); return b;
    case N_HOMESCR: return "Home screen"; case N_NIGHTG: return "Night"; case N_KEYS: return "Keys and knob";
    case N_KEY:   snprintf(b, sizeof b, "Key %d", s_ki + 1); return b;
    case N_TIMEG: return "Time and clock"; case N_DEVICE: return "Device"; case N_SOFTWARE: return "Software"; case N_SPEAKER: return "Speaker";
    case N_DISPLAY: return "Display"; case N_SHOWS: return "Shows"; case N_KEYLIGHTS: return "Key lights"; case N_MENUS: return "Menus"; case N_EQ: return "Equaliser";
    case N_PANEL: return "Panel";
    default: return "";
    }
}

/* ---- what each row does: the line under the preview ---------------------------
 * A description of the setting (what it does when it is on), never how to work
 * the knob: the top bar already labels the keys. Dynamic lines show the state. */
void row_desc(const row_def_t *r, char *b, size_t n)
{
    b[0] = 0;
    switch (r->id) {
    case S_RAIN:      snprintf(b, n, "Rain, ocean, wind, a recording or the radio"); break;
    case S_RADIO:     snprintf(b, n, "Internet radio: SomaFM and your own stations"); break;
    case S_LIGHTS:    snprintf(b, n, "The bedroom lights, their brightness and colour"); break;
    case S_AC:        snprintf(b, n, "Mode, temperature and fan"); break;
    case S_ALARMS:    snprintf(b, n, "Alarms, the dawn chorus and snooze"); break;
    case C_DASH:      snprintf(b, n, "Live, drawn here, or a page from the server"); break;
    case S_SETTINGS:  snprintf(b, n, "Screen, night, keys, speaker, time and device"); break;
    case T_RAIN:      snprintf(b, n, "Plays the sleep sound at the volume set"); break;
    case V_RAIN_LVL:  snprintf(b, n, "How heavy the rain, the sea or the wind is"); break;
    case C_RAIN_KIND: snprintf(b, n, "Rain, ocean, noise, wind, a recording or radio"); break;
    case C_SURFACE:   snprintf(b, n, "What the rain lands on"); break;
    case C_THUNDER:   snprintf(b, n, "Distant rolls, or a storm overhead"); break;
    case C_SLEEP:     snprintf(b, n, "Fades the sound out after this long"); break;
    case T_LIGHTS:    snprintf(b, n, "Switches the bedroom lights"); break;
    case V_BRIGHT:    snprintf(b, n, "How bright the lights are"); break;
    case C_COLOUR:    snprintf(b, n, "Warm white, or a dim night glow"); break;
    case T_AC:        snprintf(b, n, "Switches the air conditioner"); break;
    case V_SETPOINT:  snprintf(b, n, "The room temperature to hold"); break;
    case C_AC_MODE:   snprintf(b, n, "Cool, heat, fan or dry"); break;
    case C_FAN:       snprintf(b, n, "Fan speed, or automatic"); break;
    case S_ALARM_ITEM: snprintf(b, n, "Time, days, early wake, delete"); break;
    case A_ADD_ALARM: snprintf(b, n, "Adds an alarm at 07:00 on weekdays"); break;
    case S_CHORUS:    snprintf(b, n, "Birds that build up before the alarm"); break;
    case C_ALARM_TONE: snprintf(b, n, S.tone_playing && S.tone_until ? "Playing; rings after the chorus has built" : "Rings after the chorus has built; each plays as you pass it"); break;
    case C_SNOOZE:    snprintf(b, n, "How long a snooze lasts"); break;
    case T_ALARM_ON:  snprintf(b, n, "Rings at its time on its days"); break;
    case V_ALARM_TIME: snprintf(b, n, "When it rings"); break;
    case S_DAYS:      snprintf(b, n, "Which days it rings; none means once"); break;
    case C_EARLY:     snprintf(b, n, "Chorus and lights start this long before"); break;
    case A_DELETE_ALARM: snprintf(b, n, "Removes this alarm"); break;
    case T_DAY:       snprintf(b, n, "Rings on this day"); break;
    case T_BIRD:      snprintf(b, n, "Sings in the dawn chorus"); break;
    case V_BIRD_VOL:  snprintf(b, n, "How loud the chorus is when the alarm rings"); break;
    case V_BIRD_SOFT: snprintf(b, n, "Takes the edge off whistles and chirps"); break;
    case A_PREVIEW:   snprintf(b, n, s_preview_len_ms ? "Playing; press to stop" : "Plays the chorus building up"); break;
    case S_HOMESCR:   snprintf(b, n, "The overlay and the Live page"); break;
    case S_DISPLAY:   snprintf(b, n, "Clock updates, text size, a refresh"); break;
    case S_NIGHTG:    snprintf(b, n, "Night mode, when it comes on, the keys at night"); break;
    case S_KEYS:      snprintf(b, n, "What the keys do, their lights, the menus"); break;
    case S_SPEAKER:   snprintf(b, n, "On or off, presets, the equaliser, low cut, loud moments"); break;
    case S_TIMEG:     snprintf(b, n, "Automatic time, the zone, 12 or 24 hours"); break;
    case S_DEVICE:    snprintf(b, n, "Software, the panel, power, Wi-Fi, restart"); break;
    case C_OVERLAY:   { int x, y, w, h; viewport(&x, &y, &w, &h); snprintf(b, n, "Where the clock and controls sit; dashboard %d x %d", w, h); } break;
    case S_SHOWS:     snprintf(b, n, "What the overlay shows beside the dashboard"); break;
    case C_ATNIGHT:   snprintf(b, n, "What the overlay keeps while night mode is on"); break;
    case C_LIVE_LAYOUT: snprintf(b, n, "Bento, a big clock, the weather or tonight's plan"); break;
    case C_LIVE_DARK: snprintf(b, n, "White on black, for the night"); break;
    case T_DEMO:      snprintf(b, n, "Fills the Live page with sample values"); break;
    case T_SHOW:      snprintf(b, n, "Shown in the overlay"); break;
    case C_CLOCKDU:   snprintf(b, n, "Two-level: black-and-white digits, no flash, no fade"); break;
    case T_LARGE:     snprintf(b, n, "Bigger menu text, eight rows to a page"); break;
    case A_REFRESH:   snprintf(b, n, "Clears and repaints the whole screen"); break;
    case S_PANEL:     snprintf(b, n, "Screen updates, clean refreshes, the waveform"); break;
    case C_REFRESHMODE: snprintf(b, n, "%s", S.refresh_mode == 2 ? "Whole screen, every change; flashes"
                                             : S.refresh_mode == 1 ? "Full quality, just what changed" : "Just what changed, a clean-up when you stop"); break;
    case C_CLEANMIN:  snprintf(b, n, "Partial updates wear the picture; this resets it while idle"); break;
    case C_WFTEMP:    { int s = it8951_temp_sensed(); if (s >= 0) snprintf(b, n, "Board sensor says %d C; lower is darker", s); else snprintf(b, n, "Lower is darker, higher is lighter"); } break;
    case T_NIGHT:     snprintf(b, n, "Dim keys, a quiet overlay, a dark Live page"); break;
    case C_AUTONIGHT: snprintf(b, n, "Night mode comes on by itself between these hours"); break;
    case C_NIGHTKEYS: snprintf(b, n, "The key lights at night: a low glow, or off"); break;
    case S_KEY:       snprintf(b, n, "What a press and a hold of key %d do", r->arg + 1); break;
    case C_KNOB_HOLD: snprintf(b, n, "What holding the knob down does"); break;
    case S_KEYLIGHTS: snprintf(b, n, "Brightness and colours"); break;
    case S_MENUS:     snprintf(b, n, "Turn counts and quick moves"); break;
    case V_KEYS:      snprintf(b, n, "How bright the key lights are"); break;
    case C_KEYCOL:    snprintf(b, n, "The colour of keys 1 to 3"); break;
    case C_MENUCOL:   snprintf(b, n, "The colour of the MENU key"); break;
    case T_KNOB:      snprintf(b, n, "Lights the ring around the knob"); break;
    case T_TURNHINTS: snprintf(b, n, "Beside each row: turns to reach it, + clockwise"); break;
    case T_QUICKMENU: snprintf(b, n, "Steps in black and white, full quality when you stop"); break;
    case T_QUICK_A2:  snprintf(b, n, "The fastest waveform for steps; faint ghosts until the clean refresh"); break;
    case T_SETTLEFULL: snprintf(b, n, "When you stop: the whole screen, cleared first; flashes"); break;
    case C_KEY_PRESS: case C_KEY_HOLD: snprintf(b, n, "Key %d, %s", s_ki + 1, r->id == C_KEY_HOLD ? "held" : "pressed"); break;
    case I_FWVER:     { int sl = ota_boot_partition(); snprintf(b, n, "slot %c " DOT " from github.com/%s", sl >= 0 ? 'A' + sl : '-', "dmellok"); } break;
    case A_OTACHECK:  snprintf(b, n, "%d release(s) listed; asks GitHub for the list", ota_release_count()); break;
    case T_AUTOUPD:   snprintf(b, n, "Newest stable, 03:10, never near an alarm"); break;
    case A_RELEASE:   { const ota_release_t *e = ota_release(r->arg);
                        if (e) snprintf(b, n, "%s " DOT " %s", e->date, s_rel_armed == r->arg ? "press again to install" : "installs this release"); } break;
    case T_AUTOTIME:  snprintf(b, n, "The time from the network, the zone from Tesserae"); break;
    case C_TZ:        snprintf(b, n, "Your own zone instead of the server's"); break;
    case V_TIME: case V_DATE: snprintf(b, n, S.auto_time ? "Set automatically; switch that off to change it" : "Set by hand"); break;
    case C_CLOCK:     snprintf(b, n, "12-hour or 24-hour"); break;
    case S_SOFTWARE:  snprintf(b, n, "Version, updates, overnight auto-update"); break;
    case I_STATUS:    snprintf(b, n, "Wi-Fi %s " DOT " %s", net_connected() ? "on" : "off", tesserae_state_name()); break;
    case T_POWERMON:  snprintf(b, n, "Watches the battery and says when it is low"); break;
    case A_WIFISETUP: snprintf(b, n, s_armed == A_WIFISETUP ? "Press again: restarts into setup" : "Restarts into setup; join its Wi-Fi from a phone"); break;
    case A_RESTART:   snprintf(b, n, s_armed == A_RESTART ? "Press again to restart" : "Restarts the panel; it asks twice"); break;
    case C_RESET:     snprintf(b, n, "Back to factory settings; it asks first"); break;
    case T_SPEAKER:   snprintf(b, n, "Plays through the speaker"); break;
    case C_SPK_PRESET: snprintf(b, n, "A starting point; the bands below fine-tune it"); break;
    case S_EQ:        snprintf(b, n, "Five bands, 80 Hz to 7 kHz, and the level"); break;
    case V_SPK_EQ:    snprintf(b, n, "Boost or cut around %s", SPK_EQ_NAMES[r->arg]); break;
    case V_SPK_GAIN:  snprintf(b, n, "The overall level into the speaker"); break;
    case C_SPK_SOFT:  snprintf(b, n, "Rolls off everything above this: sharp calls, hiss"); break;
    case C_SPK_HPF:   snprintf(b, n, "Removes rumble below this"); break;
    case C_SPK_LIMIT: snprintf(b, n, "Holds the loudest moments down"); break;
    case C_SPK_TEST:  snprintf(b, n, "Plays for 20 s at the current volume"); break;
    case T_RADIO:     { const char *t = radio_title(); snprintf(b, n, "%.24s " DOT " %s", radio_name(), radio_state() == RADIO_PLAYING && t[0] ? t : radio_state_name()); } break;
    case S_STATIONS:  snprintf(b, n, "Your own streams, set in the web app"); break;
    case C_STATION:   { const char *t = radio_title(); if (radio_state() == RADIO_PLAYING && t[0]) snprintf(b, n, "%.60s", t); else snprintf(b, n, "%s " DOT " stations are set in the web app", radio_state_name()); } break;
    case A_SOMA:      snprintf(b, n, "%.62s", radio_soma(r->arg)->desc); break;
    case A_STATION:   { const radio_preset_t *p = radio_preset(r->arg); snprintf(b, n, "%.62s", p ? p->url : ""); } break;
    default: break;
    }
}


/* The window of a scrolling list: the first row shown, moved only when the
 * selection would leave it. Short lists (under LIST_SCROLL rows) show whole. */

/* ---- the model behind the rows ---- */
static const char *const SLEEPS[4]  = { "Off", "15 min", "30 min", "60 min" };
static const int SLEEP_V[4] = { 0, 15, 30, 60 };
static const char *const COLOURS[2] = { "Warm", "Night" };
static const char *const MODES4[4]  = { "Cool", "Heat", "Fan", "Dry" };
static const int MODE_V[4] = { 3, 1, 2, 4 };
static const char *const EARLY[5]   = { "Off", "5 min", "10 min", "15 min", "30 min" };
static const int EARLY_V[5] = { 0, 5, 10, 15, 30 };

bool toggle_get(const row_def_t *r)
{
    switch (r->id) {
    case T_RADIO: return S.rain_on && S.rain_kind == RS_RADIO;
    case T_RAIN: return S.rain_on; case T_LIGHTS: return S.lights_on; case T_AC: return S.ac_mode != 0;
    case T_ALARM_ON: return s_alarms[r->arg].on; case T_DAY: return s_alarms[s_ai].days[r->arg];
    case T_BIRD: return (S.birds >> r->arg) & 1;
    case T_KNOB: return S.knob_light;
    case T_SHOW: return (S.show >> r->arg) & 1;
    case T_AUTOTIME: return S.auto_time;
    case T_AUTOUPD: return S.auto_update;
    case T_SPEAKER: return S.speaker; case T_POWERMON: return S.power_mon;
    case T_TURNHINTS: return S.turn_hints; case T_QUICKMENU: return S.quick_menus; case T_SETTLEFULL: return S.settle_full;
    case T_QUICK_A2: return S.quick_a2;
    case T_LARGE: return S.large_text;
    case T_DEMO: return S.live_demo;
    case T_NIGHT: return S.night; default: return false;
    }
}
void toggle_flip(const row_def_t *r)
{
    switch (r->id) {
    case T_RAIN:   S.rain_on = !S.rain_on; publish("set/rain", S.rain_on ? "play" : "pause"); break;
    case T_RADIO:  if (S.rain_on && S.rain_kind == RS_RADIO) S.rain_on = false; else { S.rain_kind = RS_RADIO; S.rain_on = true; }
                   publish("set/rain", S.rain_on ? "radio" : "pause"); break;
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
    case T_QUICK_A2: S.quick_a2 = !S.quick_a2; break;
    case T_LARGE:   S.large_text = !S.large_text; break;
    case T_DEMO:    S.live_demo = !S.live_demo; break;
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

vmeta_t value_meta(item_t id)
{
    switch (id) {
    case V_RAIN_LVL:   return (vmeta_t){ S.rain_lvl, 1, 5, 1, "Drizzle", "Downpour", false };
    case V_BRIGHT:     return (vmeta_t){ S.bright, 5, 100, 5, "Dim", "Bright", false };
    case V_SETPOINT:   return (vmeta_t){ S.sp_x2, 32, 60, 1, "16" DEG, "30" DEG, false };
    case V_ALARM_TIME: return (vmeta_t){ s_alarms[s_ai].h * 60 + s_alarms[s_ai].m, 0, 1439, 5, "00:00", "23:59", true };
    case V_KEYS:       return (vmeta_t){ inputs_backlight() * 100 / 255, 0, 100, 10, "Off", "Bright", false };
    case V_BIRD_VOL:   return (vmeta_t){ S.bird_vol, 1, 10, 1, "Soft", "Loud", false };
    case V_BIRD_SOFT:  return (vmeta_t){ S.bird_soft, 0, 100, 5, "Natural", "Soft", false };
    case V_SPK_GAIN:   return (vmeta_t){ S.spk_gain, -12, 6, 1, "-12 dB", "+6 dB", false };
    case V_SPK_EQ:     return (vmeta_t){ S.spk_eq[s_eqb], -SPK_EQ_MAX_DB, SPK_EQ_MAX_DB, 1, "Cut", "Boost", false };
    case V_TIME:       { int wd, sec; int64_t mn; int v = clock_now(&wd, &sec, &mn) ? sec / 60 : 0;
                         return (vmeta_t){ v, 0, 1439, 1, "00:00", "23:59", true }; }
    case V_DATE:       return (vmeta_t){ s_man_days, -3650, 3650, 1, "", "", false };
    default:           return (vmeta_t){ 0, 0, 1, 1, "", "", false };
    }
}
void value_text(item_t id, char *b, size_t n)
{
    switch (id) {
    case V_RAIN_LVL:   snprintf(b, n, "%d of 5", S.rain_lvl); break;
    case V_BRIGHT:     snprintf(b, n, "%d%%", S.bright); break;
    case V_SETPOINT:   snprintf(b, n, "%d.%d" DEG, S.sp_x2 / 2, (S.sp_x2 % 2) * 5); break;
    case V_ALARM_TIME: snprintf(b, n, "%02d:%02d", s_alarms[s_ai].h, s_alarms[s_ai].m); break;
    case V_KEYS:       snprintf(b, n, "%d%%", inputs_backlight() * 100 / 255); break;
    case V_BIRD_VOL:   snprintf(b, n, "%d of 10", S.bird_vol); break;
    case V_BIRD_SOFT:  snprintf(b, n, S.bird_soft ? "%d%%" : "Natural", S.bird_soft); break;
    case V_SPK_GAIN:   snprintf(b, n, "%+d dB", S.spk_gain); break;
    case V_SPK_EQ:     snprintf(b, n, "%+d dB", S.spk_eq[s_eqb]); break;
    case V_TIME:       { int wd, sec; int64_t mn; if (clock_now(&wd, &sec, &mn)) fmt_hm(b, n, sec / 60); else snprintf(b, n, "--:--"); } break;
    case V_DATE:       { int y, m, d, wd; if (local_ymd(&y, &m, &d, &wd)) snprintf(b, n, "%.3s %d %.3s", DAY_NAMES[wd], d, MONTHS[m - 1]); else snprintf(b, n, "--"); } break;
    default:           b[0] = 0; break;
    }
}
/* d is the number of detents (signed); accel scales the step for fast turns. */
void value_step(item_t id, int d, int accel)
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
    case V_BIRD_SOFT:  S.bird_soft = v; S.audition_bird = BIRD_WHISTLER; S.audition++; break;   /* a whistle to judge it by */
    case V_SPK_GAIN:   S.spk_gain = v; break;
    case V_SPK_EQ:     S.spk_eq[s_eqb] = v; break;
    case V_KEYS:       { uint8_t lv = (uint8_t)(v * 255 / 100); inputs_set_backlight(lv); config_set_lights(lv, S.night); config_save(); } break;
    case V_TIME:       { int delta = v - m.v; if (delta > 720) delta -= 1440; if (delta < -720) delta += 1440;
                         s_man_base += (int64_t)delta * 60; rebuild(); } break;
    case V_DATE:       s_man_base += (int64_t)(v - s_man_days) * 86400; s_man_days = v; rebuild(); break;
    default: break;
    }
}
int choice_opts(item_t id, const char *const **o)
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
    case C_ALARM_TONE: *o = RS_TONE_NAMES; return RS_TONES;
    case C_RESET:     *o = RESETS; return 2;
    case C_THUNDER:   *o = THUNDERS; return 3;
    case C_OVERLAY:   *o = OVERLAYS; return 4;
    case C_ATNIGHT:   *o = ATNIGHT; return 2;
    case C_CLOCKDU:   *o = CLOCKDU; return 3;
    case C_STATION: {                                   /* the presets with a URL, by name */
        static const char *names[RADIO_PRESETS]; int k = 0;
        for (int i = 0; i < radio_preset_count(); i++) { const radio_preset_t *p = radio_preset(i); names[i] = p->name[0] ? p->name : (p->url[0] ? p->url : "(empty)"); if (p->url[0]) k = i + 1; }
        *o = names; return k ? k : 1; }
    case C_NIGHTKEYS: *o = NIGHTKEYS; return 2;
    case C_KEY_PRESS: case C_KEY_HOLD: *o = KEYACT_NAME; return N_KEYACT;
    case C_KNOB_HOLD: *o = KNOBACT_NAME; return N_KNOBACT;
    case C_TZ:        *o = TZ_NAMES; return N_TZ;
    case C_REFRESHMODE: *o = REFRESHES; return 3;
    case C_WFTEMP:    *o = WFTEMPS; return N_WFTEMP;
    case C_CLEANMIN:  *o = CLEANS; return N_CLEAN;
    case C_SPK_PRESET: {                                /* the presets, then "Custom" for anything else */
        static const char *names[SPK_N_PRESETS + 1];
        if (!names[0]) { for (int i = 0; i < SPK_N_PRESETS; i++) names[i] = SPK_PRESETS[i].name; names[SPK_N_PRESETS] = "Custom"; }
        *o = names; return SPK_N_PRESETS + 1; }
    case C_LIVE_LAYOUT: *o = LIVE_LAYOUTS; return 4;
    case C_LIVE_DARK: *o = LIVE_DARKS; return 3;
    case C_SPK_SOFT:  *o = SOFTENS; return 6;
    case C_SPK_HPF:   *o = LOWCUTS; return 5;
    case C_SPK_LIMIT: *o = PEAKS; return 3;
    case C_SPK_TEST:  *o = TESTS; return 5;
    default: *o = NULL; return 0;
    }
}
int choice_get(item_t id)
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
    case C_ALARM_TONE: return S.alarm_tone;
    case C_RESET:     return 0;
    case C_THUNDER:   return !S.thunder ? 0 : S.thunder_near < 50 ? 1 : 2;
    case C_OVERLAY:   return S.overlay;
    case C_ATNIGHT:   return S.at_night;
    case C_CLOCKDU:   return S.clock_du;
    case C_STATION:   return S.station;
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
    case C_SPK_PRESET: { int p = spk_preset_match(S.spk_eq); return p < 0 ? SPK_N_PRESETS : p; }
    case C_LIVE_LAYOUT: return S.live_layout;
    case C_LIVE_DARK: return S.live_dark;
    case C_SPK_SOFT:  return S.spk_soft;
    case C_SPK_HPF:   return S.spk_hpf;
    case C_SPK_LIMIT: return S.spk_limit;
    case C_SPK_TEST:  return S.test;
    default:          return 0;
    }
}
void choice_set(item_t id, int k)
{
    switch (id) {
    case C_RAIN_KIND: S.rain_kind = k; if (k != RS_RADIO) S.sleep_kind = k; S.rain_on = true; publish("set/rain", "%s", KINDS[k]); break;
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
    case C_ALARM_TONE: S.alarm_tone = k; publish("config/alarm_tone", "%s", RS_TONE_NAMES[k]); break;
    case C_RESET:     if (k == 1) { printf("local: master reset asked for\n"); s_req = LREQ_RESET; } break;
    case C_OVERLAY:   S.overlay = k; overlay_apply(); break;
    case C_ATNIGHT:   S.at_night = k; break;
    case C_CLOCKDU:   S.clock_du = k; break;
    case C_STATION:   S.station = k; S.rain_on = true; break;
    case C_NIGHTKEYS: S.night_keys = k; apply_colours(); break;
    case C_KEY_PRESS: case C_KEY_HOLD:
        config_set_bind((id == C_KEY_HOLD ? CONFIG_BIND_KEY1_HOLD : CONFIG_BIND_KEY1) + s_ki, KEYACT_ID[k]); config_save(); break;
    case C_KNOB_HOLD: config_set_bind(CONFIG_BIND_KNOB_HOLD, KNOBACT_ID[k]); config_save(); break;
    case C_TZ:        S.tz = k; rebuild(); break;
    case C_REFRESHMODE: S.refresh_mode = k; s_req = LREQ_REFRESH; break;   /* one clean refresh now */
    case C_WFTEMP:      S.wf_temp = WFTEMP_V[k]; s_req = LREQ_REFRESH; break;
    case C_CLEANMIN:    S.clean_min = CLEAN_V[k]; break;
    case C_SPK_PRESET:  if (k < SPK_N_PRESETS) for (int i = 0; i < SPK_EQ_BANDS; i++) S.spk_eq[i] = SPK_PRESETS[k].db[i]; break;
    case C_LIVE_LAYOUT: S.live_layout = k; break;
    case C_LIVE_DARK:   S.live_dark = k; break;
    case C_SPK_SOFT:    S.spk_soft = k; break;
    case C_SPK_HPF:     S.spk_hpf = k; break;
    case C_SPK_LIMIT:   S.spk_limit = k; break;
    case C_SPK_TEST:    S.test = k; S.test_until = k ? now_ms() + 20000u : 0; break;
    case C_THUNDER:   if (k == 0) S.thunder = 0;
                      else if (k == 1) { if (!S.thunder) S.thunder = 35; S.thunder_near = 20; }
                      else { if (S.thunder < 60) S.thunder = 70; S.thunder_near = 75; }
                      publish("set/rain", "thunder %s", THUNDERS[k]); break;
    default: break;
    }
}
/* Is this station (a preset, or a SomaFM channel when station is -1) the one the radio is playing? */
static bool radio_now(int station, const char *soma)
{
    if (!S.rain_on || S.rain_kind != RS_RADIO || S.station != station) return false;
    return station >= 0 || (soma && !strcmp(S.soma, soma));
}
/* What the radio is on, for the rows that say so. */
const char *radio_name(void)
{
    if (S.station >= 0) { const radio_preset_t *p = radio_preset(S.station); return p && p->name[0] ? p->name : "Radio"; }
    int i = radio_soma_find(S.soma); return i >= 0 ? radio_soma(i)->title : "Radio";
}

void row_value(const row_def_t *r, char *b, size_t n)
{
    const char *const *o;
    b[0] = 0;
    switch (r->kind) {
    case K_TOGGLE: snprintf(b, n, "%s", toggle_get(r) ? "On" : "Off"); break;
    case K_VALUE:  if (r->id == V_SPK_EQ) snprintf(b, n, "%+d dB", S.spk_eq[r->arg]); else value_text(r->id, b, n); break;
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
                   if ((r->id == A_RESTART || r->id == A_WIFISETUP) && s_armed == (int)r->id) snprintf(b, n, "Press again");
                   if (r->id == A_SOMA && radio_now(-1, radio_soma(r->arg)->id)) snprintf(b, n, "Playing");
                   if (r->id == A_STATION && radio_now(r->arg, NULL)) snprintf(b, n, "Playing");
                   if (r->id == A_OTACHECK && ota_checked_ms()) { int m = (int)((now_ms() - ota_checked_ms()) / 60000); if (m < 1) snprintf(b, n, "just now"); else snprintf(b, n, "%d min ago", m); }
                   if (r->id == A_RELEASE) { const ota_release_t *e = ota_release(r->arg); int c = e ? ota_version_cmp(e->ver, FW_VERSION) : 0;
                                             snprintf(b, n, "%s", !e ? "" : s_rel_armed == r->arg ? "Press again" : !strcmp(e->ver, FW_VERSION) ? "Installed" : c > 0 ? "Newer" : "Older"); }
                   break;
    case K_SUB:
        switch (r->id) {
        case S_RAIN:   if (S.rain_on) snprintf(b, n, "%s %d", KINDS[S.rain_kind], S.rain_lvl); else snprintf(b, n, "Off"); break;
        case S_RADIO:  if (S.rain_on && S.rain_kind == RS_RADIO) snprintf(b, n, "%.22s", radio_name()); else snprintf(b, n, "Off"); break;
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
node_t sub_node(item_t id)
{
    switch (id) {
    case S_CHORUS: return N_CHORUS;
    case S_RAIN: return N_RAIN; case S_LIGHTS: return N_LIGHTS; case S_AC: return N_AC; case S_ALARMS: return N_ALARMS;
    case S_ALARM_ITEM: return N_ALARM; case S_DAYS: return N_DAYS;
    case S_HOMESCR: return N_HOMESCR; case S_NIGHTG: return N_NIGHTG; case S_KEYS: return N_KEYS; case S_KEY: return N_KEY;
    case S_TIMEG: return N_TIMEG; case S_DEVICE: return N_DEVICE; case S_SOFTWARE: return N_SOFTWARE; case S_SPEAKER: return N_SPEAKER; case S_RADIO: return N_RADIO; case S_STATIONS: return N_STATIONS;
    case S_DISPLAY: return N_DISPLAY; case S_SHOWS: return N_SHOWS; case S_KEYLIGHTS: return N_KEYLIGHTS; case S_MENUS: return N_MENUS; case S_EQ: return N_EQ;
    case S_PANEL: return N_PANEL;
    default: return N_SETTINGS;
    }
}


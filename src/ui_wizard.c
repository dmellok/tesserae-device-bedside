/*
 * ui_wizard.c: first-run setup, after the panel has joined WiFi (the WiFi
 * itself is the captive-portal page in net.c).
 *
 * One wizard, shared: the device holds the step (S.wiz, -1 when it is not
 * running) and both the panel and the web app show that step; either one moves
 * it on, and the other follows. On the panel each step is a list made of the
 * ordinary setting rows (the same toggles, choices and values as in Settings)
 * plus a Continue row, so nothing here draws a control of its own. Steps that
 * need typing (the server, Home Assistant, the weather location) show their
 * state here and are filled in on the phone.
 *
 * It runs after the first WiFi join of a fresh device (or after a reset); a
 * device that is already paired skips it. Settings > Device > Run setup again
 * starts it any time. ui_menu.c does the navigation (open, step, back).
 */
#include "ui_internal.h"

enum { WZ_WELCOME, WZ_TESSERAE, WZ_TIME, WZ_NIGHT, WZ_ALARM, WZ_HA, WZ_DONE };

/* the info rows (I_WZ, by arg) */
enum { WI_PHONE, WI_TSTATE, WI_SERVER, WI_LOCATION, WI_HA, WI_DASH, WI_ALARM };

static const char *const HEADS[WZ_STEPS] = { "Welcome", "Tesserae", "Time and place", "Night", "Alarm", "Home Assistant", "All set" };

static bool paired(void)    { return tesserae_state() == TESS_READY; }
static bool has_frame(void) { return tesserae_frame() != NULL; }

const char *wizard_title(void)
{
    static char b[32];
    snprintf(b, sizeof b, "Setup " DOT " %d of %d", S.wiz + 1, WZ_STEPS);
    return b;
}
const char *wizard_heading(void) { return S.wiz >= 0 && S.wiz < WZ_STEPS ? HEADS[S.wiz] : ""; }
const char *wizard_icon(void)
{
    switch (S.wiz) {
    case WZ_WELCOME: return ICON_HOUSE;   case WZ_TESSERAE: return ICON_LIST;  case WZ_TIME: return ICON_SUN_HORIZON;
    case WZ_NIGHT:   return ICON_MOON;    case WZ_ALARM:    return ICON_ALARM; case WZ_HA:   return ICON_LIGHTBULB;
    default:         return ICON_CHECK;
    }
}

/* What the step is for: the paragraph in the preview, under the heading. */
void wizard_intro(char *b, size_t n)
{
    b[0] = 0;
    switch (S.wiz) {
    case WZ_WELCOME:  snprintf(b, n, "A few steps to get the panel ready. Use the knob here, or open the address on the left on your phone: both follow along."); break;
    case WZ_TESSERAE:
        if (!config_get()->server_url[0])       snprintf(b, n, "Tesserae draws the dashboards. No server is set yet: add its address on your phone.");
        else if (has_frame())                   snprintf(b, n, "Paired, and a dashboard is showing. You can bind more in Tesserae any time.");
        else if (paired())                      snprintf(b, n, "Paired. Now bind a dashboard to this panel in Tesserae and press Send.");
        else if (tesserae_state() == TESS_DISCOVERING) snprintf(b, n, "In Tesserae open Settings > Devices > Discovered and press Register beside %s.", config_get()->device_name);
        else if (tesserae_last_error()[0])      snprintf(b, n, "Can't reach the server yet. Check its address on your phone.");
        else                                    snprintf(b, n, "Tesserae draws the dashboards. Waiting for the server to answer.");
        break;
    case WZ_TIME:     snprintf(b, n, "The time comes from the network. Choose 12 or 24 hours. The weather location is set on your phone."); break;
    case WZ_NIGHT:    snprintf(b, n, "Night mode dims the keys and darkens the screen. It can come on by itself every evening."); break;
    case WZ_ALARM:    snprintf(b, n, s_nalarms ? "A dawn chorus of birds builds up before it rings. More alarms are under Alarms in the menu."
                                               : "Wake to a dawn chorus that builds up before the alarm. Optional: Continue skips it."); break;
    case WZ_HA:       snprintf(b, n, "Optional. With Home Assistant the keys switch your lights and A/C, and the panel shows up there. Set it up on your phone."); break;
    case WZ_DONE:     snprintf(b, n, "Everything here can be changed in the menu. Run this again from Settings > Device."); break;
    default: break;
    }
}

int wizard_rows(row_def_t *r)
{
    int n = 0;
    switch (S.wiz) {
    case WZ_WELCOME:
        r[n++] = (row_def_t){ I_WZ, K_INFO, "On your phone", NULL, WI_PHONE };
        r[n++] = (row_def_t){ A_WZ_NEXT, K_ACTION, "Start", NULL, 0 };
        r[n++] = (row_def_t){ A_WZ_LATER, K_ACTION, "Set up later", NULL, 0 };
        break;
    case WZ_TESSERAE:
        r[n++] = (row_def_t){ I_WZ, K_INFO, "Status", NULL, WI_TSTATE };
        r[n++] = (row_def_t){ I_WZ, K_INFO, "Server", NULL, WI_SERVER };
        r[n++] = (row_def_t){ A_WZ_NEXT, K_ACTION, has_frame() ? "Continue" : "Skip for now", NULL, 0 };
        break;
    case WZ_TIME:
        r[n++] = (row_def_t){ T_AUTOTIME, K_TOGGLE, "Set automatically", NULL, 0 };
        r[n++] = (row_def_t){ C_TZ, K_CHOICE, "Time zone", NULL, 0 };
        r[n++] = (row_def_t){ C_CLOCK, K_CHOICE, "Clock", NULL, 0 };
        r[n++] = (row_def_t){ I_WZ, K_INFO, "Weather location", NULL, WI_LOCATION };
        r[n++] = (row_def_t){ A_WZ_NEXT, K_ACTION, "Continue", NULL, 0 };
        break;
    case WZ_NIGHT:
        r[n++] = (row_def_t){ C_AUTONIGHT, K_CHOICE, "Auto night", NULL, 0 };
        r[n++] = (row_def_t){ C_NIGHTKEYS, K_CHOICE, "Keys at night", NULL, 0 };
        r[n++] = (row_def_t){ A_WZ_NEXT, K_ACTION, "Continue", NULL, 0 };
        break;
    case WZ_ALARM:
        if (!s_nalarms) r[n++] = (row_def_t){ A_ADD_ALARM, K_ACTION, "Add an alarm", NULL, 0 };
        else {                                           /* the first alarm, with the rows of Alarms > Alarm 1 */
            s_ai = 0;
            r[n++] = (row_def_t){ T_ALARM_ON, K_TOGGLE, "Alarm", NULL, 0 };
            r[n++] = (row_def_t){ V_ALARM_TIME, K_VALUE, "Time", NULL, 0 };
            r[n++] = (row_def_t){ S_DAYS, K_SUB, "Days", NULL, 0 };
            r[n++] = (row_def_t){ C_EARLY, K_CHOICE, "Wake up early", NULL, 0 };
        }
        r[n++] = (row_def_t){ A_WZ_NEXT, K_ACTION, "Continue", NULL, 0 };
        break;
    case WZ_HA:
        r[n++] = (row_def_t){ I_WZ, K_INFO, "Home Assistant", NULL, WI_HA };
        r[n++] = (row_def_t){ I_WZ, K_INFO, "On your phone", NULL, WI_PHONE };
        r[n++] = (row_def_t){ A_WZ_NEXT, K_ACTION, ha_connected() ? "Continue" : "Skip for now", NULL, 0 };
        break;
    case WZ_DONE:
        r[n++] = (row_def_t){ I_WZ, K_INFO, "Dashboard", NULL, WI_DASH };
        r[n++] = (row_def_t){ I_WZ, K_INFO, "Alarm", NULL, WI_ALARM };
        r[n++] = (row_def_t){ I_WZ, K_INFO, "Home Assistant", NULL, WI_HA };
        r[n++] = (row_def_t){ A_WZ_DONE, K_ACTION, "Finish", NULL, 0 };
        break;
    default: break;
    }
    return n;
}

/* Where the highlight starts on a step: the first row that does something
 * (on a step of facts only, that is Continue). */
int wizard_default_sel(void)
{
    row_def_t r[ROWS_MAX];
    int n = wizard_rows(r);
    for (int i = 0; i < n; i++) if (r[i].kind != K_INFO) return i;
    return n ? n - 1 : 0;
}

void wizard_row_value(const row_def_t *r, char *b, size_t n)
{
    b[0] = 0;
    switch (r->arg) {
    case WI_PHONE:    if (net_ip()[0]) snprintf(b, n, "http://%s", net_ip()); else snprintf(b, n, "No WiFi"); break;
    case WI_TSTATE:   snprintf(b, n, "%s", has_frame() ? "Showing" : paired() ? "Paired" : tesserae_state() == TESS_DISCOVERING ? "Register it"
                                       : tesserae_state() == TESS_OFF ? "No server" : tesserae_last_error()[0] ? "Not reachable" : "Waiting"); break;
    case WI_SERVER:   { const char *u = config_get()->server_url; if (!strncmp(u, "http://", 7)) u += 7; snprintf(b, n, "%.22s", u[0] ? u : "Not set"); } break;
    case WI_LOCATION: { float la, lo; char a[16], o[16]; live_location(&la, &lo); live_fmt_dec(a, sizeof a, la, 2); live_fmt_dec(o, sizeof o, lo, 2); snprintf(b, n, "%s, %s", a, o); } break;   /* no %f in this printf */
    case WI_HA:       snprintf(b, n, "%s", ha_connected() ? "Connected" : ha_enabled() ? "Not connected" : "Not set up"); break;
    case WI_DASH:     snprintf(b, n, "%s", has_frame() ? "Showing" : paired() ? "None bound" : "Not paired"); break;
    case WI_ALARM:    { int i = -1; for (int k = 0; k < s_nalarms; k++) if (s_alarms[k].on) { i = k; break; }
                        if (i >= 0) { char hm[8]; fmt_hm(hm, sizeof hm, s_alarms[i].h * 60 + s_alarms[i].m); snprintf(b, n, "%s", hm); }
                        else snprintf(b, n, "Off"); } break;
    default: break;
    }
}

void wizard_row_desc(const row_def_t *r, char *b, size_t n)
{
    b[0] = 0;
    switch (r->id) {
    case A_WZ_NEXT:  snprintf(b, n, "%s", S.wiz == WZ_WELCOME ? "Five short steps; Back goes to the one before" : "On to the next step; Back goes to the one before"); break;
    case A_WZ_LATER: snprintf(b, n, "Leaves setup; Settings > Device starts it again"); break;
    case A_WZ_DONE:  snprintf(b, n, "Back to the dashboard"); break;
    case A_WZ_RERUN: snprintf(b, n, "Walks through pairing, time, night, alarm and Home Assistant"); break;
    case I_WZ:
        switch (r->arg) {
        case WI_PHONE:    snprintf(b, n, "Open this on a phone on the same WiFi"); break;
        case WI_TSTATE:   snprintf(b, n, "%s", tesserae_state_name()); break;
        case WI_SERVER:   snprintf(b, n, "Change it on your phone"); break;
        case WI_LOCATION: snprintf(b, n, "For the weather on the Live page; change it on your phone"); break;
        case WI_HA:       snprintf(b, n, "MQTT broker, user and password: on your phone"); break;
        case WI_DASH:     snprintf(b, n, "Bound to this panel in Tesserae"); break;
        case WI_ALARM:    snprintf(b, n, "The next alarm that is on"); break;
        default: break;
        }
        break;
    default: break;
    }
}

/* What a step's rows show that can change underneath it (pairing, Home
 * Assistant, the address): when it does, the list is drawn again. */
bool wizard_changed(void)
{
    static char last[96];
    char now[96];
    snprintf(now, sizeof now, "%d|%d|%d|%d|%d|%s|%d|%.40s", S.wiz, (int)tesserae_state(), has_frame(), ha_connected(), ha_enabled(),
             net_ip(), s_nalarms, tesserae_last_error());
    if (!strcmp(now, last)) return false;
    snprintf(last, sizeof last, "%s", now);
    return true;
}

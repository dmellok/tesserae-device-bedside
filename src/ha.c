/*
 * ha.c: Home Assistant over MQTT with discovery. See ha.h.
 *
 * lwIP's MQTT client (built from the SDK's copy by src/vendor/lwip_mqtt.c).
 * Its callbacks run in the lwIP (interrupt) context; everything else here runs
 * from the main loop under cyw43_arch_lwip_begin/end. Incoming commands are
 * queued as app command "ha" and translated on the main loop, so they take
 * the same path as the web app's.
 */
#include "ha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "config.h"
#include "net.h"
#include "local.h"
#include "birds.h"
#include "tesserae.h"
#include "live.h"

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif
#ifndef FW_VERSION
#define FW_VERSION "0.1.0"
#endif

void app_command(const char *name, const char *value);   /* main.c */

/* ---- settings (flash blob slot 2) ---------------------------------------------- */
#define HA_MAGIC 0x48414331u   /* "HAC1" */
typedef struct {
    uint8_t  enabled, pad;
    uint16_t port;
    char     host[64], user[48], pass[64], prefix[32], base[32];
} ha_cfg_t;
static ha_cfg_t C;

static void cfg_defaults(void)
{
    memset(&C, 0, sizeof C);
    C.port = 1883;
    snprintf(C.prefix, sizeof C.prefix, "homeassistant");
    snprintf(C.base, sizeof C.base, "bedside");
#ifdef MQTT_HOST
    snprintf(C.host, sizeof C.host, "%s", MQTT_HOST); C.enabled = 1;
#endif
#ifdef MQTT_PORT
    C.port = MQTT_PORT;
#endif
#ifdef MQTT_USER
    snprintf(C.user, sizeof C.user, "%s", MQTT_USER);
#endif
#ifdef MQTT_PASS
    snprintf(C.pass, sizeof C.pass, "%s", MQTT_PASS);
#endif
}

/* ---- connection state ---------------------------------------------------------- */
typedef enum { HS_OFF, HS_DNS, HS_CONNECTING, HS_UP, HS_WAIT } hs_t;
static hs_t           s_st = HS_OFF;
static mqtt_client_t *s_cli;
static ip_addr_t      s_ip;
static volatile int   s_dns_done;          /* 0 pending, 1 ok, -1 failed */
static volatile int   s_conn;              /* 0 pending, 1 accepted, -1 refused/lost */
static volatile int   s_conn_status;
static uint32_t       s_retry_at, s_backoff = 2000;
static char           s_err[64];
static char           s_uid[24];           /* "bedside_a1b2c3" */
static char           s_cid[32];           /* client id */
static char           s_will[48];
static int            s_disc;              /* next discovery entity to publish; -1 = done */
static int            s_disc_total;
static bool           s_subscribed;
static volatile bool  s_rediscover;
static uint32_t       s_state_hash, s_state_at, s_diag_at;
static bool           s_online_sent;
static int            s_live_sub;          /* next live slot to subscribe */
static uint32_t       s_live_gen;          /* live sources these subscriptions are for */

/* events from local.c, published from the main loop */
#define EVQ 6
static char           s_evq[EVQ][64];
static volatile int   s_evh, s_evt;

/* incoming publish being assembled (lwIP context) */
static char           s_in_topic[96];
static char           s_in_data[200];
static size_t         s_in_len;

/* ---- helpers ----------------------------------------------------------------- */
static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static uint32_t fnv(const char *s, size_t n) { uint32_t h = 2166136261u; while (n--) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }

static void set_err(const char *e) { snprintf(s_err, sizeof s_err, "%s", e); if (e[0]) printf("ha: %s\n", e); }

static err_t pub(const char *topic, const char *payload, size_t len, bool retain)
{
    if (!s_cli) return ERR_CONN;
    cyw43_arch_lwip_begin();
    err_t e = mqtt_client_is_connected(s_cli) ? mqtt_publish(s_cli, topic, payload, (u16_t)len, 0, retain ? 1 : 0, NULL, NULL) : ERR_CONN;
    cyw43_arch_lwip_end();
    return e;
}

/* ---- lwIP callbacks ------------------------------------------------------------- */
static void dns_cb(const char *name, const ip_addr_t *ip, void *arg)
{
    (void)name; (void)arg;
    if (ip) { s_ip = *ip; s_dns_done = 1; } else s_dns_done = -1;
}

static void conn_cb(mqtt_client_t *c, void *arg, mqtt_connection_status_t status)
{
    (void)c; (void)arg;
    s_conn_status = (int)status;
    s_conn = status == MQTT_CONNECT_ACCEPTED ? 1 : -1;
}

static void inpub_cb(void *arg, const char *topic, u32_t tot_len)
{
    (void)arg; (void)tot_len;
    snprintf(s_in_topic, sizeof s_in_topic, "%s", topic);
    s_in_len = 0;
}

static void indata_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    (void)arg;
    size_t room = sizeof s_in_data - 1 - s_in_len;
    if (len > room) len = (u16_t)room;
    memcpy(s_in_data + s_in_len, data, len); s_in_len += len;
    if (!(flags & MQTT_DATA_FLAG_LAST)) return;
    s_in_data[s_in_len] = 0;
    char status_t[48]; snprintf(status_t, sizeof status_t, "%s/status", C.prefix);
    char set_p[48];    snprintf(set_p, sizeof set_p, "%s/set/", C.base);
    size_t sl = strlen(set_p);
    if (!strcmp(s_in_topic, status_t)) {
        if (!strcmp(s_in_data, "online")) s_rediscover = true;     /* HA restarted: announce again */
    } else if (!strncmp(s_in_topic, set_p, sl)) {
        char v[200];
        snprintf(v, sizeof v, "%.40s|%.150s", s_in_topic + sl, s_in_data);
        app_command("ha", v);
    } else {
        live_on_message(s_in_topic, s_in_data, C.base);   /* the live dashboard's values */
    }
}

/* ---- discovery -------------------------------------------------------------------- */
typedef struct { const char *comp, *key, *name, *icon, *vt, *extra; char topic; } ent_t;   /* topic: 's' state, 'd' diag, 'e' event */
#define SW   "\"pl_on\":\"1\",\"pl_off\":\"0\",\"stat_on\":\"1\",\"stat_off\":\"0\""
#define PCT  "\"min\":0,\"max\":100,\"step\":5,\"unit_of_meas\":\"%\",\"mode\":\"slider\""
static const ent_t ENTS[] = {
    /* sleep sounds */
    { "switch", "rain_on", "Sleep sounds", "mdi:weather-pouring", "{{ value_json.rain.on }}", SW, 's' },
    { "select", "rain_kind", "Sleep sound", "mdi:waves", "{{ ['Rain','Ocean','Noise','Wind','Recording'][value_json.rain.kind] }}",
      "\"ops\":[\"Rain\",\"Ocean\",\"Noise\",\"Wind\",\"Recording\"],\"cmd_tpl\":\"{{ ['Rain','Ocean','Noise','Wind','Recording'].index(value) }}\"", 's' },
    { "number", "rain_lvl", "Rain intensity", "mdi:weather-rainy", "{{ value_json.rain.lvl }}", "\"min\":1,\"max\":5,\"step\":1,\"mode\":\"slider\"", 's' },
    { "select", "sleep", "Sleep timer", "mdi:timer-sand", "{{ {0:'Off',15:'15 min',30:'30 min',60:'60 min'}[value_json.rain.sleep] }}",
      "\"ops\":[\"Off\",\"15 min\",\"30 min\",\"60 min\"],\"cmd_tpl\":\"{{ {'Off':0,'15 min':15,'30 min':30,'60 min':60}[value] }}\"", 's' },
    { "sensor", "sleep_left", "Sleep timer remaining", "mdi:timer-outline", "{{ (value_json.rain.sleep_left_s / 60) | round(0) | int }}", "\"unit_of_meas\":\"min\"", 's' },
    { "number", "vol", "Volume", "mdi:volume-high", "{{ value_json.vol }}", "\"min\":0,\"max\":10,\"step\":1,\"mode\":\"slider\"", 's' },
    /* rain shaping and thunder */
    { "select", "surface", "Rain falling on", "mdi:home-roof", "{{ ['Ground','Leaves','Tin roof','Window'][value_json.shape.surface] }}",
      "\"ops\":[\"Ground\",\"Leaves\",\"Tin roof\",\"Window\"],\"cmd_tpl\":\"{{ ['Ground','Leaves','Tin roof','Window'].index(value) }}\"", 's' },
    { "number", "patter", "Rain drops", "mdi:water", "{{ value_json.shape.patter }}", PCT, 's' },
    { "number", "wash", "Rain hiss", "mdi:water-outline", "{{ value_json.shape.wash }}", PCT, 's' },
    { "number", "tone", "Rain tone", "mdi:tune-vertical", "{{ value_json.shape.tone }}", PCT, 's' },
    { "number", "wind", "Wind", "mdi:weather-windy", "{{ value_json.shape.wind }}", PCT, 's' },
    { "number", "thunder", "Thunder frequency", "mdi:weather-lightning", "{{ value_json.shape.thunder }}", PCT, 's' },
    { "number", "thunder_near", "Thunder closeness", "mdi:weather-lightning-rainy", "{{ value_json.shape.thunder_near }}", PCT, 's' },
    { "button", "strike", "Roll thunder", "mdi:flash", NULL, "\"pl_prs\":\"1\"", 's' },
    /* dawn chorus */
    { "number", "bird_vol", "Dawn chorus volume", "mdi:bird", "{{ value_json.chorus.vol }}", "\"min\":1,\"max\":10,\"step\":1,\"mode\":\"slider\"", 's' },
    { "button", "chorus_preview", "Preview dawn chorus", "mdi:play-circle", NULL, "\"pl_prs\":\"90\"", 's' },
    { "sensor", "chorus_level", "Dawn chorus level", "mdi:bird", "{{ value_json.chorus.level }}", "\"unit_of_meas\":\"%\"", 's' },
    /* the alarm clock */
    { "sensor", "alarm_phase", "Alarm state", "mdi:alarm", "{{ ['idle','waking','ringing'][value_json.phase] }}",
      "\"dev_cla\":\"enum\",\"ops\":[\"idle\",\"waking\",\"ringing\"]", 's' },
    { "button", "alarm_stop", "Stop alarm", "mdi:alarm-off", NULL, "\"pl_prs\":\"1\"", 's' },
    { "button", "alarm_snooze", "Snooze alarm", "mdi:alarm-snooze", NULL, "\"pl_prs\":\"1\"", 's' },
    { "button", "alarm_add", "Add alarm", "mdi:alarm-plus", NULL, "\"pl_prs\":\"1\",\"ent_cat\":\"config\"", 's' },
    /* room */
    { "switch", "lights_on", "Lights", "mdi:lightbulb", "{{ value_json.lights.on }}", SW, 's' },
    { "number", "bright", "Light brightness", "mdi:brightness-6", "{{ value_json.lights.bright }}",
      "\"min\":5,\"max\":100,\"step\":5,\"unit_of_meas\":\"%\",\"mode\":\"slider\"", 's' },
    { "select", "colour", "Light colour", "mdi:palette", "{{ ['Warm','Night'][value_json.lights.colour] }}",
      "\"ops\":[\"Warm\",\"Night\"],\"cmd_tpl\":\"{{ ['Warm','Night'].index(value) }}\"", 's' },
    { "select", "ac_mode", "A/C mode", "mdi:air-conditioner", "{{ ['Off','Heat','Fan','Cool','Dry'][value_json.ac.mode] }}",
      "\"ops\":[\"Off\",\"Heat\",\"Fan\",\"Cool\",\"Dry\"],\"cmd_tpl\":\"{{ ['Off','Heat','Fan','Cool','Dry'].index(value) }}\"", 's' },
    { "number", "setpoint", "A/C setpoint", "mdi:thermometer", "{{ value_json.ac.sp }}",
      "\"min\":16,\"max\":30,\"step\":0.5,\"unit_of_meas\":\"\xc2\xb0""C\",\"dev_cla\":\"temperature\"", 's' },
    { "select", "fan", "A/C fan", "mdi:fan", "{{ ['Auto','Low','Mid','High'][value_json.ac.fan] }}",
      "\"ops\":[\"Auto\",\"Low\",\"Mid\",\"High\"],\"cmd_tpl\":\"{{ ['Auto','Low','Mid','High'].index(value) }}\"", 's' },
    /* panel */
    { "switch", "night", "Night mode", "mdi:weather-night", "{{ value_json.night }}", SW, 's' },
    /* hardware */
    { "switch", "speaker", "Speaker", "mdi:speaker", "{{ value_json.hw.speaker }}", SW ",\"ent_cat\":\"config\"", 's' },
    { "switch", "gestures", "Wave gestures", "mdi:gesture-swipe", "{{ value_json.hw.gestures }}", SW ",\"ent_cat\":\"config\"", 's' },
    { "sensor", "battery", "Battery", "mdi:battery", "{{ value_json.hw.pct if value_json.hw.power_mon else None }}",
      "\"unit_of_meas\":\"%\",\"dev_cla\":\"battery\",\"stat_cla\":\"measurement\"", 's' },
    { "binary_sensor", "mains", "Mains power", "mdi:power-plug", "{{ 'OFF' if value_json.hw.on_battery else 'ON' }}", "\"dev_cla\":\"power\"", 's' },
    { "number", "keylights", "Key lights", "mdi:keyboard", "{{ value_json.keylights }}", PCT ",\"ent_cat\":\"config\"", 's' },
    { "select", "dash", "Dashboard", "mdi:view-dashboard", NULL, NULL, 's' },              /* options built at run time */
    { "button", "cmd_fetch", "Fetch dashboard", "mdi:download", NULL, "\"pl_prs\":\"1\"", 's' },
    { "button", "cmd_refresh", "Re-render dashboard", "mdi:refresh", NULL, "\"pl_prs\":\"1\"", 's' },
    { "button", "cmd_reboot", "Restart", NULL, NULL, "\"pl_prs\":\"1\",\"dev_cla\":\"restart\",\"ent_cat\":\"config\"", 's' },
    /* panel settings */
    { "select", "key_col", "Key colour", "mdi:palette", NULL, NULL, 's' },        /* palette options built at run time */
    { "select", "menu_col", "Menu key colour", "mdi:palette", NULL, NULL, 's' },
    { "select", "night_col", "Night key colour", "mdi:palette", NULL, NULL, 's' },
    { "switch", "knob_light", "Knob light", "mdi:knob", "{{ value_json.panel.knob_light }}", SW ",\"ent_cat\":\"config\"", 's' },
    { "select", "auto_night", "Auto night", "mdi:weather-night", "{{ ['Off','21:00 to 07:00','22:00 to 07:00','23:00 to 07:00'][value_json.panel.auto_night] }}",
      "\"ops\":[\"Off\",\"21:00 to 07:00\",\"22:00 to 07:00\",\"23:00 to 07:00\"],\"cmd_tpl\":\"{{ ['Off','21:00 to 07:00','22:00 to 07:00','23:00 to 07:00'].index(value) }}\",\"ent_cat\":\"config\"", 's' },
    { "select", "clock12", "Clock format", "mdi:clock-outline", "{{ ['24-hour','12-hour'][value_json.panel.clock12] }}",
      "\"ops\":[\"24-hour\",\"12-hour\"],\"cmd_tpl\":\"{{ ['24-hour','12-hour'].index(value) }}\",\"ent_cat\":\"config\"", 's' },
    { "number", "snooze", "Snooze length", "mdi:alarm-snooze", "{{ value_json.panel.snooze }}",
      "\"min\":1,\"max\":30,\"step\":1,\"unit_of_meas\":\"min\",\"ent_cat\":\"config\"", 's' },
    /* diagnostics */
    { "sensor", "rssi", "Wi-Fi signal", NULL, "{{ value_json.rssi }}",
      "\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"", 'd' },
    { "sensor", "uptime", "Uptime", NULL, "{{ value_json.uptime_s }}",
      "\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",\"ent_cat\":\"diagnostic\"", 'd' },
    { "sensor", "ip", "IP address", "mdi:ip-network", "{{ value_json.ip }}", "\"ent_cat\":\"diagnostic\"", 'd' },
    { "sensor", "tess", "Tesserae link", "mdi:monitor-dashboard", "{{ value_json.tess }}", "\"ent_cat\":\"diagnostic\"", 'd' },
    /* events for automations */
    { "event", "event", "Events", "mdi:bell-ring", NULL,
      "\"evt_typ\":[\"prewake\",\"alarm_ringing\",\"alarm_snoozed\",\"alarm_stopped\",\"thunder\",\"chorus_preview\",\"chorus_preview_stop\",\"gesture_left\",\"gesture_right\",\"gesture_up\",\"gesture_down\",\"gesture_wave\",\"on_battery\",\"mains\"]", 'e' },
};
#define N_FIXED   ((int)(sizeof ENTS / sizeof ENTS[0]))
#define N_SLOTS   4
#define PER_SLOT  5
#define N_ENTS    (N_FIXED + 2 * BIRD_COUNT + N_SLOTS * PER_SLOT)

static const char *const EARLY_OPS = "[\"Off\",\"5 min\",\"10 min\",\"15 min\",\"30 min\"]";

/* Build discovery entity k: topic and payload. */
static bool disc_build(int k, char *topic, size_t tn, char *pl, size_t pn)
{
    char comp[8], key[24], name[48], icon[40] = "", vt[220] = "", extra[420] = "", avty[300] = "";
    char tsel = 's';
    if (k < N_FIXED) {
        const ent_t *e = &ENTS[k];
        snprintf(comp, sizeof comp, "%s", e->comp); snprintf(key, sizeof key, "%s", e->key);
        snprintf(name, sizeof name, "%s", e->name);
        if (e->icon) snprintf(icon, sizeof icon, "%s", e->icon);
        if (e->vt) snprintf(vt, sizeof vt, "%s", e->vt);
        if (e->extra) snprintf(extra, sizeof extra, "%s", e->extra);
        tsel = e->topic;
        if (!strcmp(key, "key_col") || !strcmp(key, "menu_col") || !strcmp(key, "night_col")) {   /* the palette, by name */
            size_t n = (size_t)snprintf(extra, sizeof extra, "\"ent_cat\":\"config\",\"ops\":[");
            for (int i = 0; i < local_palette_count(); i++) n += (size_t)snprintf(extra + n, sizeof extra - n, "%s\"%s\"", i ? "," : "", local_palette_name(i));
            n += (size_t)snprintf(extra + n, sizeof extra - n, "],\"cmd_tpl\":\"{{ [");
            for (int i = 0; i < local_palette_count(); i++) n += (size_t)snprintf(extra + n, sizeof extra - n, "%s'%s'", i ? "," : "", local_palette_name(i));
            snprintf(extra + n, sizeof extra - n, "].index(value) }}\"");
            n = (size_t)snprintf(vt, sizeof vt, "{{ value_json.panel.palette[value_json.panel.%s] }}", key);
        }
        if (!strcmp(key, "dash")) {             /* the dashboards, by name */
            size_t n = (size_t)snprintf(extra, sizeof extra, "\"ops\":[");
            for (int i = 0; i < local_dash_count(); i++) n += (size_t)snprintf(extra + n, sizeof extra - n, "%s\"%s\"", i ? "," : "", local_dash_name(i));
            n += (size_t)snprintf(extra + n, sizeof extra - n, "],\"cmd_tpl\":\"{{ [");
            for (int i = 0; i < local_dash_count(); i++) n += (size_t)snprintf(extra + n, sizeof extra - n, "%s'%s'", i ? "," : "", local_dash_name(i));
            n += (size_t)snprintf(extra + n, sizeof extra - n, "].index(value) }}\"");
            n = (size_t)snprintf(vt, sizeof vt, "{{ [");
            for (int i = 0; i < local_dash_count(); i++) n += (size_t)snprintf(vt + n, sizeof vt - n, "%s'%s'", i ? "," : "", local_dash_name(i));
            snprintf(vt + n, sizeof vt - n, "][value_json.dash] }}");
        }
    } else if (k < N_FIXED + 2 * BIRD_COUNT) {
        int i = (k - N_FIXED) / 2;
        if ((k - N_FIXED) % 2 == 0) {
            snprintf(comp, sizeof comp, "switch"); snprintf(key, sizeof key, "chorus_%d", i);
            snprintf(name, sizeof name, "Chorus: %s", bird_name(i));
            snprintf(vt, sizeof vt, "{{ ((value_json.chorus.birds // %d) %% 2) }}", 1 << i);
            snprintf(extra, sizeof extra, SW ",\"ent_cat\":\"config\"");
        } else {
            snprintf(comp, sizeof comp, "button"); snprintf(key, sizeof key, "play_%d", i);
            snprintf(name, sizeof name, "Play %s", bird_name(i));
            snprintf(extra, sizeof extra, "\"pl_prs\":\"1\"");
        }
        snprintf(icon, sizeof icon, "mdi:bird");
    } else if (k < N_ENTS) {
        int j = k - N_FIXED - 2 * BIRD_COUNT, i = j / PER_SLOT, f = j % PER_SLOT;
        snprintf(avty, sizeof avty,
                 "\"avty\":[{\"t\":\"%s/status\"},{\"t\":\"%s/state\",\"val_tpl\":\"{{ 'online' if value_json.alarms|length > %d else 'offline' }}\"}],\"avty_mode\":\"all\"",
                 C.base, C.base, i);
        snprintf(icon, sizeof icon, "mdi:alarm");
        switch (f) {
        case 0: snprintf(comp, sizeof comp, "switch"); snprintf(key, sizeof key, "alarm%d_on", i); snprintf(name, sizeof name, "Alarm %d", i + 1);
                snprintf(vt, sizeof vt, "{{ value_json.alarms[%d].on if value_json.alarms|length > %d else 0 }}", i, i);
                snprintf(extra, sizeof extra, SW); break;
        case 1: snprintf(comp, sizeof comp, "text"); snprintf(key, sizeof key, "alarm%d_time", i); snprintf(name, sizeof name, "Alarm %d time", i + 1);
                snprintf(vt, sizeof vt, "{{ '%%02d:%%02d' %% (value_json.alarms[%d].h, value_json.alarms[%d].m) if value_json.alarms|length > %d else '' }}", i, i, i);
                snprintf(extra, sizeof extra, "\"pattern\":\"^([01][0-9]|2[0-3]):[0-5][0-9]$\",\"min\":5,\"max\":5"); break;
        case 2: snprintf(comp, sizeof comp, "text"); snprintf(key, sizeof key, "alarm%d_days", i); snprintf(name, sizeof name, "Alarm %d days", i + 1);
                snprintf(vt, sizeof vt, "{{ value_json.alarms[%d].dt if value_json.alarms|length > %d else '' }}", i, i);
                snprintf(icon, sizeof icon, "mdi:calendar-week"); break;
        case 3: snprintf(comp, sizeof comp, "select"); snprintf(key, sizeof key, "alarm%d_early", i); snprintf(name, sizeof name, "Alarm %d wake-up window", i + 1);
                snprintf(vt, sizeof vt, "{{ {0:'Off',5:'5 min',10:'10 min',15:'15 min',30:'30 min'}.get(value_json.alarms[%d].early, 'Off') if value_json.alarms|length > %d else 'Off' }}", i, i);
                snprintf(extra, sizeof extra, "\"ops\":%s,\"cmd_tpl\":\"{{ {'Off':0,'5 min':5,'10 min':10,'15 min':15,'30 min':30}[value] }}\"", EARLY_OPS);
                snprintf(icon, sizeof icon, "mdi:weather-sunset-up"); break;
        default: snprintf(comp, sizeof comp, "button"); snprintf(key, sizeof key, "alarm%d_del", i); snprintf(name, sizeof name, "Delete alarm %d", i + 1);
                snprintf(extra, sizeof extra, "\"pl_prs\":\"1\",\"ent_cat\":\"config\""); snprintf(icon, sizeof icon, "mdi:alarm-off"); break;
        }
    } else return false;

    snprintf(topic, tn, "%s/%s/%s/%s/config", C.prefix, comp, s_uid, key);
    char ip[20]; snprintf(ip, sizeof ip, "%s", net_ip());
    const config_t *cfg = config_get();
    size_t n = (size_t)snprintf(pl, pn, "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\"", name, s_uid, key);
    const char *st = tsel == 'd' ? "diag" : tsel == 'e' ? "event" : "state";
    bool commands = strcmp(comp, "sensor") && strcmp(comp, "binary_sensor") && strcmp(comp, "event");
    if (strcmp(comp, "button")) n += (size_t)snprintf(pl + n, pn - n, ",\"stat_t\":\"%s/%s\"", C.base, st);
    if (vt[0]) n += (size_t)snprintf(pl + n, pn - n, ",\"val_tpl\":\"%s\"", vt);
    if (commands) n += (size_t)snprintf(pl + n, pn - n, ",\"cmd_t\":\"%s/set/%s\"", C.base, key);
    if (avty[0]) n += (size_t)snprintf(pl + n, pn - n, ",%s", avty);
    else n += (size_t)snprintf(pl + n, pn - n, ",\"avty_t\":\"%s/status\"", C.base);
    if (icon[0]) n += (size_t)snprintf(pl + n, pn - n, ",\"ic\":\"%s\"", icon);
    if (extra[0]) n += (size_t)snprintf(pl + n, pn - n, ",%s", extra);
    n += (size_t)snprintf(pl + n, pn - n,
        ",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Jorgo\",\"mdl\":\"Bedside Dashboard\",\"sw\":\"%s\",\"cu\":\"http://%s/\"}"
        ",\"o\":{\"name\":\"tesserae-device-bedside\"}}",
        s_uid, cfg->device_name[0] ? cfg->device_name : "Bedside", FW_VERSION, ip);
    return n < pn;
}

/* ---- public ------------------------------------------------------------------------ */
bool ha_connected(void) { return s_st == HS_UP; }

void ha_init(void)
{
    if (!config_blob_load(2, HA_MAGIC, &C, sizeof C)) cfg_defaults();
    C.host[sizeof C.host - 1] = 0; C.user[sizeof C.user - 1] = 0; C.pass[sizeof C.pass - 1] = 0;
    C.prefix[sizeof C.prefix - 1] = 0; C.base[sizeof C.base - 1] = 0;
    if (!C.prefix[0]) snprintf(C.prefix, sizeof C.prefix, "homeassistant");
    if (!C.base[0]) snprintf(C.base, sizeof C.base, "bedside");
    s_disc = -1;
}

static void teardown(void)
{
    if (s_cli) {
        cyw43_arch_lwip_begin();
        if (mqtt_client_is_connected(s_cli)) mqtt_disconnect(s_cli);
        mqtt_client_free(s_cli);
        cyw43_arch_lwip_end();
        s_cli = NULL;
    }
    s_subscribed = false; s_disc = -1; s_online_sent = false; s_state_hash = 0; s_live_sub = 0;
}

static void fail(const char *why)
{
    set_err(why);
    teardown();
    s_st = HS_WAIT;
    s_retry_at = now_ms() + s_backoff;
    if (s_backoff < 60000) s_backoff *= 2;
}

void ha_event(const char *text)
{
    int n = (s_evh + 1) % EVQ;
    if (n == s_evt) return;                     /* full: drop */
    snprintf(s_evq[s_evh], sizeof s_evq[0], "%s", text);
    s_evh = n;
}

void ha_poll(void)
{
    uint32_t t = now_ms();
    if (!C.enabled || !C.host[0] || !net_connected()) {
        if (s_st != HS_OFF) { teardown(); s_st = HS_OFF; }
        return;
    }
    switch (s_st) {
    case HS_OFF:
    case HS_WAIT:
        if (s_st == HS_WAIT && (int32_t)(t - s_retry_at) < 0) return;
        {
            char mac[18]; net_mac(mac, sizeof mac);
            snprintf(s_uid, sizeof s_uid, "bedside_%c%c%c%c%c%c", mac[9], mac[10], mac[12], mac[13], mac[15], mac[16]);
            snprintf(s_cid, sizeof s_cid, "%s", s_uid);
        }
        s_dns_done = 0;
        if (ipaddr_aton(C.host, &s_ip)) s_dns_done = 1;
        else {
            cyw43_arch_lwip_begin();
            err_t e = dns_gethostbyname(C.host, &s_ip, dns_cb, NULL);
            cyw43_arch_lwip_end();
            if (e == ERR_OK) s_dns_done = 1;
            else if (e != ERR_INPROGRESS) { fail("broker name lookup failed"); return; }
        }
        s_retry_at = t + 10000;                 /* lookup / connect timeout */
        s_st = HS_DNS;
        return;
    case HS_DNS:
        if (s_dns_done < 0) { fail("broker name not found"); return; }
        if (!s_dns_done) { if ((int32_t)(t - s_retry_at) >= 0) fail("broker name lookup timed out"); return; }
        {
            snprintf(s_will, sizeof s_will, "%s/status", C.base);
            struct mqtt_connect_client_info_t ci = {
                .client_id = s_cid, .client_user = C.user[0] ? C.user : NULL, .client_pass = C.pass[0] ? C.pass : NULL,
                .keep_alive = 60, .will_topic = s_will, .will_msg = "offline", .will_msg_len = 7, .will_qos = 0, .will_retain = 1,
            };
            s_conn = 0;
            cyw43_arch_lwip_begin();
            s_cli = mqtt_client_new();
            err_t e = s_cli ? mqtt_client_connect(s_cli, &s_ip, C.port, conn_cb, NULL, &ci) : ERR_MEM;
            if (s_cli) mqtt_set_inpub_callback(s_cli, inpub_cb, indata_cb, NULL);
            cyw43_arch_lwip_end();
            if (e != ERR_OK) { fail("could not connect to the broker"); return; }
            printf("ha: connecting to %s:%u as %s\n", C.host, C.port, C.user[0] ? C.user : "(anonymous)");
            s_retry_at = t + 10000;
            s_st = HS_CONNECTING;
        }
        return;
    case HS_CONNECTING:
        if (s_conn < 0) {
            fail(s_conn_status == MQTT_CONNECT_REFUSED_USERNAME_PASS || s_conn_status == MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_
                 ? "broker refused the username or password" : "broker refused or dropped the connection");
            return;
        }
        if (!s_conn) { if ((int32_t)(t - s_retry_at) >= 0) fail("broker did not answer"); return; }
        printf("ha: connected\n");
        set_err(""); s_backoff = 2000;
        s_st = HS_UP; s_disc = 0; s_disc_total = N_ENTS; s_rediscover = false;
        return;
    case HS_UP:
        break;
    }

    /* connected */
    if (s_conn < 0 || !s_cli || !mqtt_client_is_connected(s_cli)) { fail("connection to the broker lost"); return; }
    if (!s_subscribed) {
        char a[64], b[64];
        snprintf(a, sizeof a, "%s/set/#", C.base);
        snprintf(b, sizeof b, "%s/status", C.prefix);
        cyw43_arch_lwip_begin();
        err_t e1 = mqtt_sub_unsub(s_cli, a, 0, NULL, NULL, 1);
        err_t e2 = mqtt_sub_unsub(s_cli, b, 0, NULL, NULL, 1);
        cyw43_arch_lwip_end();
        if (e1 != ERR_OK || e2 != ERR_OK) return;   /* retry next pass */
        s_subscribed = true;
        s_live_sub = 0; s_live_gen = live_sources_gen();
    }
    if (s_live_gen != live_sources_gen()) {     /* sources changed: subscribe the new topics (stale ones only match nothing) */
        s_live_gen = live_sources_gen(); s_live_sub = 0;
    }
    while (s_live_sub < LV_COUNT) {             /* the live dashboard's topics, a few per pass */
        char t[112];
        live_topic(s_live_sub, C.base, t, sizeof t);
        cyw43_arch_lwip_begin();
        err_t e = mqtt_sub_unsub(s_cli, t, 0, NULL, NULL, 1);
        cyw43_arch_lwip_end();
        if (e != ERR_OK) return;                /* request slots full: next pass */
        s_live_sub++;
    }
    if (!s_online_sent) {
        if (pub(s_will, "online", 6, true) != ERR_OK) return;
        s_online_sent = true;
    }
    if (s_rediscover) { s_rediscover = false; s_disc = 0; s_state_hash = 0; s_diag_at = 0; }

    static char topic[128], pl[1400];
    for (int burst = 0; s_disc >= 0 && burst < 3; burst++) {    /* announce, a few per pass */
        if (!disc_build(s_disc, topic, sizeof topic, pl, sizeof pl)) { s_disc = -1; printf("ha: announced %d entities\n", s_disc_total); break; }
        if (pub(topic, pl, strlen(pl), true) != ERR_OK) return;  /* buffer full: next pass */
        s_disc++;
    }
    if (s_disc >= 0) return;                    /* state after discovery */

    while (s_evt != s_evh) {                    /* events */
        const char *ev = s_evq[s_evt];
        char type[32], j[160];
        size_t k = strcspn(ev, " ");
        snprintf(type, sizeof type, "%.*s", (int)(k < 31 ? k : 31), ev);
        snprintf(j, sizeof j, "{\"event_type\":\"%s\",\"detail\":\"%s\"}", type, ev[k] ? ev + k + 1 : "");
        char et[64]; snprintf(et, sizeof et, "%s/event", C.base);
        if (pub(et, j, strlen(j), false) != ERR_OK) break;
        s_evt = (s_evt + 1) % EVQ;
    }

    if (t - s_state_at >= 250) {                /* state, when it changed */
        static char st[2600];
        size_t n = local_state_json(st, sizeof st);
        uint32_t h = fnv(st, n);
        if (h != s_state_hash) {
            char tp[64]; snprintf(tp, sizeof tp, "%s/state", C.base);
            if (pub(tp, st, n, true) == ERR_OK) { s_state_hash = h; s_state_at = t; }
        } else s_state_at = t;
    }
    if (!s_diag_at || t - s_diag_at >= 60000) {
        char d[200], tp[64];
        int n = snprintf(d, sizeof d, "{\"rssi\":%d,\"uptime_s\":%lu,\"ip\":\"%s\",\"tess\":\"%s\",\"fw\":\"%s\"}",
                         net_rssi(), (unsigned long)(t / 1000), net_ip(), tesserae_state_name(), FW_VERSION);
        snprintf(tp, sizeof tp, "%s/diag", C.base);
        if (pub(tp, d, (size_t)n, true) == ERR_OK) s_diag_at = t ? t : 1;
    }
}

/* ---- incoming commands --------------------------------------------------------------- */
static int day_mask(const char *s)
{
    char b[80]; size_t n = 0;
    for (; *s && n < sizeof b - 1; s++) b[n++] = (char)tolower((unsigned char)*s);
    b[n] = 0;
    if (strstr(b, "every") || strstr(b, "daily")) return 127;
    if (strstr(b, "weekday")) return 31;
    if (strstr(b, "weekend")) return 96;
    if (strstr(b, "once") || strstr(b, "never") || !b[0]) return 0;
    static const char *const D[7] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
    int m = 0;
    for (char *tok = strtok(b, " ,;/"); tok; tok = strtok(NULL, " ,;/"))
        for (int d = 0; d < 7; d++) if (!strncmp(tok, D[d], 2) && (tok[2] == 0 || !strncmp(tok, D[d], 3))) m |= 1 << d;
    return m;
}

bool ha_translate(const char *msg, char *name, size_t nn, char *value, size_t vn)
{
    const char *bar = strchr(msg, '|');
    if (!bar) return false;
    char key[48]; snprintf(key, sizeof key, "%.*s", (int)(bar - msg), msg);
    const char *p = bar + 1;
    int v = atoi(p);
    printf("ha: %s = %s\n", key, p);

    static const char *const SET[] = { "vol", "rain_on", "rain_lvl", "rain_kind", "sleep", "lights_on", "bright", "colour",
        "ac_mode", "setpoint", "fan", "night", "keylights", "speaker", "gestures", "dash", "patter", "wash", "tone", "wind", "surface", "thunder",
        "thunder_near", "strike", "bird_vol", "chorus_preview", "alarm_stop", "alarm_snooze", "alarm_add",
        "key_col", "menu_col", "night_col", "knob_light", "auto_night", "clock12", "snooze" };
    for (size_t i = 0; i < sizeof SET / sizeof SET[0]; i++)
        if (!strcmp(key, SET[i])) { snprintf(name, nn, "set"); snprintf(value, vn, "%s|%s", key, p); return true; }

    if (!strcmp(key, "cmd_fetch"))   { snprintf(name, nn, "fetch");  value[0] = 0; return true; }
    if (!strcmp(key, "cmd_refresh")) { snprintf(name, nn, "action"); snprintf(value, vn, "refresh"); return true; }
    if (!strcmp(key, "cmd_reboot"))  { snprintf(name, nn, "reboot"); value[0] = 0; return true; }

    int i;
    if (sscanf(key, "chorus_%d", &i) == 1 && i >= 0 && i < BIRD_COUNT) {
        unsigned m = local_state()->birds;
        if (v) m |= 1u << i; else m &= ~(1u << i);
        snprintf(name, nn, "set"); snprintf(value, vn, "birds|%u", m); return true;
    }
    if (sscanf(key, "play_%d", &i) == 1 && i >= 0 && i < BIRD_COUNT) { snprintf(name, nn, "set"); snprintf(value, vn, "audition|%d", i); return true; }

    char field[12];
    if (sscanf(key, "alarm%d_%11s", &i, field) == 2) {
        if (!strcmp(field, "del")) { snprintf(name, nn, "set"); snprintf(value, vn, "alarm_del|%d", i); return true; }
        int h, m, on, mask, early;
        if (!local_alarm_get(i, &h, &m, &on, &mask, &early)) return false;
        if (!strcmp(field, "on")) on = v != 0;
        else if (!strcmp(field, "time")) { int hh, mm; if (sscanf(p, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) return false; h = hh; m = mm; }
        else if (!strcmp(field, "days")) mask = day_mask(p);
        else if (!strcmp(field, "early")) early = v;
        else return false;
        snprintf(name, nn, "set"); snprintf(value, vn, "alarm|%d,%d,%d,%d,%d,%d", i, h, m, on, mask, early);
        return true;
    }
    return false;
}

bool ha_configure(const char *v)
{
    /* enabled|host|port|user|prefix|base|password */
    char f[7][80] = { { 0 } };
    int k = 0;
    const char *s = v;
    while (k < 6) {
        const char *b = strchr(s, '|');
        if (!b) return false;
        snprintf(f[k], sizeof f[k], "%.*s", (int)(b - s), s);
        s = b + 1; k++;
    }
    snprintf(f[6], sizeof f[6], "%s", s);
    ha_cfg_t n = C;
    n.enabled = atoi(f[0]) != 0;
    snprintf(n.host, sizeof n.host, "%.63s", f[1]);
    n.port = (uint16_t)(atoi(f[2]) > 0 ? atoi(f[2]) : 1883);
    snprintf(n.user, sizeof n.user, "%.47s", f[3]);
    snprintf(n.prefix, sizeof n.prefix, "%.31s", f[4][0] ? f[4] : "homeassistant");
    snprintf(n.base, sizeof n.base, "%.31s", f[5][0] ? f[5] : "bedside");
    if (strcmp(f[6], "*")) snprintf(n.pass, sizeof n.pass, "%.63s", f[6]);   /* "*": keep the stored password */
    C = n;
    config_blob_save(2, HA_MAGIC, &C, sizeof C);
    printf("ha: settings saved (%s, %s:%u)\n", C.enabled ? "on" : "off", C.host, C.port);
    teardown(); s_st = HS_OFF; s_backoff = 2000; set_err("");
    return true;
}

size_t ha_status_json(char *b, size_t cap)
{
    const char *st = !C.enabled ? "off" : !C.host[0] ? "no broker set" : s_st == HS_UP ? (s_disc >= 0 ? "announcing" : "connected")
                   : s_st == HS_DNS ? "looking up the broker" : s_st == HS_CONNECTING ? "connecting" : s_st == HS_WAIT ? "retrying" : "starting";
    int n = snprintf(b, cap, "{\"enabled\":%d,\"host\":\"%s\",\"port\":%u,\"user\":\"%s\",\"has_pass\":%d,\"prefix\":\"%s\",\"base\":\"%s\","
                     "\"state\":\"%s\",\"error\":\"%s\",\"entities\":%d,\"announced\":%d,\"uid\":\"%s\"}",
                     C.enabled, C.host, C.port, C.user, C.pass[0] ? 1 : 0, C.prefix, C.base, st, s_err, N_ENTS,
                     s_st == HS_UP && s_disc < 0 ? N_ENTS : (s_disc > 0 ? s_disc : 0), s_uid);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/*
 * tesserae.c: always-on Tesserae client. See tesserae.h and docs/tesserae.md.
 *
 * Cycle (when due):
 *   1. no token -> discover (or register with a stored pairing code); on a
 *      claim adopt the server's device id, store the token, save config.
 *   2. GET /frame with If-None-Match (+ ?button=... on a key press).
 *      200 -> download the .bin into PSRAM, verify the size, paint, store ETag.
 *      304 -> nothing to paint. 204 -> nothing rendered yet. 401/403 -> wipe
 *      the token and re-pair.
 *   3. POST /status: telemetry + the same button event (deduped server-side);
 *      adopt next_poll_s, always_on / awake_poll_s, local_time.
 * The heartbeat follows the frame so the key's action, dispatched on the
 * frame GET, is reflected in the painted frame on the same cycle.
 */
#include "tesserae.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "config.h"
#include "net.h"
#include "net_rest.h"
#include "it8951.h"

#ifndef FW_VERSION
#define FW_VERSION "0.1.0"
#endif

#define REQ_TIMEOUT_MS      10000
#define FRAME_TIMEOUT_MS    30000
#define POLL_MIN_S          5
#define POLL_MAX_S          3600
#define DEFAULT_AWAKE_POLL  15
#define UNREACHABLE_RETRY_S 15

static uint8_t       *s_buf;            /* the frame on screen */
static uint8_t       *s_dl;             /* downloads land here, then swap */
static size_t         s_cap;
static tess_paint_fn  s_on_frame;
static void         (*s_guard)(const uint8_t *buf);
void tesserae_set_buffer_guard(void (*fn)(const uint8_t *buf)) { s_guard = fn; }

static tess_state_t   s_state;
static bool           s_have_frame;
static uint32_t       s_frames;
static uint32_t       s_next_due_ms;      /* ms-since-boot of the next cycle */
static int32_t        s_poll_s = DEFAULT_AWAKE_POLL;
static bool           s_force_full;        /* drop ETag on the next GET */
static char           s_button[16];        /* pending key press */
static uint32_t       s_button_id;
static char           s_local_time[40];
static int32_t        s_tz_offset_s;
static bool           s_tz_known;
static char           s_error[64];
static int            s_vp[4];             /* reported viewport; w 0 = whole panel */
static int            s_geom[4] = { 0, 0, IT8951_W, IT8951_H };   /* where the frame on the glass sits */

static inline uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void schedule(int32_t s)
{
    if (s < POLL_MIN_S) s = POLL_MIN_S;
    if (s > POLL_MAX_S) s = POLL_MAX_S;
    s_next_due_ms = now_ms() + (uint32_t)s * 1000u;
}

static void set_error(const char *what, int code)
{
    snprintf(s_error, sizeof s_error, "%s (%d)", what, code);
    printf("tesserae: %s\n", s_error);
}

void tesserae_init(uint8_t *frame_buf, uint8_t *dl_buf, size_t frame_cap, tess_paint_fn on_frame)
{
    s_dl = dl_buf ? dl_buf : frame_buf;
    s_buf = frame_buf; s_cap = frame_cap; s_on_frame = on_frame;
    const config_t *c = config_get();
    s_poll_s = (c->poll_s >= POLL_MIN_S) ? c->poll_s : DEFAULT_AWAKE_POLL;
    s_button_id = c->button_event_id;
    s_state = config_has_server() ? TESS_NO_WIFI : TESS_OFF;
    /* The ETag in flash describes a frame we no longer hold after a power
     * cycle: fetch in full on the first poll. */
    s_force_full = true;
    s_next_due_ms = now_ms();
}

void tesserae_kick(bool drop_etag)
{
    if (drop_etag) s_force_full = true;
    s_next_due_ms = now_ms();
}

void tesserae_button(const char *name)
{
    if (!name || !name[0]) return;
    snprintf(s_button, sizeof s_button, "%s", name);
    s_button_id++;                       /* monotonic; retries reuse the value */
    config_set_button_event_id(s_button_id);
    printf("tesserae: key \"%s\" queued (event %lu)\n", s_button, (unsigned long)s_button_id);
    tesserae_kick(false);
}

void tesserae_forget(void)
{
    config_set_device_token("");
    config_set_pairing_code("");
    config_set_frame_etag("");
    config_save();
    s_state = config_has_server() ? TESS_NO_WIFI : TESS_OFF;
    tesserae_kick(true);
}

/* ---- bootstrap ---- */

static bool bootstrap(const rest_identity_t *id)
{
    const config_t *c = config_get();
    if (c->device_token[0]) return true;

    if (c->pairing_code[0]) {
        s_state = TESS_PAIRING;
        rest_register_out_t ro;
        rest_status_t rs = rest_register(id, &ro, REQ_TIMEOUT_MS);
        if (rs == REST_OK) {
            config_set_device_token(ro.token);
            if (ro.device_id[0] && strcmp(ro.device_id, config_device_id()) != 0) {
                printf("tesserae: adopting server device id '%s' (was '%s')\n", ro.device_id, config_device_id());
                config_set_device_id(ro.device_id);
            }
            config_set_pairing_code("");
            config_save();
            printf("tesserae: registered with pairing code (id=%s)\n", config_device_id());
            return true;
        }
        if (rs == REST_UNAUTH || rs == REST_FORBIDDEN) {
            set_error("pairing code rejected; clearing it, falling back to discover", rs);
            config_set_pairing_code(""); config_save();
            schedule(UNREACHABLE_RETRY_S);
        } else if (rs == REST_RATELIMIT) {
            set_error("register rate-limited", rs);
            schedule(ro.retry_after_s > 0 ? ro.retry_after_s : 60);
        } else {
            set_error("register failed", rs);
            schedule(UNREACHABLE_RETRY_S);
            if (rs == REST_NET_ERR) s_state = TESS_ERROR;
        }
        return false;
    }

    s_state = TESS_DISCOVERING;
    rest_discover_out_t dd;
    rest_status_t ds = rest_discover(id, &dd, REQ_TIMEOUT_MS);
    if (ds == REST_OK && dd.registered) {
        config_set_device_token(dd.token);
        if (dd.device_id[0] && strcmp(dd.device_id, config_device_id()) != 0) {
            printf("tesserae: adopting server device id '%s' (was '%s')\n", dd.device_id, config_device_id());
            config_set_device_id(dd.device_id);
        }
        config_save();
        printf("tesserae: claimed token via discover (id=%s)\n", config_device_id());
        return true;
    }
    if (ds == REST_OK) {
        s_error[0] = '\0';
        printf("tesserae: discovered as '%s'; waiting for Register in Settings -> Devices (retry %ds)\n",
               config_device_id(), dd.retry_after_s);
        schedule(dd.retry_after_s > 0 ? dd.retry_after_s : 30);
    } else if (ds == REST_RATELIMIT) {
        set_error("discover rate-limited", ds);
        schedule(dd.retry_after_s > 0 ? dd.retry_after_s : 60);
    } else {
        set_error("discover failed (server URL right?)", ds);
        s_state = TESS_ERROR;
        schedule(UNREACHABLE_RETRY_S);
    }
    return false;
}

/* A frame is either the whole panel or exactly the reported viewport
 * (4 bpp, rows packed). Records where it goes. */
static bool frame_fits(size_t got)
{
    if (got == IT8951_FRAME_BYTES) { s_geom[0] = 0; s_geom[1] = 0; s_geom[2] = IT8951_W; s_geom[3] = IT8951_H; return true; }
    if (s_vp[2] > 0 && got == (size_t)s_vp[2] * (size_t)s_vp[3] / 2) { memcpy(s_geom, s_vp, sizeof s_geom); return true; }
    return false;
}

void tesserae_frame_geom(int *x, int *y, int *w, int *h) { *x = s_geom[0]; *y = s_geom[1]; *w = s_geom[2]; *h = s_geom[3]; }

void tesserae_set_viewport(int x, int y, int w, int h)
{
    if (w >= IT8951_W && h >= IT8951_H) w = h = x = y = 0;   /* the whole panel: nothing to report */
    if (x == s_vp[0] && y == s_vp[1] && w == s_vp[2] && h == s_vp[3]) return;
    s_vp[0] = x; s_vp[1] = y; s_vp[2] = w; s_vp[3] = h;
    printf("tesserae: viewport %d,%d %dx%d\n", x, y, w ? w : IT8951_W, h ? h : IT8951_H);
    tesserae_kick(true);                                     /* ask for a frame at the new size */
}

/* ---- one cycle ---- */

static void cycle(void)
{
    char mac[18];
    net_mac(mac, sizeof mac);
    rest_identity_t id = { .mac = mac, .fw_version = FW_VERSION, .panel_w = IT8951_W, .panel_h = IT8951_H };

    if (!bootstrap(&id)) return;
    s_state = TESS_READY;
    s_error[0] = '\0';

    bool cfg_dirty = false;
    const char *btn = s_button[0] ? s_button : NULL;
    bool btn_acked = false;

    /* 2. frame */
    rest_frame_out_t fo;
    bool force = s_force_full || (btn && strcmp(btn, "refresh") == 0);
    rest_status_t fs = rest_get_frame(btn, s_button_id, force, &fo, REQ_TIMEOUT_MS);
    if (fs == REST_OK || fs == REST_NOT_MODIFIED || fs == REST_NO_CONTENT) btn_acked = true;

    if (fs == REST_OK) {
        if (fo.format[0] && strcmp(fo.format, "bin") != 0) {
            set_error("server sent a non-bin frame; check the device kind / renderer", 0);
        } else if ((fo.native_w && fo.native_w != IT8951_W && fo.native_w != s_vp[2]) ||
                   (fo.native_h && fo.native_h != IT8951_H && fo.native_h != s_vp[3])) {
            set_error("frame geometry mismatch; check the panel block on the server", fo.native_w);
        } else if (!s_buf) {
            set_error("no PSRAM frame buffer", 0);
        } else {
            char url[300];
            rest_resolve_url(fo.url, url, sizeof url);
            size_t got = 0;
            if (s_guard) s_guard(s_dl);
            if (rest_download(url, s_dl, s_cap, &got, FRAME_TIMEOUT_MS)) {
                if (frame_fits(got)) {
                    uint8_t *t = s_buf; s_buf = s_dl; s_dl = t;   /* the screen may repaint mid-download */
                    s_have_frame = true;
                    s_frames++;
                    s_force_full = false;
                    config_set_frame_etag(fo.etag);
                    cfg_dirty = true;
                    if (fo.has_rotation)
                        printf("tesserae: rotation %s step %d/%d (%s)\n", fo.rotation_id,
                               fo.step_index + 1, fo.step_count, fo.step_page_id);
                    if (s_on_frame) s_on_frame();
                } else {
                    set_error("frame size wrong (want the panel or the viewport)", (int)got);
                }
            } else {
                set_error("frame download failed", 0);
            }
        }
    } else if (fs == REST_NOT_MODIFIED) {
        s_force_full = false;
        if (!s_have_frame && fo.content_location[0]) {
            /* Booted with a stored ETag but no pixels: the spec puts the URL in
             * Content-Location on 304 for exactly this case. */
            size_t got = 0;
            if (s_guard) s_guard(s_dl);
            if (s_buf && rest_download(fo.content_location, s_dl, s_cap, &got, FRAME_TIMEOUT_MS) &&
                frame_fits(got)) {
                uint8_t *t = s_buf; s_buf = s_dl; s_dl = t;
                s_have_frame = true; s_frames++;
                if (s_on_frame) s_on_frame();
            }
        }
    } else if (fs == REST_NO_CONTENT) {
        printf("tesserae: no frame rendered yet; bind a dashboard to '%s'\n", config_device_id());
    } else if (fs == REST_UNAUTH || fs == REST_FORBIDDEN) {
        set_error("token rejected; re-pairing", fs);
        config_set_device_token(""); cfg_dirty = true;
    } else {
        set_error("frame request failed", fs);
        if (fs == REST_NET_ERR) s_state = TESS_ERROR;
    }

    /* 3. heartbeat (only while we still hold a token) */
    if (config_get()->device_token[0]) {
        rest_telemetry_t t = {
            .rssi = net_rssi(), .ip = net_ip(),
            .power_plugged = true,          /* PowerBoost mains sense lands in Phase 2 (power) */
            .battery_mv = 0, .battery_pct = -1,
            .next_sleep_s = s_poll_s,
            .button = btn, .button_event_id = s_button_id,
            .vp_x = s_vp[0], .vp_y = s_vp[1], .vp_w = s_vp[2], .vp_h = s_vp[3],
        };
        rest_status_out_t so;
        rest_status_t ss = rest_post_status(&id, &t, &so, REQ_TIMEOUT_MS);
        if (ss == REST_OK) {
            btn_acked = true;
            if (so.local_time[0]) snprintf(s_local_time, sizeof s_local_time, "%s", so.local_time);
            if (so.tz_offset_s || so.local_time[0]) { s_tz_offset_s = so.tz_offset_s; s_tz_known = so.tz_offset_s != 0; }
            /* Cadence: the server's next_poll_s wins (it already folds in
             * awake_poll_s when always_on is set, and pulls the poll earlier for
             * projected content changes). Fall back to awake_poll_s, then keep
             * the last value. */
            int32_t next = so.next_poll_s > 0 ? so.next_poll_s
                         : (so.awake_poll_s > 0 ? so.awake_poll_s : s_poll_s);
            if (so.always_on == 0 && so.awake_poll_s < 0 && so.next_poll_s <= 0 && so.sleep_interval_s > 0)
                next = so.sleep_interval_s;
            if (next != s_poll_s) { s_poll_s = next; config_set_poll_s(next); cfg_dirty = true; }
            if (so.always_on == 0)
                printf("tesserae: note: 'Stay awake' is off on the server; this device never sleeps, "
                       "it just polls every %lds\n", (long)s_poll_s);
        } else if (ss == REST_UNAUTH || ss == REST_FORBIDDEN) {
            set_error("status rejected; re-pairing", ss);
            config_set_device_token(""); cfg_dirty = true;
        } else {
            set_error("status post failed", ss);
            if (ss == REST_NET_ERR) s_state = TESS_ERROR;
        }
    }

    if (btn && btn_acked) s_button[0] = '\0';     /* one press, one event */
    if (cfg_dirty) printf(config_save() ? "config: saved\n" : "config: SAVE FAILED\n");

    schedule(s_state == TESS_ERROR ? UNREACHABLE_RETRY_S : s_poll_s);
}

void tesserae_poll(void)
{
    if (!config_has_server()) { s_state = TESS_OFF; return; }
    if (!net_connected()) { s_state = TESS_NO_WIFI; return; }
    if (s_state == TESS_NO_WIFI || s_state == TESS_OFF) { s_state = TESS_DISCOVERING; s_next_due_ms = now_ms(); }
    if ((int32_t)(now_ms() - s_next_due_ms) < 0) return;
    cycle();
}

tess_state_t tesserae_state(void) { return s_state; }

const char *tesserae_state_name(void)
{
    switch (s_state) {
    case TESS_OFF:         return "off (no server URL)";
    case TESS_NO_WIFI:     return "waiting for WiFi";
    case TESS_DISCOVERING: return "discovered: click Register in Tesserae";
    case TESS_PAIRING:     return "pairing";
    case TESS_READY:       return "paired";
    default:               return "server unreachable";
    }
}

const uint8_t *tesserae_frame(void)      { return s_have_frame ? s_buf : NULL; }
const char    *tesserae_frame_etag(void) { return config_get()->last_frame_etag; }
int32_t        tesserae_poll_s(void)     { return s_poll_s; }
const char    *tesserae_local_time(void) { return s_local_time; }

/* The server's UTC offset, from the status reply, else from the "+hh:mm" tail
 * of its local time. */
int32_t tesserae_tz_offset_s(void)
{
    if (s_tz_known) return s_tz_offset_s;
    const char *t = strchr(s_local_time, 'T');
    const char *z = t ? strpbrk(t, "+-Z") : NULL;
    int hh = 0, mm = 0;
    if (z && *z != 'Z' && sscanf(z + 1, "%d:%d", &hh, &mm) >= 1) return (z[0] == '-' ? -1 : 1) * (hh * 3600 + mm * 60);
    return 0;
}
const char    *tesserae_last_error(void) { return s_error; }
uint32_t       tesserae_frames_painted(void) { return s_frames; }

uint32_t tesserae_next_poll_in_s(void)
{
    int32_t d = (int32_t)(s_next_due_ms - now_ms());
    return d > 0 ? (uint32_t)d / 1000u : 0;
}

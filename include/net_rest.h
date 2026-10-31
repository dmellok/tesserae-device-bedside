/*
 * net_rest.h: Tesserae v1 REST device API client.
 *
 * The protocol is docs/tesserae.md (distilled from the server's
 * docs/dev/client-protocol.md). Surface used here:
 *
 *   POST /api/v1/device/discover          zero-touch announce, token by MAC match
 *   POST /api/v1/device/register          6-digit pairing code (X-Pairing-Code)
 *   GET  /api/v1/device/<id>/frame        envelope {url, format, render_id, ...};
 *                                         If-None-Match -> 304; 204 = nothing yet;
 *                                         ?button=<name>&button_event_id=<n> on a key press
 *   POST /api/v1/device/<id>/status       heartbeat; reply carries config,
 *                                         next_poll_s, server_time, local_time
 *   GET  <frame url>                      the 1,314,144-byte 4bpp frame, no auth
 *
 * One synchronous HTTP/1.1 request at a time on lwIP's raw TCP API (the SDK's
 * httpc app cannot set headers or expose the status line). Small responses are
 * parsed from a static buffer; a body sink streams large ones (the frame) into
 * PSRAM as the segments arrive. lwIP runs threadsafe-background here, so the
 * main-thread calls are wrapped in cyw43_arch_lwip_begin/end and the callbacks
 * already run under the lock.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    REST_OK = 0,          /* 200 / 201 */
    REST_NOT_MODIFIED,    /* 304: frame unchanged */
    REST_NO_CONTENT,      /* 204: nothing rendered for this device yet */
    REST_UNAUTH,          /* 401: token missing / revoked */
    REST_FORBIDDEN,       /* 403: token for another device, bad pairing code */
    REST_RATELIMIT,       /* 429: retry_after_s populated */
    REST_HTTP_ERR,        /* other 4xx / 5xx, or a malformed body */
    REST_NET_ERR,         /* DNS / TCP / timeout */
} rest_status_t;

/* What the firmware says about itself on discover / register / status. */
typedef struct {
    const char *mac;            /* "aa:bb:cc:dd:ee:ff" (real, never a placeholder) */
    const char *fw_version;
    uint16_t    panel_w, panel_h;
} rest_identity_t;

typedef struct {
    bool     registered;        /* admin has clicked Register: token present */
    char     token[256];
    char     device_id[33];     /* canonical id (MAC match); adopt it */
    bool     device_id_changed;
    int32_t  sleep_interval_s;  /* config.sleep_interval_s, -1 if absent */
    uint32_t server_time;       /* unix seconds, 0 if absent */
    int      retry_after_s;     /* next discover, seconds */
} rest_discover_out_t;

typedef struct {
    char     token[256];
    char     device_id[33];
    int32_t  sleep_interval_s;
    uint32_t server_time;
    int      retry_after_s;     /* set on REST_RATELIMIT */
} rest_register_out_t;

typedef struct {
    char     url[256];          /* frame URL (may be relative to the server) */
    char     format[16];        /* "bin" expected */
    uint16_t panel_w, panel_h;  /* composition dims */
    uint16_t native_w, native_h;/* framebuffer dims when the server knows them */
    char     etag[80];          /* render_id; also set on 304 */
    char     content_location[256]; /* absolute frame URL; also set on 304 */
    /* rotation block, when bound to one */
    bool     has_rotation;
    char     rotation_id[41];
    char     step_page_id[65];
    int      step_index, step_count;
} rest_frame_out_t;

typedef struct {
    int32_t  next_poll_s;       /* seconds until the next status/frame poll, -1 if absent */
    int32_t  sleep_interval_s;  /* config, -1 if absent */
    int      always_on;         /* config.always_on: 0/1, -1 if absent */
    int32_t  awake_poll_s;      /* config.awake_poll_s, -1 if absent */
    uint32_t server_time;       /* unix seconds, 0 if absent */
    char     local_time[40];    /* ISO 8601 with offset, for the clock overlay */
    char     tz[48];
    int32_t  tz_offset_s;
    int      retry_after_s;
} rest_status_out_t;

/* Device-side readings for the heartbeat. power_plugged is the mains-sense
 * line; battery_mv 0 = unknown (field omitted). */
typedef struct {
    int         rssi;
    const char *ip;
    bool        power_plugged;
    uint32_t    battery_mv;
    int         battery_pct;        /* -1 = unknown */
    int32_t     next_sleep_s;       /* our own poll cadence, informational */
    /* Pending key press to report (name NULL = none). Same event on /frame and
     * /status is deduped server-side by button_event_id. */
    const char *button;
    uint32_t    button_event_id;
    /* The part of the panel the dashboard owns once the device's overlay is
     * cut out (docs/tesserae.md, "viewport"). w 0 = the whole panel. */
    int         vp_x, vp_y, vp_w, vp_h;
} rest_telemetry_t;

/* POST /discover (no auth). REST_OK: inspect out->registered. */
rest_status_t rest_discover(const rest_identity_t *id, rest_discover_out_t *out, uint32_t timeout_ms);

/* POST /register with the stored pairing code. Idempotent server-side. */
rest_status_t rest_register(const rest_identity_t *id, rest_register_out_t *out, uint32_t timeout_ms);

/* GET /frame with Bearer + If-None-Match (from config.last_frame_etag unless
 * force is set, which drops it so a refresh key always gets a 200). button may
 * be NULL. */
rest_status_t rest_get_frame(const char *button, uint32_t button_event_id, bool force,
                             rest_frame_out_t *out, uint32_t timeout_ms);

/* POST /status with Bearer. */
rest_status_t rest_post_status(const rest_identity_t *id, const rest_telemetry_t *t,
                               rest_status_out_t *out, uint32_t timeout_ms);

/* GET an arbitrary http:// URL, streaming the body into dst (cap bytes). True
 * only on a 2xx whose body fit; *out_len = bytes received. Used for the frame. */
bool rest_download(const char *url, uint8_t *dst, size_t cap, size_t *out_len, uint32_t timeout_ms);

/* Resolve a possibly-relative URL against the server origin in config. */
void rest_resolve_url(const char *u, char *out, size_t cap);

/* Wall clock: set from every response's Date header (no SNTP needed on a LAN).
 * 0 until the first response. */
uint32_t rest_epoch_now(void);

/* Called repeatedly while a request waits on the network, so the caller can
 * keep servicing input and repaints. NULL = plain sleep. */
void rest_set_idle_hook(void (*fn)(void));

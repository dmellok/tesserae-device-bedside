/*
 * net_rest.c: Tesserae REST client on lwIP raw TCP. See net_rest.h.
 *
 * Ported from tesserae-device-pico-bin (same board, same lwIP arch) and
 * extended for this device: a streaming body sink for the frame download, the
 * identity fields the gray_16 panel needs (gamut, name, can_stay_awake), button
 * wakes, the always-on config block, and Content-Location on 304.
 */
#include "net_rest.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "tls_roots.h"
#include "pico/rand.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#include "config.h"
#include "cJSON.h"

#define TESSERAE_GAMUT  "gray_16"

#define REST_RX_MAX   6144          /* status line + headers (+ small JSON body) */
#define REST_REQ_MAX  2048          /* GitHub's signed asset URLs are long */

/* ---- in-flight request state (single request at a time) ---- */
static char            s_req[REST_REQ_MAX];
static size_t          s_req_len, s_req_sent;

static char            s_rx[REST_RX_MAX + 1];
static volatile size_t s_rx_len;
static volatile bool   s_overflow;

/* Body sink: once the header block is complete, body bytes go here instead. */
static uint8_t        *s_sink;
static size_t          s_sink_cap;
static volatile size_t s_sink_len;
static volatile bool   s_sink_overflow;
static volatile bool   s_hdr_done;      /* "\r\n\r\n" seen */
static size_t          s_body_off;      /* offset of the body within s_rx */
/* Transfer-Encoding: chunked (GitHub's API, now and then): the sink gets the
 * payload only, decoded as it streams; a small body is decoded in place. */
#include "http_chunked.h"
static bool            s_chunked;
static chunked_t       s_ch;
static bool header_value(const char *name, char *out, size_t cap);

static struct altcp_pcb *s_pcb;
static struct altcp_tls_config *s_tls;    /* made on the first https:// request */

/* mbedTLS's millisecond clock: time since boot. */
#include "mbedtls/platform_time.h"
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)to_ms_since_boot(get_absolute_time()); }

/* mbedTLS entropy: the RP2350's random number generator. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    for (size_t i = 0; i < len; i += 8) { uint64_t r = get_rand_64(); memcpy(output + i, &r, len - i < 8 ? len - i : 8); }
    *olen = len;
    return 0;
}
static volatile bool   s_pcb_dead;
static volatile bool   s_done;
static volatile bool   s_err;

static ip_addr_t       s_ip;
static volatile int    s_dns;           /* 0 pending, 1 ok, 2 fail */

/* Wall clock from Date headers: epoch at `s_epoch_ref_ms` since boot. */
static uint32_t s_epoch_at_ref;
static uint32_t s_epoch_ref_ms;

static inline uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void (*s_idle)(void);
void rest_set_idle_hook(void (*fn)(void)) { s_idle = fn; }
/* Wait up to `ms`, servicing the UI meanwhile; returns the time actually spent. */
static uint32_t idle_wait(uint32_t ms)
{
    uint32_t t0 = now_ms();
    if (s_idle) s_idle();
    uint32_t spent = now_ms() - t0;
    if (spent < ms) { sleep_ms(ms - spent); spent = ms; }
    return spent;
}

uint32_t rest_epoch_now(void)
{
    if (!s_epoch_at_ref) return 0;
    return s_epoch_at_ref + (now_ms() - s_epoch_ref_ms) / 1000u;
}

/* ---- raw TCP callbacks (run under the lwIP lock) ---- */

static void try_send(struct altcp_pcb *pcb)
{
    while (s_req_sent < s_req_len) {
        u16_t avail = altcp_sndbuf(pcb);
        if (avail == 0) break;
        size_t remain = s_req_len - s_req_sent;
        u16_t n = (remain < avail) ? (u16_t)remain : avail;
        if (altcp_write(pcb, s_req + s_req_sent, n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        s_req_sent += n;
    }
    altcp_output(pcb);
}

static err_t sent_cb(void *arg, struct altcp_pcb *pcb, u16_t len)
{
    (void)arg; (void)len;
    try_send(pcb);
    return ERR_OK;
}

/* Append received bytes: headers (and any small body) into s_rx; once the
 * header block is complete and a sink is set, the rest streams into the sink. */
/* Body bytes for the sink, de-chunked when the response is chunked (the payload
 * is never longer than the wire bytes, so the room check is on n). */
static void sink_put(const uint8_t *d, size_t n)
{
    if (s_sink_len + n > s_sink_cap) { s_sink_overflow = true; return; }
    if (s_chunked) s_sink_len += chunked_feed(&s_ch, d, n, s_sink + s_sink_len);
    else { memcpy(s_sink + s_sink_len, d, n); s_sink_len += n; }
}

static void absorb(const uint8_t *d, size_t n)
{
    if (s_sink && s_hdr_done) { sink_put(d, n); return; }
    size_t room = REST_RX_MAX - s_rx_len;
    size_t take = n < room ? n : room;
    if (take) { memcpy(s_rx + s_rx_len, d, take); s_rx_len += take; }
    if (take < n && !s_sink) s_overflow = true;

    if (s_sink && !s_hdr_done) {
        s_rx[s_rx_len] = '\0';
        char *end = strstr(s_rx, "\r\n\r\n");
        if (end) {
            s_hdr_done = true;
            s_body_off = (size_t)(end + 4 - s_rx);
            char te[32];
            s_chunked = header_value("Transfer-Encoding", te, sizeof te) && strcasecmp(te, "chunked") == 0;
            if (s_chunked) chunked_init(&s_ch);
            /* Move whatever body already landed in s_rx over to the sink. */
            size_t spill = s_rx_len - s_body_off;
            if (spill) { sink_put((const uint8_t *)s_rx + s_body_off, spill); s_rx_len = s_body_off; }
            /* And the part of this segment we could not fit in s_rx. */
            if (take < n) sink_put(d + take, n - take);
        } else if (take < n) {
            s_overflow = true;      /* header block larger than REST_RX_MAX */
        }
    }
}

static err_t recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (p == NULL) { s_done = true; return ERR_OK; }   /* server closed: complete */
    if (err == ERR_OK) {
        for (struct pbuf *q = p; q != NULL; q = q->next) absorb((const uint8_t *)q->payload, q->len);
        altcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err)
{
    (void)arg; (void)err;
    s_pcb = NULL;          /* lwIP has freed it */
    s_pcb_dead = true;
    s_err = true;
    s_done = true;
}

static err_t connected_cb(void *arg, struct altcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) { s_err = true; s_done = true; return ERR_OK; }
    s_req_sent = 0;
    try_send(pcb);
    return ERR_OK;
}

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name; (void)arg;
    if (ipaddr) { s_ip = *ipaddr; s_dns = 1; }
    else        { s_dns = 2; }
}

/* ---- response parsing ---- */

static int parse_status(void)
{
    if (s_rx_len < 12 || strncmp(s_rx, "HTTP/", 5) != 0) return 0;
    const char *sp = memchr(s_rx, ' ', s_rx_len);
    return sp ? atoi(sp + 1) : 0;
}

/* Case-insensitive header lookup over the header block; trimmed value -> out. */
static bool header_value(const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    const char *end = strstr(s_rx, "\r\n\r\n");
    const char *hdr_end = end ? end : (s_rx + s_rx_len);
    size_t nlen = strlen(name);

    const char *nl = memchr(s_rx, '\n', (size_t)(hdr_end - s_rx));
    const char *line = nl ? nl + 1 : hdr_end;
    while (line < hdr_end) {
        const char *eol = memchr(line, '\n', (size_t)(hdr_end - line));
        const char *lend = eol ? eol : hdr_end;
        if ((size_t)(lend - line) > nlen &&
            strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            while (v < lend && (*v == ' ' || *v == '\t')) v++;
            size_t n = 0;
            while (v < lend && *v != '\r' && *v != '\n' && n < cap - 1) out[n++] = *v++;
            out[n] = '\0';
            return true;
        }
        if (!eol) break;
        line = eol + 1;
    }
    return false;
}

static void unquote(char *s)
{
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { memmove(s, s + 1, len - 2); s[len - 2] = '\0'; }
}

/* Days since the Unix epoch for a civil date (Howard Hinnant's algorithm). */
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097L + (long)doe - 719468L;
}

/* RFC 1123 "Sun, 06 Nov 1994 08:49:37 GMT" -> epoch, 0 if implausible. */
static uint32_t parse_http_date(const char *v)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *comma = strchr(v, ',');
    const char *p = comma ? comma + 1 : v;
    while (*p == ' ') p++;
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0; char mon[4] = {0};
    if (sscanf(p, "%d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return 0;
    const char *mp = strstr(months, mon);
    if (!mp || y < 2020 || y > 2100) return 0;
    unsigned m = (unsigned)((mp - months) / 3) + 1u;
    long long e = (long long)days_from_civil(y, m, (unsigned)d) * 86400LL + hh * 3600 + mm * 60 + ss;
    return (e > 1500000000LL) ? (uint32_t)e : 0;
}

static rest_status_t map_status(int http)
{
    switch (http) {
        case 200: case 201: return REST_OK;
        case 204:           return REST_NO_CONTENT;
        case 304:           return REST_NOT_MODIFIED;
        case 401:           return REST_UNAUTH;
        case 403:           return REST_FORBIDDEN;
        case 429:           return REST_RATELIMIT;
        default:            return REST_HTTP_ERR;
    }
}

/* ---- core request ----
 * headers[] are complete header lines without CRLF. A non-NULL body is sent as
 * application/json. With a sink, the body streams there and *body_out is NULL;
 * otherwise *body_out points into the static rx buffer (valid until the next
 * call). Returns the mapped status; *http_out gets the raw code. */
static rest_status_t do_request(const char *method, const char *url,
                                const char *const *headers, int nh, const char *body,
                                uint8_t *sink, size_t sink_cap,
                                const char **body_out, int *http_out, uint32_t timeout_ms)
{
    if (body_out) *body_out = NULL;
    if (http_out) *http_out = 0;

    /* http://host[:port]/path */
    bool tls = strncmp(url, "https://", 8) == 0;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    const char *colon = memchr(p, ':', slash ? (size_t)(slash - p) : strlen(p));
    char host[128];
    size_t hlen = colon ? (size_t)(colon - p) : (slash ? (size_t)(slash - p) : strlen(p));
    if (hlen >= sizeof host) hlen = sizeof host - 1;
    memcpy(host, p, hlen); host[hlen] = '\0';
    uint16_t port = tls ? 443 : 80;
    if (colon) { int v = atoi(colon + 1); if (v > 0 && v < 65536) port = (uint16_t)v; }
    const char *path = slash ? slash : "/";

    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(s_req, sizeof s_req,
                     "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nUser-Agent: tesserae-device-bedside\r\n",
                     method, path, host, port);
    for (int i = 0; i < nh && n > 0 && n < (int)sizeof s_req; i++)
        n += snprintf(s_req + n, sizeof s_req - n, "%s\r\n", headers[i]);
    if (body && n > 0 && n < (int)sizeof s_req)
        n += snprintf(s_req + n, sizeof s_req - n,
                      "Content-Type: application/json\r\nContent-Length: %u\r\n", (unsigned)blen);
    if (n > 0 && n < (int)sizeof s_req) n += snprintf(s_req + n, sizeof s_req - n, "\r\n");
    if (n <= 0 || n >= (int)sizeof s_req - (int)blen) { printf("rest: request too large\n"); return REST_NET_ERR; }
    if (body) { memcpy(s_req + n, body, blen); n += (int)blen; }
    s_req_len = (size_t)n;

    s_rx_len = 0; s_overflow = false; s_done = false; s_err = false;
    s_pcb_dead = false; s_dns = 0;
    s_sink = sink; s_sink_cap = sink_cap; s_sink_len = 0; s_sink_overflow = false;
    s_hdr_done = false; s_body_off = 0;
    s_chunked = false; chunked_init(&s_ch);

    cyw43_arch_lwip_begin();
    err_t de = dns_gethostbyname(host, &s_ip, dns_cb, NULL);
    cyw43_arch_lwip_end();
    if (de == ERR_OK) s_dns = 1;
    else if (de != ERR_INPROGRESS) { printf("rest: dns start failed (%d)\n", de); return REST_NET_ERR; }
    uint32_t t = 0;
    while (s_dns == 0 && t < timeout_ms) { t += idle_wait(10); }
    if (s_dns != 1) { printf("rest: dns failed for %s\n", host); return REST_NET_ERR; }

    cyw43_arch_lwip_begin();
    if (tls && !s_tls) s_tls = altcp_tls_create_config_client((const u8_t *)tls_roots_pem, tls_roots_pem_len);
    if (tls && !s_tls) { cyw43_arch_lwip_end(); printf("rest: TLS setup failed\n"); return REST_NET_ERR; }
    s_pcb = tls ? altcp_tls_new(s_tls, IP_GET_TYPE(&s_ip)) : altcp_tcp_new_ip_type(IP_GET_TYPE(&s_ip));
    if (s_pcb && tls) mbedtls_ssl_set_hostname((mbedtls_ssl_context *)altcp_tls_context(s_pcb), host);   /* SNI, and the name the certificate must carry */
    if (s_pcb) {
        altcp_err(s_pcb, err_cb);
        altcp_recv(s_pcb, recv_cb);
        altcp_sent(s_pcb, sent_cb);
        if (altcp_connect(s_pcb, &s_ip, port, connected_cb) != ERR_OK) {
            altcp_abort(s_pcb); s_pcb = NULL; s_pcb_dead = true;
        }
    }
    cyw43_arch_lwip_end();
    if (s_pcb == NULL) { printf("rest: connect failed\n"); return REST_NET_ERR; }

    printf("rest: %s %s:%u%s\n", method, host, port, path);
    while (!s_done && t < timeout_ms) { t += idle_wait(5); }

    cyw43_arch_lwip_begin();
    if (s_pcb && !s_pcb_dead) {
        altcp_recv(s_pcb, NULL); altcp_sent(s_pcb, NULL); altcp_err(s_pcb, NULL);
        if (altcp_close(s_pcb) != ERR_OK) altcp_abort(s_pcb);
        s_pcb = NULL;
    }
    cyw43_arch_lwip_end();
    s_sink = NULL;

    if (!s_done) { printf("rest: timeout after %ums\n", (unsigned)t); return REST_NET_ERR; }
    if (s_err || s_rx_len == 0) { printf("rest: transport error\n"); return REST_NET_ERR; }
    if (s_overflow) printf("rest: response truncated at %d bytes\n", REST_RX_MAX);

    s_rx[s_rx_len] = '\0';
    int http = parse_status();
    if (http_out) *http_out = http;

    char date[48];
    if (header_value("Date", date, sizeof date)) {
        uint32_t e = parse_http_date(date);
        if (e) { s_epoch_at_ref = e; s_epoch_ref_ms = now_ms(); }
    }

    const char *bend = strstr(s_rx, "\r\n\r\n");
    bool chunked = s_chunked;
    if (bend && !sink) {
        /* A small chunked body is decoded where it landed (payload <= wire bytes). */
        char te[32];
        if (header_value("Transfer-Encoding", te, sizeof te) && strcasecmp(te, "chunked") == 0) {
            chunked_t c; chunked_init(&c);
            uint8_t *b = (uint8_t *)(bend + 4);
            size_t boff = (size_t)(b - (uint8_t *)s_rx);
            s_rx_len = boff + chunked_feed(&c, b, s_rx_len - boff, b);
            s_rx[s_rx_len] = '\0';
            chunked = true;
        }
        if (body_out) *body_out = bend + 4;
    }
    printf("rest: <- %d (%u hdr+body bytes%s%s)\n", http, (unsigned)s_rx_len,
           sink ? ", body to sink" : "", chunked ? ", chunked" : "");
    return map_status(http);
}

/* ---- JSON helpers ---- */

static void json_get_str(const cJSON *o, const char *k, char *out, size_t cap)
{
    out[0] = '\0';
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) snprintf(out, cap, "%s", v->valuestring);
}

static int32_t json_get_int(const cJSON *o, const char *k, int32_t dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int32_t)v->valuedouble : dflt;
}

static int json_get_bool(const cJSON *o, const char *k, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsTrue(v)) return 1;
    if (cJSON_IsFalse(v)) return 0;
    return dflt;
}

static int retry_after_hdr(void)
{
    char ra[16];
    return header_value("Retry-After", ra, sizeof ra) ? atoi(ra) : 0;
}

/* ---- identity ----
 * Shared by /discover and /register. `capabilities` adds the sticky blocks
 * (register and status carry them; discover stays identity-only). */
static char *identity_body(const rest_identity_t *id, bool capabilities)
{
    const config_t *c = config_get();
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "device_id", config_device_id());
    cJSON_AddStringToObject(o, "kind", config_kind());
    cJSON_AddNumberToObject(o, "panel_w", id->panel_w);
    cJSON_AddNumberToObject(o, "panel_h", id->panel_h);
    /* panel_w/h ARE the framebuffer (landscape-native glass): rotation 0. */
    cJSON_AddNumberToObject(o, "rotation", 0);
    cJSON_AddStringToObject(o, "gamut", TESSERAE_GAMUT);
    if (c->device_name[0]) cJSON_AddStringToObject(o, "name", c->device_name);
    cJSON_AddStringToObject(o, "fw_version", id->fw_version);
    cJSON_AddStringToObject(o, "mac", id->mac ? id->mac : "");
    if (capabilities) {
        /* Mains powered (PowerBoost load-sharing): offer the always-on setting. */
        cJSON_AddBoolToObject(o, "can_stay_awake", true);
    }
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return body;
}

rest_status_t rest_discover(const rest_identity_t *id, rest_discover_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    out->sleep_interval_s = -1;
    out->retry_after_s = 30;

    char url[200];
    snprintf(url, sizeof url, "%s/api/v1/device/discover", config_get()->server_url);
    char *body = identity_body(id, false);
    if (!body) return REST_NET_ERR;

    const char *rbody = NULL;
    rest_status_t st = do_request("POST", url, NULL, 0, body, NULL, 0, &rbody, NULL, timeout_ms);
    cJSON_free(body);      /* cJSON's allocator (the PSRAM heap when there is one), never libc's */
    if (st == REST_RATELIMIT) { int ra = retry_after_hdr(); if (ra > 0) out->retry_after_s = ra; }
    if (st != REST_OK || !rbody) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_HTTP_ERR;
    out->registered  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "registered"));
    out->server_time = (uint32_t)json_get_int(r, "server_time", 0);
    int32_t ra = json_get_int(r, "retry_after_s", 30);
    out->retry_after_s = (ra > 0) ? (int)ra : 30;
    if (out->registered) {
        json_get_str(r, "device_token", out->token, sizeof out->token);
        json_get_str(r, "device_id", out->device_id, sizeof out->device_id);
        out->device_id_changed = json_get_bool(r, "device_id_changed", 0) == 1;
        cJSON *cfg = cJSON_GetObjectItemCaseSensitive(r, "config");
        if (cfg) out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
    }
    cJSON_Delete(r);
    if (out->registered && out->token[0] == '\0') return REST_HTTP_ERR;
    return REST_OK;
}

rest_status_t rest_register(const rest_identity_t *id, rest_register_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    out->sleep_interval_s = -1;

    char url[200];
    snprintf(url, sizeof url, "%s/api/v1/device/register", config_get()->server_url);
    char *body = identity_body(id, true);
    if (!body) return REST_NET_ERR;

    char pc[64];
    snprintf(pc, sizeof pc, "X-Pairing-Code: %s", config_get()->pairing_code);
    const char *hdrs[] = { pc };

    const char *rbody = NULL;
    rest_status_t st = do_request("POST", url, hdrs, 1, body, NULL, 0, &rbody, NULL, timeout_ms);
    cJSON_free(body);      /* cJSON's allocator (the PSRAM heap when there is one), never libc's */
    if (st == REST_RATELIMIT) out->retry_after_s = retry_after_hdr();
    if (st != REST_OK || !rbody) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_HTTP_ERR;
    json_get_str(r, "device_token", out->token, sizeof out->token);
    json_get_str(r, "device_id", out->device_id, sizeof out->device_id);
    out->server_time = (uint32_t)json_get_int(r, "server_time", 0);
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(r, "config");
    if (cfg) out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
    cJSON_Delete(r);
    return out->token[0] ? REST_OK : REST_HTTP_ERR;
}

static void parse_rotation(const cJSON *r, rest_frame_out_t *out)
{
    const cJSON *rot = cJSON_GetObjectItemCaseSensitive(r, "rotation");
    if (!cJSON_IsObject(rot)) return;
    out->has_rotation = true;
    json_get_str(rot, "rotation_id", out->rotation_id, sizeof out->rotation_id);
    json_get_str(rot, "step_page_id", out->step_page_id, sizeof out->step_page_id);
    out->step_index = json_get_int(rot, "step_index", 0);
    out->step_count = json_get_int(rot, "step_count", 0);
}

rest_status_t rest_get_frame(const char *button, const char *page, uint32_t button_event_id, bool force,
                             rest_frame_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    const config_t *c = config_get();
    char url[320];
    int un = snprintf(url, sizeof url, "%s/api/v1/device/%s/frame", c->server_url, config_device_id());
    if (page && page[0] && un > 0 && un < (int)sizeof url) {
        /* A dashboard pick: the id is opaque, so percent-encode anything unusual. */
        un += snprintf(url + un, sizeof url - un, "?page=");
        for (const char *p = page; *p && un < (int)sizeof url - 4; p++) {
            unsigned char ch = (unsigned char)*p;
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') url[un++] = (char)ch;
            else un += snprintf(url + un, sizeof url - un, "%%%02X", ch);
        }
        url[un] = '\0';
    } else if (button && button[0] && un > 0 && un < (int)sizeof url)
        snprintf(url + un, sizeof url - un, "?button=%s&button_event_id=%lu", button, (unsigned long)button_event_id);

    char auth[300], inm[128];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->device_token);
    const char *hdrs[2] = { auth, NULL };
    int nh = 1;
    if (c->last_frame_etag[0] && !force) {
        snprintf(inm, sizeof inm, "If-None-Match: \"%s\"", c->last_frame_etag);
        hdrs[1] = inm; nh = 2;
    }

    const char *rbody = NULL;
    rest_status_t st = do_request("GET", url, hdrs, nh, NULL, NULL, 0, &rbody, NULL, timeout_ms);
    if (st == REST_NET_ERR) return st;          /* s_rx holds nothing from this request */
    /* ETag and Content-Location are on 200 and 304 alike. */
    if (header_value("ETag", out->etag, sizeof out->etag)) unquote(out->etag);
    header_value("Content-Location", out->content_location, sizeof out->content_location);
    if (st != REST_OK || !rbody) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_HTTP_ERR;
    json_get_str(r, "url", out->url, sizeof out->url);
    json_get_str(r, "format", out->format, sizeof out->format);
    out->panel_w  = (uint16_t)json_get_int(r, "panel_w", 0);
    out->panel_h  = (uint16_t)json_get_int(r, "panel_h", 0);
    out->native_w = (uint16_t)json_get_int(r, "native_w", 0);
    out->native_h = (uint16_t)json_get_int(r, "native_h", 0);
    if (!out->etag[0]) json_get_str(r, "render_id", out->etag, sizeof out->etag);
    parse_rotation(r, out);
    cJSON_Delete(r);
    return out->url[0] ? REST_OK : REST_HTTP_ERR;
}

rest_status_t rest_post_status(const rest_identity_t *id, const rest_telemetry_t *t,
                               rest_status_out_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof *out);
    out->next_poll_s = -1; out->sleep_interval_s = -1; out->always_on = -1; out->awake_poll_s = -1;

    const config_t *c = config_get();
    char url[256];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/status", c->server_url, config_device_id());

    cJSON *o = cJSON_CreateObject();
    if (!o) return REST_NET_ERR;
    cJSON_AddStringToObject(o, "fw_version", id->fw_version);
    cJSON_AddStringToObject(o, "model", "Pico Plus 2 W + Waveshare 10.3\" IT8951");
    cJSON_AddNumberToObject(o, "rssi", t->rssi);
    cJSON_AddStringToObject(o, "ip", t->ip ? t->ip : "");
    cJSON_AddStringToObject(o, "mac", id->mac ? id->mac : "");
    cJSON_AddNumberToObject(o, "panel_w", id->panel_w);
    cJSON_AddNumberToObject(o, "panel_h", id->panel_h);
    cJSON_AddBoolToObject(o, "power_plugged", t->power_plugged);
    if (t->battery_mv) cJSON_AddNumberToObject(o, "battery_mv", t->battery_mv);
    if (t->battery_pct >= 0) cJSON_AddNumberToObject(o, "battery_pct", t->battery_pct);
    if (t->next_sleep_s > 0) cJSON_AddNumberToObject(o, "next_sleep_s", t->next_sleep_s);
    cJSON_AddBoolToObject(o, "can_stay_awake", true);   /* re-sent every beat: explicit, never omitted */
    if (t->vp_w > 0 && t->vp_h > 0) {
        cJSON *vp = cJSON_AddObjectToObject(o, "viewport");
        if (vp) {
            cJSON_AddNumberToObject(vp, "x", t->vp_x); cJSON_AddNumberToObject(vp, "y", t->vp_y);
            cJSON_AddNumberToObject(vp, "w", t->vp_w); cJSON_AddNumberToObject(vp, "h", t->vp_h);
        }
    }
    if (t->button && t->button[0]) {
        cJSON_AddStringToObject(o, "button", t->button);
        cJSON_AddNumberToObject(o, "button_event_id", (double)t->button_event_id);
    }
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return REST_NET_ERR;

    char auth[300];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->device_token);
    const char *hdrs[] = { auth };

    const char *rbody = NULL;
    rest_status_t st = do_request("POST", url, hdrs, 1, body, NULL, 0, &rbody, NULL, timeout_ms);
    cJSON_free(body);      /* cJSON's allocator (the PSRAM heap when there is one), never libc's */
    if (st == REST_RATELIMIT) out->retry_after_s = retry_after_hdr();
    if (st != REST_OK || !rbody) return st;

    cJSON *r = cJSON_Parse(rbody);
    if (!r) return REST_OK;   /* 200 with an unparseable body: nothing to merge */
    out->next_poll_s = json_get_int(r, "next_poll_s", -1);
    out->server_time = (uint32_t)json_get_int(r, "server_time", 0);
    json_get_str(r, "local_time", out->local_time, sizeof out->local_time);
    json_get_str(r, "tz", out->tz, sizeof out->tz);
    out->tz_offset_s = json_get_int(r, "tz_offset_seconds", 0);
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(r, "config");
    if (cfg) {
        out->sleep_interval_s = json_get_int(cfg, "sleep_interval_s", -1);
        out->always_on        = json_get_bool(cfg, "always_on", -1);
        out->awake_poll_s     = json_get_int(cfg, "awake_poll_s", -1);
    }
    cJSON_Delete(r);
    return REST_OK;
}

bool rest_download(const char *url, uint8_t *dst, size_t cap, size_t *out_len, uint32_t timeout_ms)
{
    *out_len = 0;
    int http = 0;
    rest_status_t st = do_request("GET", url, NULL, 0, NULL, dst, cap, NULL, &http, timeout_ms);
    if (st == REST_NET_ERR) return false;
    if (http < 200 || http >= 300) { printf("rest: download failed, HTTP %d\n", http); return false; }
    if (s_sink_overflow) { printf("rest: download larger than %u bytes\n", (unsigned)cap); return false; }
    if (s_chunked && (!s_ch.done || s_ch.bad)) { printf("rest: chunked body %s\n", s_ch.bad ? "malformed" : "incomplete"); return false; }
    /* Honour Content-Length when present so a truncated transfer is caught. */
    char cl[16];
    if (header_value("Content-Length", cl, sizeof cl)) {
        size_t want = (size_t)strtoul(cl, NULL, 10);
        if (want != s_sink_len) {
            printf("rest: download short: got %u of %u bytes\n", (unsigned)s_sink_len, (unsigned)want);
            return false;
        }
    }
    *out_len = s_sink_len;
    printf("rest: downloaded %u bytes\n", (unsigned)s_sink_len);
    return true;
}

/* GET bytes [off, off+len) of url into dst. Follows up to 5 redirects and
 * leaves the final URL in url (cap url_cap) so the next range skips them. A
 * server that ignores Range is accepted only when it sends the whole body and
 * that fits. *total is the full size from Content-Range / Content-Length. */
bool rest_download_range(char *url, size_t url_cap, uint32_t off, uint32_t len, uint8_t *dst,
                         size_t *got, uint32_t *total, uint32_t timeout_ms)
{
    *got = 0;
    for (int hop = 0; hop < 6; hop++) {
        char range[48];
        snprintf(range, sizeof range, "Range: bytes=%lu-%lu", (unsigned long)off, (unsigned long)(off + len - 1));
        const char *hdrs[] = { range };
        int http = 0;
        rest_status_t st = do_request("GET", url, hdrs, 1, NULL, dst, len, NULL, &http, timeout_ms);
        if (st == REST_NET_ERR) return false;
        if (http == 301 || http == 302 || http == 303 || http == 307 || http == 308) {
            char loc[REST_REQ_MAX];
            if (!header_value("Location", loc, sizeof loc) || !loc[0]) { printf("rest: redirect without Location\n"); return false; }
            if (strlen(loc) >= url_cap) { printf("rest: redirect URL too long\n"); return false; }
            snprintf(url, url_cap, "%s", loc);
            continue;
        }
        if (s_sink_overflow) { printf("rest: range larger than %lu bytes\n", (unsigned long)len); return false; }
        if (s_chunked && (!s_ch.done || s_ch.bad)) { printf("rest: chunked body %s\n", s_ch.bad ? "malformed" : "incomplete"); return false; }
        char v[64];
        if (http == 206) {
            if (header_value("Content-Range", v, sizeof v)) { const char *sl = strchr(v, '/'); if (sl) *total = (uint32_t)strtoul(sl + 1, NULL, 10); }
        } else if (http == 200 && off == 0) {
            if (header_value("Content-Length", v, sizeof v)) *total = (uint32_t)strtoul(v, NULL, 10);
        } else { printf("rest: range failed, HTTP %d\n", http); return false; }
        if (header_value("Content-Length", v, sizeof v) && (size_t)strtoul(v, NULL, 10) != s_sink_len) {
            printf("rest: range short: %u of %s bytes\n", (unsigned)s_sink_len, v); return false;
        }
        *got = s_sink_len;
        return true;
    }
    printf("rest: too many redirects\n");
    return false;
}

rest_status_t rest_get_frames_json(uint8_t *dst, size_t cap, size_t *len, uint32_t timeout_ms)
{
    const config_t *c = config_get();
    char url[256], auth[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/frames.json", c->server_url, config_device_id());
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->device_token);
    const char *hdrs[] = { auth };
    int http = 0;
    *len = 0;
    rest_status_t st = do_request("GET", url, hdrs, 1, NULL, dst, cap - 1, NULL, &http, timeout_ms);
    if (st != REST_OK) return st;
    if (s_sink_overflow) { printf("rest: frames.json larger than %u bytes\n", (unsigned)cap); return REST_HTTP_ERR; }
    *len = s_sink_len; dst[s_sink_len] = 0;
    return st;
}

void rest_resolve_url(const char *u, char *out, size_t cap)
{
    if (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0) { snprintf(out, cap, "%s", u); return; }
    char origin[200];
    snprintf(origin, sizeof origin, "%s", config_get()->server_url);
    char *p = strstr(origin, "://");
    p = p ? p + 3 : origin;
    char *sl = strchr(p, '/');
    if (sl) *sl = '\0';
    snprintf(out, cap, "%s%s%s", origin, (u[0] == '/') ? "" : "/", u);
}

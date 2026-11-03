/*
 * radio.c: see radio.h.
 *
 * Threads: lwIP's callbacks run in the CYW43 background context and only
 * append to the compressed ring; radio_tick() (core 0, the main loop and the
 * REST client's idle hook) decodes from it into the PCM ring; radio_pull()
 * (core 1, the sound timer) reads the PCM ring. Each ring has one writer and
 * one reader, so the indexes need no locks, only ordering.
 */
#include "radio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/sync.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "mbedtls/ssl.h"
#include "config.h"
#include "icy.h"
#include "pcm_resample.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "vendor/minimp3.h"

#include "tls_roots.h"

#define OUT_RATE      22050
#define COMP_BYTES    (256u * 1024u)                 /* ~16 s at 128 kbps */
#define PCM_SAMPLES   (RADIO_PCM_BYTES / 2u)
#define WINDOW_BYTES  8192                           /* linear run of compressed bytes for the decoder */
#define START_MS      2000                           /* decoded sound in hand before playing starts */
#define HDR_MAX       2048

/* ---- memory in PSRAM ---- */
static int16_t          *s_pcm;                      /* PCM_SAMPLES */
static uint8_t          *s_comp;                     /* COMP_BYTES */
static uint8_t          *s_win;                      /* WINDOW_BYTES */
static mp3dec_t         *s_dec;
static int16_t          *s_frame;                    /* MINIMP3_MAX_SAMPLES_PER_FRAME */
static volatile uint32_t s_pcm_w, s_pcm_r;           /* samples, ever */
static volatile uint32_t s_comp_w, s_comp_r;         /* bytes, ever */
static uint32_t          s_win_n;

/* ---- the stream ---- */
static radio_state_t     s_state = RADIO_OFF;
static char              s_url[RADIO_URL_MAX], s_name[RADIO_NAME_MAX], s_station[64], s_err[48];
static char              s_host[96], s_path[RADIO_URL_MAX];
static uint16_t          s_port; static bool s_tls;
static struct altcp_pcb *s_pcb;
static struct altcp_tls_config *s_tls_cfg;
static ip_addr_t         s_ip;
static volatile int      s_dns;                      /* 0 waiting, 1 ok, 2 failed */
static volatile bool     s_dead, s_closed;
static char              s_hdr[HDR_MAX + 1]; static volatile size_t s_hdr_n;
static volatile bool     s_hdr_done;
static int               s_http;
static icy_t             s_icy;
static int               s_kbps;
static pcmrs_t           s_rs;
static uint32_t          s_t_state, s_t_last_data, s_t_retry;
static int               s_retries, s_no_ua;
static volatile bool     s_playing;                  /* the pull hands out sound */
static volatile uint32_t s_underruns;
static uint32_t          s_frames, s_bad;
static volatile uint32_t s_bytes_in, s_pbufs;        /* diagnostics: body bytes and segments received this connection */
static uint32_t          s_t_report;
static int               s_hz, s_ch;
static char              s_req[400];
static size_t            s_req_len, s_req_sent;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static void set_state(radio_state_t s) { if (s != s_state) { s_state = s; s_t_state = now_ms(); } }

/* ---- the compressed ring: written by lwIP, read by the decoder ---- */
static void comp_sink(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    uint32_t w = s_comp_w, r = s_comp_r;
    uint32_t room = COMP_BYTES - (w - r);
    if (n > room) { n = room; }                                /* the decoder is far behind: drop (counted as bad bytes) */
    for (size_t i = 0; i < n; ) {
        uint32_t off = (w + (uint32_t)i) % COMP_BYTES;
        size_t run = COMP_BYTES - off; if (run > n - i) run = n - i;
        memcpy(s_comp + off, d + i, run);
        i += run;
    }
    __dmb();
    s_comp_w = w + (uint32_t)n;
    s_t_last_data = now_ms();
}

/* ---- lwIP callbacks (background context) ---- */
static err_t sent_cb(void *arg, struct altcp_pcb *pcb, u16_t len) { (void)arg; (void)pcb; (void)len; return ERR_OK; }

static void try_send(struct altcp_pcb *pcb)
{
    while (s_req_sent < s_req_len) {
        size_t n = s_req_len - s_req_sent, room = altcp_sndbuf(pcb);
        if (!room) break;
        if (n > room) n = room;
        if (altcp_write(pcb, s_req + s_req_sent, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        s_req_sent += n;
    }
    altcp_output(pcb);
}

static err_t connected_cb(void *arg, struct altcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) { s_dead = true; return ERR_OK; }
    s_req_sent = 0;
    try_send(pcb);
    return ERR_OK;
}

static void absorb(const uint8_t *d, size_t n)
{
    if (s_hdr_done) { icy_feed(&s_icy, d, n, comp_sink, NULL); return; }
    size_t room = HDR_MAX - s_hdr_n, take = n < room ? n : room;
    memcpy(s_hdr + s_hdr_n, d, take); s_hdr_n += take; s_hdr[s_hdr_n] = 0;
    char *end = strstr(s_hdr, "\r\n\r\n");
    if (!end) { if (take < n) { snprintf(s_err, sizeof s_err, "headers too long"); s_dead = true; } return; }
    *end = 0;
    /* the status line: "HTTP/1.1 200 OK" or "ICY 200 OK" */
    s_http = atoi(s_hdr[0] == 'I' ? s_hdr + 4 : strchr(s_hdr, ' ') ? strchr(s_hdr, ' ') + 1 : s_hdr);
    uint32_t metaint = 0; char ctype[48] = ""; char name[64] = "";
    for (char *line = strstr(s_hdr, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        char *nl = strstr(line + 2, "\r\n"); char save = 0;
        if (nl) { save = *nl; *nl = 0; }
        icy_header(line + 2, name, sizeof name, &s_kbps, &metaint, ctype, sizeof ctype);
        if (nl) *nl = save;
    }
    if (name[0]) snprintf(s_station, sizeof s_station, "%s", name);
    if (s_http != 200) { snprintf(s_err, sizeof s_err, "HTTP %d", s_http); s_dead = true; return; }
    if (ctype[0] && strncasecmp(ctype, "audio/mpeg", 10) && strncasecmp(ctype, "audio/mp3", 9) && strncasecmp(ctype, "application/octet-stream", 24)) {
        snprintf(s_err, sizeof s_err, "not MP3 (%.24s)", ctype); s_dead = true; return;
    }
    icy_init(&s_icy, metaint);
    s_hdr_done = true;
    size_t body = s_hdr_n - (size_t)(end + 4 - s_hdr);
    if (body) icy_feed(&s_icy, (const uint8_t *)end + 4, body, comp_sink, NULL);
    if (take < n) icy_feed(&s_icy, d + take, n - take, comp_sink, NULL);
    printf("radio: %s, %d kbps, metadata every %lu bytes\n", s_station[0] ? s_station : "connected", s_kbps, (unsigned long)metaint);
}

static err_t recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (p == NULL) { s_closed = true; return ERR_OK; }
    if (err == ERR_OK) {
        s_pbufs++; s_bytes_in += p->tot_len;
        for (struct pbuf *q = p; q != NULL; q = q->next) absorb((const uint8_t *)q->payload, q->len);
        altcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err) { (void)arg; s_pcb = NULL; s_dead = true; printf("radio: tcp error %d\n", err); }

static void dns_cb(const char *name, const ip_addr_t *ip, void *arg) { (void)name; (void)arg; if (ip) { s_ip = *ip; s_dns = 1; } else s_dns = 2; }

/* ---- connection management (core 0) ---- */
static void close_pcb(void)
{
    cyw43_arch_lwip_begin();
    if (s_pcb) {
        altcp_recv(s_pcb, NULL); altcp_sent(s_pcb, NULL); altcp_err(s_pcb, NULL);
        if (altcp_close(s_pcb) != ERR_OK) altcp_abort(s_pcb);
        s_pcb = NULL;
    }
    cyw43_arch_lwip_end();
}

static bool parse_url(const char *url)
{
    const char *p = url;
    if (!strncmp(p, "https://", 8)) { s_tls = true; p += 8; }
    else if (!strncmp(p, "http://", 7)) { s_tls = false; p += 7; }
    else return false;
    const char *slash = strchr(p, '/'), *colon = strchr(p, ':');
    if (colon && slash && colon > slash) colon = NULL;
    size_t hl = colon ? (size_t)(colon - p) : slash ? (size_t)(slash - p) : strlen(p);
    if (!hl || hl >= sizeof s_host) return false;
    memcpy(s_host, p, hl); s_host[hl] = 0;
    s_port = s_tls ? 443 : 80;
    if (colon) { int v = atoi(colon + 1); if (v > 0 && v < 65536) s_port = (uint16_t)v; }
    snprintf(s_path, sizeof s_path, "%s", slash ? slash : "/");
    return true;
}

static void start_dns(void)
{
    s_dead = s_closed = false; s_hdr_done = false; s_hdr_n = 0; s_http = 0; s_err[0] = 0;
    s_comp_w = s_comp_r = 0; s_win_n = 0; s_bytes_in = s_pbufs = 0; s_t_report = now_ms();
    if (!parse_url(s_url)) { snprintf(s_err, sizeof s_err, "bad URL"); set_state(RADIO_FAILED); return; }
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(s_host, &s_ip, dns_cb, NULL);
    cyw43_arch_lwip_end();
    s_dns = e == ERR_OK ? 1 : e == ERR_INPROGRESS ? 0 : 2;
    set_state(RADIO_DNS);
}

static void start_connect(void)
{
    snprintf(s_req, sizeof s_req, "GET %s HTTP/1.1\r\nHost: %s\r\n%sIcy-MetaData: 1\r\nAccept: */*\r\nConnection: close\r\n\r\n",
             s_path, s_host, s_no_ua ? "" : "User-Agent: tesserae-device-bedside\r\n");
    s_req_len = strlen(s_req); s_req_sent = 0;
    cyw43_arch_lwip_begin();
    if (s_tls && !s_tls_cfg) s_tls_cfg = altcp_tls_create_config_client((const u8_t *)tls_roots_pem, tls_roots_pem_len);
    if (s_tls && !s_tls_cfg) { cyw43_arch_lwip_end(); snprintf(s_err, sizeof s_err, "TLS setup failed"); set_state(RADIO_FAILED); return; }
    s_pcb = s_tls ? altcp_tls_new(s_tls_cfg, IP_GET_TYPE(&s_ip)) : altcp_tcp_new_ip_type(IP_GET_TYPE(&s_ip));
    if (s_pcb && s_tls) mbedtls_ssl_set_hostname((mbedtls_ssl_context *)altcp_tls_context(s_pcb), s_host);
    bool ok = false;
    if (s_pcb) {
        altcp_err(s_pcb, err_cb); altcp_recv(s_pcb, recv_cb); altcp_sent(s_pcb, sent_cb);
        ok = altcp_connect(s_pcb, &s_ip, s_port, connected_cb) == ERR_OK;
    }
    cyw43_arch_lwip_end();
    if (!ok) { close_pcb(); snprintf(s_err, sizeof s_err, "connect failed"); set_state(RADIO_FAILED); return; }
    set_state(RADIO_CONNECTING);
}

static void give_up_or_retry(const char *why)
{
    close_pcb();
    s_playing = false;
    if (why && why[0] && !s_err[0]) snprintf(s_err, sizeof s_err, "%s", why);
    /* a 403 once more without a User-Agent: some stream servers gate on it */
    if (s_http == 403 && !s_no_ua) { s_no_ua = 1; s_retries = 0; printf("radio: 403, trying without a User-Agent\n"); start_dns(); return; }
    s_retries++;
    uint32_t wait = s_retries < 3 ? 2000 : s_retries < 6 ? 5000 : 30000;
    s_t_retry = now_ms() + wait;
    set_state(RADIO_STALLED);
    printf("radio: %s; retry %d in %lu s\n", s_err[0] ? s_err : "stream ended", s_retries, (unsigned long)wait / 1000);
}

/* ---- decoding (core 0) ---- */
static void decode_some(void)
{
    uint32_t t0 = now_ms();
    for (int guard = 0; guard < 80; guard++) {
        /* keep the ring full; stop when there is nothing left to feed. The main
         * loop runs ~10 times a second, so a pass may spend a while here while
         * the decoded sound is short, and only a moment once it is well ahead. */
        uint32_t have = s_pcm_w - s_pcm_r;
        if (have > PCM_SAMPLES - 2400) break;                           /* ring nearly full */
        uint32_t budget = have < OUT_RATE ? 40 : have < 6u * OUT_RATE ? 16 : 4;   /* ms per pass: settles at ~6 s in hand */
        if (now_ms() - t0 > budget) break;
        /* top up the linear window from the ring */
        uint32_t avail = s_comp_w - s_comp_r;
        while (s_win_n < WINDOW_BYTES && avail) {
            uint32_t off = s_comp_r % COMP_BYTES;
            uint32_t run = COMP_BYTES - off; if (run > avail) run = avail; if (run > WINDOW_BYTES - s_win_n) run = WINDOW_BYTES - s_win_n;
            memcpy(s_win + s_win_n, s_comp + off, run);
            s_win_n += run; s_comp_r += run; avail -= run;
        }
        if (s_win_n < 1500 && !s_closed) break;                          /* a frame is at most ~1.4 KB: wait for more */
        if (!s_win_n) break;
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(s_dec, s_win, (int)s_win_n, s_frame, &info);
        if (info.frame_bytes <= 0) {                                      /* no whole frame yet: wait for more, unless the window is full of junk */
            if (s_win_n >= WINDOW_BYTES) { memmove(s_win, s_win + 1024, s_win_n - 1024); s_win_n -= 1024; s_bad += 1024; continue; }
            break;
        }
        if (samples > 0) {
            s_hz = info.hz; s_ch = info.channels; s_frames++;
            static int16_t mono[1200];
            int n = pcmrs_run(&s_rs, s_frame, samples, info.channels, info.hz, OUT_RATE, mono, (int)(sizeof mono / sizeof mono[0]));
            uint32_t w = s_pcm_w;
            for (int i = 0; i < n; i++) s_pcm[(w + (uint32_t)i) % PCM_SAMPLES] = mono[i];
            __dmb();
            s_pcm_w = w + (uint32_t)n;
        } else s_bad += (uint32_t)info.frame_bytes;
        memmove(s_win, s_win + info.frame_bytes, s_win_n - (uint32_t)info.frame_bytes);
        s_win_n -= (uint32_t)info.frame_bytes;
    }
}

void radio_tick(void)
{
    if (s_state == RADIO_OFF) return;
    uint32_t t = now_ms();
    switch (s_state) {
    case RADIO_DNS:
        if (s_dns == 1) start_connect();
        else if (s_dns == 2 || t - s_t_state > 10000) give_up_or_retry("no address for the host");
        break;
    case RADIO_CONNECTING:
        if (s_dead) give_up_or_retry("connection refused");
        else if (s_hdr_n) set_state(RADIO_HEADERS);
        else if (t - s_t_state > 15000) give_up_or_retry("connect timed out");
        break;
    case RADIO_HEADERS:
        if (s_dead) give_up_or_retry("refused");
        else if (s_hdr_done) { set_state(RADIO_BUFFERING); s_t_last_data = t; s_retries = 0; }
        else if (t - s_t_state > 15000) give_up_or_retry("no answer");
        break;
    case RADIO_BUFFERING:
    case RADIO_PLAYING:
        if (s_dead) { give_up_or_retry("connection lost"); break; }
        decode_some();
        {   uint32_t have = s_pcm_w - s_pcm_r;
            if (s_state == RADIO_BUFFERING && have >= (uint32_t)OUT_RATE * START_MS / 1000) {
                s_playing = true; set_state(RADIO_PLAYING);
                printf("radio: playing %s (%d Hz %s, %d kbps)\n", s_station[0] ? s_station : s_name, s_hz, s_ch == 2 ? "stereo" : "mono", s_kbps);
            }
            if (s_closed && !have && !s_win_n) give_up_or_retry("stream ended");
            else if ((int32_t)(t - s_t_last_data) > 20000) give_up_or_retry("no data for 20 s");   /* signed: a packet can land after t was taken */
            else if (t - s_t_report > 5000) {                              /* every 5 s: is data still coming, and where is it */
                s_t_report = t;
                printf("radio: %lu bytes in %lu segments, ring %lu B, window %lu B, %d ms decoded, %lu frames, %lu bad, last data %lu ms ago\n",
                       (unsigned long)s_bytes_in, (unsigned long)s_pbufs, (unsigned long)(s_comp_w - s_comp_r), (unsigned long)s_win_n,
                       radio_buffer_ms(), (unsigned long)s_frames, (unsigned long)s_bad, (unsigned long)((int32_t)(t - s_t_last_data) > 0 ? t - s_t_last_data : 0));
            }
        }
        break;
    case RADIO_STALLED:
        if ((int32_t)(t - s_t_retry) >= 0) start_dns();
        break;
    default: break;
    }
    if (s_icy.title_new) { s_icy.title_new = false; printf("radio: now playing: %s\n", s_icy.title); }
}

/* ---- core 1 ---- */
int radio_pull(int16_t *dst, int n)
{
    if (!s_playing) { memset(dst, 0, (size_t)n * 2); return 0; }
    uint32_t w = s_pcm_w; __dmb();
    uint32_t r = s_pcm_r, have = w - r;
    if (have < (uint32_t)n) { memset(dst, 0, (size_t)n * 2); if (s_state == RADIO_PLAYING) s_underruns++; return 0; }
    for (int i = 0; i < n; i++) dst[i] = s_pcm[(r + (uint32_t)i) % PCM_SAMPLES];
    __dmb();
    s_pcm_r = r + (uint32_t)n;
    return n;
}

/* ---- control ---- */
void radio_init(uint8_t *pcm, uint8_t *work)
{
    s_pcm = (int16_t *)pcm;
    s_comp = work; s_win = work + COMP_BYTES;
    s_dec = (mp3dec_t *)(work + COMP_BYTES + WINDOW_BYTES);
    s_frame = (int16_t *)((uint8_t *)s_dec + ((sizeof *s_dec + 15) & ~15u));
    mp3dec_init(s_dec);
    pcmrs_init(&s_rs);
}

void radio_play(const char *url, const char *name)
{
    if (!s_pcm || !url || !url[0]) return;
    if (s_state != RADIO_OFF && !strcmp(url, s_url)) return;          /* already on it */
    radio_stop();
    snprintf(s_url, sizeof s_url, "%s", url);
    snprintf(s_name, sizeof s_name, "%s", name ? name : "");
    s_station[0] = 0; s_err[0] = 0; s_retries = 0; s_no_ua = 0; s_kbps = 0; s_frames = s_bad = 0; s_underruns = 0;
    s_pcm_w = s_pcm_r = 0; s_playing = false;
    mp3dec_init(s_dec); pcmrs_init(&s_rs);
    printf("radio: %s\n", url);
    start_dns();
}

void radio_stop(void)
{
    if (s_state == RADIO_OFF) return;
    s_playing = false;
    close_pcb();
    set_state(RADIO_OFF);
    s_icy.title[0] = 0;
    printf("radio: stopped (%lu frames, %lu bad bytes, %lu underruns)\n", (unsigned long)s_frames, (unsigned long)s_bad, (unsigned long)s_underruns);
}

radio_state_t radio_state(void) { return s_state; }
const char *radio_state_name(void)
{
    static const char *const N[] = { "off", "looking up", "connecting", "waiting", "buffering", "playing", "reconnecting", "failed" };
    return N[s_state];
}
const char *radio_station(void) { return s_station[0] ? s_station : s_name; }
const char *radio_title(void)   { return s_icy.title; }
int  radio_kbps(void)           { return s_kbps; }
int  radio_buffer_ms(void)      { return (int)((s_pcm_w - s_pcm_r) * 1000u / OUT_RATE); }
uint32_t radio_underruns(void)  { return s_underruns; }
const char *radio_error(void)   { return s_err; }

/* ---- presets: flash blob slot 6 ---- */
#define RADIO_MAGIC 0x31444152u   /* "RAD1" */
static radio_preset_t s_presets[RADIO_PRESETS];
static const radio_preset_t SEEDS[] = {
    { "Drone Zone",     "http://ice1.somafm.com/dronezone-128-mp3" },
    { "Groove Salad",   "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "Deep Space One", "http://ice1.somafm.com/deepspaceone-128-mp3" },
    { "Lush",           "http://ice1.somafm.com/lush-128-mp3" },
    { "Space Station",  "http://ice1.somafm.com/spacestation-128-mp3" },
    { "Radio Paradise", "http://stream.radioparadise.com/mp3-128" },
    { "FIP",            "https://icecast.radiofrance.fr/fip-midfi.mp3" },
};

int radio_preset_count(void) { return RADIO_PRESETS; }
const radio_preset_t *radio_preset(int i) { return (i >= 0 && i < RADIO_PRESETS) ? &s_presets[i] : NULL; }

void radio_presets_load(void)
{
    if (!config_blob_load(6, RADIO_MAGIC, s_presets, sizeof s_presets)) {
        memset(s_presets, 0, sizeof s_presets);
        for (unsigned i = 0; i < sizeof SEEDS / sizeof SEEDS[0] && i < RADIO_PRESETS; i++) s_presets[i] = SEEDS[i];
        printf("radio: %u preset stations seeded\n", (unsigned)(sizeof SEEDS / sizeof SEEDS[0]));
    }
    for (int i = 0; i < RADIO_PRESETS; i++) { s_presets[i].name[RADIO_NAME_MAX - 1] = 0; s_presets[i].url[RADIO_URL_MAX - 1] = 0; }
}

bool radio_preset_set(int i, const char *name, const char *url)
{
    if (i < 0 || i >= RADIO_PRESETS || !name || !url) return false;
    if (url[0] && strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return false;
    snprintf(s_presets[i].name, sizeof s_presets[i].name, "%s", name);
    snprintf(s_presets[i].url, sizeof s_presets[i].url, "%s", url);
    return config_blob_save(6, RADIO_MAGIC, s_presets, sizeof s_presets);
}

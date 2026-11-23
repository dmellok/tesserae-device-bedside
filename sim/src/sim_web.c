/*
 * sim_web.c: WiFi and the web server (the net.h API, in place of src/net.c),
 * on a host socket.
 *
 * The same routes as the device: the phone app (/ and /desktop, read from
 * www/ on every request, so a rebuilt page shows on reload), /api/state, /cmd,
 * /log, /files, /file, /soma, /frame and the /audio stream; in WiFi setup the
 * setup page from net.c and /api/setupinfo. Plus the simulator's own:
 *   /sim                the simulator page (sim/www/sim.html)
 *   /sim/glass          the panel's glass, 4 bpp (?v=N: 204 if unchanged)
 *   /sim/status         panel, keys and LEDs, sound, clock (JSON)
 *   /sim/input          key=0..3&act=down|up|tap|hold, knob=cw|ccw[&n=N],
 *                       knob=down|up|press|hold, boot=down|up|press|hold
 *   /sim/time           add=SECONDS or to=HH:MM: jump the clock forward
 *   /sim/speed          instant=1: refreshes take no time; 0: the panel's timings
 *   /sim/menu           the menu showing: title, rows, values, the highlight
 *   /sim/text           the words each layer draws: body, top bar, status bar, card
 *   /sim/reboot         restart (the flash file stays)
 * The firmware is single-threaded on this side: all of this runs from
 * net_poll() on the main thread, as the device's deferred work does.
 */
#include "sim.h"
#include "sim_sdk.h"
#include "net.h"
#include "audio.h"
#include "store.h"
#include "radio.h"
#include "it8951.h"
#include "sim_lwip.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

int64_t utc_now(void);
int32_t tz_offset_at(int64_t utc);

static const char SETUP_PAGE[] =
#include "setup_page.inc"
;

#define MAX_CONN 16
#define REQ_MAX  (72 * 1024)       /* headers and one upload chunk */
#define PUT_MAX  (64u * 1024)

typedef struct {
    int      fd;
    char     req[REQ_MAX + 1];
    size_t   req_len, hdr_end, clen;
    bool     have_hdr, done;
    uint8_t *tx; size_t tx_len, tx_off, tx_cap;
    bool     stream;               /* /audio */
    uint32_t apos;
    uint32_t t_open;
} conn_t;

static conn_t   s_conn[MAX_CONN];
static int      s_listen = -1;
static bool     s_up, s_setup;
static char     s_ip[40], s_ap[33];
static uint8_t *s_put_buf;
static void   (*s_wait_hook)(void);
static char     s_json[16384];

/* Taps and holds asked for over HTTP release themselves later. */
static struct { int what, idx; uint32_t at; } s_release[8];      /* what: 1 key, 2 knob button, 3 boot */

void net_set_upload_buffer(uint8_t *buf) { s_put_buf = buf; }
void net_set_wait_hook(void (*fn)(void)) { s_wait_hook = fn; }
bool net_connected(void)       { return s_up; }
const char *net_ip(void)       { return s_ip; }
int  net_rssi(void)            { return s_up ? -48 : 0; }
bool net_setup_active(void)    { return s_setup; }
const char *net_setup_ssid(void) { return s_ap; }
void net_mac(char *out, size_t n) { snprintf(out, n, "02:5e:5e:b3:d5:3d"); }   /* locally administered: never a real board's */

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static bool listen_on(void)
{
    if (s_listen >= 0) return true;
    for (int port = sim_opt.port; port < sim_opt.port + 10; port++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(fd, 16) == 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            s_listen = fd;
            if (port != sim_opt.port) printf("sim: port %d is taken, using %d\n", sim_opt.port, port);
            sim_opt.port = port;
            if (port == 80) snprintf(s_ip, sizeof s_ip, "%s", sim_lan_ip());
            else snprintf(s_ip, sizeof s_ip, "%s:%d", sim_lan_ip(), port);
            return true;
        }
        close(fd);
    }
    printf("sim: no free port from %d: web UI unavailable\n", sim_opt.port);
    return false;
}

bool net_init(const char *ssid, const char *pass)
{
    (void)pass;
    if (!ssid || !ssid[0]) { printf("net: no WiFi credentials (include/secrets.h or the web UI)\n"); return false; }
    printf("net: connecting to \"%s\"...\n", ssid);
    for (int i = 0; i < 10; i++) { if (s_wait_hook) s_wait_hook(); sleep_ms(50); }     /* the boot log moves, as while joining */
    if (!listen_on()) return false;
    s_up = true;
    sim_seed_epoch();
    printf("net: connected, web UI at http://%s/\n", s_ip);
    printf("sim: the simulator page is http://localhost:%d/sim\n", sim_opt.port);
    return true;
}

bool net_start_setup(const char *ap)
{
    snprintf(s_ap, sizeof s_ap, "%s", ap);
    printf("net: 3 WiFi network(s) nearby\n");
    if (!listen_on()) return false;
    s_setup = true; s_up = true;
    sim_seed_epoch();
    printf("net: setup mode: join \"%s\", then open http://192.168.4.1/\n", s_ap);
    printf("sim: in the simulator the setup page is http://localhost:%d/\n", sim_opt.port);
    return true;
}

/* ---- replies ---------------------------------------------------------------------- */

static void tx_add(conn_t *c, const void *d, size_t n)
{
    if (c->tx_len + n > c->tx_cap) {
        size_t cap = c->tx_cap ? c->tx_cap : 8192;
        while (cap < c->tx_len + n) cap *= 2;
        uint8_t *b = realloc(c->tx, cap);
        if (!b) return;
        c->tx = b; c->tx_cap = cap;
    }
    memcpy(c->tx + c->tx_len, d, n); c->tx_len += n;
}

static void respond_x(conn_t *c, int code, const char *ctype, const void *body, size_t len, const char *extra)
{
    char h[400];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n%s\r\n",
                     code, code == 200 ? "OK" : code == 204 ? "No Content" : code == 302 ? "Found" : code == 404 ? "Not Found" : "Bad Request",
                     ctype, (unsigned)len, extra ? extra : "");
    tx_add(c, h, (size_t)n);
    if (len) tx_add(c, body, len);
    c->done = true;
}
static void respond(conn_t *c, int code, const char *ctype, const void *body, size_t len) { respond_x(c, code, ctype, body, len, NULL); }
static void respond_json(conn_t *c, const char *j) { respond(c, 200, "application/json", j, strlen(j)); }

static void respond_file(conn_t *c, const char *rel, const char *ctype)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", SIM_ROOT, rel);
    FILE *f = fopen(path, "rb");
    if (!f) { char m[1200]; int n = snprintf(m, sizeof m, "not found: %s", path); respond(c, 404, "text/plain", m, (size_t)n); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n);
    size_t got = b ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    respond(c, 200, ctype, b, got);
    free(b);
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}
static void url_decode(const char *src, char *out, size_t cap)
{
    size_t o = 0;
    while (*src && *src != '&' && o + 1 < cap) {
        if (*src == '%' && hexval(src[1]) >= 0 && hexval(src[2]) >= 0) { out[o++] = (char)(hexval(src[1]) * 16 + hexval(src[2])); src += 3; }
        else { out[o++] = (*src == '+') ? ' ' : *src; src++; }
    }
    out[o] = 0;
}
static bool query_get(const char *q, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    while (q && *q) {
        if (!strncmp(q, key, kl) && q[kl] == '=') { url_decode(q + kl + 1, out, cap); return true; }
        q = strchr(q, '&');
        if (q) q++;
    }
    return false;
}

/* ---- the simulator's own routes ------------------------------------------------------- */

static void later(int what, int idx, uint32_t ms)
{
    for (int i = 0; i < 8; i++) if (!s_release[i].what) { s_release[i].what = what; s_release[i].idx = idx; s_release[i].at = (uint32_t)(sim_real_us() / 1000) + ms; return; }
}
static void release(int what, int idx)
{
    if (what == 1) sim_key(idx, false);
    else if (what == 2) sim_knob_button(false);
    else if (what == 3) sim_boot_button(false);
}

/* down / up / tap / hold for a key (what 1), the knob's button (2) or BOOT (3). */
static bool press(int what, int idx, const char *act)
{
    bool down = !strcmp(act, "down") || !strcmp(act, "tap") || !strcmp(act, "press") || !strcmp(act, "hold");
    if (what == 1) sim_key(idx, down);
    else if (what == 2) sim_knob_button(down);
    else if (what == 3) sim_boot_button(down);
    if (!strcmp(act, "tap") || !strcmp(act, "press")) later(what, idx, 150);    /* longer than the 15 ms poll, shorter than a hold */
    else if (!strcmp(act, "hold")) later(what, idx, 900);                       /* past the 600 ms long press */
    else if (strcmp(act, "down") && strcmp(act, "up")) return false;
    return true;
}

static void sim_input(conn_t *c, const char *q)
{
    char v[16], a[16] = "tap";
    query_get(q, "act", a, sizeof a);
    bool ok = false;
    if (query_get(q, "key", v, sizeof v)) ok = press(1, atoi(v), a);
    else if (query_get(q, "knob", v, sizeof v)) {
        char nn[8]; int n = query_get(q, "n", nn, sizeof nn) ? atoi(nn) : 1;
        if (!strcmp(v, "cw")) { sim_knob_turn(n); ok = true; }
        else if (!strcmp(v, "ccw")) { sim_knob_turn(-n); ok = true; }
        else ok = press(2, 0, v);
    }
    else if (query_get(q, "boot", v, sizeof v)) ok = press(3, 0, v);
    respond_json(c, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void clock_json(char *b, size_t cap)
{
    int64_t u = utc_now();
    int64_t l = u ? u + tz_offset_at(u) : 0;
    int s = (int)(((l % 86400) + 86400) % 86400);
    snprintf(b, cap, "{\"uptime_ms\":%u,\"warp_s\":%lld,\"utc\":%lld,\"local\":\"%02d:%02d:%02d\",\"tz_s\":%d}",
             now_ms(), (long long)sim_warp_total_s(), (long long)u, s / 3600, s / 60 % 60, s % 60, u ? tz_offset_at(u) : 0);
}

static void sim_time(conn_t *c, const char *q)
{
    char v[24];
    if (query_get(q, "add", v, sizeof v)) sim_warp(strtoll(v, NULL, 10));
    else if (query_get(q, "to", v, sizeof v)) {                    /* the next HH:MM on the panel's clock */
        int hh = 0, mm = 0;
        int64_t u = utc_now();
        if (u && sscanf(v, "%d:%d", &hh, &mm) == 2) {
            int64_t l = u + tz_offset_at(u);
            int64_t sod = ((l % 86400) + 86400) % 86400, want = (int64_t)hh * 3600 + mm * 60;
            int64_t d = want - sod;
            if (d <= 0) d += 86400;
            sim_warp(d);
        }
    }
    char b[200]; clock_json(b, sizeof b);
    respond_json(c, b);
}

static void sim_status(conn_t *c)
{
    size_t n = (size_t)snprintf(s_json, sizeof s_json, "{\"panel\":");
    n += sim_panel_json(s_json + n, sizeof s_json - n);
    n += (size_t)snprintf(s_json + n, sizeof s_json - n, ",\"inputs\":");
    n += sim_keys_json(s_json + n, sizeof s_json - n);
    n += (size_t)snprintf(s_json + n, sizeof s_json - n, ",\"audio\":");
    n += sim_audio_json(s_json + n, sizeof s_json - n);
    char ck[200]; clock_json(ck, sizeof ck);
    n += (size_t)snprintf(s_json + n, sizeof s_json - n, ",\"clock\":%s,\"ip\":\"%s\",\"setup\":%d,\"flash\":\"%s\",\"version\":\"%s\",\"instant\":%d}",
                          ck, s_ip, s_setup, sim_opt.flash_path, FW_VERSION, sim_opt.instant);
    respond(c, 200, "application/json", s_json, n);
}

static void sim_glass_reply(conn_t *c, const char *q)
{
    char v[16];
    uint32_t have = query_get(q, "v", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0, ver = sim_glass_version();
    char x[64]; snprintf(x, sizeof x, "X-Glass-Version: %u\r\n", ver);
    if (have == ver) { respond_x(c, 204, "application/octet-stream", NULL, 0, x); return; }
    sim_glass_lock(true);
    respond_x(c, 200, "application/octet-stream", sim_glass(), IT8951_FRAME_BYTES, x);
    sim_glass_lock(false);
}

/* ---- requests ------------------------------------------------------------------------- */

static void handle(conn_t *c)
{
    char *req = c->req;
    bool post = !strncmp(req, "POST ", 5);
    char *eol = strstr(req, "\r\n");
    if (eol) *eol = 0;
    char *sp = strchr(req, ' ');
    if (!sp) { respond(c, 400, "text/plain", "bad request", 11); return; }
    char *path = sp + 1, *sp2 = strchr(path, ' ');
    if (sp2) *sp2 = 0;
    char *query = strchr(path, '?');
    if (query) *query++ = 0; else query = "";

    if (!strcmp(path, "/sim") || !strcmp(path, "/sim/")) { respond_file(c, "sim/www/sim.html", "text/html; charset=utf-8"); return; }
    if (!strcmp(path, "/sim/glass"))  { sim_glass_reply(c, query); return; }
    if (!strcmp(path, "/sim/status")) { sim_status(c); return; }
    if (!strcmp(path, "/sim/input"))  { sim_input(c, query); return; }
    if (!strcmp(path, "/sim/time"))   { sim_time(c, query); return; }
    if (!strcmp(path, "/sim/speed"))  { char v[4]; if (query_get(query, "instant", v, sizeof v)) sim_opt.instant = atoi(v) != 0; respond_json(c, sim_opt.instant ? "{\"instant\":1}" : "{\"instant\":0}"); return; }
    if (!strcmp(path, "/sim/menu"))   { size_t n = sim_menu_json(s_json, sizeof s_json); respond(c, 200, "application/json", s_json, n); return; }
    if (!strcmp(path, "/sim/text"))   { size_t n = sim_text_json(s_json, sizeof s_json); respond(c, 200, "application/json", s_json, n); return; }
    if (!strcmp(path, "/sim/reboot")) { respond_json(c, "{\"ok\":true}"); send(c->fd, c->tx, c->tx_len, 0); sleep_ms(50); sim_reboot("restart asked for from the simulator page"); }

    if (s_setup) {
        if (!strcmp(path, "/") || !strcmp(path, "/index.html") || !strcmp(path, "/setup")) { respond(c, 200, "text/html; charset=utf-8", SETUP_PAGE, sizeof SETUP_PAGE - 1); return; }
        if (!strcmp(path, "/api/setupinfo")) {
            int n = snprintf(s_json, sizeof s_json, "{\"ssid\":\"%s\",\"server\":\"%s\",\"name\":\"%s\",\"networks\":"
                             "[{\"ssid\":\"Simulator\",\"rssi\":-40},{\"ssid\":\"Neighbours\",\"rssi\":-71},{\"ssid\":\"Cafe guest\",\"rssi\":-80}]}",
                             app_setup_ssid(), app_setup_server(), app_setup_name());
            respond(c, 200, "application/json", s_json, (size_t)n); return;
        }
        if (strcmp(path, "/api/cmd") && strcmp(path, "/api/log") && strcmp(path, "/api/state")) {
            respond_x(c, 302, "text/plain", NULL, 0, "Location: /\r\n"); return;
        }
    }
    if (!strcmp(path, "/") || !strcmp(path, "/index.html"))           respond_file(c, "www/index.html", "text/html; charset=utf-8");
    else if (!strcmp(path, "/desktop") || !strcmp(path, "/desktop.html")) respond_file(c, "www/desktop.html", "text/html; charset=utf-8");
    else if (!strcmp(path, "/api/state")) { size_t n = app_state_json(s_json, sizeof s_json); respond(c, 200, "application/json", s_json, n); }
    else if (!strcmp(path, "/api/log")) {
        char v[16]; uint32_t from = query_get(query, "from", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0, next = 0;
        int h = snprintf(s_json, sizeof s_json, "%-10lu\n", 0ul);
        size_t n = app_log_read(from, s_json + h, sizeof s_json - (size_t)h, &next);
        snprintf(s_json, sizeof s_json, "%-10lu", (unsigned long)next); s_json[10] = '\n';
        respond(c, 200, "text/plain; charset=utf-8", s_json, (size_t)h + n);
    } else if (!strcmp(path, "/api/cmd")) {
        char name[24] = "", value[200] = "";
        const char *eq = strchr(query, '=');
        if (eq && (size_t)(eq - query) < sizeof name) { memcpy(name, query, (size_t)(eq - query)); name[eq - query] = 0; url_decode(eq + 1, value, sizeof value); }
        else url_decode(query, name, sizeof name);
        respond_json(c, "{\"ok\":true}");
        send(c->fd, c->tx, c->tx_len, 0); c->tx_off = c->tx_len;     /* answer first: the command may reboot */
        app_command(name, value);
    } else if (!strcmp(path, "/audio")) {
        char h[256];
        int n = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nX-Audio-Rate: %d\r\n"
                         "X-Audio-Format: s16le-mono\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", AUDIO_SR);
        tx_add(c, h, (size_t)n);
        c->stream = true; c->apos = audio_write_pos();
        audio_listener(+1);
    } else if (!strcmp(path, "/api/files")) { size_t n = store_json(s_json, sizeof s_json); respond(c, 200, "application/json", s_json, n); }
    else if (!strcmp(path, "/api/soma"))   { size_t n = radio_soma_json(s_json, sizeof s_json); respond(c, 200, "application/json", s_json, n); }
    else if (!strcmp(path, "/api/file") && !post) {
        char name[STORE_NAME_MAX];
        const store_ent_t *e = query_get(query, "name", name, sizeof name) ? store_find(name) : NULL;
        if (!e) { respond(c, 404, "text/plain", "no such file", 12); return; }
        respond(c, 200, "application/octet-stream", store_data(e), e->size);
    } else if (!strcmp(path, "/api/file")) {
        char name[STORE_NAME_MAX] = "", v[16], err[96] = "", res[200];
        query_get(query, "name", name, sizeof name);
        int r;
        if (strstr(query, "delete=1")) r = store_delete(name) ? 1 : (snprintf(err, sizeof err, "no such file"), -1);
        else if (!s_put_buf || c->clen > PUT_MAX) { r = -1; snprintf(err, sizeof err, "chunk too big"); }
        else {
            memcpy(s_put_buf, c->req + c->hdr_end, c->clen);
            uint32_t off = query_get(query, "off", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0;
            uint32_t total = query_get(query, "total", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0;
            r = store_put(name, off, total, s_put_buf, c->clen, err, sizeof err);
        }
        int n = r < 0 ? snprintf(res, sizeof res, "{\"ok\":false,\"error\":\"%s\"}", err) : snprintf(res, sizeof res, "{\"ok\":true,\"done\":%s}", r ? "true" : "false");
        respond(c, r < 0 ? 400 : 200, "application/json", res, (size_t)n);
    } else if (!strcmp(path, "/frame")) {
        char v[8];
        int sc = query_get(query, "s", v, sizeof v) ? atoi(v) : 4;
        size_t len = 0;
        const uint8_t *buf = app_render_preview(sc < 2 ? 2 : sc > 8 ? 8 : sc, &len);
        if (buf && len) respond(c, 200, "application/octet-stream", buf, len);
        else respond(c, 400, "text/plain", "no preview", 10);
    } else respond(c, 404, "text/plain", "not found", 9);
}

static void conn_close(conn_t *c)
{
    if (c->stream) audio_listener(-1);
    close(c->fd);
    free(c->tx);
    memset(c, 0, sizeof *c);           /* fd 0: a free slot (stdin is never an accepted socket) */
}

/* Bytes in: once the headers (and a POST body) are complete, answer. */
static void conn_read(conn_t *c)
{
    for (;;) {
        if (c->req_len >= REQ_MAX && !c->have_hdr) { respond(c, 400, "text/plain", "headers too long", 16); return; }
        size_t room = REQ_MAX - c->req_len;
        if (!room) break;
        ssize_t n = recv(c->fd, c->req + c->req_len, room, 0);
        if (n == 0) { if (!c->done) conn_close(c); return; }
        if (n < 0) { if (errno != EAGAIN) conn_close(c); return; }
        c->req_len += (size_t)n;
        c->req[c->req_len] = 0;
        if (!c->have_hdr) {
            char *e = strstr(c->req, "\r\n\r\n");
            if (!e) continue;
            c->have_hdr = true;
            c->hdr_end = (size_t)(e + 4 - c->req);
            for (const char *h = c->req; h && h < e; h = strstr(h, "\r\n")) {
                if (*h == '\r') h += 2;
                if (!strncasecmp(h, "Content-Length:", 15)) { c->clen = strtoul(h + 15, NULL, 10); break; }
            }
        }
        if (c->have_hdr && c->req_len >= c->hdr_end + c->clen) { handle(c); return; }
        if (c->have_hdr && c->hdr_end + c->clen > REQ_MAX) break;       /* bigger than the buffer: below */
    }
}

/* ---- the poll ------------------------------------------------------------------------- */

void sim_web_poll(void) { net_poll(); }

void net_poll(void)
{
    uint32_t t = (uint32_t)(sim_real_us() / 1000);
    for (int i = 0; i < 8; i++) if (s_release[i].what && (int32_t)(t - s_release[i].at) >= 0) { release(s_release[i].what, s_release[i].idx); s_release[i].what = 0; }
    if (s_listen < 0) return;
    sim_lwip_pump(0);

    for (;;) {                                                       /* new connections */
        int fd = accept(s_listen, NULL, NULL);
        if (fd < 0) break;
        conn_t *c = NULL;
        for (int i = 0; i < MAX_CONN; i++) if (!s_conn[i].fd) { c = &s_conn[i]; break; }
        if (!c) { close(fd); break; }
        memset(c, 0, sizeof *c);
        c->fd = fd; c->t_open = t;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    for (int i = 0; i < MAX_CONN; i++) {
        conn_t *c = &s_conn[i];
        if (!c->fd) continue;
        if (!c->done && !c->stream) {
            conn_read(c);
            if (!c->fd) continue;
            if (!c->done && c->have_hdr && c->hdr_end + c->clen > REQ_MAX) { respond(c, 400, "application/json", "{\"ok\":false,\"error\":\"chunk too big\"}", 37); }
            if (!c->done && t - c->t_open > 20000) { conn_close(c); continue; }
        }
        if (c->stream && c->tx_len - c->tx_off < 8192) {              /* /audio: more from the ring */
            static int16_t pcm[4096];
            size_t n = audio_read(&c->apos, pcm, 4096);
            if (n) tx_add(c, pcm, n * 2);
        }
        while (c->tx_off < c->tx_len) {
            ssize_t n = send(c->fd, c->tx + c->tx_off, c->tx_len - c->tx_off, 0);
            if (n > 0) c->tx_off += (size_t)n;
            else { if (n < 0 && errno != EAGAIN) { conn_close(c); } break; }
        }
        if (!c->fd) continue;
        if (c->tx_off == c->tx_len && c->tx_len) { c->tx_off = c->tx_len = 0; if (c->done) { conn_close(c); continue; } }
        if (c->stream) {                                              /* the listener went away? */
            char x; ssize_t r = recv(c->fd, &x, 1, MSG_PEEK);
            if (r == 0 || (r < 0 && errno != EAGAIN)) conn_close(c);
        }
    }
}

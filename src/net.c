/*
 * net.c: WiFi + minimal HTTP server on the raw lwIP API. See net.h.
 */
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "audio.h"
#include "dhcpserver.h"
#include "dnsserver.h"
#include "store.h"

extern const uint8_t www_index[];
extern const uint8_t www_index_end[];

#define HTTP_PORT  80
#define MAX_CONN   6
#define REQ_MAX    1536            /* a browser POST's headers */
#define PUT_MAX    (64u * 1024)    /* one upload chunk (the web app sends 32 KB) */

typedef struct {
    struct tcp_pcb *pcb;
    char            req[REQ_MAX];
    size_t          req_len;
    char            hdr[200];
    size_t          hdr_len;
    const uint8_t  *body;
    size_t          body_len;
    bool            body_copy;      /* small/volatile body: let lwIP copy it */
    size_t          sent, acked;
    bool            want_frame;     /* deferred to net_poll() */
    int             frame_scale;
    bool            is_frame;       /* body is the shared preview buffer */
    bool            stream;         /* /audio: endless PCM from the audio ring */
    uint32_t        apos;           /* stream read position (samples) */
    int             idle;           /* tcp_poll ticks without traffic */
    bool            put;            /* POST /api/file: receiving a chunk into s_put_buf */
    bool            want_file;      /* a chunk is in, or a delete: done by net_poll() (flash) */
    uint32_t        clen, got;      /* body: Content-Length, bytes so far */
    char            fq[160];        /* the request's query */
} conn_t;

static conn_t          s_conns[MAX_CONN];
static uint8_t        *s_put_buf;     /* PSRAM, PUT_MAX; one upload at a time */
static conn_t         *s_put_owner;
void net_set_upload_buffer(uint8_t *buf) { s_put_buf = buf; }
static struct tcp_pcb *s_listen;
static char            s_ip[20];
static bool          s_setup;
static char          s_ap_ssid[33];
static dhcp_server_t s_dhcp;
static dns_server_t  s_dnss;
#define SCAN_MAX 20
static struct { char ssid[33]; int16_t rssi; } s_scan[SCAN_MAX];
static int s_nscan;

static const char SETUP_PAGE[] = "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\"><title>Set up Bedside</title>\n"
"<style>:root{--ink:#1c1b19;--sub:#6d6a64;--line:#e4e0d8;--bg:#f4f2ee;--red:#d0493c}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 -apple-system,system-ui,sans-serif;padding:22px 18px 40px}\n"
"main{max-width:460px;margin:0 auto}h1{font-size:24px;margin:0 0 4px}p{color:var(--sub);margin:0 0 18px}.card{background:#fff;border-radius:14px;padding:16px;box-shadow:0 1px 2px rgba(0,0,0,.06);margin-bottom:14px}\n"
"label{display:block;font-size:13px;color:var(--sub);margin:12px 0 6px}label:first-child{margin-top:0}input,select{width:100%;font:inherit;padding:11px 12px;border:1px solid var(--line);border-radius:10px;background:#fff;color:var(--ink)}\n"
"button{width:100%;font:600 16px inherit;padding:13px;border:0;border-radius:12px;background:var(--ink);color:#fff;margin-top:6px}.mark{display:flex;align-items:center;gap:10px;margin-bottom:16px;font-weight:700;font-size:20px}\n"
".tile{width:34px;height:34px;border-radius:10px;background:var(--ink);position:relative}.tile:before{content:\"\";position:absolute;left:50%;top:21%;width:29%;height:29%;background:var(--red);border-top-right-radius:30%}.tile:after{content:\"\";position:absolute;left:21%;top:50%;width:29%;height:29%;background:#fffdf8;border-bottom-left-radius:30%}\n"
".hint{font-size:13px;color:var(--sub);margin-top:8px}#done{display:none}</style></head><body><main>\n"
"<div class=mark><span class=tile></span>Tesserae</div>\n"
"<div id=form><h1>Set up your bedside panel</h1><p>Choose your WiFi, then where your Tesserae server is. The panel restarts and joins your network.</p>\n"
"<form id=f class=card><label for=ssid>WiFi network</label><select id=ssid></select><input id=ssid2 placeholder=\"Network name\" style=\"display:none;margin-top:8px\" autocapitalize=off autocomplete=off>\n"
"<label for=pass>Password</label><input id=pass type=password placeholder=\"Leave empty for an open network\" autocomplete=off>\n"
"<label for=server>Tesserae server</label><input id=server placeholder=\"http://192.168.1.50:8765\" autocapitalize=off autocomplete=off inputmode=url>\n"
"<div class=hint>Plain http, with the port. The panel can't resolve .local names.</div>\n"
"<label for=name>Panel name</label><input id=name placeholder=\"Bedside\">\n"
"<button>Save and connect</button></form></div>\n"
"<div id=done class=card><h1>Saved</h1><p id=dmsg></p><p>You can close this page. Your phone will go back to your usual WiFi.</p></div>\n"
"<script>\n"
"const $=s=>document.querySelector(s),enc=encodeURIComponent;let info={};\n"
"fetch('/api/setupinfo').then(r=>r.json()).then(j=>{info=j;const s=$('#ssid');\n"
" s.innerHTML=j.networks.map(n=>`<option ${n.ssid===j.ssid?'selected':''}>${n.ssid.replace(/</g,'&lt;')}</option>`).join('')+'<option value=\"\">Other…</option>';\n"
" if(!j.networks.length)s.value='';s.onchange=()=>{$('#ssid2').style.display=s.value?'none':'block'};s.onchange();\n"
" $('#server').value=j.server||'';$('#name').value=j.name||''}).catch(()=>{});\n"
"$('#f').onsubmit=async e=>{e.preventDefault();const ssid=$('#ssid').value||$('#ssid2').value.trim();if(!ssid)return;\n"
" const cmd=q=>fetch('/api/cmd?'+q,{method:'POST'});\n"
" await cmd('wifi='+enc(ssid+'|'+$('#pass').value));\n"
" const sv=$('#server').value.trim();if(sv&&sv!==info.server)await cmd('server='+enc(sv));\n"
" const nm=$('#name').value.trim();if(nm&&nm!==info.name)await cmd('name='+enc(nm));\n"
" $('#dmsg').textContent='The panel is restarting and joining \"'+ssid+'\".';$('#form').style.display='none';$('#done').style.display='block';\n"
" setTimeout(()=>cmd('reboot=1'),800)};\n"
"</script></main></body></html>";


static bool            s_up;
static int             s_frame_users;
static char            s_json[10240];   /* state JSON incl. bindings + action table */

bool        net_connected(void) { return s_up; }
const char *net_ip(void)        { return s_ip; }

int net_rssi(void)
{
    int32_t rssi = 0;
    if (!s_up) return 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    return (int)rssi;
}

/* ---- connection plumbing (lwIP context unless noted) ------------------------ */

static void conn_release(conn_t *c)
{
    if (s_put_owner == c) s_put_owner = NULL;
    if (c->is_frame && s_frame_users > 0) s_frame_users--;
    if (c->stream) audio_listener(-1);
    memset(c, 0, sizeof *c);
}

static void conn_close(conn_t *c)
{
    struct tcp_pcb *pcb = c->pcb;
    if (pcb) {
        tcp_arg(pcb, NULL); tcp_recv(pcb, NULL); tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL); tcp_poll(pcb, NULL, 0);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
    conn_release(c);
}

/* /audio: after the header, push whatever the ring holds, in batches of at
 * least ~46 ms so the stream is not a flood of tiny segments. */
#define STREAM_MIN 1024
static void stream_pump(conn_t *c)
{
    static int16_t tmp[2 * TCP_MSS / 2];
    while (c->pcb && c->sent < c->hdr_len) {
        size_t room = tcp_sndbuf(c->pcb), n = c->hdr_len - c->sent;
        if (!room) break;
        if (n > room) n = room;
        if (tcp_write(c->pcb, c->hdr + c->sent, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        c->sent += n;
    }
    while (c->pcb && c->sent >= c->hdr_len) {
        size_t room = tcp_sndbuf(c->pcb) / 2;
        if (room > sizeof tmp / 2) room = sizeof tmp / 2;
        if (room < STREAM_MIN / 2) break;
        uint32_t w = audio_write_pos();
        if (w - c->apos < STREAM_MIN && w - c->apos < audio_ring_samples()) break;
        uint32_t before = c->apos;
        size_t n = audio_read(&c->apos, tmp, room);
        if (!n) break;
        if (tcp_write(c->pcb, tmp, (u16_t)(n * 2), TCP_WRITE_FLAG_COPY) != ERR_OK) { c->apos = before; break; }
    }
    if (c->pcb) tcp_output(c->pcb);
}

static void conn_pump(conn_t *c)
{
    if (c->stream) { stream_pump(c); return; }
    size_t total = c->hdr_len + c->body_len;
    while (c->pcb && c->sent < total) {
        size_t room = tcp_sndbuf(c->pcb);
        if (room == 0) break;
        const uint8_t *src; size_t avail; u8_t flags;
        if (c->sent < c->hdr_len) {
            src = (const uint8_t *)c->hdr + c->sent; avail = c->hdr_len - c->sent; flags = TCP_WRITE_FLAG_COPY;
        } else {
            size_t off = c->sent - c->hdr_len;
            src = c->body + off; avail = c->body_len - off; flags = c->body_copy ? TCP_WRITE_FLAG_COPY : 0;
        }
        size_t n = avail < room ? avail : room;
        if (n > 2 * TCP_MSS) n = 2 * TCP_MSS;
        if (tcp_write(c->pcb, src, (u16_t)n, flags) != ERR_OK) break;
        c->sent += n;
    }
    if (c->pcb) tcp_output(c->pcb);
    if (c->pcb && c->sent >= total && c->acked >= total) conn_close(c);
}

static void respond(conn_t *c, int code, const char *ctype, const void *body, size_t len, bool copy)
{
    c->hdr_len = (size_t)snprintf(c->hdr, sizeof c->hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n",
        code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request", ctype, (unsigned)len);
    c->body = body; c->body_len = len; c->body_copy = copy;
    c->sent = c->acked = 0;
    conn_pump(c);
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Copy a URL-encoded token (up to '&' or end) into out, decoding %XX and '+'. */
static void url_decode(const char *src, char *out, size_t cap)
{
    size_t o = 0;
    while (*src && *src != '&' && o + 1 < cap) {
        if (*src == '%' && hexval(src[1]) >= 0 && hexval(src[2]) >= 0) {
            out[o++] = (char)(hexval(src[1]) * 16 + hexval(src[2])); src += 3;
        } else {
            out[o++] = (*src == '+') ? ' ' : *src; src++;
        }
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

static void handle_request(conn_t *c, size_t hdr_end)
{
    c->req[c->req_len] = 0;
    bool post = !strncmp(c->req, "POST ", 5);
    uint32_t clen = 0;                               /* Content-Length, from the headers only */
    for (const char *h = c->req; h && h < c->req + hdr_end; h = strstr(h, "\r\n")) {
        if (*h == '\r') h += 2;
        if (!strncasecmp(h, "Content-Length:", 15)) { clen = (uint32_t)strtoul(h + 15, NULL, 10); break; }
    }
    char *eol = strstr(c->req, "\r\n");
    if (eol) *eol = 0;
    char *sp = strchr(c->req, ' ');
    if (!sp) { respond(c, 400, "text/plain", "bad request", 11, true); return; }
    char *path = sp + 1;
    char *sp2 = strchr(path, ' ');
    if (sp2) *sp2 = 0;
    char *query = strchr(path, '?');
    if (query) *query++ = 0; else query = "";

    if (s_setup) {                                   /* setup mode: the setup page, or a redirect to it */
        if (!strcmp(path, "/") || !strcmp(path, "/index.html") || !strcmp(path, "/setup")) {
            respond(c, 200, "text/html; charset=utf-8", SETUP_PAGE, sizeof SETUP_PAGE - 1, false); return;
        }
        if (!strcmp(path, "/api/setupinfo")) {
            size_t n = (size_t)snprintf(s_json, sizeof s_json, "{\"ssid\":\"%s\",\"server\":\"%s\",\"name\":\"%s\",\"networks\":[",
                                        app_setup_ssid(), app_setup_server(), app_setup_name());
            for (int i = 0; i < s_nscan && n < sizeof s_json - 80; i++) {
                n += (size_t)snprintf(s_json + n, sizeof s_json - n, "%s{\"ssid\":\"", i ? "," : "");
                for (const char *p = s_scan[i].ssid; *p && n < sizeof s_json - 16; p++) {    /* JSON-escape */
                    if (*p == '"' || *p == '\\') s_json[n++] = '\\';
                    s_json[n++] = (*p >= 32) ? *p : ' ';
                }
                n += (size_t)snprintf(s_json + n, sizeof s_json - n, "\",\"rssi\":%d}", s_scan[i].rssi);
            }
            n += (size_t)snprintf(s_json + n, sizeof s_json - n, "]}");
            respond(c, 200, "application/json", s_json, n, true); return;
        }
        if (strcmp(path, "/api/cmd") && strcmp(path, "/api/log") && strcmp(path, "/api/state")) {
            c->hdr_len = (size_t)snprintf(c->hdr, sizeof c->hdr,
                "HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/\r\nContent-Length: 0\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
            c->body = NULL; c->body_len = 0; c->body_copy = false; c->sent = c->acked = 0;
            conn_pump(c); return;
        }
    }
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(c, 200, "text/html; charset=utf-8", www_index, (size_t)(www_index_end - www_index), false);
    } else if (!strcmp(path, "/api/state")) {
        size_t n = app_state_json(s_json, sizeof s_json);
        respond(c, 200, "application/json", s_json, n, true);
    } else if (!strcmp(path, "/api/log")) {        /* first line: where to read next; then the text */
        char v[16]; uint32_t from = query_get(query, "from", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0, next = 0;
        int h = snprintf(s_json, sizeof s_json, "%-10lu\n", 0ul);
        size_t n = app_log_read(from, s_json + h, sizeof s_json - (size_t)h, &next);
        snprintf(s_json, sizeof s_json, "%-10lu", (unsigned long)next); s_json[10] = '\n';
        respond(c, 200, "text/plain; charset=utf-8", s_json, (size_t)h + n, true);
    } else if (!strcmp(path, "/api/cmd")) {
        char name[24] = "", value[200] = "";
        const char *eq = strchr(query, '=');
        if (eq && (size_t)(eq - query) < sizeof name) {
            memcpy(name, query, (size_t)(eq - query)); name[eq - query] = 0;
            url_decode(eq + 1, value, sizeof value);
        } else {
            url_decode(query, name, sizeof name);
        }
        app_command(name, value);
        respond(c, 200, "application/json", "{\"ok\":true}", 11, true);
    } else if (!strcmp(path, "/audio")) {
        /* Raw signed 16-bit little-endian mono PCM, forever. */
        c->hdr_len = (size_t)snprintf(c->hdr, sizeof c->hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nX-Audio-Rate: %d\r\n"
            "X-Audio-Format: s16le-mono\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", AUDIO_SR);
        c->sent = 0;
        c->stream = true;
        c->apos = audio_write_pos();
        audio_listener(+1);
        stream_pump(c);
    } else if (!strcmp(path, "/api/files")) {
        size_t n = store_json(s_json, sizeof s_json);
        respond(c, 200, "application/json", s_json, n, true);
    } else if (!strcmp(path, "/api/file") && !post) {      /* download, straight from flash */
        char name[STORE_NAME_MAX];
        const store_ent_t *e = query_get(query, "name", name, sizeof name) ? store_find(name) : NULL;
        if (!e) { respond(c, 404, "text/plain", "no such file", 12, true); return; }
        respond(c, 200, "application/octet-stream", store_data(e), e->size, false);
    } else if (!strcmp(path, "/api/file")) {               /* upload a chunk, or ?delete=1 */
        snprintf(c->fq, sizeof c->fq, "%s", query);
        if (strstr(query, "delete=1")) { c->want_file = true; return; }
        if (!s_put_buf || clen > PUT_MAX) { respond(c, 400, "application/json", "{\"ok\":false,\"error\":\"chunk too big\"}", 37, true); return; }
        if (s_put_owner && s_put_owner != c) { respond(c, 400, "application/json", "{\"ok\":false,\"error\":\"another upload is running\"}", 48, true); return; }
        s_put_owner = c; c->put = true; c->clen = clen; c->got = 0;
        if (!clen) c->want_file = true;
    } else if (!strcmp(path, "/frame")) {
        char v[8];
        int sc = query_get(query, "s", v, sizeof v) ? atoi(v) : 4;
        if (sc < 2) sc = 2;
        if (sc > 8) sc = 8;
        c->want_frame = true;           /* rendered + answered by net_poll() */
        c->frame_scale = sc;
    } else {
        respond(c, 404, "text/plain", "not found", 9, true);
    }
}

/* Upload body bytes from p (from offset skip) into the chunk buffer. */
static void put_take(conn_t *c, struct pbuf *p, u16_t skip)
{
    if (c->want_file || skip >= p->tot_len) return;
    uint32_t n = p->tot_len - skip;
    if (n > c->clen - c->got) n = c->clen - c->got;
    pbuf_copy_partial(p, s_put_buf + c->got, (u16_t)n, skip);
    c->got += n;
    if (c->got >= c->clen) c->want_file = true;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    conn_t *c = arg;
    (void)err;
    if (!p) {                                   /* peer closed */
        if (c) conn_close(c); else tcp_close(pcb);
        return ERR_OK;
    }
    if (c) {
        c->idle = 0;
        if (c->put) {
            put_take(c, p, 0);
        } else if (c->body == NULL && !c->want_frame && !c->stream && !c->want_file) {
            size_t room = REQ_MAX - 1 - c->req_len;
            size_t n = p->tot_len < room ? p->tot_len : room;
            pbuf_copy_partial(p, c->req + c->req_len, (u16_t)n, 0);
            c->req_len += n;
            c->req[c->req_len] = 0;
            char *e = strstr(c->req, "\r\n\r\n");
            if (e || c->req_len >= REQ_MAX - 1) {
                size_t hdr_end = e ? (size_t)(e + 4 - c->req) : c->req_len, have = c->req_len;
                handle_request(c, hdr_end);
                if (c->put) {                       /* the body's start came with the headers */
                    size_t b = have - hdr_end;
                    if (b > c->clen) b = c->clen;
                    memcpy(s_put_buf, c->req + hdr_end, b);
                    c->got = (uint32_t)b;
                    if (c->got >= c->clen) c->want_file = true;
                    put_take(c, p, (u16_t)n);
                }
            }
        }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    conn_t *c = arg;
    if (!c) return ERR_OK;
    c->idle = 0;
    c->acked += len;
    conn_pump(c);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)err;
    conn_t *c = arg;
    if (c) { c->pcb = NULL; conn_release(c); }   /* pcb already freed by lwIP */
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb)
{
    conn_t *c = arg;
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }
    if (++c->idle > 8) { conn_close(c); return ERR_ABRT; }   /* ~16 s idle */
    if (c->body || c->stream) conn_pump(c);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    conn_t *c = NULL;
    for (int i = 0; i < MAX_CONN; i++) if (!s_conns[i].pcb) { c = &s_conns[i]; break; }
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }
    memset(c, 0, sizeof *c);
    c->pcb = pcb;
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 4);                  /* every 2 s */
    return ERR_OK;
}

/* ---- public ----------------------------------------------------------------- */

static bool s_cyw43_up;

void net_mac(char *out, size_t n)
{
    uint8_t mac[6] = {0};
    if (s_cyw43_up) cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Called every 50 ms while the WiFi join is in progress (the boot screen
 * repaints its log from it). */
static void (*s_wait_hook)(void);
void net_set_wait_hook(void (*fn)(void)) { s_wait_hook = fn; }

static int join(const char *ssid, const char *pass, uint32_t auth, uint32_t timeout_ms)
{
    if (cyw43_arch_wifi_connect_async(ssid, pass, auth)) return -1;
    absolute_time_t until = make_timeout_time_ms(timeout_ms);
    while (!time_reached(until)) {
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (st == CYW43_LINK_UP) return 0;
        if (st == CYW43_LINK_FAIL || st == CYW43_LINK_NONET || st == CYW43_LINK_BADAUTH) return st;
        if (s_wait_hook) s_wait_hook();
        sleep_ms(50);
    }
    return PICO_ERROR_TIMEOUT;
}

/* ---- setup mode: the panel's own WiFi network and a setup page -------------------
 * A phone that joins "Bedside-setup-xxxx" gets an address from the DHCP server,
 * every name resolves to the panel (DNS server), and every path but the setup
 * page and its two endpoints redirects to it, so the phone's captive-portal
 * check opens it by itself. The page saves through /api/cmd (wifi=, server=,
 * name=) and restarts the panel. */
static int on_scan(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;
    if (!r || !r->ssid_len || r->ssid_len > 32) return 0;
    char name[33]; memcpy(name, r->ssid, r->ssid_len); name[r->ssid_len] = 0;
    for (int i = 0; i < s_nscan; i++) if (!strcmp(s_scan[i].ssid, name)) { if (r->rssi > s_scan[i].rssi) s_scan[i].rssi = r->rssi; return 0; }
    if (s_nscan < SCAN_MAX) { snprintf(s_scan[s_nscan].ssid, sizeof s_scan[0].ssid, "%s", name); s_scan[s_nscan].rssi = r->rssi; s_nscan++; }
    return 0;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
bool net_setup_active(void) { return s_setup; }
const char *net_setup_ssid(void) { return s_ap_ssid; }

bool net_start_setup(const char *ap_ssid)
{
    if (!s_cyw43_up) {
        if (cyw43_arch_init()) { printf("net: cyw43 init failed\n"); return false; }
        s_cyw43_up = true;
    }
    /* Nearby networks first, for the page's list (the radio scans as a station). */
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_scan_options_t opts = { 0 };
    s_nscan = 0;
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, on_scan) == 0) {
        absolute_time_t until = make_timeout_time_ms(8000);
        while (cyw43_wifi_scan_active(&cyw43_state) && !time_reached(until)) { if (s_wait_hook) s_wait_hook(); sleep_ms(50); }
    }
    for (int i = 1; i < s_nscan; i++)                   /* strongest first */
        for (int j = i; j > 0 && s_scan[j].rssi > s_scan[j - 1].rssi; j--) { __typeof__(s_scan[0]) t = s_scan[j]; s_scan[j] = s_scan[j - 1]; s_scan[j - 1] = t; }
    printf("net: %d WiFi network(s) nearby\n", s_nscan);

    snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s", ap_ssid);
    cyw43_arch_enable_ap_mode(s_ap_ssid, NULL, CYW43_AUTH_OPEN);
    ip_addr_t gw, mask;
    IP4_ADDR(ip_2_ip4(&gw), 192, 168, 4, 1);
    IP4_ADDR(ip_2_ip4(&mask), 255, 255, 255, 0);
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_AP];
    cyw43_arch_lwip_begin();
    netif_set_addr(nif, ip_2_ip4(&gw), ip_2_ip4(&mask), ip_2_ip4(&gw));
    dhcp_server_init(&s_dhcp, nif, &gw, &mask);
    dns_server_init(&s_dnss, nif, &gw);
    if (!s_listen) {
        struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
        if (pcb && tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) == ERR_OK) { s_listen = tcp_listen_with_backlog(pcb, 4); tcp_accept(s_listen, on_accept); }
    }
    cyw43_arch_lwip_end();
    snprintf(s_ip, sizeof s_ip, "192.168.4.1");
    s_setup = true;
    printf("net: setup mode: join \"%s\", then open http://192.168.4.1/\n", s_ap_ssid);
    return s_listen != NULL;
}

bool net_init(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) { printf("net: no WiFi credentials (include/secrets.h or the web UI)\n"); return false; }
    if (!s_cyw43_up) {
        if (cyw43_arch_init()) {
            printf("net: cyw43 init failed\n");
            return false;
        }
        s_cyw43_up = true;
    }
    cyw43_arch_enable_sta_mode();
    bool open_net = (pass == NULL || pass[0] == '\0');
    uint32_t auth = open_net ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
    printf("net: connecting to \"%s\"...\n", ssid);
    int rc = join(ssid, open_net ? NULL : pass, auth, 30000);
#ifdef CYW43_AUTH_WPA3_WPA2_AES_PSK
    if (rc && !open_net) {
        printf("net: WPA2 join failed (%d), trying WPA3/WPA2\n", rc);
        rc = join(ssid, pass, CYW43_AUTH_WPA3_WPA2_AES_PSK, 30000);
    }
#endif
    if (rc) {
        printf("net: connect failed (%d); web UI unavailable\n", rc);
        return false;
    }
    snprintf(s_ip, sizeof s_ip, "%s", ip4addr_ntoa(netif_ip4_addr(netif_default)));

    cyw43_arch_lwip_begin();
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb && tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) == ERR_OK) {
        s_listen = tcp_listen_with_backlog(pcb, 4);
        tcp_accept(s_listen, on_accept);
    }
    cyw43_arch_lwip_end();
    if (!s_listen) { printf("net: listen failed\n"); return false; }

    s_up = true;
    printf("net: connected, web UI at http://%s/\n", s_ip);
    return true;
}

void net_poll(void)
{
    if (!s_up) return;
    cyw43_arch_poll();                          /* no-op for the background arch */

    if (audio_listeners() > 0) {
        cyw43_arch_lwip_begin();
        for (int i = 0; i < MAX_CONN; i++) if (s_conns[i].pcb && s_conns[i].stream) stream_pump(&s_conns[i]);
        cyw43_arch_lwip_end();
    }

    /* An upload chunk or a delete: flash work, so here in main context. */
    {
        conn_t *fc = NULL; struct tcp_pcb *fpcb = NULL;
        cyw43_arch_lwip_begin();
        for (int i = 0; i < MAX_CONN; i++) if (s_conns[i].pcb && s_conns[i].want_file) { fc = &s_conns[i]; fpcb = fc->pcb; break; }
        cyw43_arch_lwip_end();
        if (fc) {
            static char res[200];
            char name[STORE_NAME_MAX] = "", v[16], err[96] = "";
            query_get(fc->fq, "name", name, sizeof name);
            int r;
            if (strstr(fc->fq, "delete=1")) r = store_delete(name) ? 1 : (snprintf(err, sizeof err, "no such file"), -1);
            else {
                uint32_t off = query_get(fc->fq, "off", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0;
                uint32_t total = query_get(fc->fq, "total", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0;
                r = store_put(name, off, total, s_put_buf, fc->got, err, sizeof err);
            }
            int n = r < 0 ? snprintf(res, sizeof res, "{\"ok\":false,\"error\":\"%s\"}", err)
                          : snprintf(res, sizeof res, "{\"ok\":true,\"done\":%s}", r ? "true" : "false");
            cyw43_arch_lwip_begin();
            if (fc->pcb == fpcb && fc->want_file) {
                fc->want_file = false; fc->put = false;
                if (s_put_owner == fc) s_put_owner = NULL;
                respond(fc, r < 0 ? 400 : 200, "application/json", res, (size_t)n, true);
            }
            cyw43_arch_lwip_end();
        }
    }

    /* Serve at most one deferred frame per call: render in main context, hand
     * over under the lwIP lock. */
    conn_t *target = NULL; int scale = 4; struct tcp_pcb *pcb = NULL;
    cyw43_arch_lwip_begin();
    for (int i = 0; i < MAX_CONN; i++) {
        if (s_conns[i].pcb && s_conns[i].want_frame) { target = &s_conns[i]; scale = target->frame_scale; pcb = target->pcb; break; }
    }
    cyw43_arch_lwip_end();
    if (!target) return;

    size_t len = 0;
    const uint8_t *buf = app_render_preview(s_frame_users == 0 ? scale : 0, &len);

    cyw43_arch_lwip_begin();
    if (target->pcb == pcb && target->want_frame) {
        target->want_frame = false;
        if (buf && len) {
            target->is_frame = true;
            s_frame_users++;
            respond(target, 200, "application/octet-stream", buf, len, false);
        } else {
            respond(target, 400, "text/plain", "no preview", 10, true);
        }
    }
    cyw43_arch_lwip_end();
}

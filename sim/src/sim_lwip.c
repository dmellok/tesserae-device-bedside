/*
 * sim_lwip.c: lwIP's DNS, altcp (TCP) and MQTT client on the host's sockets
 * (see include/sim_lwip.h).
 *
 * Everything runs on the main thread: the firmware waits for its callbacks in
 * sleep_ms() (net_rest.c's request loop) or polls (cyw43_arch_poll), and both
 * end up in sim_lwip_pump(), which polls the sockets and calls back.
 *
 * Time warp: the wall clock comes from the Date header of the server's replies
 * (net_rest.c). When the simulator's clock has been jumped forward, the header
 * is moved forward by the same amount on its way in, so the server keeps the
 * clock where the jump put it.
 */
#include "sim.h"
#include "sim_lwip.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

cyw43_t cyw43_state;

/* ---- addresses -------------------------------------------------------------------- */

int ipaddr_aton(const char *cp, ip_addr_t *addr)
{
    struct in_addr a;
    if (!cp || inet_pton(AF_INET, cp, &a) != 1) return 0;
    if (addr) addr->addr = a.s_addr;
    return 1;
}

char *ip4addr_ntoa(const ip4_addr_t *addr)
{
    static char b[16];
    struct in_addr a = { .s_addr = addr->addr };
    inet_ntop(AF_INET, &a, b, sizeof b);
    return b;
}

const char *sim_lan_ip(void)
{
    static char ip[16];
    if (ip[0]) return ip;
    snprintf(ip, sizeof ip, "127.0.0.1");
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return ip;
    for (int pass = 0; pass < 2; pass++)              /* en0 first (WiFi / Ethernet), then anything up */
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
            if (pass == 0 && strcmp(i->ifa_name, "en0")) continue;
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
            freeifaddrs(ifs);
            return ip;
        }
    freeifaddrs(ifs);
    return ip;
}

/* ---- pbufs ------------------------------------------------------------------------ */

static struct pbuf *pbuf_make(const void *d, size_t n)
{
    struct pbuf *p = malloc(sizeof *p + n);
    if (!p) return NULL;
    p->next = NULL; p->payload = p + 1; p->len = p->tot_len = (u16_t)n;
    memcpy(p->payload, d, n);
    return p;
}
u8_t pbuf_free(struct pbuf *p) { u8_t n = 0; while (p) { struct pbuf *x = p->next; free(p); p = x; n++; } return n; }
u16_t pbuf_copy_partial(const struct pbuf *p, void *data, u16_t len, u16_t off)
{
    u16_t done = 0;
    for (; p && done < len; p = p->next) {
        if (off >= p->len) { off -= p->len; continue; }
        u16_t n = (u16_t)(p->len - off < len - done ? p->len - off : len - done);
        memcpy((uint8_t *)data + done, (uint8_t *)p->payload + off, n);
        done += n; off = 0;
    }
    return done;
}

/* ---- sockets ------------------------------------------------------------------------ */

static int tcp_open(const ip_addr_t *ip, u16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip->addr };
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0 && errno != EINPROGRESS) { close(fd); return -1; }
    return fd;
}

static bool connect_done(int fd, bool *ok)
{
    int e = 0; socklen_t l = sizeof e;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
    *ok = e == 0;
    return true;
}

typedef struct { uint8_t *b; size_t n, cap; } buf_t;
static bool buf_add(buf_t *t, const void *d, size_t n)
{
    if (t->n + n > t->cap) {
        size_t c = t->cap ? t->cap : 4096;
        while (c < t->n + n) c *= 2;
        uint8_t *nb = realloc(t->b, c);
        if (!nb) return false;
        t->b = nb; t->cap = c;
    }
    memcpy(t->b + t->n, d, n); t->n += n;
    return true;
}
static void buf_drop(buf_t *t, size_t n) { memmove(t->b, t->b + n, t->n - n); t->n -= n; }

/* The time warp, applied to an HTTP reply's Date header (same length, in place). */
static void warp_date(char *d, size_t n)
{
    int64_t w = sim_warp_total_s();
    if (!w) return;
    char *h = NULL;
    for (size_t i = 0; i + 36 < n; i++) if (!strncasecmp(d + i, "\r\nDate: ", 8)) { h = d + i + 8; break; }
    if (!h) return;
    char v[30]; memcpy(v, h, 29); v[29] = 0;
    struct tm tm = { 0 };
    if (!strptime(v, "%a, %d %b %Y %H:%M:%S GMT", &tm)) return;
    time_t t = timegm(&tm) + (time_t)w;
    gmtime_r(&t, &tm);
    char o[32];
    if (strftime(o, sizeof o, "%a, %d %b %Y %H:%M:%S GMT", &tm) == 29) memcpy(h, o, 29);
}

/* ---- DNS ----------------------------------------------------------------------------- */

#define DNS_PEND 8
static struct { dns_found_callback cb; void *arg; char name[128]; bool on; } s_dns[DNS_PEND];

static bool resolve(const char *name, ip_addr_t *out)
{
    if (ipaddr_aton(name, out)) return true;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res) return false;
    out->addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

err_t dns_gethostbyname(const char *name, ip_addr_t *addr, dns_found_callback found, void *arg)
{
    if (resolve(name, addr)) return ERR_OK;
    for (int i = 0; i < DNS_PEND; i++) if (!s_dns[i].on) {      /* not found: say so from the pump, as lwIP would */
        s_dns[i].on = true; s_dns[i].cb = found; s_dns[i].arg = arg;
        snprintf(s_dns[i].name, sizeof s_dns[i].name, "%s", name);
        return ERR_INPROGRESS;
    }
    return ERR_MEM;
}

/* ---- altcp ----------------------------------------------------------------------------- */

struct altcp_pcb {
    int fd;
    bool connecting, eof, dead, rx_seen;
    void *arg;
    altcp_connected_fn on_conn;
    altcp_recv_fn on_recv;
    altcp_sent_fn on_sent;
    altcp_err_fn on_err;
    altcp_poll_fn on_poll; uint32_t poll_ms, polled_ms;
    buf_t tx;
    struct altcp_pcb *next;
};
static struct altcp_pcb *s_pcbs;
struct altcp_tls_config { int unused; };

struct altcp_pcb *altcp_tcp_new_ip_type(u8_t t)
{
    (void)t;
    struct altcp_pcb *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->fd = -1;
    p->next = s_pcbs; s_pcbs = p;
    return p;
}

struct altcp_pcb *altcp_tls_new(struct altcp_tls_config *c, u8_t t)
{
    (void)c; (void)t;
    static bool said;
    if (!said) { printf("sim: https is not simulated (no TLS): GitHub releases and https streams can't be reached\n"); said = true; }
    return NULL;
}
struct altcp_tls_config *altcp_tls_create_config_client(const u8_t *cert, size_t len) { (void)cert; (void)len; static struct altcp_tls_config c; return &c; }
void *altcp_tls_context(struct altcp_pcb *p) { (void)p; return NULL; }

void altcp_arg(struct altcp_pcb *p, void *arg)            { if (p) p->arg = arg; }
void altcp_recv(struct altcp_pcb *p, altcp_recv_fn f)     { if (p) p->on_recv = f; }
void altcp_sent(struct altcp_pcb *p, altcp_sent_fn f)     { if (p) p->on_sent = f; }
void altcp_err(struct altcp_pcb *p, altcp_err_fn f)       { if (p) p->on_err = f; }
void altcp_poll(struct altcp_pcb *p, altcp_poll_fn f, u8_t iv) { if (p) { p->on_poll = f; p->poll_ms = (uint32_t)iv * 500u; p->polled_ms = to_ms_since_boot(get_absolute_time()); } }
void altcp_nagle_disable(struct altcp_pcb *p)             { (void)p; }
void altcp_recved(struct altcp_pcb *p, u16_t len)         { (void)p; (void)len; }

err_t altcp_connect(struct altcp_pcb *p, const ip_addr_t *ip, u16_t port, altcp_connected_fn cb)
{
    p->fd = tcp_open(ip, port);
    if (p->fd < 0) return ERR_RTE;
    p->connecting = true; p->on_conn = cb;
    return ERR_OK;
}

err_t altcp_write(struct altcp_pcb *p, const void *d, u16_t len, u8_t flags)
{
    (void)flags;
    if (!p || p->dead) return ERR_CLSD;
    return buf_add(&p->tx, d, len) ? ERR_OK : ERR_MEM;
}
err_t altcp_output(struct altcp_pcb *p) { (void)p; return ERR_OK; }    /* the pump sends */
u16_t altcp_sndbuf(struct altcp_pcb *p)
{
    if (!p || p->connecting) return p && p->connecting ? 0 : TCP_SND_BUF;
    return p->tx.n >= TCP_SND_BUF ? 0 : (u16_t)(TCP_SND_BUF - p->tx.n);
}

static void pcb_kill(struct altcp_pcb *p)
{
    if (p->fd >= 0) close(p->fd);
    p->fd = -1; p->dead = true;
    p->on_recv = NULL; p->on_sent = NULL; p->on_err = NULL; p->on_conn = NULL; p->on_poll = NULL;
}
err_t altcp_close(struct altcp_pcb *p) { if (p) pcb_kill(p); return ERR_OK; }
void  altcp_abort(struct altcp_pcb *p) { if (p) pcb_kill(p); }

/* An error frees the pcb in lwIP: the callback is told, then it is gone. */
static void pcb_fail(struct altcp_pcb *p, err_t e)
{
    altcp_err_fn f = p->on_err; void *a = p->arg;
    pcb_kill(p);
    if (f) f(a, e);
}

static void pcb_service(struct altcp_pcb *p, short rev)
{
    if (p->dead || p->fd < 0) return;
    if (p->connecting) {
        if (!(rev & (POLLOUT | POLLERR | POLLHUP))) return;
        bool ok; connect_done(p->fd, &ok);
        p->connecting = false;
        if (!ok) { pcb_fail(p, ERR_RST); return; }
        if (p->on_conn) p->on_conn(p->arg, p, ERR_OK);
        if (p->dead) return;
    }
    if (p->tx.n && (rev & POLLOUT)) {
        ssize_t n = send(p->fd, p->tx.b, p->tx.n, 0);
        if (n > 0) { buf_drop(&p->tx, (size_t)n); if (p->on_sent) p->on_sent(p->arg, p, (u16_t)n); if (p->dead) return; }
        else if (n < 0 && errno != EAGAIN) { pcb_fail(p, ERR_RST); return; }
    }
    if (!p->eof && (rev & (POLLIN | POLLHUP))) {
        uint8_t b[16384];
        ssize_t n = recv(p->fd, b, sizeof b, 0);
        if (n > 0) {
            if (!p->rx_seen) { p->rx_seen = true; warp_date((char *)b, (size_t)n); }
            for (ssize_t off = 0; off < n && !p->dead; off += 65535) {       /* pbufs carry 16-bit lengths */
                size_t m = (size_t)(n - off) < 65535 ? (size_t)(n - off) : 65535;
                struct pbuf *pb = pbuf_make(b + off, m);
                if (p->on_recv) p->on_recv(p->arg, p, pb, ERR_OK); else pbuf_free(pb);
            }
        } else if (n == 0) {
            p->eof = true;
            if (p->on_recv) p->on_recv(p->arg, p, NULL, ERR_OK);           /* the far end closed */
        } else if (errno != EAGAIN) pcb_fail(p, ERR_RST);
    }
}

/* ---- MQTT 3.1.1 ------------------------------------------------------------------------- */

struct mqtt_client_s {
    int fd, state;                       /* 0 off, 1 TCP connecting, 2 waiting for CONNACK, 3 connected */
    mqtt_connection_cb_t cb; void *arg;
    mqtt_incoming_publish_cb_t pub_cb; mqtt_incoming_data_cb_t data_cb; void *in_arg;
    buf_t tx, rx;
    u16_t keep_alive, pkt_id;
    uint32_t t_start, t_tx;
    bool freed;
    struct mqtt_client_s *next;
};
static struct mqtt_client_s *s_mqtt;

static void put16(buf_t *b, unsigned v) { uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v }; buf_add(b, x, 2); }
static void putstr(buf_t *b, const char *s) { size_t n = s ? strlen(s) : 0; put16(b, (unsigned)n); if (n) buf_add(b, s, n); }
static void packet(mqtt_client_t *c, uint8_t type, const buf_t *body)
{
    uint8_t h[5]; int n = 0; size_t len = body ? body->n : 0;
    h[n++] = type;
    do { uint8_t d = len & 0x7F; len >>= 7; if (len) d |= 0x80; h[n++] = d; } while (len && n < 5);
    buf_add(&c->tx, h, (size_t)n);
    if (body && body->n) buf_add(&c->tx, body->b, body->n);
    c->t_tx = to_ms_since_boot(get_absolute_time());
}

mqtt_client_t *mqtt_client_new(void)
{
    mqtt_client_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = -1;
    c->next = s_mqtt; s_mqtt = c;
    return c;
}

static void mqtt_drop(mqtt_client_t *c, mqtt_connection_status_t why, bool tell)
{
    bool was = c->state != 0;
    if (c->fd >= 0) close(c->fd);
    c->fd = -1; c->state = 0; c->tx.n = c->rx.n = 0;
    if (tell && was && c->cb) c->cb(c, c->arg, why);
}

void mqtt_client_free(mqtt_client_t *c) { if (c) { mqtt_drop(c, MQTT_CONNECT_DISCONNECTED, false); c->freed = true; } }
u8_t mqtt_client_is_connected(mqtt_client_t *c) { return c && c->state == 3; }
void mqtt_set_inpub_callback(mqtt_client_t *c, mqtt_incoming_publish_cb_t p, mqtt_incoming_data_cb_t d, void *arg)
{
    c->pub_cb = p; c->data_cb = d; c->in_arg = arg;
}

err_t mqtt_client_connect(mqtt_client_t *c, const ip_addr_t *ip, u16_t port, mqtt_connection_cb_t cb, void *arg,
                          const struct mqtt_connect_client_info_t *ci)
{
    if (c->state) return ERR_ISCONN;
    c->fd = tcp_open(ip, port);
    if (c->fd < 0) return ERR_RTE;
    c->cb = cb; c->arg = arg; c->keep_alive = ci->keep_alive; c->state = 1;
    c->t_start = to_ms_since_boot(get_absolute_time());
    buf_t b = { 0 };
    uint8_t flags = 0x02;                                            /* clean session */
    if (ci->will_topic) flags |= 0x04 | (uint8_t)((ci->will_qos & 3) << 3) | (ci->will_retain ? 0x20 : 0);
    if (ci->client_user) flags |= 0x80;
    if (ci->client_pass) flags |= 0x40;
    putstr(&b, "MQTT");
    uint8_t lv[2] = { 4, flags }; buf_add(&b, lv, 2);
    put16(&b, ci->keep_alive);
    putstr(&b, ci->client_id);
    if (ci->will_topic) { putstr(&b, ci->will_topic); put16(&b, ci->will_msg_len); buf_add(&b, ci->will_msg, ci->will_msg_len); }
    if (ci->client_user) putstr(&b, ci->client_user);
    if (ci->client_pass) putstr(&b, ci->client_pass);
    packet(c, 0x10, &b);
    free(b.b);
    return ERR_OK;
}

void mqtt_disconnect(mqtt_client_t *c)
{
    if (!c || c->fd < 0) return;
    if (c->state == 3) { uint8_t d[2] = { 0xE0, 0 }; send(c->fd, d, 2, 0); }
    mqtt_drop(c, MQTT_CONNECT_DISCONNECTED, false);
}

err_t mqtt_publish(mqtt_client_t *c, const char *topic, const void *payload, u16_t len, u8_t qos, u8_t retain,
                   mqtt_request_cb_t cb, void *arg)
{
    if (!mqtt_client_is_connected(c)) return ERR_CONN;
    buf_t b = { 0 };
    putstr(&b, topic);
    if (qos) put16(&b, ++c->pkt_id ? c->pkt_id : ++c->pkt_id);
    if (len) buf_add(&b, payload, len);
    packet(c, (uint8_t)(0x30 | ((qos & 3) << 1) | (retain ? 1 : 0)), &b);
    free(b.b);
    if (cb) cb(arg, ERR_OK);
    return ERR_OK;
}

err_t mqtt_sub_unsub(mqtt_client_t *c, const char *topic, u8_t qos, mqtt_request_cb_t cb, void *arg, u8_t sub)
{
    if (!mqtt_client_is_connected(c)) return ERR_CONN;
    buf_t b = { 0 };
    put16(&b, ++c->pkt_id ? c->pkt_id : ++c->pkt_id);
    putstr(&b, topic);
    if (sub) { uint8_t q = qos & 3; buf_add(&b, &q, 1); }
    packet(c, sub ? 0x82 : 0xA2, &b);
    free(b.b);
    if (cb) cb(arg, ERR_OK);
    return ERR_OK;
}

/* One whole packet at the front of rx: handle it; false if more bytes are needed. */
static bool mqtt_take(mqtt_client_t *c)
{
    size_t len = 0, i = 1; int shift = 0;
    for (;; i++) {
        if (i >= c->rx.n) return false;
        len |= (size_t)(c->rx.b[i] & 0x7F) << shift; shift += 7;
        if (!(c->rx.b[i] & 0x80)) break;
        if (i >= 4) { mqtt_drop(c, MQTT_CONNECT_DISCONNECTED, true); return false; }
    }
    size_t hdr = i + 1;
    if (c->rx.n < hdr + len) return false;
    uint8_t type = c->rx.b[0];
    const uint8_t *d = c->rx.b + hdr;
    if ((type & 0xF0) == 0x20 && len >= 2) {                         /* CONNACK */
        if (d[1] == 0) { c->state = 3; if (c->cb) c->cb(c, c->arg, MQTT_CONNECT_ACCEPTED); }
        else { mqtt_connection_status_t st = (mqtt_connection_status_t)d[1]; mqtt_drop(c, st, false); if (c->cb) c->cb(c, c->arg, st); return false; }
    } else if ((type & 0xF0) == 0x30 && len >= 2) {                  /* PUBLISH */
        size_t tl = (size_t)(d[0] << 8 | d[1]), p = 2 + tl;
        int qos = (type >> 1) & 3;
        if (p + (qos ? 2 : 0) <= len) {
            char topic[256];
            snprintf(topic, sizeof topic, "%.*s", (int)tl, (const char *)d + 2);
            if (qos) { uint8_t ack[4] = { 0x40, 2, d[p], d[p + 1] }; buf_add(&c->tx, ack, 4); p += 2; }
            size_t pl = len - p;
            if (c->pub_cb) c->pub_cb(c->in_arg, topic, (u32_t)pl);
            if (c->data_cb) {
                const uint8_t *q = d + p;
                do { u16_t n = (u16_t)(pl > 1024 ? 1024 : pl); pl -= n;    /* lwIP hands data over in pieces too */
                     c->data_cb(c->in_arg, q, n, pl ? 0 : MQTT_DATA_FLAG_LAST); q += n; } while (pl);
            }
        }
    }                                                                /* SUBACK, PUBACK, PINGRESP: nothing to do */
    if (c->fd >= 0) buf_drop(&c->rx, hdr + len);
    return c->fd >= 0;
}

static void mqtt_service(mqtt_client_t *c, short rev)
{
    if (c->fd < 0 || c->freed) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (c->state == 1) {
        if (rev & (POLLOUT | POLLERR | POLLHUP)) {
            bool ok; connect_done(c->fd, &ok);
            if (!ok) { mqtt_drop(c, MQTT_CONNECT_DISCONNECTED, false); if (c->cb) c->cb(c, c->arg, MQTT_CONNECT_TIMEOUT); return; }
            c->state = 2;
        } else if (now - c->t_start > 10000) { mqtt_drop(c, MQTT_CONNECT_TIMEOUT, false); if (c->cb) c->cb(c, c->arg, MQTT_CONNECT_TIMEOUT); return; }
        else return;
    }
    if (c->state == 2 && now - c->t_start > 10000) { mqtt_drop(c, MQTT_CONNECT_TIMEOUT, false); if (c->cb) c->cb(c, c->arg, MQTT_CONNECT_TIMEOUT); return; }
    if (c->state == 3 && c->keep_alive && now - c->t_tx > c->keep_alive * 500u) packet(c, 0xC0, NULL);   /* PINGREQ */
    if (c->tx.n) {
        ssize_t n = send(c->fd, c->tx.b, c->tx.n, 0);
        if (n > 0) buf_drop(&c->tx, (size_t)n);
        else if (n < 0 && errno != EAGAIN) { mqtt_drop(c, MQTT_CONNECT_DISCONNECTED, true); return; }
    }
    if (rev & (POLLIN | POLLHUP)) {
        uint8_t b[8192];
        ssize_t n = recv(c->fd, b, sizeof b, 0);
        if (n > 0) { buf_add(&c->rx, b, (size_t)n); while (c->fd >= 0 && mqtt_take(c)) {} }
        else if (n == 0 || errno != EAGAIN) mqtt_drop(c, MQTT_CONNECT_DISCONNECTED, true);
    }
}

/* ---- the pump ---------------------------------------------------------------------------- */

void sim_lwip_pump(uint32_t wait_ms)
{
    static int depth;
    if (depth) { if (wait_ms) usleep(wait_ms * 1000u); return; }      /* a callback that sleeps */
    depth++;

    for (int i = 0; i < DNS_PEND; i++) if (s_dns[i].on) {
        s_dns[i].on = false;
        printf("sim: no address for \"%s\"\n", s_dns[i].name);
        if (s_dns[i].cb) s_dns[i].cb(s_dns[i].name, NULL, s_dns[i].arg);
    }

    struct pollfd fds[64]; void *who[64]; bool is_mqtt[64]; int n = 0;
    for (struct altcp_pcb *p = s_pcbs; p && n < 64; p = p->next) {
        if (p->dead || p->fd < 0) continue;
        fds[n] = (struct pollfd){ .fd = p->fd, .events = (short)((p->eof ? 0 : POLLIN) | (p->connecting || p->tx.n ? POLLOUT : 0)) };
        who[n] = p; is_mqtt[n] = false; n++;
    }
    for (mqtt_client_t *c = s_mqtt; c && n < 64; c = c->next) {
        if (c->freed || c->fd < 0) continue;
        fds[n] = (struct pollfd){ .fd = c->fd, .events = (short)(POLLIN | (c->state == 1 || c->tx.n ? POLLOUT : 0)) };
        who[n] = c; is_mqtt[n] = true; n++;
    }
    if (n) poll(fds, (nfds_t)n, (int)wait_ms);
    else if (wait_ms) usleep(wait_ms * 1000u);

    for (int i = 0; i < n; i++) {
        if (is_mqtt[i]) mqtt_service(who[i], fds[i].revents);
        else pcb_service(who[i], fds[i].revents);
    }
    for (mqtt_client_t *c = s_mqtt; c; c = c->next) if (!c->freed && c->fd >= 0 && (c->state == 3 || c->state == 2)) mqtt_service(c, 0);   /* timeouts, pings, queued sends */

    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (struct altcp_pcb *p = s_pcbs; p; p = p->next)
        if (!p->dead && p->on_poll && p->poll_ms && now - p->polled_ms >= p->poll_ms) { p->polled_ms = now; p->on_poll(p->arg, p); }

    /* free what was closed, now that no callback is using it */
    for (struct altcp_pcb **pp = &s_pcbs; *pp; ) {
        struct altcp_pcb *p = *pp;
        if (p->dead) { *pp = p->next; free(p->tx.b); free(p); } else pp = &p->next;
    }
    for (mqtt_client_t **pp = &s_mqtt; *pp; ) {
        mqtt_client_t *c = *pp;
        if (c->freed) { *pp = c->next; free(c->tx.b); free(c->rx.b); free(c); } else pp = &c->next;
    }
    depth--;
}

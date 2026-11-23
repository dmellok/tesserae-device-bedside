/*
 * sim_lwip.h: the lwIP, cyw43 and mbedTLS calls the firmware's network code
 * makes, on the host's sockets (sim/src/sim_lwip.c).
 *
 * Only what net_rest.c, radio.c and ha.c use: DNS, TCP connections through
 * lwIP's "altcp" API (callbacks for connected / received / sent / error), and
 * lwIP's MQTT client. Callbacks run on the main thread from sleep_ms() and
 * cyw43_arch_poll(), where the firmware waits for them. TLS is not simulated:
 * an https connection fails the way an unreachable server would.
 */
#pragma once

#include "sim_sdk.h"

typedef uint8_t  u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef int8_t   s8_t;
typedef int16_t  s16_t;
typedef int32_t  s32_t;
typedef s8_t     err_t;

#define ERR_OK          0
#define ERR_MEM        -1
#define ERR_BUF        -2
#define ERR_TIMEOUT    -3
#define ERR_RTE        -4
#define ERR_INPROGRESS -5
#define ERR_VAL        -6
#define ERR_WOULDBLOCK -7
#define ERR_USE        -8
#define ERR_ALREADY    -9
#define ERR_ISCONN     -10
#define ERR_CONN       -11
#define ERR_IF         -12
#define ERR_ABRT       -13
#define ERR_RST        -14
#define ERR_CLSD       -15
#define ERR_ARG        -16

/* ---- addresses (IPv4) ---- */
typedef struct { u32_t addr; } ip4_addr_t;
typedef ip4_addr_t ip_addr_t;
#define IPADDR_TYPE_V4  0
#define IPADDR_TYPE_ANY 46
#define IP_GET_TYPE(ip) IPADDR_TYPE_V4
#define ip_2_ip4(ip) (ip)
#define IP4_ADDR(ip, a, b, c, d) ((ip)->addr = (u32_t)(((a) & 0xff) | (((b) & 0xff) << 8) | (((c) & 0xff) << 16) | (((u32_t)(d) & 0xff) << 24)))
int   ipaddr_aton(const char *cp, ip_addr_t *addr);
char *ip4addr_ntoa(const ip4_addr_t *addr);
#define ipaddr_ntoa(a) ip4addr_ntoa(a)

/* ---- pbufs: one contiguous buffer each ---- */
struct pbuf { struct pbuf *next; void *payload; u16_t tot_len; u16_t len; };
u8_t  pbuf_free(struct pbuf *p);
u16_t pbuf_copy_partial(const struct pbuf *p, void *data, u16_t len, u16_t offset);

/* ---- DNS: answered at once from the host's resolver ---- */
typedef void (*dns_found_callback)(const char *name, const ip_addr_t *ipaddr, void *arg);
err_t dns_gethostbyname(const char *hostname, ip_addr_t *addr, dns_found_callback found, void *arg);

/* ---- altcp: TCP connections ---- */
struct altcp_pcb;
struct altcp_tls_config;
typedef err_t (*altcp_connected_fn)(void *arg, struct altcp_pcb *conn, err_t err);
typedef err_t (*altcp_recv_fn)(void *arg, struct altcp_pcb *conn, struct pbuf *p, err_t err);
typedef err_t (*altcp_sent_fn)(void *arg, struct altcp_pcb *conn, u16_t len);
typedef err_t (*altcp_poll_fn)(void *arg, struct altcp_pcb *conn);
typedef void  (*altcp_err_fn)(void *arg, err_t err);
#define TCP_WRITE_FLAG_COPY 0x01
#define TCP_WRITE_FLAG_MORE 0x02
#define TCP_MSS 1460
#define TCP_SND_BUF (8 * TCP_MSS)

struct altcp_pcb *altcp_tcp_new_ip_type(u8_t ip_type);
struct altcp_pcb *altcp_tls_new(struct altcp_tls_config *config, u8_t ip_type);
struct altcp_tls_config *altcp_tls_create_config_client(const u8_t *cert, size_t cert_len);
void *altcp_tls_context(struct altcp_pcb *conn);
void  altcp_arg(struct altcp_pcb *conn, void *arg);
void  altcp_recv(struct altcp_pcb *conn, altcp_recv_fn recv);
void  altcp_sent(struct altcp_pcb *conn, altcp_sent_fn sent);
void  altcp_err(struct altcp_pcb *conn, altcp_err_fn err);
void  altcp_poll(struct altcp_pcb *conn, altcp_poll_fn poll, u8_t interval);
err_t altcp_connect(struct altcp_pcb *conn, const ip_addr_t *ipaddr, u16_t port, altcp_connected_fn connected);
err_t altcp_write(struct altcp_pcb *conn, const void *dataptr, u16_t len, u8_t apiflags);
err_t altcp_output(struct altcp_pcb *conn);
u16_t altcp_sndbuf(struct altcp_pcb *conn);
void  altcp_recved(struct altcp_pcb *conn, u16_t len);
err_t altcp_close(struct altcp_pcb *conn);
void  altcp_abort(struct altcp_pcb *conn);
void  altcp_nagle_disable(struct altcp_pcb *conn);

/* ---- cyw43: the WiFi chip; the simulator is always on the network ---- */
typedef struct { int dummy; } cyw43_t;
extern cyw43_t cyw43_state;
static inline void cyw43_arch_lwip_begin(void) {}
static inline void cyw43_arch_lwip_end(void) {}
void sim_lwip_pump(uint32_t wait_ms);
static inline void cyw43_arch_poll(void) { sim_lwip_pump(0); }
static inline int  cyw43_wifi_get_rssi(cyw43_t *s, int32_t *rssi) { (void)s; *rssi = -48; return 0; }

/* ---- mbedTLS: only the names net_rest.c and radio.c mention ---- */
typedef int64_t mbedtls_ms_time_t;
typedef struct mbedtls_ssl_context mbedtls_ssl_context;
static inline int mbedtls_ssl_set_hostname(mbedtls_ssl_context *ssl, const char *hostname) { (void)ssl; (void)hostname; return 0; }

/* ---- lwIP's MQTT client (MQTT 3.1.1 on a host socket) ---- */
typedef struct mqtt_client_s mqtt_client_t;
typedef enum {
    MQTT_CONNECT_ACCEPTED                 = 0,
    MQTT_CONNECT_REFUSED_PROTOCOL_VERSION = 1,
    MQTT_CONNECT_REFUSED_IDENTIFIER       = 2,
    MQTT_CONNECT_REFUSED_SERVER           = 3,
    MQTT_CONNECT_REFUSED_USERNAME_PASS    = 4,
    MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_  = 5,
    MQTT_CONNECT_DISCONNECTED             = 256,
    MQTT_CONNECT_TIMEOUT                  = 257
} mqtt_connection_status_t;
enum { MQTT_DATA_FLAG_LAST = 1 };
typedef void (*mqtt_connection_cb_t)(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);
typedef void (*mqtt_incoming_publish_cb_t)(void *arg, const char *topic, u32_t tot_len);
typedef void (*mqtt_incoming_data_cb_t)(void *arg, const u8_t *data, u16_t len, u8_t flags);
typedef void (*mqtt_request_cb_t)(void *arg, err_t err);
struct mqtt_connect_client_info_t {
    const char *client_id, *client_user, *client_pass;
    u16_t keep_alive;
    const char *will_topic, *will_msg;
    u8_t will_msg_len, will_qos, will_retain;
};
mqtt_client_t *mqtt_client_new(void);
void  mqtt_client_free(mqtt_client_t *client);
err_t mqtt_client_connect(mqtt_client_t *client, const ip_addr_t *ipaddr, u16_t port, mqtt_connection_cb_t cb, void *arg,
                          const struct mqtt_connect_client_info_t *client_info);
void  mqtt_disconnect(mqtt_client_t *client);
u8_t  mqtt_client_is_connected(mqtt_client_t *client);
void  mqtt_set_inpub_callback(mqtt_client_t *client, mqtt_incoming_publish_cb_t pub_cb, mqtt_incoming_data_cb_t data_cb, void *arg);
err_t mqtt_publish(mqtt_client_t *client, const char *topic, const void *payload, u16_t payload_length, u8_t qos, u8_t retain,
                   mqtt_request_cb_t cb, void *arg);
err_t mqtt_sub_unsub(mqtt_client_t *client, const char *topic, u8_t qos, mqtt_request_cb_t cb, void *arg, u8_t sub);
#define mqtt_subscribe(c, t, q, cb, a)   mqtt_sub_unsub(c, t, q, cb, a, 1)
#define mqtt_unsubscribe(c, t, cb, a)    mqtt_sub_unsub(c, t, 0, cb, a, 0)

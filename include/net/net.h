#pragma once
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */
typedef uint8_t  mac_t[6];
typedef uint32_t ip4_t;   /* network byte order */

/* ------------------------------------------------------------------ */
/* Byte-order helpers                                                  */
/* ------------------------------------------------------------------ */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xFFu) | ((x >> 8) & 0xFF00u) |
           ((x << 8) & 0xFF0000u) | ((x << 24) & 0xFF000000u);
}
#define ntohs htons
#define ntohl htonl

/* ------------------------------------------------------------------ */
/* Network stack                                                       */
/* ------------------------------------------------------------------ */

/* Initialise: runs DHCP to obtain an IP.
 * Returns 0 on success, -1 if NIC missing, -2 if DHCP timed out. */
int  net_init(void);

int  net_ready(void);           /* 1 once DHCP complete             */
void net_get_ip(uint8_t ip[4]); /* our IPv4 address                 */
void net_get_mac(uint8_t m[6]); /* our MAC                          */

/* Process one received frame (call from polling loops).             */
void net_poll(void);

/* ICMP echo (ping).  Returns round-trip ms, or -1 on timeout.      */
int  net_ping(ip4_t target, uint32_t timeout_ms);

/* DNS — resolve hostname to IPv4.  Returns 0 on success.           */
int  net_dns_resolve(const char *host, ip4_t *out);

/* ------------------------------------------------------------------ */
/* TCP connection (one at a time)                                     */
/* ------------------------------------------------------------------ */
typedef struct net_tcp net_tcp_t;

/* Connect to remote host:port.  Returns 0 on success.              */
int  tcp_connect(ip4_t remote, uint16_t port, net_tcp_t **conn);

/* Send data.  Returns 0 on success.                                 */
int  tcp_send(net_tcp_t *c, const void *data, uint32_t len);

/* Receive up to max_len bytes.  Returns byte count (0 = no data yet,
 * -1 = connection closed).                                           */
int  tcp_recv(net_tcp_t *c, void *buf, uint32_t max_len, uint32_t timeout_ms);

/* Close connection.                                                  */
void tcp_close(net_tcp_t *c);

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

/* Simple HTTP GET.  Calls cb(data, len, ud) for each chunk of body.
 * Returns HTTP status code, or negative on error.                    */
typedef void (*http_chunk_cb)(const uint8_t *data, uint32_t len, void *ud);
int  http_get(const char *host_ip, uint16_t port, const char *path,
              http_chunk_cb cb, void *ud);

/* HTTP GET into a fixed buffer.  Returns body length or negative.   */
int  http_get_buf(const char *host_ip, uint16_t port, const char *path,
                  uint8_t *buf, uint32_t cap);

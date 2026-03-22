#include <stdint.h>
#include <stddef.h>
#include "net/net.h"
#include "drivers/e1000.h"
#include "drivers/pit.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void mem_zero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}
static void mem_copy(void *d, const void *s, uint32_t n) {
    uint8_t *dp = (uint8_t *)d;
    const uint8_t *sp = (const uint8_t *)s;
    while (n--) *dp++ = *sp++;
}
static int mem_eq(const void *a, const void *b, uint32_t n) {
    const uint8_t *ap = (const uint8_t *)a, *bp = (const uint8_t *)b;
    while (n--) if (*ap++ != *bp++) return 0;
    return 1;
}
static uint32_t str_len(const char *s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}
static void uint_to_str(uint32_t v, char *buf) {
    char tmp[12]; int i = 0;
    if (!v) { buf[0]='0'; buf[1]='\0'; return; }
    while (v) { tmp[i++] = '0' + (char)(v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}
static uint32_t str_to_uint(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}
/* ms elapsed since boot */
static uint32_t now_ms(void) {
    return (uint32_t)(pit_ticks() * 1000u / PIT_HZ);
}

/* ------------------------------------------------------------------ */
/* Ethernet                                                            */
/* ------------------------------------------------------------------ */

#define ETH_ARP  0x0806u
#define ETH_IP4  0x0800u

struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;  /* big-endian */
} __attribute__((packed));

static uint8_t my_mac[6];
static uint8_t bcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static uint8_t tx_frame[1536];
static uint8_t rx_frame[1536];

static void eth_send(const uint8_t dst[6], uint16_t type,
                     const void *payload, uint16_t plen) {
    struct eth_hdr *eh = (struct eth_hdr *)tx_frame;
    mem_copy(eh->dst, dst, 6);
    mem_copy(eh->src, my_mac, 6);
    eh->type = htons(type);
    mem_copy(tx_frame + sizeof(*eh), payload, plen);
    e1000_send(tx_frame, (uint16_t)(sizeof(*eh) + plen));
}

/* ------------------------------------------------------------------ */
/* IP checksum                                                         */
/* ------------------------------------------------------------------ */

static uint16_t ip_csum(const void *data, uint32_t len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ------------------------------------------------------------------ */
/* ARP                                                                 */
/* ------------------------------------------------------------------ */

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[6]; uint32_t spa;
    uint8_t  tha[6]; uint32_t tpa;
} __attribute__((packed));

#define ARP_CACHE 8
static struct { uint32_t ip; uint8_t mac[6]; int valid; } arp_cache[ARP_CACHE];
static ip4_t my_ip, gw_ip;
static int net_up = 0;

static void arp_cache_set(uint32_t ip, const uint8_t mac_addr[6]) {
    /* overwrite existing or use first free */
    for (int i = 0; i < ARP_CACHE; i++) {
        if (!arp_cache[i].valid || arp_cache[i].ip == ip) {
            arp_cache[i].ip = ip;
            mem_copy(arp_cache[i].mac, mac_addr, 6);
            arp_cache[i].valid = 1;
            return;
        }
    }
    arp_cache[0].ip = ip; mem_copy(arp_cache[0].mac, mac_addr, 6);
    arp_cache[0].valid = 1;
}

static int arp_cache_get(uint32_t ip, uint8_t out[6]) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            mem_copy(out, arp_cache[i].mac, 6);
            return 0;
        }
    return -1;
}

static void arp_send_request(uint32_t target_ip) {
    struct arp_pkt ap;
    mem_zero(&ap, sizeof(ap));
    ap.htype = htons(1); ap.ptype = htons(0x0800);
    ap.hlen = 6; ap.plen = 4; ap.op = htons(1);
    mem_copy(ap.sha, my_mac, 6); ap.spa = my_ip;
    mem_copy(ap.tha, bcast_mac, 6); ap.tpa = target_ip;
    eth_send(bcast_mac, ETH_ARP, &ap, sizeof(ap));
}

static void arp_handle(const uint8_t *p, uint16_t len) {
    if (len < (uint16_t)sizeof(struct arp_pkt)) return;
    const struct arp_pkt *ap = (const struct arp_pkt *)p;
    if (ntohs(ap->htype) != 1 || ntohs(ap->ptype) != 0x0800) return;
    /* Cache sender */
    arp_cache_set(ap->spa, ap->sha);
    /* Reply if it's a request for our IP */
    if (ntohs(ap->op) == 1 && ap->tpa == my_ip) {
        struct arp_pkt rp;
        rp.htype = htons(1); rp.ptype = htons(0x0800);
        rp.hlen = 6; rp.plen = 4; rp.op = htons(2);
        mem_copy(rp.sha, my_mac, 6); rp.spa = my_ip;
        mem_copy(rp.tha, ap->sha, 6); rp.tpa = ap->spa;
        eth_send(ap->sha, ETH_ARP, &rp, sizeof(rp));
    }
}

/* Resolve IP to MAC, sending ARP request and waiting */
static int arp_resolve(uint32_t ip, uint8_t out[6], uint32_t timeout_ms) {
    if (arp_cache_get(ip, out) == 0) return 0;
    arp_send_request(ip);
    uint32_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        int n = e1000_recv(rx_frame, sizeof(rx_frame));
        if (n >= (int)sizeof(struct eth_hdr)) {
            struct eth_hdr *eh = (struct eth_hdr *)rx_frame;
            if (ntohs(eh->type) == ETH_ARP)
                arp_handle(rx_frame + sizeof(*eh), (uint16_t)(n - (int)sizeof(*eh)));
        }
        if (arp_cache_get(ip, out) == 0) return 0;
        __asm__ volatile ("pause");
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* IPv4                                                                */
/* ------------------------------------------------------------------ */

struct ip4_hdr {
    uint8_t  ver_ihl, tos;
    uint16_t total_len, id, flags_frag;
    uint8_t  ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

#define IP_ICMP  1u
#define IP_TCP   6u
#define IP_UDP  17u

static uint16_t ip_id_counter = 0;

static void ip_send(uint32_t dst_ip, uint8_t proto,
                    const void *payload, uint16_t plen) {
    static uint8_t pkt[1500];
    struct ip4_hdr *ih = (struct ip4_hdr *)pkt;
    ih->ver_ihl   = 0x45;
    ih->tos       = 0;
    ih->total_len = htons((uint16_t)(20 + plen));
    ih->id        = htons(ip_id_counter++);
    ih->flags_frag = 0;
    ih->ttl       = 64;
    ih->proto     = proto;
    ih->csum      = 0;
    ih->src       = my_ip;
    ih->dst       = dst_ip;
    ih->csum      = ip_csum(ih, 20);
    mem_copy(pkt + 20, payload, plen);

    /* Determine next-hop MAC */
    uint8_t dst_mac[6];

    if (dst_ip == 0xFFFFFFFFu || dst_ip == 0u) {
        /* Broadcast — never ARP, go directly to broadcast MAC */
        mem_copy(dst_mac, bcast_mac, 6);
    } else {
        uint32_t next_hop = dst_ip;
        uint32_t subnet_m = (uint32_t)0xFFFFFF00u;
        if (my_ip && (dst_ip & subnet_m) != (my_ip & subnet_m))
            next_hop = gw_ip;
        if (arp_resolve(next_hop, dst_mac, 2000) != 0) return;
    }

    eth_send(dst_mac, ETH_IP4, pkt, (uint16_t)(20 + plen));
}

/* ------------------------------------------------------------------ */
/* UDP                                                                 */
/* ------------------------------------------------------------------ */

struct udp_hdr {
    uint16_t src_port, dst_port, length, csum;
} __attribute__((packed));

static void udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                     const void *data, uint16_t dlen) {
    static uint8_t buf[1400];
    struct udp_hdr *uh = (struct udp_hdr *)buf;
    uh->src_port = htons(src_port);
    uh->dst_port = htons(dst_port);
    uh->length   = htons((uint16_t)(8 + dlen));
    uh->csum     = 0;
    mem_copy(buf + 8, data, dlen);
    ip_send(dst_ip, IP_UDP, buf, (uint16_t)(8 + dlen));
}

/* ------------------------------------------------------------------ */
/* DHCP                                                                */
/* ------------------------------------------------------------------ */

#define DHCP_MAGIC 0x63825363u
#define DHCP_DISCOVER 1u
#define DHCP_OFFER    2u
#define DHCP_REQUEST  3u
#define DHCP_ACK      5u

struct dhcp_pkt {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[308];
} __attribute__((packed));

static uint32_t dhcp_xid = 0x12345678u;

static void dhcp_send(uint8_t msg_type, uint32_t server_ip, uint32_t req_ip) {
    struct dhcp_pkt p;
    mem_zero(&p, sizeof(p));
    p.op = 1; p.htype = 1; p.hlen = 6;
    p.xid = htonl(dhcp_xid);
    p.flags = htons(0x8000u);  /* broadcast */
    mem_copy(p.chaddr, my_mac, 6);
    p.magic = htonl(DHCP_MAGIC);

    uint8_t *o = p.options;
    *o++ = 53; *o++ = 1; *o++ = msg_type;        /* DHCP Message Type */
    if (msg_type == DHCP_REQUEST) {
        if (req_ip) { *o++ = 50; *o++ = 4;        /* Requested IP */
            mem_copy(o, &req_ip, 4); o += 4; }
        if (server_ip) { *o++ = 54; *o++ = 4;     /* Server ID */
            mem_copy(o, &server_ip, 4); o += 4; }
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6; /* Param request */
    *o++ = 255;  /* End */

    /* DHCP uses UDP port 68 (client) → 67 (server) on broadcast */
    uint32_t dst = 0xFFFFFFFFu;
    if (server_ip && msg_type == DHCP_REQUEST) dst = server_ip;
    uint32_t saved_ip = my_ip;
    my_ip = 0;   /* source must be 0.0.0.0 during DHCP bootstrap */
    udp_send(dst, 68, 67, &p, (uint16_t)sizeof(p));
    my_ip = saved_ip;
}

static int dhcp_run(uint32_t timeout_ms) {
    uint32_t offered_ip = 0, server_ip = 0;

    /* Send Discover */
    dhcp_send(DHCP_DISCOVER, 0, 0);
    uint32_t deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        int n = e1000_recv(rx_frame, sizeof(rx_frame));
        if (n < (int)(sizeof(struct eth_hdr) + 20 + 8)) {
            __asm__ volatile ("pause"); continue;
        }
        struct eth_hdr *eh = (struct eth_hdr *)rx_frame;
        if (ntohs(eh->type) != ETH_IP4) continue;
        struct ip4_hdr *ih = (struct ip4_hdr *)(rx_frame + sizeof(*eh));
        if (ih->proto != IP_UDP) continue;
        struct udp_hdr *uh = (struct udp_hdr *)((uint8_t *)ih + 20);
        if (ntohs(uh->dst_port) != 68) continue;
        struct dhcp_pkt *dp = (struct dhcp_pkt *)((uint8_t *)uh + 8);
        if (ntohl(dp->magic) != DHCP_MAGIC) continue;
        if (ntohl(dp->xid) != dhcp_xid) continue;

        /* Parse message type option */
        uint8_t *o = dp->options;
        uint8_t msg = 0;
        uint32_t sid = 0;
        while (*o != 255 && o < dp->options + 308) {
            uint8_t tag = *o++;
            if (tag == 0) continue;
            uint8_t len = *o++;
            if (tag == 53 && len == 1) msg = *o;
            if (tag == 54 && len == 4) mem_copy(&sid, o, 4);
            o += len;
        }

        if (msg == DHCP_OFFER) {
            offered_ip = dp->yiaddr;
            server_ip  = sid;
            /* Parse gateway from options */
            o = dp->options;
            while (*o != 255 && o < dp->options + 308) {
                uint8_t tag = *o++;
                if (tag == 0) continue;
                uint8_t len = *o++;
                if (tag == 3 && len >= 4) mem_copy(&gw_ip, o, 4);
                o += len;
            }
            dhcp_send(DHCP_REQUEST, server_ip, offered_ip);
        } else if (msg == DHCP_ACK) {
            my_ip = dp->yiaddr;
            return 0;
        }
    }
    return -2;
}

/* ------------------------------------------------------------------ */
/* ICMP                                                                */
/* ------------------------------------------------------------------ */

struct icmp_hdr {
    uint8_t  type, code;
    uint16_t csum, id, seq;
} __attribute__((packed));

int net_ping(ip4_t target, uint32_t timeout_ms) {
    static uint16_t ping_seq = 0;
    struct icmp_hdr req;
    req.type = 8; req.code = 0; req.csum = 0;
    req.id = htons(0x4145); req.seq = htons(ping_seq++);
    req.csum = ip_csum(&req, sizeof(req));

    uint32_t t0 = now_ms();
    ip_send(target, IP_ICMP, &req, sizeof(req));
    uint32_t deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        int n = e1000_recv(rx_frame, sizeof(rx_frame));
        if (n < (int)(sizeof(struct eth_hdr) + 20 + (int)sizeof(struct icmp_hdr)))
            { __asm__ volatile ("pause"); continue; }
        struct eth_hdr *eh = (struct eth_hdr *)rx_frame;
        if (ntohs(eh->type) != ETH_IP4) continue;
        struct ip4_hdr *ih = (struct ip4_hdr *)(rx_frame + sizeof(*eh));
        if (ih->proto != IP_ICMP) continue;
        struct icmp_hdr *ic = (struct icmp_hdr *)((uint8_t *)ih + 20);
        if (ic->type == 0 && ic->id == htons(0x4145))
            return (int)(now_ms() - t0);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* TCP                                                                 */
/* ------------------------------------------------------------------ */

struct tcp_hdr {
    uint16_t src_port, dst_port;
    uint32_t seq, ack_seq;
    uint8_t  data_off, flags;
    uint16_t window, csum, urgent;
} __attribute__((packed));

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

struct net_tcp {
    uint32_t remote_ip;
    uint16_t remote_port, local_port;
    uint32_t seq, ack;
    int      state;  /* 0=closed 1=syn_sent 2=estab 3=fin_wait */
    uint8_t  rx_buf[8192];
    uint32_t rx_head, rx_tail;
};

static struct net_tcp tcp_slot;

static uint16_t tcp_port_counter = 49152;

/* Pseudo-header checksum for TCP */
static uint16_t tcp_csum(uint32_t src, uint32_t dst,
                          const void *tcp_data, uint16_t tcp_len) {
    uint32_t sum = 0;
    /* Pseudo header */
    sum += (src >> 16) & 0xFFFF;
    sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF;
    sum += dst & 0xFFFF;
    sum += htons(IP_TCP);
    sum += htons(tcp_len);
    /* TCP segment */
    const uint16_t *p = (const uint16_t *)tcp_data;
    uint16_t len = tcp_len;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void tcp_send_raw(struct net_tcp *c, uint8_t flags,
                          const void *data, uint16_t dlen) {
    static uint8_t seg[1500];
    struct tcp_hdr *th = (struct tcp_hdr *)seg;
    th->src_port = htons(c->local_port);
    th->dst_port = htons(c->remote_port);
    th->seq      = htonl(c->seq);
    th->ack_seq  = (flags & TCP_ACK) ? htonl(c->ack) : 0;
    th->data_off = 0x50;  /* 20 bytes header */
    th->flags    = flags;
    th->window   = htons(8192);
    th->csum     = 0;
    th->urgent   = 0;
    if (dlen) mem_copy(seg + 20, data, dlen);
    th->csum = tcp_csum(my_ip, c->remote_ip, seg, (uint16_t)(20 + dlen));
    ip_send(c->remote_ip, IP_TCP, seg, (uint16_t)(20 + dlen));
    if (flags & (TCP_SYN | TCP_FIN)) c->seq++;
    c->seq += dlen;
}

/* Receive and dispatch one frame; process TCP for connection c */
static void tcp_rx_once(struct net_tcp *c) {
    int n = e1000_recv(rx_frame, sizeof(rx_frame));
    if (n < (int)(sizeof(struct eth_hdr) + 20)) return;

    struct eth_hdr *eh = (struct eth_hdr *)rx_frame;
    if (ntohs(eh->type) == ETH_ARP) {
        arp_handle(rx_frame + sizeof(*eh), (uint16_t)(n - (int)sizeof(*eh)));
        return;
    }
    if (ntohs(eh->type) != ETH_IP4) return;

    struct ip4_hdr *ih = (struct ip4_hdr *)(rx_frame + sizeof(*eh));
    if (ih->proto != IP_TCP) return;
    if (ih->src != c->remote_ip) return;

    uint16_t ip_total = ntohs(ih->total_len);
    uint16_t ip_hlen  = (uint16_t)((ih->ver_ihl & 0xF) * 4);
    struct tcp_hdr *th = (struct tcp_hdr *)((uint8_t *)ih + ip_hlen);

    if (ntohs(th->dst_port) != c->local_port)  return;
    if (ntohs(th->src_port) != c->remote_port) return;

    uint16_t tcp_hlen = (uint16_t)((th->data_off >> 4) * 4);
    uint16_t data_len = (uint16_t)(ip_total - ip_hlen - tcp_hlen);
    uint8_t *data     = (uint8_t *)th + tcp_hlen;

    uint8_t f = th->flags;

    if (c->state == 1) {  /* SYN_SENT — waiting for SYN+ACK */
        if ((f & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            c->ack = ntohl(th->seq) + 1;
            c->state = 2;
            tcp_send_raw(c, TCP_ACK, NULL, 0);
        }
    } else if (c->state == 2) {  /* ESTABLISHED */
        if (f & TCP_RST) { c->state = 0; return; }
        if (data_len > 0) {
            /* Put data in circular rx_buf */
            for (uint16_t i = 0; i < data_len; i++) {
                uint32_t next = (c->rx_tail + 1) % 8192;
                if (next != c->rx_head)
                    c->rx_buf[c->rx_tail = next] = data[i];
            }
            c->ack = ntohl(th->seq) + data_len;
            tcp_send_raw(c, TCP_ACK, NULL, 0);
        }
        if (f & TCP_FIN) {
            c->ack++;
            tcp_send_raw(c, TCP_ACK | TCP_FIN, NULL, 0);
            c->state = 3;
        }
    }
}

int tcp_connect(ip4_t remote, uint16_t port, net_tcp_t **conn) {
    struct net_tcp *c = &tcp_slot;
    mem_zero(c, sizeof(*c));
    c->remote_ip   = remote;
    c->remote_port = port;
    c->local_port  = tcp_port_counter++;
    c->seq         = 0xABCD1234u;
    c->state       = 1;

    tcp_send_raw(c, TCP_SYN, NULL, 0);

    uint32_t deadline = now_ms() + 5000;
    while (now_ms() < deadline && c->state == 1)
        tcp_rx_once(c);

    if (c->state != 2) { c->state = 0; return -1; }
    *conn = c;
    return 0;
}

int tcp_send(net_tcp_t *c, const void *data, uint32_t len) {
    if (c->state != 2) return -1;
    /* Send in 1024-byte chunks */
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        uint16_t chunk = (uint16_t)(len > 1024 ? 1024 : len);
        tcp_send_raw(c, TCP_ACK | TCP_PSH, p, chunk);
        p += chunk; len -= chunk;
    }
    return 0;
}

int tcp_recv(net_tcp_t *c, void *buf, uint32_t max_len, uint32_t timeout_ms) {
    uint32_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        tcp_rx_once(c);
        uint32_t avail = (c->rx_tail - c->rx_head + 8192) % 8192;
        if (avail > 0) {
            uint32_t rd = avail < max_len ? avail : max_len;
            uint8_t *out = (uint8_t *)buf;
            for (uint32_t i = 0; i < rd; i++) {
                c->rx_head = (c->rx_head + 1) % 8192;
                out[i] = c->rx_buf[c->rx_head];
            }
            return (int)rd;
        }
        if (c->state != 2) return -1;
        __asm__ volatile ("pause");
    }
    return 0;
}

void tcp_close(net_tcp_t *c) {
    if (c->state == 2) {
        tcp_send_raw(c, TCP_FIN | TCP_ACK, NULL, 0);
        c->state = 3;
    }
    c->state = 0;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

int http_get(const char *host_ip, uint16_t port, const char *path,
             http_chunk_cb cb, void *ud) {
    /* Parse IP string to uint32 */
    uint32_t ip = 0;
    const char *s = host_ip;
    for (int oct = 0; oct < 4; oct++) {
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
        if (*s == '.') s++;
        ip = (ip << 8) | (v & 0xFF);
    }
    ip4_t dst = htonl(ip);

    net_tcp_t *c;
    if (tcp_connect(dst, port, &c) != 0) return -1;

    /* Build GET request */
    static char req[512];
    uint32_t pos = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host_ip,
                             "\r\nConnection: close\r\n\r\n" };
    for (int i = 0; i < 5; i++) {
        const char *p = parts[i];
        while (*p && pos < 510) req[pos++] = *p++;
    }
    req[pos] = '\0';

    if (tcp_send(c, req, pos) != 0) { tcp_close(c); return -1; }

    /* Read response */
    static uint8_t resp[4096];
    int status = -1;
    int header_done = 0;
    static uint8_t hdr_buf[512];
    uint32_t hdr_pos = 0;

    while (1) {
        int n = tcp_recv(c, resp, sizeof(resp), 5000);
        if (n < 0) break;
        if (n == 0) { if (c->state != 2) break; continue; }

        if (!header_done) {
            /* Accumulate header */
            for (int i = 0; i < n && hdr_pos < 511; i++)
                hdr_buf[hdr_pos++] = resp[i];
            hdr_buf[hdr_pos] = '\0';
            /* Find \r\n\r\n */
            for (uint32_t i = 0; i + 3 < hdr_pos; i++) {
                if (hdr_buf[i]=='\r' && hdr_buf[i+1]=='\n' &&
                    hdr_buf[i+2]=='\r' && hdr_buf[i+3]=='\n') {
                    /* Parse status from first line */
                    status = (int)str_to_uint((char *)hdr_buf + 9);
                    uint32_t body_start = i + 4;
                    header_done = 1;
                    /* Pass remaining data as body */
                    if (body_start < (uint32_t)n && cb)
                        cb(resp + body_start, (uint32_t)n - body_start, ud);
                    break;
                }
            }
        } else {
            if (cb) cb(resp, (uint32_t)n, ud);
        }
    }
    tcp_close(c);
    return status;
}

struct buf_ctx { uint8_t *buf; uint32_t cap; uint32_t pos; };
static void buf_chunk(const uint8_t *data, uint32_t len, void *ud) {
    struct buf_ctx *ctx = (struct buf_ctx *)ud;
    if (ctx->pos + len > ctx->cap) len = ctx->cap - ctx->pos;
    mem_copy(ctx->buf + ctx->pos, data, len);
    ctx->pos += len;
}

int http_get_buf(const char *host_ip, uint16_t port, const char *path,
                 uint8_t *buf, uint32_t cap) {
    struct buf_ctx ctx = { buf, cap, 0 };
    int status = http_get(host_ip, port, path, buf_chunk, &ctx);
    buf[ctx.pos] = '\0';
    if (status != 200) return -1;
    return (int)ctx.pos;
}

/* ------------------------------------------------------------------ */
/* DNS (minimal — resolve A record)                                    */
/* ------------------------------------------------------------------ */

struct dns_hdr {
    uint16_t id, flags, qdcount, ancount, nscount, arcount;
} __attribute__((packed));

int net_dns_resolve(const char *host, ip4_t *out) {
    /* Build query */
    static uint8_t qbuf[512];
    mem_zero(qbuf, sizeof(qbuf));
    struct dns_hdr *dh = (struct dns_hdr *)qbuf;
    dh->id = htons(0xAB01); dh->flags = htons(0x0100); /* RD */
    dh->qdcount = htons(1);
    uint8_t *p = qbuf + 12;
    /* Encode hostname as DNS labels */
    const char *h = host;
    while (*h) {
        const char *dot = h;
        while (*dot && *dot != '.') dot++;
        uint8_t llen = (uint8_t)(dot - h);
        *p++ = llen;
        while (h < dot) *p++ = (uint8_t)*h++;
        if (*h == '.') h++;
    }
    *p++ = 0; /* root */
    *p++ = 0; *p++ = 1;  /* QTYPE A */
    *p++ = 0; *p++ = 1;  /* QCLASS IN */
    uint16_t qlen = (uint16_t)(p - qbuf);

    /* Send to DNS server (use gateway as resolver) */
    ip4_t dns = gw_ip;
    udp_send(dns, 1025, 53, qbuf, qlen);

    /* Wait for reply */
    uint32_t deadline = now_ms() + 3000;
    while (now_ms() < deadline) {
        int n = e1000_recv(rx_frame, sizeof(rx_frame));
        if (n < (int)(sizeof(struct eth_hdr) + 20 + 8)) {
            __asm__ volatile ("pause"); continue;
        }
        struct eth_hdr *eh = (struct eth_hdr *)rx_frame;
        if (ntohs(eh->type) != ETH_IP4) continue;
        struct ip4_hdr *ih = (struct ip4_hdr *)(rx_frame + sizeof(*eh));
        if (ih->proto != IP_UDP) continue;
        struct udp_hdr *uh = (struct udp_hdr *)((uint8_t *)ih + 20);
        if (ntohs(uh->dst_port) != 1025) continue;
        uint8_t *rp = (uint8_t *)uh + 8;
        struct dns_hdr *rh = (struct dns_hdr *)rp;
        if (rh->id != htons(0xAB01)) continue;
        /* Skip question section */
        uint8_t *ap = rp + 12;
        while (*ap) ap += *ap + 1; ap += 5; /* skip name + qtype + qclass */
        uint16_t ancount = ntohs(rh->ancount);
        for (uint16_t i = 0; i < ancount; i++) {
            /* Skip name (may be pointer) */
            if ((*ap & 0xC0) == 0xC0) ap += 2;
            else { while (*ap) ap += *ap + 1; ap++; }
            uint16_t rtype = (uint16_t)(((uint16_t)ap[0]<<8)|ap[1]); ap += 2;
            ap += 6; /* class + TTL */
            uint16_t rdlen = (uint16_t)(((uint16_t)ap[0]<<8)|ap[1]); ap += 2;
            if (rtype == 1 && rdlen == 4) {
                mem_copy(out, ap, 4); return 0;
            }
            ap += rdlen;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void net_poll(void) {
    int n = e1000_recv(rx_frame, sizeof(rx_frame));
    if (n < (int)sizeof(struct eth_hdr)) return;
    struct eth_hdr *eh = (struct eth_hdr *)rx_frame;
    if (ntohs(eh->type) == ETH_ARP)
        arp_handle(rx_frame + sizeof(*eh), (uint16_t)(n - (int)sizeof(*eh)));
}

int net_init(void) {
    if (e1000_init() != 0) return -1;
    e1000_get_mac(my_mac);
    return dhcp_run(10000);
}

int net_ready(void) { return net_up; }

void net_get_ip(uint8_t ip[4]) { mem_copy(ip, &my_ip, 4); }
void net_get_mac(uint8_t m[6]) { mem_copy(m, my_mac, 6); }

/* ============================================================================
 * NexxoN OS - Network stack core (Ethernet / ARP / IPv4 / ICMP / UDP / TCP)
 * ----------------------------------------------------------------------------
 * Single-file lightweight stack: enough to drive a `ping` shell command,
 * answer ICMP Echo Requests, and run an HTTP/1.1 GET via a minimal TCP
 * state machine.  No retransmission timer beyond the user's per-call
 * timeout; no out-of-order reassembly past the 256 KiB receive window
 * (was 32 KiB in v1.0 — grown so the thin-client browser can buffer a
 * complete 1024x768 JPEG frame from the proxy without collapsing the
 * advertised window to zero mid-stream); no MSS negotiation past the
 * default 536 bytes.  These limits are enough for the GET / 200 OK
 * round trips the NexxoN Browser needs.
 *
 * Defaults follow QEMU user-net (10.0.2.0/24, gateway 10.0.2.2) so the
 * stack works out of the box in the standard test harness.  Real
 * hardware deployments configure their own values via Gépház.
 *
 * Endianness: all on-wire fields are big-endian.  Helpers htons/htonl
 * are inline-defined here to avoid pulling in a host libc.
 * ============================================================================ */
#include "net.h"
#include "netif.h"
#include "e1000.h"
#include "string.h"
#include "debug.h"
#include "pit.h"
#include "window.h"

/* ---- Idle hook ------------------------------------------------------- *
 * Blocking network calls (ping, DHCP, NTP, the TCP handshake/recv) run in
 * the shell task and would otherwise freeze the desktop: the WM only
 * redraws and the USB mouse only polls from the shell idle loop, which is
 * stuck inside the blocking call.  The shell registers a pump here so the
 * GUI keeps rendering and the pointer keeps moving while we wait.  A
 * re-entrancy guard prevents recursion when the hook itself drives an app
 * tick that issues network I/O (e.g. the browser). */
static void (*g_net_idle_hook)(void) = NULL;
static bool  g_net_idle_busy = false;
void net_set_idle_hook(void (*fn)(void)) { g_net_idle_hook = fn; }

/* ---- Kernel yield (cooperative scheduler hint) ----------------------- *
 * In a single-threaded cooperative kernel, tight spin loops freeze the
 * entire system.  `sti; hlt` enables interrupts and halts the CPU until
 * the next IRQ (PIT 100 Hz, NIC, keyboard, mouse).  This lets the Ring 0
 * scheduler service hardware while we wait for network I/O. */
static inline void kernel_yield(void) {
    if (g_net_idle_hook && !g_net_idle_busy) {
        g_net_idle_busy = true;
        g_net_idle_hook();
        g_net_idle_busy = false;
    }
    __asm__ volatile ("sti; hlt");
}

/* ---- Endianness ------------------------------------------------------- */
static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x000000FFu) << 24);
}
#define htons(x) bswap16(x)
#define htonl(x) bswap32(x)
#define ntohs(x) bswap16(x)
#define ntohl(x) bswap32(x)

/* ---- Configuration ---------------------------------------------------- */
static net_config_t g_cfg = {
    .mac     = { 0 },
    /* Fallback addresses if DHCP doesn't answer (QEMU user-net range). */
    .ip      = 0x0F02000Au,         /* 10.0.2.15  in network byte order  */
    .netmask = 0x00FFFFFFu,         /* 255.255.255.0                     */
    .gateway = 0x0202000Au,         /* 10.0.2.2                           */
    .dns     = 0x0302000Au,         /* 10.0.2.3                           */
    .dhcp    = true,                /* default to automatic DHCP          */
};
static bool g_link = false;

void net_set_config(const net_config_t *cfg) {
    if (!cfg) return;
    g_cfg = *cfg;
    e1000_mac(g_cfg.mac);
}
void net_get_config(net_config_t *out) {
    if (!out) return;
    *out = g_cfg;
}
bool net_link_up(void) { return g_link; }

/* ---- IPv4 helpers (network byte order) ------------------------------- */
uint32_t net_ip_aton(const char *s) {
    uint32_t b[4] = {0,0,0,0};
    int n = 0;
    while (*s && n < 4) {
        if (*s >= '0' && *s <= '9') {
            b[n] = b[n] * 10 + (uint32_t)(*s - '0');
            s++;
        } else if (*s == '.') { n++; s++; }
        else break;
    }
    return  ((b[0] & 0xFF))
         | ((b[1] & 0xFF) <<  8)
         | ((b[2] & 0xFF) << 16)
         | ((b[3] & 0xFF) << 24);
}
void net_ip_ntoa(uint32_t ip, char *out, size_t cap) {
    uint8_t a = (uint8_t)(ip       & 0xFF);
    uint8_t b = (uint8_t)((ip >> 8) & 0xFF);
    uint8_t c = (uint8_t)((ip >>16) & 0xFF);
    uint8_t d = (uint8_t)((ip >>24) & 0xFF);
    ksnprintf(out, cap, "%u.%u.%u.%u", a, b, c, d);
}

/* ---- Ethernet header -------------------------------------------------- */
#define ETH_HDR_LEN  14
#define ET_IPV4      0x0800
#define ET_ARP       0x0806

typedef struct PACKED {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;
} eth_hdr_t;

static const uint8_t BROADCAST_MAC[6] =
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- ARP -------------------------------------------------------------- */
#define ARP_HW_ETH    1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

typedef struct PACKED {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t op;
    uint8_t  sender_mac[6];
    uint32_t sender_ip;
    uint8_t  target_mac[6];
    uint32_t target_ip;
} arp_pkt_t;

typedef struct {
    bool      valid;
    uint32_t  ip;
    uint8_t   mac[6];
    uint32_t  last_seen_ms;
} arp_entry_t;

#define ARP_CACHE_N 16
static arp_entry_t g_arp[ARP_CACHE_N];

static void arp_cache_put(uint32_t ip, const uint8_t mac[6]) {
    /* Update existing entry. */
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(g_arp[i].mac, mac, 6);
            g_arp[i].last_seen_ms = pit_ms();
            return;
        }
    }
    /* Empty slot. */
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (!g_arp[i].valid) {
            g_arp[i].valid = true;
            g_arp[i].ip = ip;
            memcpy(g_arp[i].mac, mac, 6);
            g_arp[i].last_seen_ms = pit_ms();
            return;
        }
    }
    /* Otherwise overwrite oldest. */
    int oldest = 0;
    for (int i = 1; i < ARP_CACHE_N; i++) {
        if (g_arp[i].last_seen_ms < g_arp[oldest].last_seen_ms) oldest = i;
    }
    g_arp[oldest].valid = true;
    g_arp[oldest].ip = ip;
    memcpy(g_arp[oldest].mac, mac, 6);
    g_arp[oldest].last_seen_ms = pit_ms();
}

static bool arp_cache_lookup(uint32_t ip, uint8_t out_mac[6]) {
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(out_mac, g_arp[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void eth_send(const uint8_t dst_mac[6], uint16_t etype,
                     const void *payload, uint16_t len) {
    uint8_t frame[1600];
    if ((uint32_t)(ETH_HDR_LEN + len) > sizeof(frame)) return;
    eth_hdr_t *h = (eth_hdr_t *)frame;
    memcpy(h->dst, dst_mac, 6);
    memcpy(h->src, g_cfg.mac, 6);
    h->ethertype = htons(etype);
    memcpy(frame + ETH_HDR_LEN, payload, len);
    /* Pad to minimum 60 (Ethernet expects 64 incl. FCS, NIC adds the 4). */
    uint16_t total = (uint16_t)(ETH_HDR_LEN + len);
    if (total < 60) {
        memset(frame + total, 0, 60u - total);
        total = 60;
    }
    netif_send(frame, total);
}

static void arp_send_request(uint32_t target_ip) {
    arp_pkt_t pkt = {
        .hw_type    = htons(ARP_HW_ETH),
        .proto_type = htons(ET_IPV4),
        .hw_len     = 6,
        .proto_len  = 4,
        .op         = htons(ARP_OP_REQUEST),
        .sender_ip  = g_cfg.ip,
        .target_mac = { 0 },
        .target_ip  = target_ip,
    };
    memcpy(pkt.sender_mac, g_cfg.mac, 6);
    eth_send(BROADCAST_MAC, ET_ARP, &pkt, sizeof(pkt));
}

static void arp_send_reply(const arp_pkt_t *req) {
    arp_pkt_t pkt = {
        .hw_type    = htons(ARP_HW_ETH),
        .proto_type = htons(ET_IPV4),
        .hw_len     = 6,
        .proto_len  = 4,
        .op         = htons(ARP_OP_REPLY),
        .sender_ip  = g_cfg.ip,
        .target_ip  = req->sender_ip,
    };
    memcpy(pkt.sender_mac, g_cfg.mac, 6);
    memcpy(pkt.target_mac, req->sender_mac, 6);
    eth_send(req->sender_mac, ET_ARP, &pkt, sizeof(pkt));
}

static void arp_handle(const arp_pkt_t *p) {
    if (ntohs(p->hw_type) != ARP_HW_ETH ||
        ntohs(p->proto_type) != ET_IPV4 ||
        p->hw_len != 6 || p->proto_len != 4) return;
    uint16_t op = ntohs(p->op);
    arp_cache_put(p->sender_ip, p->sender_mac);
    if (op == ARP_OP_REQUEST && p->target_ip == g_cfg.ip) {
        arp_send_reply(p);
    }
}

int arp_resolve(uint32_t ip, uint8_t out_mac[6], uint32_t timeout_ms) {
    if (arp_cache_lookup(ip, out_mac)) return 0;
    arp_send_request(ip);
    uint32_t start = pit_ms();
    while (pit_ms() - start < timeout_ms) {
        net_tick();
        if (arp_cache_lookup(ip, out_mac)) return 0;
        kernel_yield();
    }
    return -1;
}

/* ---- IPv4 ------------------------------------------------------------- */
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP  17

typedef struct PACKED {
    uint8_t  ver_ihl;          /* 0x45 = IPv4 + 5-word header */
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} ip_hdr_t;

static uint16_t ip_csum(const void *data, uint16_t bytes) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    for (uint32_t i = 0; (i + 1u) < (uint32_t)bytes; i += 2u) sum += *p++;
    if (bytes & 1) sum += ((const uint8_t *)data)[bytes - 1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static int net_send_ip(uint32_t dst_ip, uint8_t proto,
                       const void *payload, uint16_t paylen) {
    uint8_t pkt[1500];
    if (sizeof(ip_hdr_t) + paylen > sizeof(pkt)) return -1;
    ip_hdr_t *h = (ip_hdr_t *)pkt;
    static uint16_t g_ip_id = 0x4242;
    h->ver_ihl   = 0x45;
    h->tos       = 0;
    h->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + paylen));
    h->id        = htons(++g_ip_id);
    h->frag      = 0;
    h->ttl       = 64;
    h->proto     = proto;
    h->checksum  = 0;
    h->src       = g_cfg.ip;
    h->dst       = dst_ip;
    h->checksum  = ip_csum(h, sizeof(ip_hdr_t));
    memcpy(pkt + sizeof(ip_hdr_t), payload, paylen);

    /* Pick the correct next-hop: on-link target = direct ARP;
     * off-link = resolve gateway's MAC. */
    uint32_t next_hop = dst_ip;
    if ((dst_ip & g_cfg.netmask) != (g_cfg.ip & g_cfg.netmask)) {
        next_hop = g_cfg.gateway;
    }
    uint8_t mac[6];
    if (arp_resolve(next_hop, mac, 500) != 0) return -1;
    eth_send(mac, ET_IPV4, pkt, (uint16_t)(sizeof(ip_hdr_t) + paylen));
    return 0;
}

/* ---- ICMP ------------------------------------------------------------- */
#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0

typedef struct PACKED {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

static volatile uint16_t g_ping_seq = 0;
static volatile bool     g_ping_got = false;
static volatile uint32_t g_ping_t0  = 0;
static volatile uint32_t g_ping_rtt = 0;
static volatile uint16_t g_ping_id  = 0xCAFE;

static void icmp_send_reply(uint32_t dst_ip, const icmp_hdr_t *req,
                            const uint8_t *payload, uint16_t paylen) {
    uint8_t pkt[1500];
    uint16_t total = (uint16_t)(sizeof(icmp_hdr_t) + paylen);
    if (total > sizeof(pkt)) return;
    icmp_hdr_t *h = (icmp_hdr_t *)pkt;
    h->type     = ICMP_ECHO_REPLY;
    h->code     = 0;
    h->checksum = 0;
    h->id       = req->id;
    h->seq      = req->seq;
    memcpy(pkt + sizeof(icmp_hdr_t), payload, paylen);
    h->checksum = ip_csum(pkt, total);
    net_send_ip(dst_ip, IPPROTO_ICMP, pkt, total);
}

static void icmp_handle(uint32_t src_ip, const uint8_t *data, uint16_t len) {
    if (len < sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t *h = (const icmp_hdr_t *)data;
    if (h->type == ICMP_ECHO_REQUEST) {
        icmp_send_reply(src_ip, h, data + sizeof(icmp_hdr_t),
                        len - (uint16_t)sizeof(icmp_hdr_t));
    } else if (h->type == ICMP_ECHO_REPLY) {
        if (ntohs(h->id) == g_ping_id && ntohs(h->seq) == g_ping_seq) {
            g_ping_got = true;
            g_ping_rtt = pit_ms() - g_ping_t0;
        }
    }
}

int net_ping(uint32_t target_ip, uint32_t timeout_ms) {
    if (!g_link) return -1;
    g_ping_seq++;
    g_ping_got = false;
    g_ping_t0 = pit_ms();

    icmp_hdr_t req = {
        .type = ICMP_ECHO_REQUEST,
        .code = 0,
        .checksum = 0,
        .id  = htons(g_ping_id),
        .seq = htons(g_ping_seq),
    };
    uint8_t buf[sizeof(req) + 32];
    memcpy(buf, &req, sizeof(req));
    for (int i = 0; i < 32; i++) buf[sizeof(req) + i] = (uint8_t)('a' + (i & 31));
    icmp_hdr_t *h = (icmp_hdr_t *)buf;
    h->checksum = ip_csum(buf, sizeof(buf));

    if (net_send_ip(target_ip, IPPROTO_ICMP, buf, sizeof(buf)) != 0) return -1;

    while (pit_ms() - g_ping_t0 < timeout_ms) {
        net_tick();
        if (g_ping_got) return (int)g_ping_rtt;
        kernel_yield();
    }
    return -1;
}

/* ---- TCP (very, very minimal) ---------------------------------------- *
 * Single-connection state machine, no congestion control.  Used by the
 * HTTP client and the thin-client browser's proxy uplink.  The three-way
 * handshake is synchronous; data is pushed as it arrives into a per-
 * connection ring buffer (see TCP_RB_BYTES below). */

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

typedef struct PACKED {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

/* Per-connection receive ring buffer.  Sized to comfortably hold a single
 * 1024x768 JPEG frame plus the next tile arriving from the proxy so the
 * advertised window never collapses to zero mid-stream while the browser
 * is busy decoding the previous frame. */
#define TCP_RB_BYTES   (256 * 1024)
typedef enum {
    TCP_S_CLOSED = 0,
    TCP_S_SYN_SENT,
    TCP_S_ESTAB,
    TCP_S_FIN_WAIT,
    TCP_S_CLOSE_WAIT,
    TCP_S_CLOSING,
} tcp_state_t;

typedef struct {
    bool        in_use;
    tcp_state_t state;
    uint32_t    peer_ip;
    uint16_t    peer_port;
    uint16_t    local_port;
    uint32_t    snd_nxt;
    uint32_t    rcv_nxt;
    uint8_t     rb[TCP_RB_BYTES];
    uint32_t    rb_head;     /* writer pos (set by net_tick on recv)    */
    uint32_t    rb_tail;     /* reader pos (set by tcp_recv consumer)    */
    bool        peer_closed;
    /* TIME_WAIT-lite: after tcp_close() the slot lingers in FIN_WAIT
     * until this deadline so the peer's FIN/ACK still matches a known
     * connection (and gets ACKed) instead of triggering an RST per
     * segment.  Reclaimed lazily by the tcp_connect slot scan. */
    uint32_t    close_deadline_ms;
    /* Single-segment retransmit buffer.  The browser's uplink is short
     * command lines (Maps/CLICK/PING fit one segment); without ANY
     * retransmission one lost command segment gapped the stream forever
     * — the peer buffered everything after the hole and the app went
     * deaf for commands until a watchdog reconnect.  Keeping the LAST
     * data segment + the peer's ack level closes that hole for the
     * traffic pattern we actually have. */
    uint32_t    snd_una;             /* highest peer-acked sequence      */
    uint8_t     rtx_buf[1024];
    uint16_t    rtx_len;             /* 0 = nothing awaiting ack         */
    uint32_t    rtx_seq;             /* first seq of rtx_buf             */
    uint32_t    rtx_sent_ms;         /* last (re)send time               */
    uint8_t     rtx_tries;
} tcp_conn_t;

#define TCP_MAX_CONN 8
static tcp_conn_t g_tcp[TCP_MAX_CONN];
static uint16_t   g_next_local_port = 0xC000;

static uint16_t tcp_csum(uint32_t src_ip, uint32_t dst_ip,
                         const tcp_hdr_t *hdr, const uint8_t *payload,
                         uint16_t paylen) {
    uint32_t sum = 0;
    sum += (src_ip & 0xFFFF) + ((src_ip >> 16) & 0xFFFF);
    sum += (dst_ip & 0xFFFF) + ((dst_ip >> 16) & 0xFFFF);
    sum += htons(IPPROTO_TCP);
    uint16_t tcplen = (uint16_t)(sizeof(tcp_hdr_t) + paylen);
    sum += htons(tcplen);

    const uint16_t *p = (const uint16_t *)hdr;
    for (uint32_t i = 0; i < sizeof(tcp_hdr_t) / 2; i++) sum += p[i];
    p = (const uint16_t *)payload;
    uint16_t even_bytes = (uint16_t)(paylen & ~1u);
    for (uint16_t i = 0; i < even_bytes / 2; i++) sum += p[i];
    if (paylen & 1) sum += ((uint16_t)payload[paylen - 1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void tcp_send_seg(tcp_conn_t *c, uint8_t flags,
                         const uint8_t *payload, uint16_t paylen) {
    uint8_t pkt[1500];
    if (sizeof(tcp_hdr_t) + paylen > sizeof(pkt)) return;
    uint32_t seq0 = c->snd_nxt;
    tcp_hdr_t *h = (tcp_hdr_t *)pkt;
    memset(h, 0, sizeof(*h));
    h->src_port  = htons(c->local_port);
    h->dst_port  = htons(c->peer_port);
    h->seq       = htonl(seq0);
    h->ack       = htonl(c->rcv_nxt);
    h->data_off  = 0x50;             /* 5 * 4 = 20-byte header */
    h->flags     = flags;
    /* Dynamic window: advertise how much free space is in the ring buffer. */
    uint32_t avail = (c->rb_head + TCP_RB_BYTES - c->rb_tail) % TCP_RB_BYTES;
    uint32_t free_space = TCP_RB_BYTES - avail - 1;
    uint16_t adv_win = (free_space > 0xFFFFu) ? 0xFFFFu : (uint16_t)free_space;
    h->window    = htons(adv_win);
    if (paylen) memcpy(pkt + sizeof(tcp_hdr_t), payload, paylen);
    h->checksum  = tcp_csum(g_cfg.ip, c->peer_ip, h,
                            pkt + sizeof(tcp_hdr_t), paylen);
    net_send_ip(c->peer_ip, IPPROTO_TCP, pkt,
                (uint16_t)(sizeof(tcp_hdr_t) + paylen));
    if (flags & TCP_SYN) c->snd_nxt++;
    if (flags & TCP_FIN) c->snd_nxt++;
    c->snd_nxt += paylen;
    /* Stash the most recent data segment for the RTO retransmit (the
     * uplink is single-segment command lines; see tcp_conn_t.rtx_buf). */
    if (paylen > 0 && paylen <= sizeof(c->rtx_buf)) {
        memcpy(c->rtx_buf, payload, paylen);
        c->rtx_len     = paylen;
        c->rtx_seq     = seq0;
        c->rtx_sent_ms = pit_ms();
        c->rtx_tries   = 0;
    }
}

/* Resend the stashed segment verbatim (same seq — does NOT advance
 * snd_nxt), with a current ack/window. */
static void tcp_retransmit(tcp_conn_t *c) {
    uint8_t pkt[1500];
    tcp_hdr_t *h = (tcp_hdr_t *)pkt;
    memset(h, 0, sizeof(*h));
    h->src_port  = htons(c->local_port);
    h->dst_port  = htons(c->peer_port);
    h->seq       = htonl(c->rtx_seq);
    h->ack       = htonl(c->rcv_nxt);
    h->data_off  = 0x50;
    h->flags     = TCP_PSH | TCP_ACK;
    uint32_t avail = (c->rb_head + TCP_RB_BYTES - c->rb_tail) % TCP_RB_BYTES;
    uint32_t free_space = TCP_RB_BYTES - avail - 1;
    h->window    = htons((free_space > 0xFFFFu) ? 0xFFFFu
                                                : (uint16_t)free_space);
    memcpy(pkt + sizeof(tcp_hdr_t), c->rtx_buf, c->rtx_len);
    h->checksum  = tcp_csum(g_cfg.ip, c->peer_ip, h,
                            pkt + sizeof(tcp_hdr_t), c->rtx_len);
    net_send_ip(c->peer_ip, IPPROTO_TCP, pkt,
                (uint16_t)(sizeof(tcp_hdr_t) + c->rtx_len));
}

/* RTO scan, driven from net_tick(): one outstanding segment per
 * connection, 400 ms timeout, 6 tries, then the connection is declared
 * dead so the app's reconnect logic takes over. */
#define TCP_RTO_MS      400
#define TCP_RTO_TRIES   6
static void tcp_rtx_tick(void) {
    /* net_send_ip can nest back into net_tick via arp_resolve's wait
     * loop; don't let the scan recurse into itself. */
    static bool busy = false;
    if (busy) return;
    busy = true;
    uint32_t now = pit_ms();
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        tcp_conn_t *c = &g_tcp[i];
        if (!c->in_use || c->state != TCP_S_ESTAB || c->rtx_len == 0)
            continue;
        if ((int32_t)(now - c->rtx_sent_ms) < TCP_RTO_MS) continue;
        if (c->rtx_tries >= TCP_RTO_TRIES) {
            debug_printf("[net] tcp: port %u dead after %u retransmits\n",
                         c->local_port, (unsigned)c->rtx_tries);
            c->state  = TCP_S_CLOSED;
            c->in_use = false;
            continue;
        }
        c->rtx_tries++;
        c->rtx_sent_ms = now;
        debug_printf("[net] tcp: RTO retransmit #%u on port %u "
                     "(seq %u, %u bytes)\n", (unsigned)c->rtx_tries,
                     c->local_port, c->rtx_seq, (unsigned)c->rtx_len);
        tcp_retransmit(c);
    }
    busy = false;
}

static void tcp_handle(uint32_t src_ip, const uint8_t *data, uint16_t len) {
    if (len < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *h = (const tcp_hdr_t *)data;
    uint16_t hdr_words = (h->data_off >> 4) * 4u;
    if (hdr_words < sizeof(tcp_hdr_t) || hdr_words > len) return;
    uint16_t plen = (uint16_t)(len - hdr_words);
    const uint8_t *pay = data + hdr_words;

    uint16_t dst_port = ntohs(h->dst_port);
    uint16_t src_port = ntohs(h->src_port);
    tcp_conn_t *c = NULL;
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (g_tcp[i].in_use && g_tcp[i].local_port == dst_port &&
            g_tcp[i].peer_ip == src_ip && g_tcp[i].peer_port == src_port) {
            c = &g_tcp[i];
            break;
        }
    }
    if (!c) {
        /* Reject - send RST.  CRITICAL: we used to build a synthetic
         * tcp_conn_t on the stack and hand it to tcp_send_seg.  After
         * we grew TCP_RB_BYTES to 256 KiB that struct ballooned to
         * ~256 KiB too, blowing right past our 64 KiB kernel stack and
         * (because GCC stack-probes by writing every page below ESP)
         * silently zeroing pages of the kernel's .text section.  Any
         * function whose code landed in one of those zeroed pages
         * later faulted with #UD or #GP at a phantom EIP — that was
         * the "browser crashes right after the proxy connects"
         * mystery.  Build the RST inline instead so no stack copy of
         * tcp_conn_t is ever made. */
        if (!(h->flags & TCP_RST)) {
            /* RFC 793 reset generation: if the offending segment carries
             * an ACK, the RST must use THAT ack as its own sequence
             * number (and no ACK flag).  We used to send seq=0 always —
             * real peer stacks treat that as out-of-window (RFC 5961),
             * IGNORE the RST and keep the connection half-open: the
             * proxy host accumulated zombie sockets after every browser
             * reconnect.  seq=seg.ack lands exactly in-window. */
            uint8_t pkt[64];
            tcp_hdr_t *rst = (tcp_hdr_t *)pkt;
            memset(rst, 0, sizeof(*rst));
            rst->src_port = htons(dst_port);
            rst->dst_port = htons(src_port);
            rst->data_off = 0x50;
            rst->window   = htons(0);
            if (h->flags & TCP_ACK) {
                rst->seq   = h->ack;            /* already network order */
                rst->ack   = htonl(0);
                rst->flags = TCP_RST;
            } else {
                uint32_t ack_seq = ntohl(h->seq) + plen +
                                   ((h->flags & TCP_SYN) ? 1 : 0) +
                                   ((h->flags & TCP_FIN) ? 1 : 0);
                rst->seq   = htonl(0);
                rst->ack   = htonl(ack_seq);
                rst->flags = TCP_RST | TCP_ACK;
            }
            rst->checksum = tcp_csum(g_cfg.ip, src_ip, rst, NULL, 0);
            debug_printf("[net] tcp: RST to %u (no conn for local port %u, "
                         "flags 0x%02x)\n", src_port, dst_port, h->flags);
            net_send_ip(src_ip, IPPROTO_TCP, pkt,
                        (uint16_t)sizeof(tcp_hdr_t));
        }
        return;
    }

    uint32_t seg_seq = ntohl(h->seq);

    if (h->flags & TCP_RST) {
        debug_printf("[net] tcp: RST from peer on port %u (state %d)\n",
                     c->local_port, (int)c->state);
        c->state = TCP_S_CLOSED;
        c->in_use = false;
        return;
    }

    /* Track the peer's cumulative ack so the single-segment retransmit
     * buffer can retire (see tcp_send / net_tick). */
    if (h->flags & TCP_ACK) {
        uint32_t seg_ack = ntohl(h->ack);
        if ((int32_t)(seg_ack - c->snd_una) > 0) c->snd_una = seg_ack;
        if (c->rtx_len &&
            (int32_t)(c->snd_una - (c->rtx_seq + c->rtx_len)) >= 0) {
            c->rtx_len = 0;                  /* fully acknowledged */
            c->rtx_tries = 0;
        }
    }

    if (c->state == TCP_S_SYN_SENT) {
        if ((h->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            c->rcv_nxt = seg_seq + 1;
            c->snd_una = c->snd_nxt;
            c->state = TCP_S_ESTAB;
            debug_printf("[net] tcp: port %u ESTABLISHED with %u\n",
                         c->local_port, c->peer_port);
            tcp_send_seg(c, TCP_ACK, NULL, 0);
            return;
        }
    }

    if (c->state == TCP_S_ESTAB || c->state == TCP_S_FIN_WAIT ||
        c->state == TCP_S_CLOSE_WAIT) {
        /* Partial overlap: a retransmitted segment whose head we already
         * consumed (seq < rcv_nxt < seq+len) still carries NEW bytes in
         * its tail.  Trim the consumed head and accept the rest — without
         * this, one clamped/half-written segment made every retransmission
         * of it look like a duplicate and the connection went deaf. */
        if (plen > 0 && (int32_t)(c->rcv_nxt - seg_seq) > 0 &&
            (int32_t)(seg_seq + plen - c->rcv_nxt) > 0) {
            uint32_t skip = c->rcv_nxt - seg_seq;
            pay += skip;
            plen = (uint16_t)(plen - skip);
            seg_seq = c->rcv_nxt;
        }
        if (plen > 0 && seg_seq == c->rcv_nxt) {
            /* Check for ring buffer overflow before writing.  If the
             * incoming segment would overwrite unread data, clamp the
             * write to available space and advertise a zero window to
             * force the sender to pause. */
            uint32_t avail = (c->rb_head + TCP_RB_BYTES - c->rb_tail) % TCP_RB_BYTES;
            uint32_t free_space = TCP_RB_BYTES - avail - 1;  /* -1 to keep head!=tail */
            uint16_t write_len = (plen > free_space) ? (uint16_t)free_space : plen;
            for (uint16_t i = 0; i < write_len; i++) {
                uint32_t pos = (c->rb_head + i) % TCP_RB_BYTES;
                c->rb[pos] = pay[i];
            }
            c->rb_head = (c->rb_head + write_len) % TCP_RB_BYTES;
            c->rcv_nxt += write_len;
            /* Advertise remaining window so sender knows our capacity. */
            uint32_t new_avail = (c->rb_head + TCP_RB_BYTES - c->rb_tail) % TCP_RB_BYTES;
            uint32_t new_free = TCP_RB_BYTES - new_avail - 1;
            uint16_t adv_win = (new_free > 0xFFFFu) ? 0xFFFFu : (uint16_t)new_free;
            /* Send ACK with updated window. */
            uint8_t ack_pkt[1500];
            if (sizeof(tcp_hdr_t) <= sizeof(ack_pkt)) {
                tcp_hdr_t *ah = (tcp_hdr_t *)ack_pkt;
                memset(ah, 0, sizeof(*ah));
                ah->src_port  = htons(c->local_port);
                ah->dst_port  = htons(c->peer_port);
                ah->seq       = htonl(c->snd_nxt);
                ah->ack       = htonl(c->rcv_nxt);
                ah->data_off  = 0x50;
                ah->flags     = TCP_ACK;
                ah->window    = htons(adv_win);
                ah->checksum  = tcp_csum(g_cfg.ip, c->peer_ip, ah, NULL, 0);
                net_send_ip(c->peer_ip, IPPROTO_TCP, ack_pkt,
                            (uint16_t)sizeof(tcp_hdr_t));
            }
        } else if (plen > 0 && seg_seq != c->rcv_nxt) {
            /* Out-of-order or duplicate segment.  Emit a duplicate ACK so
             * the sender's RFC 5681 fast-retransmit path kicks in after
             * three dup-ACKs instead of waiting for the full RTO (~1-3s).
             * Done via tcp_send_seg so we don't open-code yet another
             * IRQ-context 1500-byte stack buffer (which was triggering
             * a #GP through nested IRQ/ARP-resolve re-entry on the
             * post-connect path). */
            tcp_send_seg(c, TCP_ACK, NULL, 0);
        }
        if ((h->flags & TCP_FIN) && seg_seq + plen != c->rcv_nxt) {
            /* Out-of-order FIN: data is still missing in front of it
             * (dropped segments in flight).  Processing it here used to
             * mark peer_closed early and the reader saw a truncated-EOF
             * body.  Re-ACK the current edge and wait for the peer to
             * retransmit the hole; the FIN comes back after it. */
            tcp_send_seg(c, TCP_ACK, NULL, 0);
        } else if (h->flags & TCP_FIN) {
            c->rcv_nxt++;
            c->peer_closed = true;
            tcp_send_seg(c, TCP_ACK, NULL, 0);
            if (c->state == TCP_S_ESTAB) {
                debug_printf("[net] tcp: peer FIN on port %u -> CLOSE_WAIT\n",
                             c->local_port);
                c->state = TCP_S_CLOSE_WAIT;
            } else if (c->state == TCP_S_FIN_WAIT) {
                /* Both directions closed and ACKed — the lingering slot
                 * has served its purpose. */
                debug_printf("[net] tcp: close handshake done on port %u\n",
                             c->local_port);
                c->state  = TCP_S_CLOSED;
                c->in_use = false;
            }
        }
    }
}

static int tcp_slot_scan(void) {
    int slot = -1;
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        /* Lazily reclaim lingering FIN_WAIT carcasses whose grace period
         * has passed (see tcp_close). */
        if (g_tcp[i].in_use && g_tcp[i].state == TCP_S_FIN_WAIT &&
            (int32_t)(pit_ms() - g_tcp[i].close_deadline_ms) >= 0) {
            debug_printf("[net] tcp: port %u linger expired, slot %d freed\n",
                         g_tcp[i].local_port, i);
            g_tcp[i].in_use = false;
            g_tcp[i].state  = TCP_S_CLOSED;
        }
        if (!g_tcp[i].in_use && slot < 0) slot = i;
    }
    return slot;
}

tcp_handle_t tcp_connect(uint32_t dst_ip, uint16_t dst_port,
                          uint32_t timeout_ms) {
    int slot = tcp_slot_scan();
    if (slot < 0 && timeout_ms > 0) {
        /* Every slot is a lingering FIN_WAIT carcass (back-to-back
         * downloads land here).  Failing instantly used to burn a whole
         * retry round; the oldest linger expires within 2 s, so wait for
         * it instead. */
        debug_printf("[net] tcp: all slots lingering, waiting for one\n");
        uint32_t w0 = pit_ms();
        while (slot < 0 && pit_ms() - w0 < 2500) {
            net_tick();
            kernel_yield();
            slot = tcp_slot_scan();
        }
    }
    if (slot < 0) {
        debug_printf("[net] tcp: connect failed — no free slot\n");
        return TCP_INVALID;
    }

    tcp_conn_t *c = &g_tcp[slot];
    memset(c, 0, sizeof(*c));
    c->in_use     = true;
    c->state      = TCP_S_SYN_SENT;
    c->peer_ip    = dst_ip;
    c->peer_port  = dst_port;
    c->local_port = g_next_local_port++;
    c->snd_nxt    = 0;
    c->rcv_nxt    = 0;
    tcp_send_seg(c, TCP_SYN, NULL, 0);

    /* Non-blocking mode: return immediately after SYN.  Caller must poll
     * with tcp_is_connected() to detect handshake completion. */
    if (timeout_ms == 0) return (tcp_handle_t)slot;

    uint32_t t0 = pit_ms();
    uint32_t last_syn = t0;
    while (pit_ms() - t0 < timeout_ms) {
        net_tick();
        kernel_yield();
        if (c->state == TCP_S_ESTAB) return (tcp_handle_t)slot;
        /* SYN retransmit: a single lost SYN used to mean waiting out the
         * whole timeout in silence (the loop only listened).  Re-send with
         * the same ISS every second until the SYN-ACK lands. */
        if (c->state == TCP_S_SYN_SENT &&
            (uint32_t)(pit_ms() - last_syn) >= 1000u) {
            c->snd_nxt--;                    /* tcp_send_seg re-consumes it */
            tcp_send_seg(c, TCP_SYN, NULL, 0);
            last_syn = pit_ms();
            debug_printf("[net] tcp: SYN retransmit on port %u\n",
                         c->local_port);
        }
    }
    c->in_use = false;
    return TCP_INVALID;
}

int tcp_send(tcp_handle_t s, const void *buf, uint32_t len) {
    if (s < 0 || s >= TCP_MAX_CONN) return -1;
    tcp_conn_t *c = &g_tcp[s];
    if (!c->in_use || c->state != TCP_S_ESTAB) return -1;
    /* Naive sender: chunk into 1024-byte segments. */
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t left = len;
    while (left) {
        uint32_t chunk = left > 1024 ? 1024 : left;
        tcp_send_seg(c, TCP_PSH | TCP_ACK, p, (uint16_t)chunk);
        p += chunk;
        left -= chunk;
    }
    return (int)len;
}

int tcp_recv(tcp_handle_t s, void *buf, uint32_t cap, uint32_t timeout_ms) {
    if (s < 0 || s >= TCP_MAX_CONN) return -1;
    tcp_conn_t *c = &g_tcp[s];
    if (!c->in_use || c->state == TCP_S_FIN_WAIT) return -1;

    uint32_t t0 = pit_ms();
    while (1) {
        net_tick();
        uint32_t avail = (c->rb_head + TCP_RB_BYTES - c->rb_tail) % TCP_RB_BYTES;
        if (avail > 0) {
            uint32_t n = avail < cap ? avail : cap;
            uint8_t *out = (uint8_t *)buf;
            for (uint32_t i = 0; i < n; i++) {
                out[i] = c->rb[(c->rb_tail + i) % TCP_RB_BYTES];
            }
            c->rb_tail = (c->rb_tail + n) % TCP_RB_BYTES;
            
            if (c->state == TCP_S_ESTAB || c->state == TCP_S_CLOSE_WAIT) {
                uint32_t new_avail = (c->rb_head + TCP_RB_BYTES - c->rb_tail) % TCP_RB_BYTES;
                uint32_t new_free = TCP_RB_BYTES - new_avail - 1;
                uint16_t adv_win = (new_free > 0xFFFFu) ? 0xFFFFu : (uint16_t)new_free;
                
                uint8_t ack_pkt[1500];
                if (sizeof(tcp_hdr_t) <= sizeof(ack_pkt)) {
                    tcp_hdr_t *ah = (tcp_hdr_t *)ack_pkt;
                    memset(ah, 0, sizeof(*ah));
                    ah->src_port  = htons(c->local_port);
                    ah->dst_port  = htons(c->peer_port);
                    ah->seq       = htonl(c->snd_nxt);
                    ah->ack       = htonl(c->rcv_nxt);
                    ah->data_off  = 0x50;
                    ah->flags     = TCP_ACK;
                    ah->window    = htons(adv_win);
                    ah->checksum  = tcp_csum(g_cfg.ip, c->peer_ip, ah, NULL, 0);
                    net_send_ip(c->peer_ip, IPPROTO_TCP, ack_pkt, (uint16_t)sizeof(tcp_hdr_t));
                }
            }
            
            return (int)n;
        }
        if (c->peer_closed && avail == 0) return 0;
        if (timeout_ms == 0) return -1;  /* non-blocking: immediate return */
        if (pit_ms() - t0 >= timeout_ms) return -1;
        kernel_yield();
    }
}

void tcp_close(tcp_handle_t s) {
    if (s < 0 || s >= TCP_MAX_CONN) return;
    tcp_conn_t *c = &g_tcp[s];
    if (!c->in_use) return;
    if (c->state == TCP_S_ESTAB || c->state == TCP_S_CLOSE_WAIT) {
        tcp_send_seg(c, TCP_FIN | TCP_ACK, NULL, 0);
        /* Linger in FIN_WAIT instead of freeing the slot on the spot:
         * the peer's FIN/ACK is still in flight, and with the slot gone
         * each of those segments hit the no-connection path and drew an
         * RST.  The slot is invisible to the app from here on
         * (tcp_is_open == false) and is reclaimed by the tcp_connect
         * slot scan after the deadline, or as soon as the peer's FIN
         * completes the handshake. */
        c->state = TCP_S_FIN_WAIT;
        c->close_deadline_ms = pit_ms() + 2000;
        debug_printf("[net] tcp: close port %u -> FIN_WAIT (linger)\n",
                     c->local_port);
        return;
    }
    debug_printf("[net] tcp: close port %u (state %d) -> free\n",
                 c->local_port, (int)c->state);
    c->state = TCP_S_CLOSED;
    c->in_use = false;
}

bool tcp_is_open(tcp_handle_t s) {
    if (s < 0 || s >= TCP_MAX_CONN) return false;
    /* FIN_WAIT slots are lingering carcasses kept only to absorb the
     * peer's closing segments — the app-level handle is dead. */
    return g_tcp[s].in_use && g_tcp[s].state != TCP_S_FIN_WAIT;
}

bool tcp_is_connected(tcp_handle_t s) {
    if (s < 0 || s >= TCP_MAX_CONN) return false;
    return g_tcp[s].in_use && g_tcp[s].state == TCP_S_ESTAB;
}

uint32_t tcp_available(tcp_handle_t s) {
    if (s < 0 || s >= TCP_MAX_CONN) return 0;
    tcp_conn_t *c = &g_tcp[s];
    if (!c->in_use || c->state == TCP_S_FIN_WAIT) return 0;
    return (c->rb_head + TCP_RB_BYTES - c->rb_tail) % TCP_RB_BYTES;
}

/* ---- Top-level frame dispatch ---------------------------------------- */
/* Per-port UDP listener — single-slot, single-consumer.  Used by DHCP and
 * NTP to receive their reply datagrams synchronously without instantiating
 * a full socket abstraction.  Capacity matches the largest DHCPACK we
 * expect (~ 590 bytes incl. options). */
#define UDP_RX_MAX  768
typedef struct {
    bool     active;
    uint16_t port;
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t length;
    uint8_t  buf[UDP_RX_MAX];
} udp_listener_t;

#define UDP_LISTENERS 12
static udp_listener_t g_udp_listeners[UDP_LISTENERS];

typedef struct PACKED {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

/* Build + send a raw IPv4 datagram with a caller-supplied source IP so
 * DHCP DISCOVER can ship from 0.0.0.0 to 255.255.255.255 before the
 * stack knows its own address.  Bypasses ARP entirely when dst_ip is the
 * full broadcast (0xFFFFFFFF) — uses FF:FF:FF:FF:FF:FF as the MAC. */
static int net_send_ip_raw(uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                           const void *payload, uint16_t paylen,
                           const uint8_t *dst_mac_override) {
    uint8_t pkt[1500];
    if (sizeof(ip_hdr_t) + paylen > sizeof(pkt)) return -1;
    ip_hdr_t *h = (ip_hdr_t *)pkt;
    static uint16_t g_ip_id_raw = 0x6000;
    h->ver_ihl   = 0x45;
    h->tos       = 0;
    h->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + paylen));
    h->id        = htons(++g_ip_id_raw);
    h->frag      = 0;
    h->ttl       = 64;
    h->proto     = proto;
    h->checksum  = 0;
    h->src       = src_ip;
    h->dst       = dst_ip;
    h->checksum  = ip_csum(h, sizeof(ip_hdr_t));
    memcpy(pkt + sizeof(ip_hdr_t), payload, paylen);

    uint8_t mac[6];
    if (dst_mac_override) {
        memcpy(mac, dst_mac_override, 6);
    } else if (dst_ip == 0xFFFFFFFFu) {
        memset(mac, 0xFF, 6);
    } else {
        uint32_t next_hop = dst_ip;
        if ((dst_ip & g_cfg.netmask) != (g_cfg.ip & g_cfg.netmask)) {
            next_hop = g_cfg.gateway;
        }
        if (arp_resolve(next_hop, mac, 500) != 0) return -1;
    }
    eth_send(mac, ET_IPV4, pkt, (uint16_t)(sizeof(ip_hdr_t) + paylen));
    return 0;
}

/* UDP checksum is optional on IPv4 — zero means "skip".  DHCP servers
 * accept that, so we don't bother computing one. */
int net_send_udp(uint32_t src_ip, uint32_t dst_ip,
                 uint16_t src_port, uint16_t dst_port,
                 const void *payload, uint16_t paylen) {
    uint8_t pkt[1500];
    if (sizeof(udp_hdr_t) + paylen > sizeof(pkt)) return -1;
    udp_hdr_t *u = (udp_hdr_t *)pkt;
    u->src_port = htons(src_port);
    u->dst_port = htons(dst_port);
    u->length   = htons((uint16_t)(sizeof(udp_hdr_t) + paylen));
    u->checksum = 0;
    memcpy(pkt + sizeof(udp_hdr_t), payload, paylen);
    return net_send_ip_raw(src_ip, dst_ip, IPPROTO_UDP,
                           pkt, (uint16_t)(sizeof(udp_hdr_t) + paylen),
                           NULL);
}

/* Reserve a UDP listener slot for a given local port.  Returns the slot
 * index (>=0) on success, -1 if all listeners are busy. */
int net_udp_listen(uint16_t port) {
    for (int i = 0; i < UDP_LISTENERS; i++) {
        if (g_udp_listeners[i].active && g_udp_listeners[i].port == port) {
            g_udp_listeners[i].length = 0;
            return i;
        }
    }
    for (int i = 0; i < UDP_LISTENERS; i++) {
        if (!g_udp_listeners[i].active) {
            g_udp_listeners[i].active   = true;
            g_udp_listeners[i].port     = port;
            g_udp_listeners[i].length   = 0;
            g_udp_listeners[i].src_ip   = 0;
            g_udp_listeners[i].src_port = 0;
            return i;
        }
    }
    return -1;
}

void net_udp_unlisten(int slot) {
    if (slot < 0 || slot >= UDP_LISTENERS) return;
    g_udp_listeners[slot].active = false;
    g_udp_listeners[slot].length = 0;
}

/* Blocking receive: spins on net_tick() until a datagram lands on the
 * reserved slot or the timeout expires.  Returns the byte count copied
 * out of the listener's buffer, or -1 on timeout. */
int net_udp_recv(int slot, void *buf, uint16_t cap,
                 uint32_t *out_src_ip, uint16_t *out_src_port,
                 uint32_t timeout_ms) {
    if (slot < 0 || slot >= UDP_LISTENERS) return -1;
    if (!g_udp_listeners[slot].active) return -1;
    uint32_t t0 = pit_ms();
    while (pit_ms() - t0 < timeout_ms) {
        net_tick();
        if (g_udp_listeners[slot].length > 0) {
            uint16_t n = g_udp_listeners[slot].length;
            if (n > cap) n = cap;
            memcpy(buf, g_udp_listeners[slot].buf, n);
            if (out_src_ip)   *out_src_ip   = g_udp_listeners[slot].src_ip;
            if (out_src_port) *out_src_port = g_udp_listeners[slot].src_port;
            g_udp_listeners[slot].length = 0;
            return (int)n;
        }
    }
    return -1;
}

/* Non-blocking: hand back a buffered datagram if one is waiting, else 0.  No
 * spin, no net_tick() — the caller pumps the NIC from its own idle loop. */
int net_udp_poll(int slot, void *buf, uint16_t cap,
                 uint32_t *out_src_ip, uint16_t *out_src_port) {
    if (slot < 0 || slot >= UDP_LISTENERS) return -1;
    if (!g_udp_listeners[slot].active) return -1;
    if (g_udp_listeners[slot].length == 0) return 0;
    uint16_t n = g_udp_listeners[slot].length;
    if (n > cap) n = cap;
    memcpy(buf, g_udp_listeners[slot].buf, n);
    if (out_src_ip)   *out_src_ip   = g_udp_listeners[slot].src_ip;
    if (out_src_port) *out_src_port = g_udp_listeners[slot].src_port;
    g_udp_listeners[slot].length = 0;
    return (int)n;
}

/* ============================================================================
 * Minimal DNS A-record resolver (RFC 1035)
 * ----------------------------------------------------------------------------
 * Builds a single-question packet, ships it to g_cfg.dns:53 from an
 * ephemeral source port, and parses the first A answer out of the reply.
 * Compression pointers in the answer's NAME field are tolerated by
 * skipping past them - we do not actually decode the name back, the
 * RDATA offset is what we care about.
 * ============================================================================ */

/* Encode "reddit.com" into DNS label form ("\x06reddit\x03com\x00").
 * Returns the encoded length, or -1 if the label sequence is malformed. */
static int dns_encode_name(const char *host, uint8_t *out, int cap) {
    int o = 0;
    const char *p = host;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '.') p++;
        int seg_len = (int)(p - seg);
        if (seg_len == 0 || seg_len > 63) return -1;
        if (o + 1 + seg_len + 1 > cap) return -1;
        out[o++] = (uint8_t)seg_len;
        memcpy(out + o, seg, (size_t)seg_len);
        o += seg_len;
        if (*p == '.') p++;
    }
    if (o + 1 > cap) return -1;
    out[o++] = 0x00;     /* root label */
    return o;
}

/* Skip a DNS name in the response.  Handles both label-by-label and
 * compression pointers.  Returns the new offset, or -1 on bounds error. */
static int dns_skip_name(const uint8_t *pkt, int len, int off) {
    while (off < len) {
        uint8_t b = pkt[off];
        if (b == 0x00) return off + 1;
        if ((b & 0xC0) == 0xC0) {        /* compression pointer (2 bytes) */
            if (off + 2 > len) return -1;
            return off + 2;
        }
        if (b > 63) return -1;
        off += 1 + b;
    }
    return -1;
}

int net_dns_resolve(const char *host, uint32_t *out_ip, uint32_t timeout_ms) {
    if (!host || !out_ip) return -1;
    *out_ip = 0;

    if (strcmp(host, "localhost") == 0) {
        *out_ip = 0x0100007Fu;
        return 0;
    }

    if (net_hosts_lookup(host, out_ip) == 0)
        return 0;

    /* Accept a literal dotted-quad without round-tripping through DNS. */
    {
        int dots = 0; bool only_dq = true;
        for (const char *p = host; *p; p++) {
            if (*p == '.') dots++;
            else if (*p < '0' || *p > '9') { only_dq = false; break; }
        }
        if (only_dq && dots == 3) {
            *out_ip = net_ip_aton(host);
            return *out_ip ? 0 : -1;
        }
    }

    net_config_t nc;
    net_get_config(&nc);
    uint32_t dns = nc.dns ? nc.dns : nc.gateway;
    if (!dns) return -1;

    uint16_t src_port = 0xC000 | (uint16_t)(pit_ms() & 0x3FFF);
    int slot = net_udp_listen(src_port);
    if (slot < 0) return -1;

    uint8_t  q[300];
    uint16_t xid = (uint16_t)(pit_ms() & 0xFFFFu);
    q[0] = (uint8_t)(xid >> 8); q[1] = (uint8_t)(xid);
    q[2] = 0x01; q[3] = 0x00;   /* flags: standard query, recursion desired */
    q[4] = 0x00; q[5] = 0x01;   /* QDCOUNT = 1                              */
    q[6] = 0x00; q[7] = 0x00;
    q[8] = 0x00; q[9] = 0x00;
    q[10] = 0x00; q[11] = 0x00;
    int nlen = dns_encode_name(host, q + 12, (int)sizeof(q) - 12 - 4);
    if (nlen < 0) { net_udp_unlisten(slot); return -1; }
    int qlen = 12 + nlen;
    q[qlen++] = 0x00; q[qlen++] = 0x01;     /* QTYPE A   */
    q[qlen++] = 0x00; q[qlen++] = 0x01;     /* QCLASS IN */

    if (net_send_udp(g_cfg.ip, dns, src_port, 53, q, (uint16_t)qlen) < 0) {
        net_udp_unlisten(slot);
        return -1;
    }

    uint8_t  rx[512];
    uint32_t deadline = pit_ms() + timeout_ms;
    while (pit_ms() < deadline) {
        int n = net_udp_recv(slot, rx, sizeof(rx), NULL, NULL,
                             deadline - pit_ms());
        if (n <= 0) continue;
        if (n < 12) continue;
        uint16_t resp_xid  = ((uint16_t)rx[0] << 8) | rx[1];
        if (resp_xid != xid) continue;
        uint16_t ancount   = ((uint16_t)rx[6] << 8) | rx[7];
        if (ancount == 0) { net_udp_unlisten(slot); return -1; }
        int off = 12;
        /* Skip the echoed question (name + QTYPE + QCLASS). */
        off = dns_skip_name(rx, n, off);
        if (off < 0 || off + 4 > n) { net_udp_unlisten(slot); return -1; }
        off += 4;
        for (uint16_t a = 0; a < ancount; a++) {
            off = dns_skip_name(rx, n, off);
            if (off < 0 || off + 10 > n) break;
            uint16_t atype = ((uint16_t)rx[off] << 8) | rx[off + 1];
            uint16_t rdlen = ((uint16_t)rx[off + 8] << 8) | rx[off + 9];
            off += 10;
            if (off + rdlen > n) break;
            if (atype == 0x0001 && rdlen == 4) {
                uint32_t ip =  (uint32_t)rx[off]
                            | ((uint32_t)rx[off + 1] << 8)
                            | ((uint32_t)rx[off + 2] << 16)
                            | ((uint32_t)rx[off + 3] << 24);
                *out_ip = ip;
                net_udp_unlisten(slot);
                return 0;
            }
            off += rdlen;
        }
        break;
    }
    net_udp_unlisten(slot);
    return -1;
}

static void udp_handle(uint32_t src_ip, const uint8_t *data, uint16_t len) {
    if (len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t *u = (const udp_hdr_t *)data;
    uint16_t dst_port = ntohs(u->dst_port);
    uint16_t src_port = ntohs(u->src_port);
    uint16_t ulen     = ntohs(u->length);
    if (ulen < sizeof(udp_hdr_t) || ulen > len) return;
    uint16_t paylen   = (uint16_t)(ulen - sizeof(udp_hdr_t));

    for (int i = 0; i < UDP_LISTENERS; i++) {
        if (!g_udp_listeners[i].active) continue;
        if (g_udp_listeners[i].port != dst_port) continue;
        if (paylen > UDP_RX_MAX) paylen = UDP_RX_MAX;
        memcpy(g_udp_listeners[i].buf, data + sizeof(udp_hdr_t), paylen);
        g_udp_listeners[i].length   = paylen;
        g_udp_listeners[i].src_ip   = src_ip;
        g_udp_listeners[i].src_port = src_port;
        return;
    }
}

static void net_rx(const uint8_t *frame, uint16_t len, void *user) {
    (void)user;
    if (len < ETH_HDR_LEN) return;
    const eth_hdr_t *eh = (const eth_hdr_t *)frame;
    uint16_t et = ntohs(eh->ethertype);
    const uint8_t *payload = frame + ETH_HDR_LEN;
    uint16_t paylen = (uint16_t)(len - ETH_HDR_LEN);

    if (et == ET_ARP) {
        if (paylen < sizeof(arp_pkt_t)) return;
        arp_handle((const arp_pkt_t *)payload);
        return;
    }
    if (et == ET_IPV4) {
        if (paylen < sizeof(ip_hdr_t)) return;
        const ip_hdr_t *iph = (const ip_hdr_t *)payload;
        if ((iph->ver_ihl >> 4) != 4) return;
        uint16_t hlen = (uint16_t)((iph->ver_ihl & 0x0F) * 4u);
        uint16_t tot  = ntohs(iph->total_len);
        if (hlen < 20 || tot < hlen || tot > paylen) return;
        /* Accept frames addressed to us or to broadcast.  EXCEPTION: a DHCP
         * reply (UDP -> client port 68) must be accepted even when its dst IP is
         * neither — during DHCP we don't have an IP yet, and a router that
         * honours the broadcast flag still often unicasts the OFFER/ACK to the
         * offered address.  Without this, DHCP can never complete on such a
         * router. */
        bool dhcp_reply = false;
        if (iph->proto == IPPROTO_UDP && tot >= (uint16_t)(hlen + 8)) {
            const uint8_t *uh = payload + hlen;
            uint16_t udport = (uint16_t)((uh[2] << 8) | uh[3]);
            if (udport == 68) dhcp_reply = true;
        }
        if (iph->dst != g_cfg.ip && iph->dst != 0xFFFFFFFFu && !dhcp_reply) return;

        const uint8_t *inner = payload + hlen;
        uint16_t innerlen = (uint16_t)(tot - hlen);
        switch (iph->proto) {
            case IPPROTO_ICMP: icmp_handle(iph->src, inner, innerlen); break;
            case IPPROTO_TCP:  tcp_handle (iph->src, inner, innerlen); break;
            case IPPROTO_UDP:  udp_handle (iph->src, inner, innerlen); break;
            default: break;
        }
    }
}

void net_tick(void) {
    if (!netif_present()) return;
    netif_poll();
    tcp_rtx_tick();
}

void net_init(void) {
    debug_step("net: initialising network stack");
    memset(g_arp,  0, sizeof(g_arp));
    memset(g_tcp,  0, sizeof(g_tcp));
    g_link = netif_present();
    if (g_link) {
        netif_mac(g_cfg.mac);
        netif_set_rx_cb((netif_rx_cb_t)net_rx, NULL);
        /* Print the ACTUAL configured addresses, not a hardcoded literal — at
         * init this is still the pre-DHCP fallback, and DHCP overwrites it
         * shortly after boot.  A stale literal here mis-led network debugging. */
        char ip[24], gw[24], mk[24], ns[24];
        net_ip_ntoa(g_cfg.ip, ip, sizeof(ip));
        net_ip_ntoa(g_cfg.gateway, gw, sizeof(gw));
        net_ip_ntoa(g_cfg.netmask, mk, sizeof(mk));
        net_ip_ntoa(g_cfg.dns, ns, sizeof(ns));
        debug_printf("[net] link UP via %s   "
                     "MAC=%02x:%02x:%02x:%02x:%02x:%02x  "
                     "IP=%s mask=%s GW=%s DNS=%s %s\n",
                     netif_name(),
                     g_cfg.mac[0], g_cfg.mac[1], g_cfg.mac[2],
                     g_cfg.mac[3], g_cfg.mac[4], g_cfg.mac[5],
                     ip, mk, gw, ns, g_cfg.dhcp ? "(DHCP pending)" : "(static)");
    } else {
        debug_printf("[net] no NIC - stack disabled\n");
    }
}

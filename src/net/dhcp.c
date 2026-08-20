/* ============================================================================
 * NexxoN OS - DHCP client  (TASK 2)
 * ----------------------------------------------------------------------------
 * Synchronous, single-flight DHCPv4 client.  Issues a DISCOVER/REQUEST
 * pair from 0.0.0.0:68 to 255.255.255.255:67 and parses the OFFER/ACK
 * options for:
 *     IP address          (yiaddr)
 *     Subnet mask         (option 1)
 *     Router/gateway      (option 3)
 *     Primary DNS         (option 6)
 *     Lease time          (option 51 — informational only)
 *     DHCP server ID      (option 54 — echoed back in REQUEST)
 *
 * Designed to plug straight into net_set_config(): on success the global
 * net_config_t is updated, gephaz.c's "Switch to DHCP" button and the
 * shell `dhcp` command both invoke us, and the WM's status bar reflects
 * the new lease immediately.
 *
 * No retransmission loop yet — a single timeout window covers OFFER then
 * ACK.  Good enough for QEMU user-net and real DHCP servers on a LAN.
 * ============================================================================ */
#include "net.h"
#include "netif.h"
#include "string.h"
#include "debug.h"
#include "pit.h"

#define DHCP_SRV_PORT   67
#define DHCP_CLI_PORT   68
#define DHCP_MAGIC      0x63825363u  /* "magic cookie", network order */
#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY   2
#define DHCP_HTYPE_ETH  1
#define DHCP_HLEN_ETH   6

#define DHCP_MSG_DISCOVER  1
#define DHCP_MSG_OFFER     2
#define DHCP_MSG_REQUEST   3
#define DHCP_MSG_DECLINE   4
#define DHCP_MSG_ACK       5
#define DHCP_MSG_NAK       6
#define DHCP_MSG_RELEASE   7

/* Option codes we emit / parse. */
#define DHCP_OPT_SUBNET    1
#define DHCP_OPT_ROUTER    3
#define DHCP_OPT_DNS       6
#define DHCP_OPT_REQ_IP    50
#define DHCP_OPT_LEASE     51
#define DHCP_OPT_MSG_TYPE  53
#define DHCP_OPT_SERVER_ID 54
#define DHCP_OPT_PARAM_LST 55
#define DHCP_OPT_END       255

/* Retransmission: a single DISCOVER/OFFER exchange is easily lost over UDP (the
 * OFFER is a broadcast and the RX ring can momentarily run dry — RNBC), so we
 * retransmit the way a real DHCP client does (RFC 2131 §4.1).  The xid stays
 * constant across retransmissions of one transaction. */
#define DHCP_DISCOVER_TRIES 4
#define DHCP_REQUEST_TRIES  3

/* On-wire DHCP fixed portion (RFC 2131).  Options follow the magic. */
typedef struct PACKED {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;          /* 0x63825363 in network order */
} dhcp_pkt_t;

/* Build the DISCOVER / REQUEST payload.  Options written sequentially
 * after the magic; we always finish with OPT_END (no padding needed).
 * Returns the total byte count including the fixed header.            */
static int build_packet(uint8_t *out, uint16_t cap,
                        uint8_t msg_type, uint32_t xid,
                        const uint8_t mac[6],
                        uint32_t requested_ip,
                        uint32_t server_id) {
    if (cap < sizeof(dhcp_pkt_t) + 16) return -1;
    dhcp_pkt_t *p = (dhcp_pkt_t *)out;
    memset(p, 0, sizeof(*p));
    p->op    = DHCP_OP_REQUEST;
    p->htype = DHCP_HTYPE_ETH;
    p->hlen  = DHCP_HLEN_ETH;
    p->xid   = xid;
    /* Broadcast bit set so the server replies to the broadcast address — we
     * don't have an IP yet to receive a unicast.  RFC 2131: the B flag is the
     * MOST-significant bit of the 16-bit flags field, i.e. wire bytes 80 00.
     * On little-endian x86 that is the stored value 0x0080 (NOT 0x8000, which
     * stores as wire 00 80 and leaves B clear — a real router then honours
     * "no broadcast" and unicasts the OFFER to the offered IP, which a client
     * without an IP can't receive; QEMU's slirp broadcasts regardless, hiding
     * the bug). Verified on the wire via a pcap capture. */
    p->flags = (uint16_t)0x0080;        /* htons(0x8000): RFC2131 broadcast */
    memcpy(p->chaddr, mac, 6);
    p->magic = 0x63538263u;             /* 0x63825363 byte-swapped to BE */

    uint8_t *opt = out + sizeof(dhcp_pkt_t);
    uint16_t pos = 0;

    /* Option 53: DHCP Message Type */
    opt[pos++] = DHCP_OPT_MSG_TYPE;
    opt[pos++] = 1;
    opt[pos++] = msg_type;

    if (msg_type == DHCP_MSG_REQUEST) {
        /* Option 50: Requested IP (the address the OFFER proposed). */
        opt[pos++] = DHCP_OPT_REQ_IP;
        opt[pos++] = 4;
        memcpy(opt + pos, &requested_ip, 4); pos += 4;
        /* Option 54: DHCP Server Identifier (echo the OFFER's source). */
        opt[pos++] = DHCP_OPT_SERVER_ID;
        opt[pos++] = 4;
        memcpy(opt + pos, &server_id, 4); pos += 4;
    }

    /* Option 55: Parameter Request List — ask for the fields we care
     * about so the server actually includes them in the OFFER/ACK. */
    opt[pos++] = DHCP_OPT_PARAM_LST;
    opt[pos++] = 4;
    opt[pos++] = DHCP_OPT_SUBNET;
    opt[pos++] = DHCP_OPT_ROUTER;
    opt[pos++] = DHCP_OPT_DNS;
    opt[pos++] = DHCP_OPT_LEASE;

    opt[pos++] = DHCP_OPT_END;

    return (int)(sizeof(dhcp_pkt_t) + pos);
}

/* Scan the options block.  Returns true if a usable lease was extracted;
 * fills out_* with the discovered values.  Caller decides what to do
 * with msg_type (OFFER vs ACK). */
static bool parse_options(const uint8_t *buf, uint16_t len,
                          uint8_t *out_msg_type,
                          uint32_t *out_subnet,
                          uint32_t *out_router,
                          uint32_t *out_dns,
                          uint32_t *out_server_id,
                          uint32_t *out_lease) {
    if (len < sizeof(dhcp_pkt_t) + 1) return false;
    const dhcp_pkt_t *p = (const dhcp_pkt_t *)buf;
    if (p->magic != 0x63538263u) return false;     /* magic check */
    *out_msg_type = 0;
    *out_subnet   = 0;
    *out_router   = 0;
    *out_dns      = 0;
    *out_server_id= 0;
    *out_lease    = 0;

    const uint8_t *opt = buf + sizeof(dhcp_pkt_t);
    uint16_t pos  = 0;
    uint16_t omax = (uint16_t)(len - sizeof(dhcp_pkt_t));
    while (pos < omax) {
        uint8_t code = opt[pos++];
        if (code == 0) continue;          /* padding */
        if (code == DHCP_OPT_END) break;
        if (pos >= omax) break;
        uint8_t olen = opt[pos++];
        if (pos + olen > omax) break;
        switch (code) {
            case DHCP_OPT_MSG_TYPE:
                if (olen == 1) *out_msg_type = opt[pos];
                break;
            case DHCP_OPT_SUBNET:
                if (olen == 4) memcpy(out_subnet,    opt + pos, 4);
                break;
            case DHCP_OPT_ROUTER:
                if (olen >= 4) memcpy(out_router,    opt + pos, 4);
                break;
            case DHCP_OPT_DNS:
                if (olen >= 4) memcpy(out_dns,       opt + pos, 4);
                break;
            case DHCP_OPT_SERVER_ID:
                if (olen == 4) memcpy(out_server_id, opt + pos, 4);
                break;
            case DHCP_OPT_LEASE:
                if (olen == 4) {
                    uint32_t v;
                    memcpy(&v, opt + pos, 4);
                    /* Network-order 32-bit -> host-order. */
                    *out_lease = ((v & 0xFF000000u) >> 24) |
                                 ((v & 0x00FF0000u) >>  8) |
                                 ((v & 0x0000FF00u) <<  8) |
                                 ((v & 0x000000FFu) << 24);
                }
                break;
            default: break;
        }
        pos = (uint16_t)(pos + olen);
    }
    return *out_msg_type != 0;
}

int dhcp_request(uint32_t timeout_ms) {
    if (!netif_present()) {
        debug_printf("[dhcp] no NIC, aborting\n");
        return -1;
    }
    uint8_t mac[6];
    netif_mac(mac);

    /* Pseudo-random xid derived from PIT + MAC bytes — entropy doesn't
     * need to be strong, just unique per lease attempt. */
    uint32_t xid = (pit_ms() << 8) ^
                   ((uint32_t)mac[3] << 16) ^
                   ((uint32_t)mac[4] << 8) ^
                   (uint32_t)mac[5];

    int listener = net_udp_listen(DHCP_CLI_PORT);
    if (listener < 0) {
        debug_printf("[dhcp] no free UDP listener slot\n");
        return -1;
    }

    /* Per-retransmission OFFER/ACK wait window. */
    uint32_t per_try = timeout_ms / 3;
    if (per_try < 700) per_try = 700;

    uint8_t  pkt[600];
    uint8_t  rxbuf[768];
    uint32_t src_ip; uint16_t src_port;
    uint32_t subnet = 0, router = 0, dns = 0, server_id = 0, lease = 0;
    uint32_t offered_ip = 0;

    /* --- DISCOVER / OFFER (retransmitted) ------------------------------- */
    int plen = build_packet(pkt, sizeof(pkt), DHCP_MSG_DISCOVER, xid, mac, 0, 0);
    if (plen < 0) { net_udp_unlisten(listener); return -1; }
    bool got_offer = false;
    for (int attempt = 0; attempt < DHCP_DISCOVER_TRIES && !got_offer; attempt++) {
        debug_printf("[dhcp] DISCOVER (try %d) xid=0x%08x mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                     attempt + 1, xid, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        if (net_send_udp(0, 0xFFFFFFFFu, DHCP_CLI_PORT, DHCP_SRV_PORT,
                         pkt, (uint16_t)plen) != 0) {
            debug_printf("[dhcp] DISCOVER send failed - retrying\n");
            continue;                       /* TX hiccup: retransmit */
        }
        /* Collect datagrams until this try's window elapses; skip anything that
         * isn't a matching OFFER (stray broadcast, wrong xid, etc.). */
        uint32_t t0 = pit_ms();
        while ((pit_ms() - t0) < per_try) {
            uint32_t left = per_try - (pit_ms() - t0);
            int n = net_udp_recv(listener, rxbuf, sizeof(rxbuf),
                                 &src_ip, &src_port, left);
            if (n < (int)sizeof(dhcp_pkt_t)) break;       /* window expired */
            dhcp_pkt_t *off = (dhcp_pkt_t *)rxbuf;
            if (off->xid != xid || off->op != DHCP_OP_REPLY) continue;
            uint8_t mt = 0; uint32_t sn=0, rt=0, dn=0, sid=0, ls=0;
            if (!parse_options(rxbuf, (uint16_t)n, &mt, &sn, &rt, &dn, &sid, &ls))
                continue;
            if (mt != DHCP_MSG_OFFER) continue;
            offered_ip = off->yiaddr;
            subnet = sn; router = rt; dns = dn; server_id = sid; lease = ls;
            got_offer = true;
            break;
        }
    }
    if (!got_offer) {
        debug_printf("[dhcp] no OFFER after %d tries\n", DHCP_DISCOVER_TRIES);
        net_udp_unlisten(listener);
        return -1;
    }
    {
        char ip[24], gw[24], mk[24], ns[24];
        net_ip_ntoa(offered_ip, ip, sizeof(ip));
        net_ip_ntoa(router,     gw, sizeof(gw));
        net_ip_ntoa(subnet,     mk, sizeof(mk));
        net_ip_ntoa(dns,        ns, sizeof(ns));
        debug_printf("[dhcp] OFFER  ip=%s mask=%s gw=%s dns=%s lease=%us\n",
                     ip, mk, gw, ns, lease);
    }

    /* --- REQUEST / ACK (retransmitted) ---------------------------------- */
    plen = build_packet(pkt, sizeof(pkt), DHCP_MSG_REQUEST, xid, mac,
                        offered_ip, server_id);
    if (plen < 0) { net_udp_unlisten(listener); return -1; }
    bool got_ack = false;
    for (int attempt = 0; attempt < DHCP_REQUEST_TRIES && !got_ack; attempt++) {
        if (net_send_udp(0, 0xFFFFFFFFu, DHCP_CLI_PORT, DHCP_SRV_PORT,
                         pkt, (uint16_t)plen) != 0)
            continue;                       /* TX hiccup: retransmit */
        uint32_t t0 = pit_ms();
        while ((pit_ms() - t0) < per_try) {
            uint32_t left = per_try - (pit_ms() - t0);
            int n = net_udp_recv(listener, rxbuf, sizeof(rxbuf),
                                 &src_ip, &src_port, left);
            if (n < (int)sizeof(dhcp_pkt_t)) break;
            dhcp_pkt_t *ack = (dhcp_pkt_t *)rxbuf;
            if (ack->xid != xid) continue;
            uint8_t mt = 0; uint32_t sn=0, rt=0, dn=0, sid=0, ls=0;
            if (!parse_options(rxbuf, (uint16_t)n, &mt, &sn, &rt, &dn, &sid, &ls))
                continue;
            if (mt == DHCP_MSG_NAK) {
                debug_printf("[dhcp] server NAK'd the REQUEST\n");
                net_udp_unlisten(listener);
                return -1;
            }
            if (mt != DHCP_MSG_ACK) continue;
            offered_ip = ack->yiaddr;       /* ACK is authoritative */
            if (sn) subnet = sn;
            if (rt) router = rt;
            if (dn) dns    = dn;
            if (ls) lease  = ls;
            got_ack = true;
            break;
        }
    }
    net_udp_unlisten(listener);
    if (!got_ack) {
        debug_printf("[dhcp] no ACK after %d tries\n", DHCP_REQUEST_TRIES);
        return -1;
    }

    /* Commit the lease into the global config.  netmask/gateway/dns may
     * legitimately be zero if the server omits the option — preserve
     * the previously configured values in that case rather than zeroing
     * the system out. */
    net_config_t cur;
    net_get_config(&cur);
    cur.ip      = offered_ip;
    if (subnet) cur.netmask = subnet;
    if (router) cur.gateway = router;
    if (dns)    cur.dns     = dns;
    cur.dhcp = true;
    net_set_config(&cur);

    {
        char ip[24], gw[24], mk[24], ns[24];
        net_ip_ntoa(cur.ip,      ip, sizeof(ip));
        net_ip_ntoa(cur.gateway, gw, sizeof(gw));
        net_ip_ntoa(cur.netmask, mk, sizeof(mk));
        net_ip_ntoa(cur.dns,     ns, sizeof(ns));
        debug_printf("[dhcp] LEASE  ip=%s mask=%s gw=%s dns=%s\n",
                     ip, mk, gw, ns);
    }
    return 0;
}

/* ===========================================================================
 * Asynchronous, non-blocking DHCP (boot path)
 * ---------------------------------------------------------------------------
 * The synchronous dhcp_request() blocks the cooperative kernel for the whole
 * OFFER/ACK handshake — fine for a user-initiated `dhcp`, but at boot it froze
 * the just-painted desktop for a few seconds per attempt (and once per retry).
 * This state machine is ticked from the idle loop instead: it sends a packet,
 * returns immediately, and on later ticks polls the listener (non-blocking) and
 * retransmits on its own deadlines.  The NIC is pumped by net_tick() in the
 * same idle loop, so OFFER/ACK frames land in our listener between ticks.
 * ========================================================================== */
typedef enum { DHA_IDLE, DHA_DISCOVER, DHA_REQUEST, DHA_DONE, DHA_FAILED } dha_state_t;

#define DHA_RETRANS_MS    1500u  /* per-phase retransmit interval            */
#define DHA_DISCOVER_MAX  8      /* DISCOVERs before giving up               */
#define DHA_REQUEST_MAX   4      /* REQUESTs before falling back to DISCOVER */

static dha_state_t g_dha_state    = DHA_IDLE;
static int      g_dha_listener    = -1;
static uint32_t g_dha_xid         = 0;
static uint8_t  g_dha_mac[6];
static uint32_t g_dha_offered, g_dha_subnet, g_dha_router, g_dha_dns,
                g_dha_server, g_dha_lease;
static uint32_t g_dha_deadline    = 0;
static int      g_dha_dtries = 0, g_dha_rtries = 0;
static uint8_t  g_dha_pkt[600];

static void dha_send(uint8_t msg_type) {
    int plen = build_packet(g_dha_pkt, sizeof(g_dha_pkt), msg_type, g_dha_xid,
                            g_dha_mac,
                            msg_type == DHCP_MSG_REQUEST ? g_dha_offered : 0,
                            msg_type == DHCP_MSG_REQUEST ? g_dha_server  : 0);
    if (plen > 0)
        net_send_udp(0, 0xFFFFFFFFu, DHCP_CLI_PORT, DHCP_SRV_PORT,
                     g_dha_pkt, (uint16_t)plen);
    g_dha_deadline = pit_ms() + DHA_RETRANS_MS;
}

void dhcp_async_start(void) {
    if (g_dha_state == DHA_DISCOVER || g_dha_state == DHA_REQUEST) return;
    if (!netif_present()) { g_dha_state = DHA_FAILED; return; }
    netif_mac(g_dha_mac);
    g_dha_xid = (pit_ms() << 8) ^ ((uint32_t)g_dha_mac[3] << 16)
              ^ ((uint32_t)g_dha_mac[4] << 8) ^ (uint32_t)g_dha_mac[5];
    if (g_dha_listener < 0) g_dha_listener = net_udp_listen(DHCP_CLI_PORT);
    if (g_dha_listener < 0) { g_dha_state = DHA_FAILED; return; }
    g_dha_dtries = 1; g_dha_rtries = 0;
    g_dha_offered = g_dha_subnet = g_dha_router = g_dha_dns = g_dha_server = g_dha_lease = 0;
    g_dha_state = DHA_DISCOVER;
    debug_printf("[dhcp/async] DISCOVER xid=0x%08x\n", g_dha_xid);
    dha_send(DHCP_MSG_DISCOVER);
}

static void dha_restart_discover(void) {
    g_dha_xid ^= 0x9e3779b9u;          /* fresh transaction id */
    g_dha_dtries = 1;
    g_dha_state = DHA_DISCOVER;
    dha_send(DHCP_MSG_DISCOVER);
}

int dhcp_async_tick(void) {
    if (g_dha_state != DHA_DISCOVER && g_dha_state != DHA_REQUEST) return 0;

    uint8_t rx[768]; uint32_t sip; uint16_t sp;
    int n = net_udp_poll(g_dha_listener, rx, sizeof(rx), &sip, &sp);
    if (n >= (int)sizeof(dhcp_pkt_t)) {
        dhcp_pkt_t *r = (dhcp_pkt_t *)rx;
        if (r->xid == g_dha_xid && r->op == DHCP_OP_REPLY) {
            uint8_t mt = 0; uint32_t sn=0, rt=0, dn=0, sid=0, ls=0;
            if (parse_options(rx, (uint16_t)n, &mt, &sn, &rt, &dn, &sid, &ls)) {
                if (mt == DHCP_MSG_NAK) { dha_restart_discover(); return 0; }
                if (g_dha_state == DHA_DISCOVER && mt == DHCP_MSG_OFFER) {
                    g_dha_offered = r->yiaddr; g_dha_subnet = sn; g_dha_router = rt;
                    g_dha_dns = dn; g_dha_server = sid; g_dha_lease = ls;
                    g_dha_rtries = 1; g_dha_state = DHA_REQUEST;
                    dha_send(DHCP_MSG_REQUEST);
                    return 0;
                }
                if (g_dha_state == DHA_REQUEST && mt == DHCP_MSG_ACK) {
                    g_dha_offered = r->yiaddr;
                    if (sn) g_dha_subnet = sn;
                    if (rt) g_dha_router = rt;
                    if (dn) g_dha_dns    = dn;
                    if (ls) g_dha_lease  = ls;
                    net_config_t cur; net_get_config(&cur);
                    cur.ip = g_dha_offered;
                    if (g_dha_subnet) cur.netmask = g_dha_subnet;
                    if (g_dha_router) cur.gateway = g_dha_router;
                    if (g_dha_dns)    cur.dns     = g_dha_dns;
                    cur.dhcp = true;
                    net_set_config(&cur);
                    net_udp_unlisten(g_dha_listener); g_dha_listener = -1;
                    g_dha_state = DHA_DONE;
                    {
                        char ip[24], gw[24], mk[24], ns[24];
                        net_ip_ntoa(cur.ip, ip, sizeof ip);
                        net_ip_ntoa(cur.gateway, gw, sizeof gw);
                        net_ip_ntoa(cur.netmask, mk, sizeof mk);
                        net_ip_ntoa(cur.dns, ns, sizeof ns);
                        debug_printf("[dhcp/async] LEASE ip=%s mask=%s gw=%s dns=%s\n",
                                     ip, mk, gw, ns);
                    }
                    return 1;   /* lease just bound */
                }
            }
        }
    }

    if (pit_ms() >= g_dha_deadline) {
        if (g_dha_state == DHA_DISCOVER) {
            if (g_dha_dtries >= DHA_DISCOVER_MAX) {
                net_udp_unlisten(g_dha_listener); g_dha_listener = -1;
                g_dha_state = DHA_FAILED;
                debug_printf("[dhcp/async] no OFFER after %d DISCOVERs\n", g_dha_dtries);
                return -1;
            }
            g_dha_dtries++; dha_send(DHCP_MSG_DISCOVER);
        } else { /* DHA_REQUEST */
            if (g_dha_rtries >= DHA_REQUEST_MAX) { dha_restart_discover(); }
            else { g_dha_rtries++; dha_send(DHCP_MSG_REQUEST); }
        }
    }
    return 0;
}

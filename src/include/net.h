/* ============================================================================
 * NexxoN OS - Network stack (Ethernet / ARP / IPv4 / ICMP / UDP / TCP)  (v1.0)
 * ----------------------------------------------------------------------------
 * Lives on top of the E1000 driver.  Single-threaded, no malloc, no jumbo
 * frames.  Provides:
 *
 *   net_init       - bring up the local IP config (defaults to a static
 *                    DHCP-style 10.0.2.15/24 + gateway 10.0.2.2 which
 *                    matches QEMU's user-net assignment).
 *   net_tick       - poll the NIC, dispatch incoming frames.
 *   net_send_ip    - hand a payload + IP header to the Ethernet layer.
 *   net_ping       - issue an ICMP Echo Request and wait for a reply
 *                    (or timeout) - drives the `ping` shell command.
 *   tcp_connect / tcp_send / tcp_recv / tcp_close
 *                  - lightweight three-way handshake TCP stream sockets
 *                    used by the HTTP client (NexxoN Browser).
 *
 * The stack stores its own MAC + IP + gateway + netmask + DNS state in a
 * single net_config_t struct so the upcoming "Gépház" settings app can
 * mutate them via syscall.
 * ============================================================================ */
#ifndef NEXXON_NET_H
#define NEXXON_NET_H

#include "types.h"

typedef struct {
    uint8_t  mac[6];
    uint32_t ip;            /* network byte order */
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    bool     dhcp;
} net_config_t;

void net_init       (void);
void net_tick       (void);
void net_set_config (const net_config_t *cfg);
void net_get_config (net_config_t *out);
bool net_link_up    (void);

/* IPv4 helpers - all addresses are in network byte order. */
uint32_t net_ip_aton(const char *str);            /* "1.2.3.4" -> 0x04030201 */
void     net_ip_ntoa(uint32_t ip, char *out, size_t cap);

/* Blocking ping.  Returns RTT in milliseconds on success, -1 on timeout. */
int  net_ping(uint32_t target_ip, uint32_t timeout_ms);

/* Register a pump callback invoked during blocking network waits so the
 * desktop keeps rendering (and the USB mouse keeps polling) while e.g.
 * `ping` is in flight.  Pass NULL to clear.  Re-entrancy is guarded. */
void net_set_idle_hook(void (*fn)(void));

/* ARP - synchronous resolve.  Returns 0 on success and writes the MAC into
 * out_mac; -1 on timeout. */
int  arp_resolve(uint32_t ip, uint8_t out_mac[6], uint32_t timeout_ms);

/* UDP datagram primitives (TASK 2).  Single-packet send + slot-based
 * blocking receive so DHCP / NTP can hold their replies without forcing
 * net.c to grow a full socket abstraction.
 *
 * send_udp accepts an explicit src_ip so callers without a configured
 * IPv4 address yet (DHCP DISCOVER) can ship from 0.0.0.0 to broadcast. */
int  net_send_udp   (uint32_t src_ip, uint32_t dst_ip,
                     uint16_t src_port, uint16_t dst_port,
                     const void *payload, uint16_t paylen);
int  net_udp_listen (uint16_t port);
void net_udp_unlisten(int slot);
int  net_udp_recv   (int slot, void *buf, uint16_t cap,
                     uint32_t *out_src_ip, uint16_t *out_src_port,
                     uint32_t timeout_ms);
/* Non-blocking variant: return a buffered datagram immediately if one is
 * waiting (length), else 0.  Does NOT spin or pump the stack — the caller is
 * expected to drive net_tick()/netif_poll() from its own loop. */
int  net_udp_poll   (int slot, void *buf, uint16_t cap,
                     uint32_t *out_src_ip, uint16_t *out_src_port);

/* DNS A-record resolver.  Accepts dotted-quad literals (returned as-is)
 * and hostnames (resolved via /etc/hosts then g_cfg.dns over UDP port 53).
 * Writes the answer in network byte order on success and returns 0;
 * non-zero on timeout / failure. */
int  net_dns_resolve(const char *host, uint32_t *out_ip,
                     uint32_t timeout_ms);
int  net_hosts_lookup(const char *host, uint32_t *out_ip);
void net_hosts_reload(void);

/* DHCP client (TASK 2).  Issues DISCOVER → OFFER → REQUEST → ACK and
 * writes the assigned lease into g_cfg (IP / netmask / gateway / DNS).
 * Returns 0 on success, -1 on timeout / NAK / parse failure.  Safe to
 * call repeatedly to renew the lease. */
int  dhcp_request   (uint32_t timeout_ms);

/* Asynchronous, NON-blocking DHCP for the boot path.  dhcp_async_start() kicks
 * off a DISCOVER and returns immediately; dhcp_async_tick() must be called
 * periodically from the idle loop (it never blocks — it polls the listener and
 * retransmits on its own deadlines).  tick() returns: 0 = still running or idle,
 * 1 = a lease was just bound (caller should persist), -1 = just gave up.  This
 * keeps the desktop smooth instead of freezing during the lease handshake. */
void dhcp_async_start(void);
int  dhcp_async_tick (void);

/* SNTPv4 client (TASK 33).  Sends a Mode-3 query to UDP port 123 and
 * decodes the transmit timestamp into Unix epoch seconds (UTC).  Pass
 * server_ip == 0 to use the built-in default (Google public NTP).
 * Returns 0 on success, -1 on timeout or parse failure.  Does NOT
 * update the CMOS RTC — caller is responsible for displaying or
 * logging the value. */
int  ntp_query_unix (uint32_t server_ip, uint32_t timeout_ms,
                     uint32_t *out_unix_seconds);

/* Minimal TCP stream socket API for the HTTP client. */
typedef int tcp_handle_t;
#define TCP_INVALID  (-1)

tcp_handle_t tcp_connect      (uint32_t dst_ip, uint16_t dst_port,
                               uint32_t timeout_ms);
int          tcp_send         (tcp_handle_t s, const void *buf, uint32_t len);
int          tcp_recv         (tcp_handle_t s, void *buf, uint32_t cap,
                               uint32_t timeout_ms);
void         tcp_close        (tcp_handle_t s);
bool         tcp_is_open      (tcp_handle_t s);
bool         tcp_is_connected (tcp_handle_t s);
/* Bytes currently buffered in the receive ring (0 when none/invalid). */
uint32_t     tcp_available    (tcp_handle_t s);

#endif /* NEXXON_NET_H */

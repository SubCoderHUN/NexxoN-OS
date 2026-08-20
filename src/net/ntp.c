/* ============================================================================
 * NexxoN OS - NTP client  (TASK 33)
 * ----------------------------------------------------------------------------
 * Tiny SNTPv4 client.  Sends a 48-byte query (mode 3 = client, version 4)
 * to UDP port 123 and decodes the transmit timestamp from offset 40 of
 * the reply.  The on-wire stamp is "seconds since 1900-01-01 UTC"; we
 * convert to Unix epoch (subtract 2 208 988 800) and then to a broken-
 * down rtc_time_t structure.
 *
 * The CMOS RTC is read-only in this kernel, so we don't try to write
 * the system clock — callers get the NTP-reported time as a struct and
 * can show it in a dialog or compare to the local RTC.
 * ============================================================================ */
#include "net.h"
#include "string.h"
#include "debug.h"

#define NTP_PORT       123
#define NTP_PKT_SIZE   48
#define NTP_EPOCH_DIFF 2208988800u   /* seconds between 1900-01-01 and 1970-01-01 */

/* Default upstream: Google Public NTP (216.239.35.0).  No DNS yet so we
 * use the literal IP — caller can pass any reachable NTP server.
 * Stored in the OS's native byte order (low byte = first octet).
 *     216.239.35.0  -> 0x00 23 EF D8 */
#define NTP_DEFAULT_IP   0x0023EFD8u

/* Endianness helpers — net.c keeps these static, so duplicate the
 * tiny ones we need locally. */
static inline uint32_t bswap32_local(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x000000FFu) << 24);
}

/* Issue an NTP query.  On success writes the server-reported Unix
 * epoch seconds into *out_unix_seconds and returns 0.  Returns -1 on
 * timeout / parse failure. */
int ntp_query_unix(uint32_t server_ip, uint32_t timeout_ms,
                   uint32_t *out_unix_seconds) {
    if (!out_unix_seconds) return -1;
    if (server_ip == 0) server_ip = NTP_DEFAULT_IP;

    int listener = net_udp_listen(NTP_PORT);
    if (listener < 0) {
        debug_printf("[ntp] no UDP listener slot free\n");
        return -1;
    }
    uint8_t pkt[NTP_PKT_SIZE];
    memset(pkt, 0, sizeof(pkt));
    /* LI = 0 (no warning), VN = 4, Mode = 3 (client).
     *   first byte = 0b00_100_011 = 0x23. */
    pkt[0] = 0x23;

    net_config_t cfg;
    net_get_config(&cfg);
    debug_printf("[ntp] querying %u.%u.%u.%u port %u\n",
                 (server_ip >>  0) & 0xFF, (server_ip >>  8) & 0xFF,
                 (server_ip >> 16) & 0xFF, (server_ip >> 24) & 0xFF,
                 NTP_PORT);
    if (net_send_udp(cfg.ip, server_ip, NTP_PORT, NTP_PORT,
                     pkt, sizeof(pkt)) != 0) {
        debug_printf("[ntp] send failed\n");
        net_udp_unlisten(listener);
        return -1;
    }

    uint8_t  reply[NTP_PKT_SIZE];
    uint32_t src_ip; uint16_t src_port;
    int n = net_udp_recv(listener, reply, sizeof(reply),
                        &src_ip, &src_port, timeout_ms);
    net_udp_unlisten(listener);
    if (n < NTP_PKT_SIZE) {
        debug_printf("[ntp] no reply (got %d bytes)\n", n);
        return -1;
    }
    /* Transmit Timestamp = offset 40, 8 bytes (seconds.fraction).  We
     * only consume the integer seconds (first 4 bytes, big-endian). */
    uint32_t ntp_secs;
    memcpy(&ntp_secs, reply + 40, 4);
    ntp_secs = bswap32_local(ntp_secs);
    if (ntp_secs < NTP_EPOCH_DIFF) {
        debug_printf("[ntp] reply timestamp too small (0x%08x)\n", ntp_secs);
        return -1;
    }
    uint32_t unix_secs = ntp_secs - NTP_EPOCH_DIFF;
    *out_unix_seconds = unix_secs;
    debug_printf("[ntp] reply: %u Unix seconds (UTC)\n", unix_secs);
    return 0;
}

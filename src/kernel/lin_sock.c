/* ============================================================================
 * NexxoN OS - Linux AF_INET socket bridge (TCP/UDP -> net.c)
 * ============================================================================ */
#include "lin_sock.h"
#include "lin_unix.h"
#include "net.h"
#include "pit.h"
#include "string.h"
#include "debug.h"

typedef struct PACKED {
    uint16_t family;
    uint16_t port_be;
    uint32_t addr_be;
    uint8_t  zero[8];
} lin_sockaddr_in_t;

static bool parse_sockaddr_in(const void *addr, int addrlen,
                              uint32_t *ip, uint16_t *port_host) {
    if (!addr || addrlen < (int)sizeof(lin_sockaddr_in_t))
        return false;
    const lin_sockaddr_in_t *sa = (const lin_sockaddr_in_t *)addr;
    if (sa->family != LIN_AF_INET)
        return false;
    *ip = sa->addr_be;
    *port_host = (uint16_t)((sa->port_be >> 8) | ((sa->port_be & 0xFFu) << 8));
    return true;
}

static void fill_sockaddr_in(void *addr, int *addrlen, uint32_t ip,
                             uint16_t port_host) {
    if (!addr || !addrlen || *addrlen < (int)sizeof(lin_sockaddr_in_t))
        return;
    lin_sockaddr_in_t *sa = (lin_sockaddr_in_t *)addr;
    sa->family = LIN_AF_INET;
    sa->port_be = (uint16_t)((port_host >> 8) | ((port_host & 0xFFu) << 8));
    sa->addr_be = ip;
    memset(sa->zero, 0, sizeof(sa->zero));
    *addrlen = (int)sizeof(lin_sockaddr_in_t);
}

void lin_sock_reset(lin_sock_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->tcp = TCP_INVALID;
    s->udp_slot = -1;
    s->domain = LIN_AF_INET;
    s->unix_link = -1;
    s->unix_listener = -1;
    s->unix_phase = LSUN_IDLE;
}

void lin_sock_close(lin_sock_t *s) {
    if (!s) return;
    if (s->domain == LIN_AF_UNIX) {
        lin_unix_close(s);
        lin_sock_reset(s);
        return;
    }
    if (s->kind == LSK_STREAM && s->tcp != TCP_INVALID)
        tcp_close(s->tcp);
    if (s->kind == LSK_DGRAM && s->udp_slot >= 0)
        net_udp_unlisten(s->udp_slot);
    lin_sock_reset(s);
}

long lin_sock_create(lin_sock_t *s, int domain, int type, int protocol) {
    if (!s) return -22;
    lin_sock_close(s);
    if (domain == LIN_AF_UNIX) {
        if (type != LIN_SOCK_STREAM) return -22;
        if (protocol != 0) return -22;
        s->kind = LSK_STREAM;
        s->domain = LIN_AF_UNIX;
        s->unix_phase = LSUN_IDLE;
        s->unix_link = -1;
        s->unix_listener = -1;
        return 0;
    }
    if (domain != LIN_AF_INET) return -97; /* EAFNOSUPPORT */
    if (protocol != 0 && protocol != 6 && protocol != 17) return -22;
    if (type == LIN_SOCK_STREAM) {
        s->kind = LSK_STREAM;
        s->tcp = TCP_INVALID;
        return 0;
    }
    if (type == LIN_SOCK_DGRAM) {
        s->kind = LSK_DGRAM;
        s->udp_slot = -1;
        return 0;
    }
    return -22;
}

long lin_sock_bind(lin_sock_t *s, const void *addr, int addrlen) {
    if (!s) return -22;
    if (s->domain == LIN_AF_UNIX)
        return lin_unix_bind(s, addr, addrlen);
    uint32_t ip = 0;
    uint16_t port = 0;
    if (!parse_sockaddr_in(addr, addrlen, &ip, &port))
        return -22;
    if (s->kind == LSK_DGRAM) {
        if (s->udp_slot >= 0) net_udp_unlisten(s->udp_slot);
        s->udp_slot = net_udp_listen(port);
        if (s->udp_slot < 0) return -24; /* EMFILE */
        s->local_port = port;
        (void)ip;
        return 0;
    }
    return -95; /* EOPNOTSUPP for TCP bind (client-only stack) */
}

long lin_sock_listen(lin_sock_t *s, int backlog) {
    if (!s) return -22;
    if (s->domain == LIN_AF_UNIX)
        return lin_unix_listen(s, backlog);
    return -95;
}

long lin_sock_accept(const lin_sock_t *listener, lin_sock_t *accepted,
                     void *addr, int *addrlen) {
    if (!listener || !accepted) return -22;
    if (listener->domain == LIN_AF_UNIX)
        return lin_unix_accept(listener, accepted, addr, addrlen);
    return -95;
}

long lin_sock_getsockname(const lin_sock_t *s, void *addr, int *addrlen) {
    if (!s) return -22;
    if (s->domain == LIN_AF_UNIX)
        return lin_unix_getsockname(s, addr, addrlen);
    if (s->kind == LSK_DGRAM && s->udp_slot >= 0) {
        fill_sockaddr_in(addr, addrlen, 0, s->local_port);
        return 0;
    }
    return -95;
}

long lin_sock_connect(lin_sock_t *s, const void *addr, int addrlen) {
    if (!s) return -22;
    if (s->domain == LIN_AF_UNIX)
        return lin_unix_connect(s, addr, addrlen);
    uint32_t ip = 0;
    uint16_t port = 0;
    if (!parse_sockaddr_in(addr, addrlen, &ip, &port))
        return -22;
    if (s->kind == LSK_STREAM) {
        if (s->tcp != TCP_INVALID) tcp_close(s->tcp);
        tcp_handle_t h = tcp_connect(ip, port, 10000);
        if (h == TCP_INVALID) return -110; /* ETIMEDOUT */
        s->tcp = h;
        s->peer_ip = ip;
        s->peer_port = port;
        s->connected = true;
        debug_printf("[linux/sock] TCP connect %u.%u.%u.%u:%u ok\n",
                     (ip) & 0xFF, (ip >> 8) & 0xFF,
                     (ip >> 16) & 0xFF, (ip >> 24) & 0xFF, port);
        return 0;
    }
    if (s->kind == LSK_DGRAM) {
        s->peer_ip = ip;
        s->peer_port = port;
        s->connected = true;
        if (s->udp_slot < 0) {
            static uint16_t g_ephem = 0xC000;
            int slot = -1;
            for (int tries = 0; tries < 32 && slot < 0; tries++) {
                slot = net_udp_listen(g_ephem++);
                if (g_ephem < 0xC000) g_ephem = 0xC000;
            }
            if (slot < 0) return -24;
            s->udp_slot = slot;
            s->local_port = (uint16_t)(g_ephem - 1);
        }
        return 0;
    }
    return -22;
}

long lin_sock_sendto(lin_sock_t *s, const void *buf, uint64_t len,
                     const void *addr, int addrlen) {
    if (!s || !buf) return -22;
    if (s->domain == LIN_AF_UNIX) {
        if (addr)
            return -95;
        return lin_unix_write(s, buf, len);
    }
    if (s->kind != LSK_DGRAM) return -95;
    net_config_t cfg;
    net_get_config(&cfg);
    uint32_t dst_ip = s->peer_ip;
    uint16_t dst_port = s->peer_port;
    if (addr) {
        if (!parse_sockaddr_in(addr, addrlen, &dst_ip, &dst_port))
            return -22;
    } else if (!s->connected) {
        return -22;
    }
    if (len > 0xFFFFu) len = 0xFFFFu;
    uint16_t src_port = s->local_port ? s->local_port : 0xC000;
    int rc = net_send_udp(cfg.ip, dst_ip, src_port, dst_port, buf, (uint16_t)len);
    return rc == 0 ? (long)len : -5;
}

long lin_sock_recvfrom(lin_sock_t *s, void *buf, uint64_t len,
                       void *addr, int *addrlen, bool nonblock) {
    if (!s || !buf) return -22;
    if (s->domain == LIN_AF_UNIX) {
        (void)addr;
        (void)addrlen;
        return lin_unix_read(s, buf, len, nonblock);
    }
    if (s->kind != LSK_DGRAM) return -95;
    if (s->udp_slot < 0) return -22;
    uint32_t timeout = nonblock ? 0 : 10000;
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    int got = net_udp_recv(s->udp_slot, buf, (uint16_t)(len > 0xFFFF ? 0xFFFF : len),
                           &src_ip, &src_port, timeout);
    if (got < 0) return nonblock ? -11 : -110;
    if (got == 0 && nonblock) return -11;
    if (addr && addrlen)
        fill_sockaddr_in(addr, addrlen, src_ip, src_port);
    return got;
}

long lin_sock_read(lin_sock_t *s, void *buf, uint64_t len, bool nonblock) {
    if (!s || !buf) return -22;
    if (s->domain == LIN_AF_UNIX)
        return lin_unix_read(s, buf, len, nonblock);
    if (s->kind == LSK_STREAM) {
        if (s->tcp == TCP_INVALID) return -22;
        uint32_t timeout = nonblock ? 0 : 30000;
        int got = tcp_recv(s->tcp, buf, (uint32_t)len, timeout);
        if (got < 0) return nonblock ? -11 : -110;
        return got;
    }
    if (s->kind == LSK_DGRAM)
        return lin_sock_recvfrom(s, buf, len, NULL, NULL, nonblock);
    return -22;
}

long lin_sock_write(lin_sock_t *s, const void *buf, uint64_t len) {
    if (!s || !buf) return -22;
    if (s->domain == LIN_AF_UNIX)
        return lin_unix_write(s, buf, len);
    if (s->kind == LSK_STREAM) {
        if (s->tcp == TCP_INVALID || !tcp_is_connected(s->tcp)) return -22;
        int got = tcp_send(s->tcp, buf, (uint32_t)len);
        return got < 0 ? -5 : got;
    }
    if (s->kind == LSK_DGRAM)
        return lin_sock_sendto(s, buf, len, NULL, 0);
    return -22;
}

bool lin_sock_poll_in(const lin_sock_t *s) {
    if (!s) return false;
    if (s->domain == LIN_AF_UNIX)
        return lin_unix_poll_in(s);
    if (s->kind == LSK_STREAM && s->tcp != TCP_INVALID)
        return tcp_available(s->tcp) > 0;
    if (s->kind == LSK_DGRAM && s->udp_slot >= 0) {
        uint8_t tmp[4];
        uint32_t sip;
        uint16_t sp;
        return net_udp_poll(s->udp_slot, tmp, sizeof(tmp), &sip, &sp) > 0;
    }
    return false;
}

bool lin_sock_poll_out(const lin_sock_t *s) {
    if (!s) return false;
    if (s->domain == LIN_AF_UNIX)
        return lin_unix_poll_out(s);
    if (s->kind == LSK_STREAM)
        return s->tcp != TCP_INVALID && tcp_is_connected(s->tcp);
    if (s->kind == LSK_DGRAM)
        return true;
    return false;
}

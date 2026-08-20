/* ============================================================================
 * NexxoN OS - Linux AF_UNIX stream sockets (in-kernel)
 * ============================================================================ */
#include "lin_unix.h"
#include "lin_x11.h"
#include "lin_pulse.h"
#include "debug.h"
#include "string.h"
#include "pit.h"

typedef struct {
    bool used;
    bool x11;
    bool pulse;
    int  peer_link;
    int  refcnt;
    bool steam_ui;
    uint8_t steam_ui_phase;
    bool steam_ui_primed;
    uint8_t rx[LIN_UNIX_RX_CAP];
    uint32_t rx_len;
    uint8_t setup[64];
    uint32_t setup_len;
} lin_unix_link_t;

typedef struct {
    bool used;
    bool abstract;
    bool x11;
    bool pulse;
    char path[LIN_UNIX_PATH_MAX];
    int backlog;
    int pending[8];
    int pending_n;
} lin_unix_listener_t;

#define LIN_UNIX_LINK_MAX      16
#define LIN_UNIX_LISTENER_MAX  8

static lin_unix_link_t      g_unix_links[LIN_UNIX_LINK_MAX];
static lin_unix_listener_t  g_unix_listeners[LIN_UNIX_LISTENER_MAX];

static bool parse_sockaddr_un(const void *addr, int addrlen,
                              char *out, bool *abstract) {
    if (!addr || addrlen < 3 || !out || !abstract)
        return false;
    const uint8_t *p = (const uint8_t *)addr;
    uint16_t family = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (family != LIN_AF_UNIX)
        return false;
    const char *raw = (const char *)(p + 2);
    int raw_len = addrlen - 2;
    if (raw_len <= 0 || raw_len > LIN_UNIX_PATH_MAX)
        return false;
    if (raw[0] == '\0') {
        *abstract = true;
        int n = 0;
        while (n + 1 < raw_len && raw[n + 1] != '\0')
            n++;
        if (n >= LIN_UNIX_PATH_MAX)
            return false;
        memcpy(out, raw + 1, (size_t)n);
        out[n] = '\0';
        return n > 0;
    }
    *abstract = false;
    int n = 0;
    while (n < raw_len && raw[n] != '\0')
        n++;
    if (n >= LIN_UNIX_PATH_MAX)
        return false;
    memcpy(out, raw, (size_t)n);
    out[n] = '\0';
    return n > 0;
}

static void fill_sockaddr_un(void *addr, int *addrlen, bool abstract,
                             const char *path) {
    if (!addr || !addrlen || *addrlen < 3 || !path)
        return;
    uint8_t *p = (uint8_t *)addr;
    p[0] = (uint8_t)(LIN_AF_UNIX & 0xFF);
    p[1] = (uint8_t)((LIN_AF_UNIX >> 8) & 0xFF);
    size_t n = strlen(path);
    if (abstract) {
        p[2] = 0;
        if (n >= LIN_UNIX_PATH_MAX - 1)
            n = LIN_UNIX_PATH_MAX - 2;
        memcpy(p + 3, path, n);
        *addrlen = (int)(3 + n);
    } else {
        if (n >= LIN_UNIX_PATH_MAX)
            n = LIN_UNIX_PATH_MAX - 1;
        memcpy(p + 2, path, n);
        p[2 + n] = 0;
        *addrlen = (int)(3 + n);
    }
}

static bool unix_path_match(bool abstract, const char *a, const char *b) {
    return abstract == (b[0] != '/') && strcmp(a, b) == 0;
}

static lin_unix_listener_t *listener_find(bool abstract, const char *path) {
    for (int i = 0; i < LIN_UNIX_LISTENER_MAX; i++) {
        lin_unix_listener_t *l = &g_unix_listeners[i];
        if (l->used && l->abstract == abstract &&
            strcmp(l->path, path) == 0)
            return l;
    }
    return NULL;
}

static lin_unix_listener_t *listener_alloc(bool abstract, const char *path,
                                           bool x11, bool pulse) {
    lin_unix_listener_t *free_slot = NULL;
    for (int i = 0; i < LIN_UNIX_LISTENER_MAX; i++) {
        lin_unix_listener_t *l = &g_unix_listeners[i];
        if (l->used && l->abstract == abstract &&
            strcmp(l->path, path) == 0)
            return l;
        if (!l->used && !free_slot)
            free_slot = l;
    }
    if (!free_slot)
        return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    free_slot->abstract = abstract;
    free_slot->x11 = x11;
    free_slot->pulse = pulse;
    ksnprintf(free_slot->path, sizeof(free_slot->path), "%s", path);
    return free_slot;
}

static int link_alloc(bool x11, bool pulse) {
    for (int i = 0; i < LIN_UNIX_LINK_MAX; i++) {
        if (!g_unix_links[i].used) {
            memset(&g_unix_links[i], 0, sizeof(g_unix_links[i]));
            g_unix_links[i].used = true;
            g_unix_links[i].x11 = x11;
            g_unix_links[i].pulse = pulse;
            g_unix_links[i].peer_link = -1;
            return i;
        }
    }
    return -1;
}

static void link_free(int ix) {
    if (ix < 0 || ix >= LIN_UNIX_LINK_MAX)
        return;
    memset(&g_unix_links[ix], 0, sizeof(g_unix_links[ix]));
}

static lin_unix_link_t *link_get(int ix) {
    if (ix < 0 || ix >= LIN_UNIX_LINK_MAX || !g_unix_links[ix].used)
        return NULL;
    return &g_unix_links[ix];
}

static void rx_push(lin_unix_link_t *lk, const void *data, uint32_t len);
static void steam_ui_prime_rx(lin_unix_link_t *lk);

static void steam_ui_send_frame(lin_unix_link_t *lk,
                                const uint8_t *body, uint32_t body_len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(body_len);
    hdr[1] = (uint8_t)(body_len >> 8);
    hdr[2] = (uint8_t)(body_len >> 16);
    hdr[3] = (uint8_t)(body_len >> 24);
    rx_push(lk, hdr, 4);
    rx_push(lk, body, body_len);
}

static void steam_ui_prime_rx(lin_unix_link_t *lk);
static void steam_ui_queue_all(lin_unix_link_t *lk) {
    if (!lk || !lk->steam_ui)
        return;
    while (lk->steam_ui_phase < 3)
        steam_ui_prime_rx(lk);
}

void lin_unix_mark_steam_ui(int link_ix) {
    lin_unix_link_t *lk = link_get(link_ix);
    if (!lk)
        return;
    lk->steam_ui = true;
    lk->steam_ui_phase = 0;
    lk->steam_ui_primed = false;
    steam_ui_queue_all(lk);
    debug_printf("[linux/unix] steam update UI marked link=%d phase=%u\n",
                 link_ix, (unsigned)lk->steam_ui_phase);
}

static void rx_push(lin_unix_link_t *lk, const void *data, uint32_t len) {
    if (!lk || !data || !len)
        return;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        if (lk->rx_len >= LIN_UNIX_RX_CAP)
            break;
        lk->rx[lk->rx_len++] = p[i];
    }
}

static void steam_ui_prime_rx(lin_unix_link_t *lk) {
    /* Body: uint32 command (LE) + protobuf-ish payload. */
    static const uint8_t k_init[] = {
        0x01, 0x00, 0x00, 0x00,
        0x0a, 0x00, 0x10, 0x00, 0x18, 0x00, 0x20, 0x00,
        0x28, 0x00, 0x30, 0x00, 0x38, 0x01,
    };
    static const uint8_t k_progress[] = {
        0x02, 0x00, 0x00, 0x00,
        0x08, 0x64, 0x10, 0x01, 0x18, 0x00, 0x20, 0x00,
    };
    static const uint8_t k_done[] = {
        0x03, 0x00, 0x00, 0x00,
        0x08, 0x01, 0x10, 0x64, 0x18, 0x01, 0x20, 0x00,
    };
    if (!lk || !lk->steam_ui)
        return;
    if (lk->steam_ui_phase == 0) {
        steam_ui_send_frame(lk, k_init, sizeof(k_init));
        lk->steam_ui_phase = 1;
        debug_printf("[linux/unix] steam update UI init (%u bytes)\n",
                     (unsigned)sizeof(k_init));
        return;
    }
    if (lk->steam_ui_phase == 1) {
        steam_ui_send_frame(lk, k_progress, sizeof(k_progress));
        lk->steam_ui_phase = 2;
        debug_printf("[linux/unix] steam update UI progress\n");
        return;
    }
    if (lk->steam_ui_phase == 2) {
        steam_ui_send_frame(lk, k_done, sizeof(k_done));
        lk->steam_ui_phase = 3;
        debug_printf("[linux/unix] steam update UI complete\n");
    }
}

void lin_unix_global_init(void) {
    memset(g_unix_links, 0, sizeof(g_unix_links));
    memset(g_unix_listeners, 0, sizeof(g_unix_listeners));
    lin_x11_display_init();
    lin_pulse_init();
}

void lin_unix_sock_reset(lin_sock_t *s) {
    if (!s)
        return;
    s->domain = LIN_AF_INET;
    s->unix_phase = LSUN_IDLE;
    s->unix_link = -1;
    s->unix_listener = -1;
    s->unix_abstract = false;
    memset(s->unix_path, 0, sizeof(s->unix_path));
}

long lin_unix_bind(lin_sock_t *s, const void *addr, int addrlen) {
    if (!s || s->kind != LSK_STREAM)
        return -22;
    char path[LIN_UNIX_PATH_MAX];
    bool abstract = false;
    if (!parse_sockaddr_un(addr, addrlen, path, &abstract))
        return -22;
    lin_unix_listener_t *l = listener_alloc(abstract, path, false, false);
    if (!l)
        return -24;
    s->domain = LIN_AF_UNIX;
    s->unix_phase = LSUN_BOUND;
    s->unix_abstract = abstract;
    ksnprintf(s->unix_path, sizeof(s->unix_path), "%s", path);
    s->unix_listener = (int)(l - g_unix_listeners);
    debug_printf("[linux/unix] bind %s%s\n",
                 abstract ? "@" : "", path);
    return 0;
}

long lin_unix_listen(lin_sock_t *s, int backlog) {
    if (!s || s->domain != LIN_AF_UNIX || s->unix_phase != LSUN_BOUND)
        return -22;
    if (s->unix_listener < 0)
        return -22;
    lin_unix_listener_t *l = &g_unix_listeners[s->unix_listener];
    l->backlog = backlog > 0 ? backlog : 8;
    s->unix_phase = LSUN_LISTENING;
    debug_printf("[linux/unix] listen %s%s backlog=%d\n",
                 s->unix_abstract ? "@" : "", s->unix_path, l->backlog);
    return 0;
}

long lin_unix_connect(lin_sock_t *s, const void *addr, int addrlen) {
    if (!s || s->kind != LSK_STREAM)
        return -22;
    char path[LIN_UNIX_PATH_MAX];
    bool abstract = false;
    if (!parse_sockaddr_un(addr, addrlen, path, &abstract))
        return -22;
    lin_unix_listener_t *l = listener_find(abstract, path);
    if (!l)
        return -111; /* ECONNREFUSED */
    int lx = link_alloc(l->x11, l->pulse);
    if (lx < 0)
        return -24;
    s->domain = LIN_AF_UNIX;
    s->unix_phase = LSUN_CONNECTED;
    s->unix_abstract = abstract;
    ksnprintf(s->unix_path, sizeof(s->unix_path), "%s", path);
    s->unix_link = lx;
    if (l->x11)
        lin_x11_on_connect(lx);
    if (!l->x11 && !l->pulse) {
        if (l->pending_n >= (int)(sizeof(l->pending) / sizeof(l->pending[0])))
            return -111;
        l->pending[l->pending_n++] = lx;
    }
    if (l->pulse)
        lin_pulse_on_connect(lx);
    debug_printf("[linux/unix] connect %s%s%s%s\n",
                 abstract ? "@" : "", path,
                 l->x11 ? " (x11)" : "",
                 l->pulse ? " (pulse)" : "");
    return 0;
}

long lin_unix_accept(const lin_sock_t *listener, lin_sock_t *accepted,
                     void *addr, int *addrlen) {
    if (!listener || !accepted ||
        listener->domain != LIN_AF_UNIX ||
        listener->unix_phase != LSUN_LISTENING ||
        listener->unix_listener < 0)
        return -22;
    lin_unix_listener_t *l = &g_unix_listeners[listener->unix_listener];
    if (l->x11 || l->pulse)
        return -11; /* kernel service — no userspace accept queue */
    if (l->pending_n <= 0)
        return -11;
    int lx = l->pending[0];
    for (int i = 1; i < l->pending_n; i++)
        l->pending[i - 1] = l->pending[i];
    l->pending_n--;
    lin_sock_reset(accepted);
    accepted->kind = LSK_STREAM;
    accepted->domain = LIN_AF_UNIX;
    accepted->unix_phase = LSUN_CONNECTED;
    accepted->unix_abstract = listener->unix_abstract;
    ksnprintf(accepted->unix_path, sizeof(accepted->unix_path), "%s",
              listener->unix_path);
    accepted->unix_link = lx;
    if (addr && addrlen)
        fill_sockaddr_un(addr, addrlen, listener->unix_abstract,
                         listener->unix_path);
    return 0;
}

long lin_unix_read(lin_sock_t *s, void *buf, uint64_t len, bool nonblock) {
    if (!s || !buf || s->unix_phase != LSUN_CONNECTED || s->unix_link < 0)
        return -22;
    lin_unix_link_t *lk = link_get(s->unix_link);
    if (!lk)
        return -22;
    for (;;) {
        if (lk->rx_len > 0) {
            uint64_t n = len < lk->rx_len ? len : lk->rx_len;
            memcpy(buf, lk->rx, (size_t)n);
            if ((uint32_t)n < lk->rx_len)
                memmove(lk->rx, lk->rx + n, lk->rx_len - (uint32_t)n);
            lk->rx_len -= (uint32_t)n;
            return (long)n;
        }
        /* Kernel-service links (X11/PulseAudio) have no peer_link — the
         * kernel IS the peer, so an empty queue is EWOULDBLOCK / wait,
         * never EOF.  Servicing the X link here is what turns pending
         * mouse/keyboard state into wire events for a blocked reader. */
        if (lk->x11 || lk->pulse) {
            if (lk->x11)
                lin_x11_service(s->unix_link);
            if (lk->rx_len > 0)
                continue;
            if (nonblock)
                return -11;
            pit_sleep(1);
            continue;
        }
        if (lk->peer_link < 0)
            return 0; /* peer closed — real EOF */
        if (nonblock)
            return -11;
        if (lk->steam_ui) {
            if (lk->steam_ui_phase >= 3 && lk->rx_len == 0)
                return 0;
            if (lk->rx_len == 0)
                steam_ui_queue_all(lk);
        }
        pit_sleep(1);
    }
}

long lin_unix_write(lin_sock_t *s, const void *buf, uint64_t len) {
    if (!s || !buf || s->unix_phase != LSUN_CONNECTED || s->unix_link < 0)
        return -22;
    lin_unix_link_t *lk = link_get(s->unix_link);
    if (!lk)
        return -22;
    if (lk->x11) {
        lin_x11_on_client_write(s->unix_link, buf, len);
        return (long)len;
    }
    if (lk->pulse) {
        lin_pulse_on_client_write(s->unix_link, buf, len);
        return (long)len;
    }
    if (lk->peer_link >= 0) {
        lin_unix_link_t *peer = link_get(lk->peer_link);
        if (!peer)
            return -22;
        if (peer->steam_ui && len > 0 && len <= 64) {
            const uint8_t *p = (const uint8_t *)buf;
            debug_printf("[linux/unix] steam update UI parent->child %u bytes:",
                         (unsigned)len);
            for (uint64_t i = 0; i < len && i < 16; i++)
                debug_printf(" %02x", p[i]);
            debug_printf("\n");
        }
        rx_push(peer, buf, (uint32_t)len);
        return (long)len;
    }
    return (long)len;
}

static void link_ref(int ix) {
    lin_unix_link_t *lk = link_get(ix);
    if (lk) lk->refcnt++;
}

void lin_unix_link_dup_refs(int link_ix) {
    link_ref(link_ix);
}

static void link_unref(int ix) {
    lin_unix_link_t *lk = link_get(ix);
    if (!lk) return;
    if (lk->refcnt > 0) lk->refcnt--;
    if (lk->refcnt > 0) return;
    if (lk->peer_link >= 0) {
        lin_unix_link_t *peer = link_get(lk->peer_link);
        if (peer) peer->peer_link = -1;
        lk->peer_link = -1;
    }
    link_free(ix);
}

void lin_unix_close(lin_sock_t *s) {
    if (!s)
        return;
    if (s->unix_link >= 0) {
        int ix = s->unix_link;
        lin_unix_link_t *lk = link_get(ix);
        if (lk && lk->x11 && lk->refcnt <= 1)
            lin_x11_on_disconnect(ix);
        if (lk && lk->peer_link >= 0) {
            lin_unix_link_t *peer = link_get(lk->peer_link);
            if (peer)
                peer->peer_link = -1;
            lk->peer_link = -1;
        }
        link_unref(ix);
    }
    s->unix_link = -1;
    s->unix_listener = -1;
    s->unix_phase = LSUN_IDLE;
}

long lin_unix_socketpair(lin_sock_t *a, lin_sock_t *b) {
    if (!a || !b)
        return -22;
    int l0 = link_alloc(false, false);
    int l1 = link_alloc(false, false);
    if (l0 < 0 || l1 < 0) {
        if (l0 >= 0) link_free(l0);
        if (l1 >= 0) link_free(l1);
        return -24;
    }
    g_unix_links[l0].peer_link = l1;
    g_unix_links[l1].peer_link = l0;
    g_unix_links[l0].refcnt = 1;
    g_unix_links[l1].refcnt = 1;

    lin_sock_reset(a);
    a->kind = LSK_STREAM;
    a->domain = LIN_AF_UNIX;
    a->unix_phase = LSUN_CONNECTED;
    a->unix_link = l0;

    lin_sock_reset(b);
    b->kind = LSK_STREAM;
    b->domain = LIN_AF_UNIX;
    b->unix_phase = LSUN_CONNECTED;
    b->unix_link = l1;
    debug_printf("[linux/unix] socketpair -> links %d/%d\n", l0, l1);
    return 0;
}

bool lin_unix_poll_in(const lin_sock_t *s) {
    if (!s || s->unix_phase != LSUN_CONNECTED || s->unix_link < 0)
        return false;
    lin_unix_link_t *lk = link_get(s->unix_link);
    if (!lk)
        return false;
    if (lk->steam_ui && lk->steam_ui_phase < 3 && lk->rx_len == 0)
        steam_ui_queue_all(lk);
    /* Poll on an X link is the event pump for select()-driven clients. */
    if (lk->x11 && lk->rx_len == 0)
        lin_x11_service(s->unix_link);
    return lk->rx_len > 0;
}

bool lin_unix_poll_out(const lin_sock_t *s) {
    if (!s)
        return false;
    return s->unix_phase == LSUN_CONNECTED ||
           s->unix_phase == LSUN_LISTENING;
}

long lin_unix_getsockname(const lin_sock_t *s, void *addr, int *addrlen) {
    if (!s || s->domain != LIN_AF_UNIX || !addr || !addrlen)
        return -22;
    if (s->unix_path[0] == '\0')
        return -22;
    fill_sockaddr_un(addr, addrlen, s->unix_abstract, s->unix_path);
    return 0;
}

void lin_unix_link_reply(int link_ix, const void *data, uint32_t len) {
    lin_unix_link_t *lk = link_get(link_ix);
    if (!lk)
        return;
    rx_push(lk, data, len);
}

void lin_unix_x11_reply(int link_ix, const void *data, uint32_t len) {
    lin_unix_link_reply(link_ix, data, len);
}

void lin_unix_x11_append_setup(int link_ix, const void *data, uint32_t len) {
    lin_unix_link_t *lk = link_get(link_ix);
    if (!lk || !data)
        return;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        if (lk->setup_len >= sizeof(lk->setup))
            break;
        lk->setup[lk->setup_len++] = p[i];
    }
}

uint32_t lin_unix_x11_setup_len(int link_ix) {
    const lin_unix_link_t *lk = link_get(link_ix);
    return lk ? lk->setup_len : 0;
}

const uint8_t *lin_unix_x11_setup_buf(int link_ix) {
    const lin_unix_link_t *lk = link_get(link_ix);
    return lk ? lk->setup : NULL;
}

void lin_unix_x11_clear_setup(int link_ix) {
    lin_unix_link_t *lk = link_get(link_ix);
    if (lk)
        lk->setup_len = 0;
}

int lin_unix_register_x11_listener(void) {
    lin_unix_listener_t *l =
        listener_alloc(true, "/tmp/.X11-unix/X0", true, false);
    return l ? 0 : -1;
}

int lin_unix_register_pulse_listener(void) {
    lin_unix_listener_t *l =
        listener_alloc(true, "/run/user/0/pulse/native", false, true);
    return l ? 0 : -1;
}

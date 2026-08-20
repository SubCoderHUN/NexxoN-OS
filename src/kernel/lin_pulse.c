/* ============================================================================
 * NexxoN OS - Minimal PulseAudio native protocol shim
 * ============================================================================ */
#include "lin_pulse.h"
#include "debug.h"
#include "string.h"

extern void lin_unix_link_reply(int link_ix, const void *data, uint32_t len);
extern int lin_unix_register_pulse_listener(void);

#define PA_NATIVE_COOKIE ((uint32_t)(('P' << 24) | ('A' << 16) | ('S' << 8) | 'O'))
#define PA_TAG_STRING  3u
#define PA_COMMAND_SET_CLIENT_NAME 0u

typedef struct {
    bool active;
    bool handshake_done;
    uint8_t in[4096];
    uint32_t in_len;
} lin_pulse_client_t;

static lin_pulse_client_t g_pulse[16];

void lin_pulse_init(void) {
    memset(g_pulse, 0, sizeof(g_pulse));
    if (lin_unix_register_pulse_listener() == 0)
        debug_printf("[linux/pulse] native @/run/user/0/pulse/native ready\n");
}

void lin_pulse_on_connect(int link_ix) {
    if (link_ix < 0 || link_ix >= 16)
        return;
    memset(&g_pulse[link_ix], 0, sizeof(g_pulse[link_ix]));
    g_pulse[link_ix].active = true;
    uint32_t hello[2];
    hello[0] = PA_NATIVE_COOKIE;
    hello[1] = 8;
    lin_unix_link_reply(link_ix, hello, sizeof(hello));
    debug_printf("[linux/pulse] handshake link=%d\n", link_ix);
}

static void pulse_send_ack(int link_ix) {
    uint8_t ack[32];
    memset(ack, 0, sizeof(ack));
    ack[0] = 0;
    ack[1] = 0;
    ack[2] = 0;
    ack[3] = 0;
    lin_unix_link_reply(link_ix, ack, sizeof(ack));
}

void lin_pulse_on_client_write(int link_ix, const void *data, uint64_t len) {
    if (link_ix < 0 || link_ix >= 16 || !data || !len)
        return;
    lin_pulse_client_t *c = &g_pulse[link_ix];
    if (!c->active)
        lin_pulse_on_connect(link_ix);
    uint32_t n = len > sizeof(c->in) - c->in_len
        ? (uint32_t)(sizeof(c->in) - c->in_len) : (uint32_t)len;
    memcpy(c->in + c->in_len, data, n);
    c->in_len += n;
    if (!c->handshake_done && c->in_len >= 4) {
        c->handshake_done = true;
        pulse_send_ack(link_ix);
        debug_printf("[linux/pulse] SetClientName ack link=%d\n", link_ix);
    }
}

/* ============================================================================
 * NexxoN OS - Network interface dispatcher
 * ----------------------------------------------------------------------------
 * Holds a registry of NIC drivers and a "selected" pointer that net.c
 * uses for all I/O.  Auto-probes each registered driver during init in
 * the order they appear so the E1000 (most common emulator) wins on
 * QEMU and the AMD PCnet wins on VirtualBox.
 * ============================================================================ */
#include "netif.h"
#include "debug.h"
#include "string.h"

#define NETIF_MAX 4
static const netif_driver_t *g_drv[NETIF_MAX];
static int                   g_n = 0;
static const netif_driver_t *g_active = NULL;

/* Cumulative byte counters for the Task Manager network graph (real data,
 * replacing the old synthetic feed).  RX is tallied by a counting wrapper
 * installed in front of the stack's rx callback; TX in netif_send. */
static uint32_t      g_rx_bytes = 0;
static uint32_t      g_tx_bytes = 0;
static netif_rx_cb_t g_user_rx_cb   = NULL;
static void         *g_user_rx_user = NULL;

bool netif_register(const netif_driver_t *drv) {
    if (!drv || g_n >= NETIF_MAX) return false;
    g_drv[g_n++] = drv;
    debug_printf("[netif] driver registered: %s (slot %d)\n",
                 drv->name, g_n - 1);
    return true;
}

bool netif_autodetect(void) {
    for (int i = 0; i < g_n; i++) {
        const netif_driver_t *d = g_drv[i];
        if (!d->probe || !d->probe()) continue;
        if (!d->init  || !d->init()) {
            debug_printf("[netif] %s probe ok but init failed - "
                         "trying next\n", d->name);
            continue;
        }
        g_active = d;
        debug_printf("[netif] bound to '%s' as default interface\n", d->name);
        return true;
    }
    debug_printf("[netif] no NIC driver matched - network disabled\n");
    return false;
}

bool netif_present(void) { return g_active != NULL; }
const char *netif_name(void) {
    return g_active ? g_active->name : "(none)";
}
void netif_mac(uint8_t out[6]) {
    if (g_active && g_active->mac) g_active->mac(out);
    else memset(out, 0, 6);
}
int netif_send(const void *frame, uint16_t len) {
    if (!g_active || !g_active->send) return -1;
    int r = g_active->send(frame, len);
    if (r >= 0) g_tx_bytes += len;
    return r;
}
void netif_poll(void) {
    if (g_active && g_active->poll) g_active->poll();
}

/* Counting shim: tally RX bytes, then hand the frame to the real stack cb. */
static void netif_rx_counting(const uint8_t *frame, uint16_t len, void *user) {
    (void)user;
    g_rx_bytes += len;
    if (g_user_rx_cb) g_user_rx_cb(frame, len, g_user_rx_user);
}
void netif_set_rx_cb(netif_rx_cb_t cb, void *user) {
    g_user_rx_cb   = cb;
    g_user_rx_user = user;
    if (g_active && g_active->set_rx_cb)
        g_active->set_rx_cb(netif_rx_counting, NULL);
}

/* Cumulative byte counters (the Task Manager turns deltas into Kbps). */
void netif_byte_counts(uint32_t *rx_bytes, uint32_t *tx_bytes) {
    if (rx_bytes) *rx_bytes = g_rx_bytes;
    if (tx_bytes) *tx_bytes = g_tx_bytes;
}

/* ============================================================================
 * NexxoN OS - Generic network interface abstraction (NIC dispatcher)  v1.0
 * ----------------------------------------------------------------------------
 * Abstracts driver-specific bring-up + Rx/Tx so the same net.c stack
 * works against any registered NIC.  Drivers fill a netif_driver_t
 * vtable and call netif_register().  The first interface registered
 * becomes the "default" network egress.
 *
 * Currently registered:
 *   - e1000.c   for Intel 82540EM (8086:100e)        - QEMU / Bochs
 *   - pcnet.c   for AMD PCnet-FAST III (1022:2000)   - VirtualBox default
 *
 * The dispatcher binds at boot via netif_autodetect() which walks
 * every driver's probe hook and picks the first that returns true.
 * ============================================================================ */
#ifndef NEXXON_NETIF_H
#define NEXXON_NETIF_H

#include "types.h"

typedef void (*netif_rx_cb_t)(const uint8_t *frame, uint16_t len, void *user);

typedef struct netif_driver {
    const char *name;
    bool      (*probe)(void);
    bool      (*init)(void);
    void      (*mac)(uint8_t out[6]);
    int       (*send)(const void *frame, uint16_t len);
    void      (*poll)(void);
    void      (*set_rx_cb)(netif_rx_cb_t cb, void *user);
} netif_driver_t;

bool netif_register   (const netif_driver_t *drv);
bool netif_autodetect (void);
bool netif_present    (void);
const char *netif_name(void);
void netif_mac        (uint8_t out[6]);
int  netif_send       (const void *frame, uint16_t len);
void netif_poll       (void);
void netif_set_rx_cb  (netif_rx_cb_t cb, void *user);
/* Cumulative RX/TX bytes since boot (Task Manager derives Kbps from deltas). */
void netif_byte_counts(uint32_t *rx_bytes, uint32_t *tx_bytes);

#endif /* NEXXON_NETIF_H */

/* ============================================================================
 * NexxoN OS - Intel 82540EM Gigabit Ethernet (E1000) driver  (v1.0)
 * ----------------------------------------------------------------------------
 * Detects PCI 8086:100e (QEMU's default "e1000"), maps its MMIO BAR0 into
 * the kernel's identity-mapped address space, reads the MAC address out of
 * the EEPROM-shadowed RAL/RAH registers, sets up small Rx/Tx descriptor
 * rings + DMA buffers, and exposes:
 *
 *   e1000_init()       - one-shot bring-up; returns true on success.
 *   e1000_present()    - "have we got a NIC?".
 *   e1000_mac(uint8_t out[6]) - copy the local MAC into out[].
 *   e1000_send(buf, len)   - synchronous packet send (blocks until TX done).
 *   e1000_poll()       - drain Rx ring; dispatches to net_rx() in net/.
 *
 * The driver is interrupt-driven: the PCI IRQ line is gathered from the
 * PCI config space (offset 0x3C - Interrupt Line, 1 byte).  We additionally
 * fall back to polling from the net stack tick so a missed IRQ never
 * permanently stalls receive.
 * ============================================================================ */
#ifndef NEXXON_E1000_H
#define NEXXON_E1000_H

#include "types.h"

#define E1000_VENDOR_ID    0x8086
/* Primary device ID (QEMU's default e1000 emulation).  Additional
 * PCI device IDs supported by this driver are matched in e1000_probe
 * via the g_e1000_devids[] table - covers Intel 82540EM (QEMU),
 * 82574L, ICH8/ICH9/ICH10/PCH GbE families including 82579V
 * (modern Sandy/Ivy Bridge laptop NICs). */
#define E1000_DEVICE_ID    0x100E

/* Family tag for log messages.  The PHY init sequence diverges between
 * the legacy 82540 / 82574 line and the PCH-bound e1000e family
 * (82577/82579/I217/I218/I219) - we branch on this. */
typedef enum {
    E1000_FAMILY_LEGACY = 0,    /* 82540EM, 82541, 82545, 82574 */
    E1000_FAMILY_ICH    = 1,    /* ICH8 / ICH9 / ICH10 GbE       */
    E1000_FAMILY_PCH    = 2,    /* 82577 / 82579 / I217 / I218   */
} e1000_family_t;

/* RX ring is deep (64) so a burst on a busy LAN doesn't run the ring dry
 * between poll()s and drop frames (RNBC) — e.g. the one DHCP OFFER we need.
 * TX stays shallow: we transmit one frame at a time and wait for it. */
#define E1000_RX_DESCS     64
#define E1000_TX_DESCS     16
#define E1000_RX_BUF_SIZE  2048
#define E1000_TX_BUF_SIZE  2048

typedef void (*e1000_rx_cb_t)(const uint8_t *frame, uint16_t len, void *user);

bool     e1000_init     (void);
bool     e1000_present  (void);
/* Brutal NIC register/ring diagnostic (link, RX/TX rings, descriptor status)
 * for debugging a NIC that should work but doesn't on bare metal.  Returns
 * bytes written into `out`. */
int      e1000_diag     (char *out, size_t cap);
void     e1000_mac      (uint8_t out[6]);
int      e1000_send     (const void *buf, uint16_t len);
void     e1000_poll     (void);
void     e1000_set_rx_cb(e1000_rx_cb_t cb, void *user);

/* IRQ-side entry point used by irq.c when the NIC line fires. */
void     e1000_irq_handler(void);

#endif /* NEXXON_E1000_H */

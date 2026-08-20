/* ============================================================================
 * NexxoN OS - AMD PCnet-FAST III (Am79C973) NIC driver  (v1.0)
 * ----------------------------------------------------------------------------
 * VirtualBox's default emulated NIC.  PCI ID 1022:2000.
 *
 * Uses I/O-port based access (BAR0 is I/O space) for the 32-bit ports:
 *   APROM        +0x00..0x0F   MAC address (EEPROM-shadowed)
 *   RDP          +0x10         CSR data
 *   RAP          +0x14         CSR/BCR register select
 *   RESET        +0x18         write any value -> hard reset
 *   BDP          +0x1C         BCR data
 *
 * Standard ring layout: 16 RX desc + 16 TX desc.  Descriptors and
 * 2 KiB DMA buffers live in BSS with 16-byte alignment.
 *
 * Implemented in 32-bit DWIO mode (set in BCR20 bit 8); init block is
 * placed in physical memory and the controller is pointed at it via
 * CSR1/CSR2.  After START the controller streams Rx/Tx independently.
 *
 * Polling-only: hooks into the netif dispatcher's poll() so the same
 * shell idle loop drains it alongside E1000.  IRQ wiring is sketched
 * out below but defaulted off because VirtualBox's default PCI line
 * configuration isn't deterministic across host versions.
 * ============================================================================ */
#include "netif.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "debug.h"
#include "pit.h"

#define PCNET_VENDOR  0x1022
#define PCNET_DEVICE  0x2000

#define PCNET_RDP     0x10
#define PCNET_RAP     0x14
#define PCNET_RESET   0x18
#define PCNET_BDP     0x1C

#define PCNET_CSR0     0
#define PCNET_CSR1     1
#define PCNET_CSR2     2
#define PCNET_CSR3     3
#define PCNET_CSR4     4
#define PCNET_BCR20   20

#define PCNET_CSR0_INIT  (1u << 0)
#define PCNET_CSR0_STRT  (1u << 1)
#define PCNET_CSR0_STOP  (1u << 2)
#define PCNET_CSR0_IDON  (1u << 8)
#define PCNET_CSR0_RINT  (1u << 10)
#define PCNET_CSR0_TINT  (1u << 9)

#define PCNET_RING_LEN_SHIFT  29   /* in init block mode_h field           */
#define PCNET_RX_DESCS  16
#define PCNET_TX_DESCS  16
#define PCNET_BUF_SIZE  2048

typedef struct PACKED {
    uint32_t addr;       /* physical buffer address                       */
    int16_t  bcnt;       /* -(buffer size), two's complement              */
    uint16_t status;     /* OWN | ERR | STP | ENP | ...                   */
    uint32_t misc;       /* RX: receive byte count; TX: transmit errors   */
    uint32_t reserved;
} pcnet_desc_t;

typedef struct PACKED {
    uint16_t mode;
    uint8_t  rlen;        /* log2 of RX ring length                       */
    uint8_t  tlen;        /* log2 of TX ring length                       */
    uint8_t  padr[6];
    uint16_t _pad0;
    uint8_t  ladrf[8];
    uint32_t rdra;        /* RX desc ring physical address                */
    uint32_t tdra;        /* TX desc ring physical address                */
} pcnet_init_block_t;

#define DESC_OWN  0x8000

ALIGNED(16) static pcnet_desc_t g_rx[PCNET_RX_DESCS];
ALIGNED(16) static pcnet_desc_t g_tx[PCNET_TX_DESCS];
ALIGNED(16) static uint8_t      g_rxbuf[PCNET_RX_DESCS][PCNET_BUF_SIZE];
ALIGNED(16) static uint8_t      g_txbuf[PCNET_TX_DESCS][PCNET_BUF_SIZE];
ALIGNED(16) static pcnet_init_block_t g_initblk;

static uint16_t g_io = 0;
static uint8_t  g_mac[6] = {0};
static int      g_rx_cur = 0;
static int      g_tx_cur = 0;
static bool     g_present = false;
static netif_rx_cb_t g_rx_cb = NULL;
static void         *g_rx_user = NULL;

static inline void csr_write(uint32_t reg, uint32_t val) {
    outl(g_io + PCNET_RAP, reg);
    outl(g_io + PCNET_RDP, val);
}
static inline uint32_t csr_read(uint32_t reg) {
    outl(g_io + PCNET_RAP, reg);
    return inl(g_io + PCNET_RDP);
}
static inline void bcr_write(uint32_t reg, uint32_t val) {
    outl(g_io + PCNET_RAP, reg);
    outl(g_io + PCNET_BDP, val);
}

static bool pcnet_probe(void) {
    pci_device_t devs[64];
    int n = pci_enumerate(devs, 64);
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id == PCNET_VENDOR &&
            devs[i].device_id == PCNET_DEVICE) return true;
    }
    return false;
}

static bool pcnet_init(void) {
    pci_device_t devs[64];
    int n = pci_enumerate(devs, 64);
    pci_device_t *match = NULL;
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id == PCNET_VENDOR &&
            devs[i].device_id == PCNET_DEVICE) { match = &devs[i]; break; }
    }
    if (!match) return false;
    g_io = (uint16_t)(match->bar[0] & 0xFFFCu);
    if (!g_io) return false;
    pci_enable_busmaster(match);

    /* Read MAC address from APROM (first 6 bytes of the I/O space). */
    for (int i = 0; i < 6; i++) g_mac[i] = inb(g_io + i);

    /* Hard reset by reading the RESET register, then write to switch to
     * 32-bit access (DWIO).  In DWIO every CSR access uses dwords. */
    (void)inl(g_io + PCNET_RESET);
    pit_sleep(5);
    /* Switch to DWIO mode by writing any 32-bit value to the RESET
     * register at the right offset; PCnet documents this as the
     * "Reset_DWord" path. */
    outl(g_io + PCNET_RESET, 0);
    pit_sleep(1);

    /* SWSTYLE = 2 (32-bit) via BCR20. */
    bcr_write(PCNET_BCR20, 0x102u);

    /* Init Rx ring. */
    memset(g_rx, 0, sizeof(g_rx));
    for (int i = 0; i < PCNET_RX_DESCS; i++) {
        g_rx[i].addr   = (uint32_t)(uintptr_t)g_rxbuf[i];
        g_rx[i].bcnt   = (int16_t)(-(int16_t)PCNET_BUF_SIZE);
        g_rx[i].status = DESC_OWN;
    }
    memset(g_tx, 0, sizeof(g_tx));
    for (int i = 0; i < PCNET_TX_DESCS; i++) {
        g_tx[i].addr   = (uint32_t)(uintptr_t)g_txbuf[i];
        g_tx[i].bcnt   = 0;
        g_tx[i].status = 0;
    }

    memset(&g_initblk, 0, sizeof(g_initblk));
    g_initblk.mode = 0x0000;         /* normal, no promisc                */
    g_initblk.rlen = 4;              /* log2(16) = 4                      */
    g_initblk.tlen = 4;
    memcpy(g_initblk.padr, g_mac, 6);
    g_initblk.rdra = (uint32_t)(uintptr_t)g_rx;
    g_initblk.tdra = (uint32_t)(uintptr_t)g_tx;

    /* Hand the init block to the controller via CSR1/CSR2. */
    uint32_t ib = (uint32_t)(uintptr_t)&g_initblk;
    csr_write(PCNET_CSR1, ib & 0xFFFFu);
    csr_write(PCNET_CSR2, (ib >> 16) & 0xFFFFu);
    /* INIT bit in CSR0.  After IDON asserts we set START. */
    csr_write(PCNET_CSR0, PCNET_CSR0_INIT);

    /* Wait for IDON.  Bounded so a hung emulator doesn't lock the boot. */
    for (int i = 0; i < 1000; i++) {
        uint32_t s = csr_read(PCNET_CSR0);
        if (s & PCNET_CSR0_IDON) {
            csr_write(PCNET_CSR0, s & 0x4u);  /* ack IDON (bit 8 clears)  */
            break;
        }
        pit_sleep(1);
    }
    /* Mask interrupts (poll-only), then start. */
    csr_write(PCNET_CSR3, 0x7F00u);
    csr_write(PCNET_CSR0, PCNET_CSR0_STRT);

    g_present = true;
    debug_printf("[pcnet] AMD Am79C973 @ I/O 0x%04x  MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                 g_io, g_mac[0], g_mac[1], g_mac[2],
                 g_mac[3], g_mac[4], g_mac[5]);
    return true;
}

static void pcnet_mac(uint8_t out[6]) {
    memcpy(out, g_mac, 6);
}

static int pcnet_send(const void *frame, uint16_t len) {
    if (!g_present || !frame || len == 0 || len > PCNET_BUF_SIZE) return -1;
    int slot = g_tx_cur;
    pcnet_desc_t *d = &g_tx[slot];
    if (d->status & DESC_OWN) return -1;     /* still busy */
    memcpy(g_txbuf[slot], frame, len);
    d->bcnt   = (int16_t)(-(int16_t)len);
    d->status = DESC_OWN | 0x0200 | 0x0100;  /* STP + ENP */
    g_tx_cur = (slot + 1) % PCNET_TX_DESCS;
    /* TDMD demand transmit bit in CSR0. */
    csr_write(PCNET_CSR0, 0x08u);
    return 0;
}

static void pcnet_poll(void) {
    if (!g_present) return;
    /* Ack RINT/TINT in CSR0 first so the controller can fire again. */
    uint32_t s = csr_read(PCNET_CSR0);
    if (s & (PCNET_CSR0_RINT | PCNET_CSR0_TINT)) {
        csr_write(PCNET_CSR0, s & 0xFFFFu);
    }
    while (!(g_rx[g_rx_cur].status & DESC_OWN)) {
        pcnet_desc_t *d = &g_rx[g_rx_cur];
        if ((d->status & 0x0300) == 0x0300) {  /* STP + ENP */
            uint16_t len = (uint16_t)(d->misc & 0x0FFFu);
            if (g_rx_cb && len > 0 && len < PCNET_BUF_SIZE) {
                g_rx_cb(g_rxbuf[g_rx_cur], len, g_rx_user);
            }
        }
        d->status = DESC_OWN;
        d->bcnt   = (int16_t)(-(int16_t)PCNET_BUF_SIZE);
        g_rx_cur = (g_rx_cur + 1) % PCNET_RX_DESCS;
    }
}

static void pcnet_rxcb(netif_rx_cb_t cb, void *user) {
    g_rx_cb = cb; g_rx_user = user;
}

const netif_driver_t pcnet_driver = {
    .name = "AMD PCnet-FAST III (Am79C973)",
    .probe = pcnet_probe,
    .init  = pcnet_init,
    .mac   = pcnet_mac,
    .send  = pcnet_send,
    .poll  = pcnet_poll,
    .set_rx_cb = pcnet_rxcb,
};

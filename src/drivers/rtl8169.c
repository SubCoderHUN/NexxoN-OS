/* ============================================================================
 * NexxoN OS - Realtek RTL8169 / RTL8111 / RTL8168 Gigabit driver  (v1.0)
 * ----------------------------------------------------------------------------
 * Native driver for the Realtek 10/100/1000 Mbps Ethernet family commonly
 * found on consumer desktops, mini-PCs and laptops.  Plugs into the
 * netif_driver_t dispatcher (registered alongside e1000 / PCnet) so the
 * existing IPv4/ICMP/TCP stack picks it up automatically.
 *
 * Supported PCI device IDs (10EC vendor):
 *   8129 - RTL-8128/8129 Fast Ethernet (legacy, included for compat)
 *   8136 - RTL-8101/8102E PCI-Express Fast Ethernet (treated as 1G)
 *   8161 - RTL-8111G/RTL-8168G PCIe Gigabit
 *   8167 - RTL-8110SC Gigabit (PCI)
 *   8168 - RTL-8111B/RTL-8168B/RTL-8111C/RTL-8111D/RTL-8111E/RTL-8168E
 *   8169 - RTL-8169 / RTL-8110SB Gigabit (PCI)
 *   8171 - RTL-8191SE wireless (declines)
 *
 * Register set follows the RTL8169 Programming Guide (1.04) chapter 4.
 * We implement just enough to push/receive raw Ethernet frames; no
 * EEE, no Wake-on-LAN, no TX flow control.  Both legacy descriptor
 * format (RTL8169) and the newer RTL8168 format are layout-compatible
 * for the bits we touch (OWN / EOR / FS / LS / size), so a single code
 * path works on the entire family.
 *
 * IRQ wiring: the BIOS routes the line via PCI Interrupt Line (config
 * offset 0x3C).  We honour that and additionally fall back to polling
 * from net_tick - matches the e1000 driver's policy.
 * ============================================================================ */
#include "netif.h"
#include "pci.h"
#include "io.h"
#include "irq.h"
#include "isr.h"
#include "string.h"
#include "debug.h"
#include "pit.h"
#include "pic.h"

#define RTL_VENDOR  0x10EC

/* Register file (BAR0 = I/O, BAR2 = MMIO).  We use I/O ports because
 * that's what every RTL8169 generation supports identically. */
#define RTL_REG_IDR0         0x00    /* MAC address                          */
#define RTL_REG_TNPDS        0x20    /* Transmit Normal Priority DescStart   */
#define RTL_REG_CR           0x37    /* Command register                     */
#define RTL_REG_TPPOLL       0x38    /* Transmit Priority Polling            */
#define RTL_REG_IMR          0x3C    /* Interrupt Mask                       */
#define RTL_REG_ISR          0x3E    /* Interrupt Status                     */
#define RTL_REG_TCR          0x40    /* Transmit Configuration               */
#define RTL_REG_RCR          0x44    /* Receive Configuration                */
#define RTL_REG_9346CR       0x50    /* EEPROM / Config access lock          */
#define RTL_REG_CONFIG1      0x52
#define RTL_REG_PHYAR        0x60    /* PHY access                           */
#define RTL_REG_RMS          0xDA    /* Rx max packet size                   */
#define RTL_REG_RDSAR        0xE4    /* Rx Descriptor Start                  */
#define RTL_REG_MTPS         0xEC    /* Max TX Packet Size                   */

/* CR bits */
#define RTL_CR_RST           0x10
#define RTL_CR_RE            0x08
#define RTL_CR_TE            0x04

/* 9346CR lock bits */
#define RTL_9346CR_UNLOCK    0xC0
#define RTL_9346CR_LOCK      0x00

/* RCR bits we care about */
#define RTL_RCR_AAP          (1u << 0)   /* accept all phys                  */
#define RTL_RCR_APM          (1u << 1)   /* accept phys-matching             */
#define RTL_RCR_AM           (1u << 2)   /* accept multicast                 */
#define RTL_RCR_AB           (1u << 3)   /* accept broadcast                 */
#define RTL_RCR_RXFTH_NONE   (0x7u << 13)

/* TCR */
#define RTL_TCR_IFG_NORMAL   (3u << 24)
#define RTL_TCR_MXDMA_2048   (7u << 8)

/* Descriptor flags */
#define RTL_DESC_OWN         (1u << 31)
#define RTL_DESC_EOR         (1u << 30)
#define RTL_DESC_FS          (1u << 29)
#define RTL_DESC_LS          (1u << 28)

#define RTL_NUM_TX_DESC      16
#define RTL_NUM_RX_DESC      16
#define RTL_BUF_SIZE         2048

typedef struct PACKED {
    uint32_t cmd;       /* OWN | EOR | FS | LS | size */
    uint32_t vlan;      /* unused, must be 0          */
    uint32_t buf_lo;    /* low 32 bits of buf phys    */
    uint32_t buf_hi;    /* high 32 bits, always 0     */
} rtl_desc_t;

/* PCI device IDs we accept */
typedef struct {
    uint16_t device_id;
    const char *name;
} rtl_devid_t;

static const rtl_devid_t g_rtl_devids[] = {
    { 0x8129, "RTL8129 Fast Ethernet"          },
    { 0x8136, "RTL8101E PCIe Fast Ethernet"    },
    { 0x8161, "RTL8168G/RTL8111G Gigabit"      },
    { 0x8167, "RTL8110SC Gigabit"              },
    { 0x8168, "RTL8168/RTL8111 Gigabit"        },
    { 0x8169, "RTL8169 Gigabit"                },
};
#define RTL_DEVID_COUNT ((int)(sizeof(g_rtl_devids) / sizeof(g_rtl_devids[0])))

static const rtl_devid_t *rtl_match(uint16_t did) {
    for (int i = 0; i < RTL_DEVID_COUNT; i++) {
        if (g_rtl_devids[i].device_id == did) return &g_rtl_devids[i];
    }
    return NULL;
}

ALIGNED(256) static rtl_desc_t g_tx_descs[RTL_NUM_TX_DESC];
ALIGNED(256) static rtl_desc_t g_rx_descs[RTL_NUM_RX_DESC];
ALIGNED(16)  static uint8_t    g_tx_buf[RTL_NUM_TX_DESC][RTL_BUF_SIZE];
ALIGNED(16)  static uint8_t    g_rx_buf[RTL_NUM_RX_DESC][RTL_BUF_SIZE];

static uint16_t g_iobase = 0;
static uint8_t  g_mac[6] = { 0 };
static int      g_tx_cur = 0;
static int      g_rx_cur = 0;
static bool     g_present = false;
static netif_rx_cb_t g_rx_cb = NULL;
static void *g_rx_user = NULL;

static inline uint8_t  rtl_r8 (uint16_t off) { return inb (g_iobase + off); }
static inline uint16_t rtl_r16(uint16_t off) { return inw (g_iobase + off); }
static inline uint32_t rtl_r32(uint16_t off) { return inl (g_iobase + off); }
static inline void rtl_w8 (uint16_t off, uint8_t  v) { outb(g_iobase + off, v); }
static inline void rtl_w16(uint16_t off, uint16_t v) { outw(g_iobase + off, v); }
static inline void rtl_w32(uint16_t off, uint32_t v) { outl(g_iobase + off, v); }

static void rtl_read_mac(void) {
    for (int i = 0; i < 6; i++) {
        g_mac[i] = rtl_r8((uint16_t)(RTL_REG_IDR0 + i));
    }
}

static void rtl_isr(registers_t *r) {
    (void)r;
    if (!g_present) return;
    uint16_t isr = rtl_r16(RTL_REG_ISR);
    /* Ack everything we know about by writing back the raised bits. */
    rtl_w16(RTL_REG_ISR, isr);
    /* The poll function does the actual RX drain - mirroring how the
     * e1000 driver treats the IRQ as a "drain now" wakeup. */
    extern void rtl_poll(void);
    rtl_poll();
}

static bool rtl_probe(void) {
    pci_device_t devs[64];
    int n = pci_enumerate(devs, 64);
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id != RTL_VENDOR) continue;
        if (rtl_match(devs[i].device_id) != NULL) return true;
    }
    return false;
}

bool rtl_init(void) {
    debug_step("rtl: searching for Realtek RTL8169/RTL8111/RTL8168 GbE");
    pci_device_t devs[64];
    int n = pci_enumerate(devs, 64);
    pci_device_t *match = NULL;
    const rtl_devid_t *match_id = NULL;
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id != RTL_VENDOR) continue;
        const rtl_devid_t *id = rtl_match(devs[i].device_id);
        if (id) { match = &devs[i]; match_id = id; break; }
    }
    if (!match) {
        debug_printf("[rtl] no RTL81xx on PCI bus\n");
        return false;
    }
    debug_printf("[rtl] %s at %02x:%02x.%x  BAR0=0x%08x BAR2=0x%08x\n",
                 match_id->name, match->bus, match->device, match->function,
                 match->bar[0], match->bar[2]);

    /* Prefer the I/O BAR (bit 0 == 1 marks an I/O BAR).  Some boards
     * map BAR0 as memory and BAR2 as I/O; some do the reverse. */
    uint16_t io = 0;
    for (int i = 0; i < 6; i++) {
        uint32_t b = match->bar[i];
        if (b == 0) continue;
        if (b & 0x1) { io = (uint16_t)(b & 0xFFFCu); break; }
    }
    if (!io) {
        debug_printf("[rtl] no I/O BAR assigned\n");
        return false;
    }
    g_iobase = io;
    pci_enable_busmaster(match);

    /* Unlock config registers. */
    rtl_w8(RTL_REG_9346CR, RTL_9346CR_UNLOCK);

    /* Power-up: clear sleep & LWAKE bits.  Many boards leave the chip
     * in a half-power state until the OS pokes CONFIG1. */
    rtl_w8(RTL_REG_CONFIG1, 0x00);

    /* Software reset.  CR.RST self-clears when the controller has
     * finished its internal reset (~100 ms worst case). */
    rtl_w8(RTL_REG_CR, RTL_CR_RST);
    {
        uint32_t start = pit_ms();
        while ((rtl_r8(RTL_REG_CR) & RTL_CR_RST) != 0) {
            if (pit_ms() - start > 200) {
                debug_printf("[rtl] reset timed out (CR=0x%02x)\n",
                             rtl_r8(RTL_REG_CR));
                break;
            }
        }
    }

    rtl_read_mac();
    debug_printf("[rtl] MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
                 g_mac[0], g_mac[1], g_mac[2],
                 g_mac[3], g_mac[4], g_mac[5]);

    /* Build descriptor rings. */
    memset(g_tx_descs, 0, sizeof(g_tx_descs));
    memset(g_rx_descs, 0, sizeof(g_rx_descs));
    for (int i = 0; i < RTL_NUM_RX_DESC; i++) {
        uint32_t flags = RTL_DESC_OWN | (RTL_BUF_SIZE & 0x3FFF);
        if (i == RTL_NUM_RX_DESC - 1) flags |= RTL_DESC_EOR;
        g_rx_descs[i].cmd    = flags;
        g_rx_descs[i].buf_lo = (uint32_t)(uintptr_t)g_rx_buf[i];
        g_rx_descs[i].buf_hi = 0;
    }
    for (int i = 0; i < RTL_NUM_TX_DESC; i++) {
        g_tx_descs[i].cmd = (i == RTL_NUM_TX_DESC - 1) ? RTL_DESC_EOR : 0;
        g_tx_descs[i].buf_lo = (uint32_t)(uintptr_t)g_tx_buf[i];
        g_tx_descs[i].buf_hi = 0;
    }

    /* Point the controller at the descriptor rings. */
    rtl_w32(RTL_REG_TNPDS,     (uint32_t)(uintptr_t)g_tx_descs);
    rtl_w32(RTL_REG_TNPDS + 4, 0);
    rtl_w32(RTL_REG_RDSAR,     (uint32_t)(uintptr_t)g_rx_descs);
    rtl_w32(RTL_REG_RDSAR + 4, 0);

    /* Rx max size = 16 KiB (plenty for 1500-byte MTU; protects us from
     * a giant unexpected packet). */
    rtl_w16(RTL_REG_RMS, 0x3FFF);
    rtl_w8 (RTL_REG_MTPS, 0x3B);

    /* TCR: enable normal IFG, max DMA burst of 2048 B.  We leave the
     * loopback bits clear so traffic actually hits the wire. */
    rtl_w32(RTL_REG_TCR, RTL_TCR_IFG_NORMAL | RTL_TCR_MXDMA_2048);

    /* RCR: accept broadcast + multicast + physical-match + all-phys
     * (promiscuous) so userspace can do raw-socket-style debugging if
     * we ever need it. */
    rtl_w32(RTL_REG_RCR,
            RTL_RCR_AAP | RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_AB |
            RTL_RCR_RXFTH_NONE | (7u << 8));

    /* Enable RX/TX.  This must happen AFTER the descriptors and
     * register values are programmed. */
    rtl_w8(RTL_REG_CR, RTL_CR_RE | RTL_CR_TE);

    /* Re-lock the config registers. */
    rtl_w8(RTL_REG_9346CR, RTL_9346CR_LOCK);

    /* Enable RX OK + TX OK + RX overflow interrupts. */
    rtl_w16(RTL_REG_IMR, 0x0007);
    rtl_w16(RTL_REG_ISR, 0xFFFF);   /* clear any latched bits */

    /* PCI interrupt line. */
    uint32_t intline = pci_read32(match->bus, match->device, match->function,
                                  0x3C) & 0xFFu;
    if (intline > 0 && intline < 16) {
        irq_install_handler((int)intline, rtl_isr);
        pic_unmask((uint8_t)intline);
        debug_printf("[rtl] IRQ%u hooked + unmasked\n", intline);
    } else {
        debug_printf("[rtl] no usable PCI IRQ line (got %u) - polling only\n",
                     intline);
    }

    g_tx_cur = 0;
    g_rx_cur = 0;
    g_present = true;
    debug_ok("rtl: ready");
    return true;
}

static void rtl_mac_vt(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = g_mac[i];
}

static int rtl_send_vt(const void *buf, uint16_t len) {
    if (!g_present || !buf || len == 0) return -1;
    if (len > RTL_BUF_SIZE - 60) return -1;
    /* Pad short frames to the Ethernet minimum (60 B + 4 B FCS). */
    uint16_t actual = len < 60 ? 60 : len;
    int slot = g_tx_cur;
    rtl_desc_t *d = &g_tx_descs[slot];
    /* Wait until HW has retired this slot. */
    uint32_t start = pit_ms();
    while ((d->cmd & RTL_DESC_OWN) != 0) {
        if (pit_ms() - start > 50) {
            debug_printf("[rtl] tx slot %d stuck (cmd=0x%08x)\n", slot, d->cmd);
            return -1;
        }
    }
    memcpy(g_tx_buf[slot], buf, len);
    if (actual > len) memset(g_tx_buf[slot] + len, 0, actual - len);
    uint32_t flags = RTL_DESC_OWN | RTL_DESC_FS | RTL_DESC_LS |
                     ((uint32_t)actual & 0x3FFF);
    if (slot == RTL_NUM_TX_DESC - 1) flags |= RTL_DESC_EOR;
    d->cmd = flags;
    /* Kick the TX engine via TPPOLL (Normal-Priority TX). */
    rtl_w8(RTL_REG_TPPOLL, 0x40);
    g_tx_cur = (slot + 1) % RTL_NUM_TX_DESC;
    return 0;
}

void rtl_poll(void) {
    if (!g_present) return;
    while ((g_rx_descs[g_rx_cur].cmd & RTL_DESC_OWN) == 0) {
        rtl_desc_t *d = &g_rx_descs[g_rx_cur];
        uint16_t len = (uint16_t)(d->cmd & 0x3FFF);
        /* Strip the 4-byte FCS that the hardware leaves on the end. */
        if (len > 4) len -= 4;
        if (g_rx_cb && len > 0 && len <= RTL_BUF_SIZE) {
            g_rx_cb(g_rx_buf[g_rx_cur], len, g_rx_user);
        }
        /* Recycle the descriptor. */
        uint32_t flags = RTL_DESC_OWN | (RTL_BUF_SIZE & 0x3FFF);
        if (g_rx_cur == RTL_NUM_RX_DESC - 1) flags |= RTL_DESC_EOR;
        d->cmd = flags;
        g_rx_cur = (g_rx_cur + 1) % RTL_NUM_RX_DESC;
    }
}

static void rtl_poll_vt(void) { rtl_poll(); }

static void rtl_rxcb_vt(netif_rx_cb_t cb, void *u) {
    g_rx_cb = cb; g_rx_user = u;
}

const netif_driver_t rtl_driver = {
    .name = "Realtek RTL8169/RTL8111 Gigabit",
    .probe = rtl_probe,
    .init  = rtl_init,
    .mac   = rtl_mac_vt,
    .send  = rtl_send_vt,
    .poll  = rtl_poll_vt,
    .set_rx_cb = rtl_rxcb_vt,
};

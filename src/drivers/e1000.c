/* ============================================================================
 * NexxoN OS - Intel 82540EM (E1000) NIC driver
 * ----------------------------------------------------------------------------
 * QEMU's default emulated NIC.  The MMIO register set follows Intel's 8254x
 * GbE family manual; we only touch the subset needed for unidirectional
 * Ethernet I/O.  No hardware checksum offload, no jumbo frames, no PHY
 * autoneg fiddling - just enough to push ARP / IPv4 / ICMP / TCP frames.
 *
 * Memory layout (BSS, hard-aligned):
 *
 *   g_rx_descs[16]   1024 bytes  (each desc is 16 bytes)
 *   g_tx_descs[16]   1024 bytes
 *   g_rx_buf [16][2048]  32 KiB  payload buffers, one per descriptor
 *   g_tx_buf [16][2048]  32 KiB
 *
 * The driver is a single-threaded design: send() blocks until the head/tail
 * indicators show the slot has been processed.  Receive uses a head/tail
 * cursor walk driven from e1000_poll() (called from the net tick / IRQ).
 * ============================================================================ */
#include "e1000.h"
#include "netif.h"
#include "pci.h"
#include "io.h"
#include "irq.h"
#include "isr.h"
#include "string.h"
#include "debug.h"
#include "pit.h"
#include "pic.h"

/* --- Subset of the E1000 register file we care about ------------------- */
#define E1000_REG_CTRL     0x0000
#define E1000_REG_STATUS   0x0008
#define E1000_REG_EERD     0x0014
#define E1000_REG_ICR      0x00C0
#define E1000_REG_IMS      0x00D0
#define E1000_REG_IMC      0x00D8
#define E1000_REG_RCTL     0x0100
#define E1000_REG_TCTL     0x0400
#define E1000_REG_TIPG     0x0410
#define E1000_REG_RDBAL    0x2800
#define E1000_REG_RDBAH    0x2804
#define E1000_REG_RDLEN    0x2808
#define E1000_REG_RDH      0x2810
#define E1000_REG_RDT      0x2818
#define E1000_REG_TDBAL    0x3800
#define E1000_REG_TDBAH    0x3804
#define E1000_REG_TDLEN    0x3808
#define E1000_REG_TDH      0x3810
#define E1000_REG_TDT      0x3818
#define E1000_REG_MTA      0x5200
#define E1000_REG_RAL0     0x5400
#define E1000_REG_RAH0     0x5404

/* Extra control + flow-control + RX-tuning registers (e1000e hardening). */
#define E1000_REG_CTRL_EXT 0x0018
#define E1000_REG_FCAL     0x0028
#define E1000_REG_FCAH     0x002C
#define E1000_REG_FCT      0x0030
#define E1000_REG_FCTTV    0x0170
#define E1000_REG_PBA      0x1000
#define E1000_REG_RDTR     0x2820
#define E1000_REG_RXDCTL   0x2828
#define E1000_REG_RADV     0x282C
#define E1000_REG_TXDCTL   0x3828
#define E1000_REG_TARC0    0x3840
#define E1000_REG_RFCTL    0x5008

/* MAC statistics block (clear-on-read).  These localise an RX failure that the
 * ring head/tail alone can't: TPR counts every frame the MAC pulled off the
 * wire, GPRC the good ones, MPC frames dropped for want of RX FIFO/descriptor,
 * RNBC "no receive buffers", CRCERRS bad-CRC frames (cable/PHY). */
#define E1000_REG_CRCERRS  0x4000
#define E1000_REG_RXERRC   0x400C
#define E1000_REG_MPC      0x4010
#define E1000_REG_RLEC     0x4040
#define E1000_REG_GPRC     0x4074
#define E1000_REG_BPRC     0x4078
#define E1000_REG_MPRC     0x407C
#define E1000_REG_GPTC     0x4080
#define E1000_REG_RNBC     0x40A0
#define E1000_REG_TPR      0x40D0
#define E1000_REG_TPT      0x40D4

/* CTRL flow-control enable bits. */
#define E1000_CTRL_RFCE    (1u << 27)
#define E1000_CTRL_TFCE    (1u << 28)

#define E1000_CTRL_RST     (1u << 26)
#define E1000_CTRL_ASDE    (1u << 5)
#define E1000_CTRL_SLU     (1u << 6)

#define E1000_RCTL_EN      (1u <<  1)
#define E1000_RCTL_SBP     (1u <<  2)
#define E1000_RCTL_UPE     (1u <<  3)   /* unicast promiscuous              */
#define E1000_RCTL_MPE     (1u <<  4)   /* multicast promiscuous            */
#define E1000_RCTL_LPE     (1u <<  5)
#define E1000_RCTL_BAM     (1u << 15)
#define E1000_RCTL_BSIZE_2048   (0u << 16)
#define E1000_RCTL_SECRC   (1u << 26)

#define E1000_TCTL_EN      (1u <<  1)
#define E1000_TCTL_PSP     (1u <<  3)
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12
#define E1000_TCTL_RTLC    (1u << 24)   /* re-transmit on late collision */

#define E1000_TXD_CMD_EOP  (1u <<  0)
#define E1000_TXD_CMD_IFCS (1u <<  1)
#define E1000_TXD_CMD_RS   (1u <<  3)
#define E1000_TXD_STAT_DD  (1u <<  0)

#define E1000_RXD_STAT_DD  (1u <<  0)
#define E1000_RXD_STAT_EOP (1u <<  1)

#define E1000_IMS_RXT0     (1u <<  7)
#define E1000_IMS_LSC      (1u <<  2)

typedef struct PACKED {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct PACKED {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  sta;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

ALIGNED(16) static e1000_rx_desc_t g_rx_descs[E1000_RX_DESCS];
ALIGNED(16) static e1000_tx_desc_t g_tx_descs[E1000_TX_DESCS];
ALIGNED(16) static uint8_t g_rx_buf[E1000_RX_DESCS][E1000_RX_BUF_SIZE];
ALIGNED(16) static uint8_t g_tx_buf[E1000_TX_DESCS][E1000_TX_BUF_SIZE];

static volatile uint8_t *g_mmio = NULL;
static uint8_t  g_mac[6] = { 0 };
static int      g_rx_cur = 0;
static int      g_tx_cur = 0;
static uint32_t g_tx_posted = 0;   /* frames handed to the TX ring (diag) */
static bool     g_present = false;
static e1000_rx_cb_t g_rx_cb = NULL;
static void         *g_rx_user = NULL;
static e1000_family_t g_family = E1000_FAMILY_LEGACY;
static const char    *g_dev_name = "?";       /* matched chip name (diag) */
static uint16_t       g_dev_id   = 0;          /* matched PCI device id    */

/* PCI device IDs the driver claims.  Adding support for a new chip is
 * one row in this table plus, if the family extends past the existing
 * PCH path, a small family-specific branch in the init code. */
typedef struct {
    uint16_t        device_id;
    e1000_family_t  family;
    const char     *name;
} e1000_devid_t;

static const e1000_devid_t g_e1000_devids[] = {
    /* Legacy / QEMU. */
    { 0x100E, E1000_FAMILY_LEGACY, "Intel 82540EM"                  },
    { 0x1004, E1000_FAMILY_LEGACY, "Intel 82543GC"                  },
    { 0x100F, E1000_FAMILY_LEGACY, "Intel 82545EM"                  },
    { 0x10D3, E1000_FAMILY_LEGACY, "Intel 82574L"                   },
    /* ICH8 / ICH9 / ICH10 PCH-bound desktop GbE. */
    { 0x10BD, E1000_FAMILY_ICH,    "Intel 82566DM (ICH8)"           },
    { 0x10C0, E1000_FAMILY_ICH,    "Intel 82562V-2 (ICH8)"          },
    { 0x10C4, E1000_FAMILY_ICH,    "Intel 82562GT-2 (ICH8)"         },
    { 0x10C5, E1000_FAMILY_ICH,    "Intel 82562G-2 (ICH8)"          },
    { 0x10CB, E1000_FAMILY_ICH,    "Intel 82567LM-3 (ICH9/ICH10)"   },
    { 0x10CE, E1000_FAMILY_ICH,    "Intel 82567V-3 (ICH9/ICH10)"    },
    { 0x10F5, E1000_FAMILY_ICH,    "Intel 82567LM (ICH9)"           },
    /* Sandy / Ivy Bridge PCH (82577 / 82579) - what the user
     * explicitly asked for. */
    { 0x10EA, E1000_FAMILY_PCH,    "Intel 82577LM"                  },
    { 0x10EB, E1000_FAMILY_PCH,    "Intel 82577LC"                  },
    { 0x1502, E1000_FAMILY_PCH,    "Intel 82579LM"                  },
    { 0x1503, E1000_FAMILY_PCH,    "Intel 82579V"                   },
    /* Lynx Point / Haswell PCH (I217 / I218). */
    { 0x153A, E1000_FAMILY_PCH,    "Intel I217-LM"                  },
    { 0x153B, E1000_FAMILY_PCH,    "Intel I217-V"                   },
    { 0x155A, E1000_FAMILY_PCH,    "Intel I218-LM"                  },
    { 0x1559, E1000_FAMILY_PCH,    "Intel I218-V"                   },
};
#define E1000_DEVID_COUNT  ((int)(sizeof(g_e1000_devids) / sizeof(g_e1000_devids[0])))

/* Look up a PCI device ID in the table.  Returns NULL if no match. */
static const e1000_devid_t *e1000_match_devid(uint16_t did) {
    for (int i = 0; i < E1000_DEVID_COUNT; i++) {
        if (g_e1000_devids[i].device_id == did) return &g_e1000_devids[i];
    }
    return NULL;
}

static inline uint32_t e1000_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_mmio + off);
}
static inline void e1000_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_mmio + off) = v;
}

/* Read 16 bits out of the EEPROM (used for the MAC address). */
static uint16_t e1000_eerd(uint8_t addr) {
    e1000_w32(E1000_REG_EERD, 1u | ((uint32_t)addr << 8));
    for (int i = 0; i < 10000; i++) {
        uint32_t v = e1000_r32(E1000_REG_EERD);
        if (v & (1u << 4)) return (uint16_t)(v >> 16);
    }
    return 0xFFFFu;
}

/* A MAC is unusable if it isn't a unicast address, or is all-zero / all-FF, or
 * is an unprogrammed NVM pattern (the 82579V on the user's board reads back
 * 88:88:88:88:87:88 — five identical bytes — with AV set).  DHCP can't complete
 * with such a bogus source address. */
static bool e1000_mac_is_bogus(const uint8_t *m) {
    if (m[0] & 0x01) return true;                  /* multicast/broadcast bit */
    int allzero = 1, allff = 1, maxrun = 0;
    for (int i = 0; i < 6; i++) {
        if (m[i]) allzero = 0;
        if (m[i] != 0xFF) allff = 0;
        int c = 0;
        for (int j = 0; j < 6; j++) if (m[j] == m[i]) c++;
        if (c > maxrun) maxrun = c;
    }
    return allzero || allff || maxrun >= 5;        /* >=5 equal bytes => junk */
}

static void e1000_read_mac(void) {
    /* RAL0/RAH0 hold the station MAC.  RAH bit31 = Address Valid (AV): on
     * PCH/e1000e parts (82579 etc.) the hardware auto-loads these from the
     * NVM, and only when AV is set is the value trustworthy — reading too
     * early (or on a part whose NVM didn't load) yields an unprogrammed
     * pattern (e.g. 88:88:88:88:87:88).  Require AV before trusting RAL/RAH;
     * otherwise fall back to the EEPROM read. */
    uint32_t ral = e1000_r32(E1000_REG_RAL0);
    uint32_t rah = e1000_r32(E1000_REG_RAH0);
    if ((rah & (1u << 31)) && (ral || (rah & 0xFFFFu))) {
        g_mac[0] = (uint8_t)(ral & 0xFF);
        g_mac[1] = (uint8_t)((ral >> 8) & 0xFF);
        g_mac[2] = (uint8_t)((ral >> 16) & 0xFF);
        g_mac[3] = (uint8_t)((ral >> 24) & 0xFF);
        g_mac[4] = (uint8_t)(rah & 0xFF);
        g_mac[5] = (uint8_t)((rah >> 8) & 0xFF);
    } else {
        uint16_t w0 = e1000_eerd(0);
        uint16_t w1 = e1000_eerd(1);
        uint16_t w2 = e1000_eerd(2);
        g_mac[0] = (uint8_t)(w0 & 0xFF);
        g_mac[1] = (uint8_t)(w0 >> 8);
        g_mac[2] = (uint8_t)(w1 & 0xFF);
        g_mac[3] = (uint8_t)(w1 >> 8);
        g_mac[4] = (uint8_t)(w2 & 0xFF);
        g_mac[5] = (uint8_t)(w2 >> 8);
    }

    /* If the part handed us a bogus address (e.g. the 82579V's unprogrammed
     * 88:88:88:88:87:88), synthesise a stable locally-administered unicast MAC
     * and program it into the Receive Address registers so both our TX source
     * address and the HW RX filter use it.  Without a real MAC DHCP never
     * completes.  The low bytes are derived from the MMIO base so two boards
     * don't collide, and it's stable across reboots on a given machine. */
    if (e1000_mac_is_bogus(g_mac)) {
        uint32_t seed = (uint32_t)(uintptr_t)g_mmio;
        g_mac[0] = 0x02;                 /* locally administered, unicast */
        g_mac[1] = 0x4E;                 /* 'N' */
        g_mac[2] = 0x58;                 /* 'X' */
        g_mac[3] = (uint8_t)(seed >> 16);
        g_mac[4] = (uint8_t)(seed >> 8);
        g_mac[5] = (uint8_t)(seed ^ 0x5A);
        uint32_t nral = (uint32_t)g_mac[0] | ((uint32_t)g_mac[1] << 8)
                      | ((uint32_t)g_mac[2] << 16) | ((uint32_t)g_mac[3] << 24);
        uint32_t nrah = (uint32_t)g_mac[4] | ((uint32_t)g_mac[5] << 8)
                      | (1u << 31);       /* Address Valid */
        e1000_w32(E1000_REG_RAL0, nral);
        e1000_w32(E1000_REG_RAH0, nrah);
        debug_printf("[e1000] bogus NVM MAC -> using %02x:%02x:%02x:%02x:%02x:%02x\n",
                     g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    }
}

static void e1000_init_rx(void) {
    memset(g_rx_descs, 0, sizeof(g_rx_descs));
    for (int i = 0; i < E1000_RX_DESCS; i++) {
        g_rx_descs[i].addr   = (uint64_t)(uintptr_t)g_rx_buf[i];
        g_rx_descs[i].status = 0;
    }
    e1000_w32(E1000_REG_RDBAL, (uint32_t)(uintptr_t)g_rx_descs);
    e1000_w32(E1000_REG_RDBAH, 0);
    e1000_w32(E1000_REG_RDLEN, sizeof(g_rx_descs));
    e1000_w32(E1000_REG_RDH, 0);
    e1000_w32(E1000_REG_RDT, E1000_RX_DESCS - 1);

    /* Clear the multicast table array (128 x 32-bit). */
    for (int i = 0; i < 128; i++) {
        e1000_w32(E1000_REG_MTA + i * 4, 0);
    }

    e1000_w32(E1000_REG_RCTL,
              E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 |
              E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_SECRC);
    g_rx_cur = 0;
}

static void e1000_init_tx(void) {
    memset(g_tx_descs, 0, sizeof(g_tx_descs));
    for (int i = 0; i < E1000_TX_DESCS; i++) {
        g_tx_descs[i].addr = (uint64_t)(uintptr_t)g_tx_buf[i];
        g_tx_descs[i].sta  = E1000_TXD_STAT_DD;
    }
    e1000_w32(E1000_REG_TDBAL, (uint32_t)(uintptr_t)g_tx_descs);
    e1000_w32(E1000_REG_TDBAH, 0);
    e1000_w32(E1000_REG_TDLEN, sizeof(g_tx_descs));
    e1000_w32(E1000_REG_TDH, 0);
    e1000_w32(E1000_REG_TDT, 0);
    /* TCTL.  Legacy (QEMU 82540) keeps the proven value untouched.  e1000e/PCH
     * parts additionally get RTLC (re-transmit on late collision) — the OSDev
     * i217/82577LM write-up flags the TCTL value as one of the three things that
     * had to change to make those e1000e NICs transmit.  This is a post-reset
     * register write (same kind init_tx already does), so it can't reproduce the
     * reset-window MMIO-read hang. */
    uint32_t tctl;
    if (g_family == E1000_FAMILY_LEGACY) {
        tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
               (0x10 << E1000_TCTL_CT_SHIFT) |
               (0x40 << E1000_TCTL_COLD_SHIFT);          /* unchanged, proven */
    } else {
        tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
               (0x0F << E1000_TCTL_CT_SHIFT) |
               (0x40 << E1000_TCTL_COLD_SHIFT) |
               E1000_TCTL_RTLC;                           /* OSDev e1000e value */
    }
    e1000_w32(E1000_REG_TCTL, tctl);
    e1000_w32(E1000_REG_TIPG, 0x0060200A);
    g_tx_cur = 0;
}

static void e1000_irq_isr(registers_t *r) {
    (void)r;
    if (!g_present) return;
    /* Read+clear interrupt cause. */
    uint32_t icr = e1000_r32(E1000_REG_ICR);
    (void)icr;
    e1000_poll();
}

void e1000_irq_handler(void) { e1000_poll(); }

void e1000_set_rx_cb(e1000_rx_cb_t cb, void *user) {
    g_rx_cb = cb;
    g_rx_user = user;
}

bool e1000_init(void) {
    debug_step("e1000(e): searching for any known Intel GbE / e1000e device");
    pci_device_t devs[64];
    int n = pci_enumerate(devs, 64);
    pci_device_t *match = NULL;
    const e1000_devid_t *match_id = NULL;
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id != E1000_VENDOR_ID) continue;
        const e1000_devid_t *id = e1000_match_devid(devs[i].device_id);
        if (id) {
            match    = &devs[i];
            match_id = id;
            break;
        }
    }
    if (!match) {
        debug_printf("[e1000] no supported 8086:* GbE device on PCI bus\n");
        return false;
    }
    g_family   = match_id->family;
    g_dev_name = match_id->name;
    g_dev_id   = match->device_id;
    debug_printf("[e1000] %s (%04x:%04x) at %02x:%02x.%x  BAR0=0x%08x family=%s\n",
                 match_id->name, match->vendor_id, match->device_id,
                 match->bus, match->device, match->function, match->bar[0],
                 g_family == E1000_FAMILY_PCH ? "PCH(e1000e)" :
                 g_family == E1000_FAMILY_ICH ? "ICH"        : "Legacy");
    uint32_t bar0 = match->bar[0] & 0xFFFFFFF0u;
    if (!bar0) {
        debug_printf("[e1000] BAR0 unassigned\n");
        return false;
    }
    pci_enable_busmaster(match);
    pci_refresh_bars(match);
    g_mmio = (volatile uint8_t *)(uintptr_t)(match->bar[0] & 0xFFFFFFF0u);

    /* Issue a software reset and let the controller settle.  PCH-bound
     * e1000e devices need a longer settle window after the reset bit
     * clears - the integrated PHY's auto-negotiation phase takes a
     * few extra millis to come online.
     *
     * IMPORTANT (bare-metal): do NOT read back any MMIO register inside the
     * reset window.  On the real 82579V a CPU MMIO read to the controller while
     * it is mid-reset never completes and HANGS the boot (it works fine under
     * QEMU, which always completes the read — which is exactly why an earlier
     * "poll CTRL.RST until it self-clears" change passed in emulation but froze
     * the desktop on hardware).  A blind, generous delay is the safe approach. */
    e1000_w32(E1000_REG_CTRL, e1000_r32(E1000_REG_CTRL) | E1000_CTRL_RST);
    pit_sleep(g_family == E1000_FAMILY_PCH ? 40 : 20);

    /* PCH-bound NICs additionally need a Setup Link (CTRL.SLU) + GIO
     * Master Enable (CTRL.GIO_MASTER_EN equivalent) sequence after the
     * software reset to bring the PHY out of low-power mode.  We
     * include those bits in the family branch.  Legacy 8254x families
     * just need ASDE+SLU. */
    uint32_t ctrl = e1000_r32(E1000_REG_CTRL);
    ctrl |= E1000_CTRL_ASDE | E1000_CTRL_SLU;
    ctrl &= ~E1000_CTRL_RST;
    /* On PCH/ICH parts, clear the LRST bit (link reset, bit 3) so the
     * MAC-to-PHY interface comes out of reset and clear PHY_RST
     * (bit 31) so the integrated PHY can start auto-negotiation. */
    if (g_family != E1000_FAMILY_LEGACY) {
        ctrl &= ~(1u << 3);   /* LRST */
        ctrl &= ~(1u << 31);  /* PHY_RST */
    }
    e1000_w32(E1000_REG_CTRL, ctrl);

    /* Some 82577/82579 boards leave the GbE LAN in PCI-D3 power state
     * after the host wakes up; bumping the PMCSR D0 bit forces the
     * controller into full-power so the MAC registers respond.  PMCSR
     * sits inside the PCI capabilities list - we walk it lazily and
     * silently skip if the cap isn't there. */
    if (g_family != E1000_FAMILY_LEGACY) {
        uint32_t status_cmd = pci_read32(match->bus, match->device,
                                         match->function, 0x04);
        if (status_cmd & (1u << 20)) {
            uint8_t cap = (uint8_t)(pci_read32(match->bus, match->device,
                                              match->function, 0x34) & 0xFF);
            int hops = 0;
            while (cap && hops++ < 12) {
                uint32_t entry = pci_read32(match->bus, match->device,
                                            match->function, cap & 0xFC);
                uint8_t cap_id = (uint8_t)(entry & 0xFF);
                if (cap_id == 0x01) {   /* Power Management */
                    uint32_t pmcsr = pci_read32(match->bus, match->device,
                                                match->function,
                                                (uint8_t)(cap + 4));
                    if ((pmcsr & 0x03) != 0) {
                        pmcsr = (pmcsr & ~0x03u);  /* force D0 */
                        pci_write32(match->bus, match->device,
                                    match->function, (uint8_t)(cap + 4),
                                    pmcsr);
                        debug_printf("[e1000] PCH PHY: kicked PMCSR to D0\n");
                        pit_sleep(10);
                    }
                    break;
                }
                cap = (uint8_t)((entry >> 8) & 0xFF);
            }
        }
    }

    e1000_read_mac();
    debug_printf("[e1000] MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
                 g_mac[0], g_mac[1], g_mac[2],
                 g_mac[3], g_mac[4], g_mac[5]);

    /* Mask all interrupts before configuring the rings to avoid spurious
     * fires during the initialisation window. */
    e1000_w32(E1000_REG_IMC, 0xFFFFFFFFu);
    (void)e1000_r32(E1000_REG_ICR);

    e1000_init_rx();
    e1000_init_tx();

    /* Re-enable the interrupts we care about. */
    e1000_w32(E1000_REG_IMS, E1000_IMS_RXT0 | E1000_IMS_LSC);

    /* PCI Interrupt Line (config offset 0x3C, low byte) tells us which
     * IRQ the BIOS routed us to.  QEMU's e1000 commonly lands on IRQ
     * 11.  Hook the handler unconditionally for that range; the IRQ
     * line might be 0 on certain hypervisors which we treat as "polling
     * only". */
    uint32_t intline = pci_read32(match->bus, match->device, match->function,
                                  0x3C) & 0xFFu;
    if (intline > 0 && intline < 16) {
        irq_install_handler((int)intline, e1000_irq_isr);
        pic_unmask((uint8_t)intline);
        debug_printf("[e1000] IRQ%d hooked + unmasked\n", intline);
    } else {
        debug_printf("[e1000] no usable IRQ line (intline=%u) - polling only\n",
                     intline);
    }

    g_present = true;
    debug_printf("[e1000] %s ready (%d Rx / %d Tx desc, family=%s)\n",
                 g_dev_name, E1000_RX_DESCS, E1000_TX_DESCS,
                 g_family == E1000_FAMILY_PCH ? "PCH(e1000e)" :
                 g_family == E1000_FAMILY_ICH ? "ICH" : "Legacy");
    debug_ok("e1000: NIC ready");
    return true;
}

bool e1000_present(void) { return g_present; }

/* ---------------------------------------------------------------------------
 * Brutal NIC diagnostic — surfaced by `ifconfig -v`.  On bare metal a NIC that
 * "should work" but doesn't (e.g. the 82579V on the ASUS P8Z77) needs the real
 * register state to debug, not guesses.  Mirrors the usbnative dump approach:
 * one screenful tells us link status, ring head/tail movement and RX/TX
 * activity so the next step is data-driven.  Returns bytes written.
 * ------------------------------------------------------------------------- */
int e1000_diag(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    if (!g_present || !g_mmio)
        return ksnprintf(out, cap, "e1000: not initialised (present=%d)\n", g_present);

    uint32_t ctrl = e1000_r32(E1000_REG_CTRL);
    uint32_t cext = e1000_r32(E1000_REG_CTRL_EXT);
    uint32_t sts  = e1000_r32(E1000_REG_STATUS);
    uint32_t rctl = e1000_r32(E1000_REG_RCTL);
    uint32_t tctl = e1000_r32(E1000_REG_TCTL);
    uint32_t icr  = e1000_r32(E1000_REG_ICR);
    uint32_t rdh  = e1000_r32(E1000_REG_RDH), rdt = e1000_r32(E1000_REG_RDT);
    uint32_t tdh  = e1000_r32(E1000_REG_TDH), tdt = e1000_r32(E1000_REG_TDT);
    uint32_t rdbal = e1000_r32(E1000_REG_RDBAL), rdlen = e1000_r32(E1000_REG_RDLEN);
    uint32_t rxdctl = e1000_r32(E1000_REG_RXDCTL), pba = e1000_r32(E1000_REG_PBA);
    uint32_t rfctl  = e1000_r32(E1000_REG_RFCTL);
    uint32_t tdbal  = e1000_r32(E1000_REG_TDBAL), tdlen = e1000_r32(E1000_REG_TDLEN);
    uint32_t txdctl = e1000_r32(E1000_REG_TXDCTL), tarc0 = e1000_r32(E1000_REG_TARC0);
    uint32_t tipg   = e1000_r32(E1000_REG_TIPG);

    /* MAC statistics (clear-on-read): read ONCE each.  These are the smoking
     * gun for a dead RX — TPR/GPRC say whether the MAC saw any frame at all,
     * MPC/RNBC whether it saw frames but had nowhere to put them, CRCERRS
     * whether the wire/PHY is delivering garbage. */
    uint32_t tpr = e1000_r32(E1000_REG_TPR),   gprc = e1000_r32(E1000_REG_GPRC);
    uint32_t bprc = e1000_r32(E1000_REG_BPRC), mprc = e1000_r32(E1000_REG_MPRC);
    uint32_t mpc = e1000_r32(E1000_REG_MPC),   rnbc = e1000_r32(E1000_REG_RNBC);
    uint32_t crcerrs = e1000_r32(E1000_REG_CRCERRS), rxerrc = e1000_r32(E1000_REG_RXERRC);
    uint32_t rlec = e1000_r32(E1000_REG_RLEC);
    uint32_t tpt = e1000_r32(E1000_REG_TPT),   gptc = e1000_r32(E1000_REG_GPTC);

    /* The MAC stat registers are CLEAR-ON-READ: each read returns the count
     * since the previous read and resets to 0.  Reading them here (and nowhere
     * else) means a second `ifconfig -v` right after the first shows ~0 — which
     * looked like "two different reports".  Accumulate into static totals so the
     * numbers only ever grow and reflect activity since boot. */
    static uint32_t a_tpr=0, a_gprc=0, a_bprc=0, a_mprc=0, a_mpc=0, a_rnbc=0;
    static uint32_t a_crc=0, a_rxerr=0, a_rlec=0, a_tpt=0, a_gptc=0;
    a_tpr+=tpr; a_gprc+=gprc; a_bprc+=bprc; a_mprc+=mprc; a_mpc+=mpc; a_rnbc+=rnbc;
    a_crc+=crcerrs; a_rxerr+=rxerrc; a_rlec+=rlec; a_tpt+=tpt; a_gptc+=gptc;

    /* STATUS: bit1 = Link Up, bits7:6 = speed (00=10,01=100,10=1000). */
    int lu = (sts >> 1) & 1;
    uint32_t spd_sel = (sts >> 6) & 3;
    const char *spd = (spd_sel == 0) ? "10M" : (spd_sel == 1) ? "100M" : "1000M";
    const char *fam = (g_family == E1000_FAMILY_PCH) ? "PCH(e1000e)"
                    : (g_family == E1000_FAMILY_ICH) ? "ICH" : "Legacy";

    /* How many RX descriptors has the HW written back (DD set)? */
    int dd = 0;
    for (int i = 0; i < E1000_RX_DESCS; i++)
        if (g_rx_descs[i].status & E1000_RXD_STAT_DD) dd++;

    size_t n = 0;
    n += ksnprintf(out + n, cap - n,
        "NIC %s (8086:%04x) family=%s\n", g_dev_name, g_dev_id, fam);
    n += ksnprintf(out + n, cap - n,
        "  CTRL=%08x CTRL_EXT=%08x STATUS=%08x  LINK=%s spd=%s FD=%d\n",
        ctrl, cext, sts, lu ? "UP" : "DOWN", spd, (sts & 1) ? 1 : 0);
    n += ksnprintf(out + n, cap - n,
        "  RCTL=%08x (RXen=%d) TCTL=%08x (TXen=%d) RFCTL=%08x ICR=%08x\n",
        rctl, (rctl >> 1) & 1, tctl, (tctl >> 1) & 1, rfctl, icr);
    n += ksnprintf(out + n, cap - n,
        "  RX ring: RDBA=%08x LEN=%u RDH=%u RDT=%u cur=%d DD=%d RXDCTL=%08x PBA=%08x\n",
        rdbal, rdlen, rdh, rdt, g_rx_cur, dd, rxdctl, pba);
    /* Last descriptor we handed the TX engine + its written-back status byte
     * (DD=done, EC=excess-collisions, LC=late-collision, TU=transmit-underrun).
     * This is the key to "queued but TPT=0": it says WHY the frame didn't go. */
    int txlast = (g_tx_cur + E1000_TX_DESCS - 1) % E1000_TX_DESCS;
    uint8_t txsta = g_tx_descs[txlast].sta;
    n += ksnprintf(out + n, cap - n,
        "  TX ring: TDBA=%08x LEN=%u TDH=%u TDT=%u cur=%d posted=%u\n",
        tdbal, tdlen, tdh, tdt, g_tx_cur, g_tx_posted);
    n += ksnprintf(out + n, cap - n,
        "  TX eng : TCTL=%08x TXDCTL=%08x TARC0=%08x TIPG=%08x  TX[%d].sta=%02x"
        " (%s%s%s%s)\n",
        tctl, txdctl, tarc0, tipg, txlast, txsta,
        (txsta & E1000_TXD_STAT_DD) ? "DD"  : "--",
        (txsta & (1u<<1))           ? ",EC" : "",
        (txsta & (1u<<2))           ? ",LC" : "",
        (txsta & (1u<<3))           ? ",TU" : "");
    uint32_t ral = e1000_r32(E1000_REG_RAL0), rah = e1000_r32(E1000_REG_RAH0);
    n += ksnprintf(out + n, cap - n,
        "  MAC %02x:%02x:%02x:%02x:%02x:%02x  RAL=%08x RAH=%08x AV=%d\n",
        g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5],
        ral, rah, (rah >> 31) & 1);
    n += ksnprintf(out + n, cap - n,
        "  RX stat: TPR=%u GPRC=%u BPRC=%u MPRC=%u MPC=%u RNBC=%u CRCERR=%u "
        "RXERR=%u RLEC=%u\n",
        a_tpr, a_gprc, a_bprc, a_mprc, a_mpc, a_rnbc, a_crc, a_rxerr, a_rlec);
    n += ksnprintf(out + n, cap - n,
        "  TX stat: TPT=%u GPTC=%u   (totals since boot)\n", a_tpt, a_gptc);

    /* Data-driven interpretation, reported separately for TX and RX and keyed
     * off the cumulative totals (NOT the instantaneous DD count — e1000_poll()
     * clears DD as it consumes packets, so DD=0 is normal on a working NIC). */
    if (!lu) {
        n += ksnprintf(out + n, cap - n,
            "  -> LINK DOWN: no DHCP/ping possible. Check cable + switch; on PCH\n"
            "     parts the MAC<->PHY link must come up (CTRL.SLU + PHY init).\n");
        return (int)n;
    }
    /* TX verdict first — DHCP can't even start without a working transmit. */
    if (g_tx_posted > 0 && a_tpt == 0) {
        n += ksnprintf(out + n, cap - n,
            "  -> TX DEAD: %u frame(s) posted but TPT=0 — the MAC took the\n"
            "     descriptor (TX[%d].sta=%02x) yet put nothing on the wire, so the\n"
            "     DHCP DISCOVER never leaves. MAC/PHY transmit gating (the 82579\n"
            "     K1/EEE/LPI class of issue), not a descriptor/DMA bug.\n",
            g_tx_posted, txlast, txsta);
    } else if (a_tpt > 0) {
        n += ksnprintf(out + n, cap - n,
            "  -> TX OK: %u packet(s) transmitted (GPTC=%u).\n", a_tpt, a_gptc);
    } else {
        n += ksnprintf(out + n, cap - n,
            "  -> TX idle: nothing posted yet — run `dhcp` then re-check.\n");
    }
    /* RX verdict. */
    if (a_tpr == 0 && a_gprc == 0) {
        n += ksnprintf(out + n, cap - n,
            "  -> RX: MAC received 0 frames (TPR=0) — receiver/PHY-MAC gated.\n");
    } else if (a_gprc == 0 && (a_mpc > 0 || a_rnbc > 0)) {
        n += ksnprintf(out + n, cap - n,
            "  -> RX: frames seen (TPR=%u) but dropped (MPC=%u RNBC=%u) — FIFO/\n"
            "     descriptor DMA.\n", a_tpr, a_mpc, a_rnbc);
    } else if (a_gprc > 0) {
        n += ksnprintf(out + n, cap - n,
            "  -> RX OK: %u good frame(s) received.\n", a_gprc);
    }
    return (int)n;
}

void e1000_mac(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = g_mac[i];
}

int e1000_send(const void *buf, uint16_t len) {
    if (!g_present || !buf || len == 0) return -1;
    if (len > E1000_TX_BUF_SIZE) return -1;
    int slot = g_tx_cur;
    e1000_tx_desc_t *d = &g_tx_descs[slot];
    /* Wait until previous use of this slot has been retired by the HW. */
    for (int spin = 0; spin < 1000000; spin++) {
        if (d->sta & E1000_TXD_STAT_DD) break;
    }
    memcpy(g_tx_buf[slot], buf, len);
    d->length = len;
    d->cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    d->sta    = 0;
    g_tx_cur = (slot + 1) % E1000_TX_DESCS;
    g_tx_posted++;
    e1000_w32(E1000_REG_TDT, g_tx_cur);
    return 0;
}

void e1000_poll(void) {
    if (!g_present) return;
    while (g_rx_descs[g_rx_cur].status & E1000_RXD_STAT_DD) {
        e1000_rx_desc_t *d = &g_rx_descs[g_rx_cur];
        uint16_t len = d->length;
        if (g_rx_cb && len > 0 && len < E1000_RX_BUF_SIZE) {
            g_rx_cb(g_rx_buf[g_rx_cur], len, g_rx_user);
        }
        d->status = 0;
        /* Advance the tail to recycle the descriptor. */
        uint32_t old = g_rx_cur;
        g_rx_cur = (g_rx_cur + 1) % E1000_RX_DESCS;
        e1000_w32(E1000_REG_RDT, old);
    }
    /* RX-hang self-heal (burst-overflow aftermath, also an Intel-silicon
     * errata pattern): the device claims "no free descriptors" (RDH==RDT)
     * while every descriptor we can see is consumed (no DD anywhere) —
     * the two views can never reconcile on their own and the NIC goes
     * permanently deaf (pcap: SYN-ACKs on the wire, none delivered).
     * Reprogram the RX ring; in-flight frames are lost and TCP/apps
     * retransmit.  Rate-limited to one reset per second. */
    if (!(g_rx_descs[g_rx_cur].status & E1000_RXD_STAT_DD)) {
        uint32_t rdh = e1000_r32(E1000_REG_RDH);
        uint32_t rdt = e1000_r32(E1000_REG_RDT);
        if (rdh == rdt) {
            bool any_dd = false;
            for (int i = 0; i < E1000_RX_DESCS && !any_dd; i++)
                any_dd = (g_rx_descs[i].status & E1000_RXD_STAT_DD) != 0;
            static uint32_t last_reset_ms;
            uint32_t now = pit_ms();
            if (!any_dd && (uint32_t)(now - last_reset_ms) >= 1000u) {
                last_reset_ms = now;
                debug_printf("[e1000] RX ring deadlock (RDH==RDT==%u, no DD)"
                             " — reprogramming RX ring\n", rdh);
                uint32_t rctl = e1000_r32(E1000_REG_RCTL);
                e1000_w32(E1000_REG_RCTL, rctl & ~E1000_RCTL_EN);
                e1000_init_rx();
            }
        }
    }
}

/* ---- Driver vtable registration with netif dispatcher --------------- */
static bool e1000_probe(void) {
    pci_device_t devs[64];
    int n = pci_enumerate(devs, 64);
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor_id != E1000_VENDOR_ID) continue;
        if (e1000_match_devid(devs[i].device_id) != NULL) return true;
    }
    return false;
}
static bool e1000_init_vt(void) { return e1000_init(); }
static void e1000_mac_vt(uint8_t out[6]) { e1000_mac(out); }
static int  e1000_send_vt(const void *f, uint16_t l) { return e1000_send(f, l); }
static void e1000_poll_vt(void) { e1000_poll(); }
static void e1000_rxcb_vt(netif_rx_cb_t cb, void *u) {
    e1000_set_rx_cb((e1000_rx_cb_t)cb, u);
}

const netif_driver_t e1000_driver = {
    .name = "Intel GbE (e1000 / e1000e)",
    .probe = e1000_probe,
    .init  = e1000_init_vt,
    .mac   = e1000_mac_vt,
    .send  = e1000_send_vt,
    .poll  = e1000_poll_vt,
    .set_rx_cb = e1000_rxcb_vt,
};

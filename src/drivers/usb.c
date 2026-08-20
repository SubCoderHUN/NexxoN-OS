/* ============================================================================
 * NexxoN OS - USB host stack (UHCI + EHCI) v2.0
 * ----------------------------------------------------------------------------
 * This is a complete rewrite of the v1.0 discovery-only stub.  The driver
 * now supports:
 *
 *   * Discovery of UHCI / OHCI / EHCI / xHCI controllers via the PCI bus.
 *   * BIOS legacy hand-off for UHCI (PCI config 0xC0) and EHCI (extended
 *     capability list, USBLEGSUP/USBLEGCTLSTS).
 *   * Root-hub reset, port enable, and live port-status polling.
 *   * Static (BSS-resident) DMA structures for UHCI's framelist + queue
 *     heads + transfer descriptors, and EHCI's async queue head + qTDs.
 *   * Synchronous SETUP / IN / OUT control + bulk transfers with PID
 *     toggle bookkeeping.
 *   * Polling-driven hot-plug detection: usb_poll() compares current
 *     PORTSC against the previous tick and re-runs enumeration on every
 *     0->1 transition.
 *   * Device enumeration: SET_ADDRESS, GET_DESCRIPTOR (device, config),
 *     SET_CONFIGURATION.  Endpoint walk picks the first bulk-IN /
 *     bulk-OUT pair for the MSC driver to consume.
 *
 * Limits we accept by design:
 *
 *   * Only the first endpoint pair on the first interface is parsed; this
 *     matches every USB mass-storage pendrive on the planet.
 *   * Low-speed devices are not enumerated through UHCI (we treat them as
 *     full-speed for the SETUP/DATA/STATUS phases).
 *   * EHCI companion controllers are not chained; if the EHCI port comes
 *     up empty we fall back to the UHCI driver on the same physical port
 *     (the UHCI controller is a separate PCI device).
 *
 * Every interesting state change logs to COM1 so bare-metal post-mortem
 * is doable when something refuses to enumerate.
 * ============================================================================ */
#include "usb.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "debug.h"
#include "pit.h"

/* ============================================================================
 * Controller table + device table
 * ============================================================================ */
#define USB_MAX_CTRL  4
/* Boot-time xHCI policy.  This gate only matters on the NON-legacy path:
 * when firmware USB-legacy is active (every CSM target board, incl. the
 * P8Z77) usb_init() preserves the BIOS input path and the xHCI is only
 * driven later by the fail-safe takeover — flipping this changes nothing
 * there.  With no firmware legacy in the way (QEMU, UEFI-ish boards) the
 * xHCI must come up at boot or USB simply does not exist on xHCI-only
 * machines: pendrives were invisible because the controller was never
 * started.  xhci_reset() failures are non-fatal (logged, controller
 * skipped), so a wedge-prone controller degrades to the old behaviour. */
#define USB_ENABLE_XHCI_EARLY 1
static usb_controller_t g_ctrls[USB_MAX_CTRL];
static int              g_n_ctrls = 0;

/* Bare-metal input-stability policy.  Set true when the firmware (BIOS SMM)
 * still owns a USB host controller for USB-legacy PS/2 emulation, i.e. the
 * boot keyboard/mouse only work because SMM is driving the controller.  In
 * that state NexxoN must NOT reset / take over the controller: doing so tears
 * the emulation out from under the firmware and leaves the desktop alive but
 * uncontrollable (the exact ASUS P8Z77 / Panther Point bare-metal symptom).
 * When true we leave the controllers untouched and rely on the PS/2 driver
 * consuming the firmware emulation. */
static bool             g_preserve_legacy = false;

/* One-line takeover diagnostic surfaced on-screen by `usbnative` (serial is
 * invisible on a real board): per-controller port connection/speed so a single
 * photo shows whether the keyboard/mouse are directly on a USB-3.0 port, behind
 * an external hub, or whether the controller didn't even reset. */
static char             g_usb_diag[400] = {0};
static void usb_diag_add(const char *fmt, ...) {
    size_t len = 0;
    while (len < sizeof(g_usb_diag) && g_usb_diag[len]) len++;
    if (len >= sizeof(g_usb_diag) - 1) return;
    va_list ap; va_start(ap, fmt);
    kvsnprintf(g_usb_diag + len, sizeof(g_usb_diag) - len, fmt, ap);
    va_end(ap);
}

static usb_device_t g_devs[USB_MAX_DEVICES];
static int          g_n_devs = 0;
static uint8_t      g_next_address = 1;

static usb_hotplug_cb_t g_hotplug = NULL;

/* ============================================================================
 * UHCI register definitions (USB Universal Host Controller, USB 1.1)
 * ============================================================================ */
#define UHCI_USBCMD       0x00
#define UHCI_USBSTS       0x02
#define UHCI_USBINTR      0x04
#define UHCI_FRNUM        0x06
#define UHCI_FRBASEADD    0x08
#define UHCI_SOFMOD       0x0C
#define UHCI_PORTSC1      0x10
#define UHCI_PORTSC2      0x12

#define UHCI_CMD_RS       (1 << 0)
#define UHCI_CMD_HCRESET  (1 << 1)
#define UHCI_CMD_GRESET   (1 << 2)
#define UHCI_CMD_EGSM     (1 << 3)
#define UHCI_CMD_FGR      (1 << 4)
#define UHCI_CMD_MAXP     (1 << 7)
#define UHCI_CMD_CF       (1 << 6)

#define UHCI_STS_USBINT   (1 << 0)
#define UHCI_STS_ERROR    (1 << 1)
#define UHCI_STS_HALTED   (1 << 5)

#define UHCI_PORTSC_CCS   (1 << 0)
#define UHCI_PORTSC_CSC   (1 << 1)
#define UHCI_PORTSC_PE    (1 << 2)
#define UHCI_PORTSC_PEC   (1 << 3)
#define UHCI_PORTSC_RD    (1 << 6)
#define UHCI_PORTSC_LS    (1 << 8)
#define UHCI_PORTSC_RESET (1 << 9)
#define UHCI_PORTSC_SUSP  (1 << 12)

/* Transfer Descriptor + Queue Head, 16-byte aligned.  See UHCI §3.2/§3.3. */
typedef volatile struct ALIGNED(16) {
    uint32_t link;        /* Next TD (bit0 = terminate, bit2 = Vf, bit1=QH) */
    uint32_t status;      /* status / control */
    uint32_t token;       /* PID, DEV addr, EP, MaxLen */
    uint32_t buffer;      /* physical address of data buffer */
    uint32_t sw[4];       /* software bookkeeping */
} uhci_td_t;

typedef volatile struct ALIGNED(16) {
    uint32_t head_link;   /* QH or TD pointer, bit0=T bit1=Q */
    uint32_t elem_link;   /* current TD */
    uint32_t sw[2];
} uhci_qh_t;

#define UHCI_TD_STS_ACTIVE   (1 << 23)
#define UHCI_TD_STS_STALLED  (1 << 22)
#define UHCI_TD_STS_DBE      (1 << 21)
#define UHCI_TD_STS_BABBLE   (1 << 20)
#define UHCI_TD_STS_NAK      (1 << 19)
#define UHCI_TD_STS_CRCTO    (1 << 18)
#define UHCI_TD_STS_BS       (1 << 17)
#define UHCI_TD_IOC          (1 << 24)
#define UHCI_TD_LS           (1 << 26)
#define UHCI_TD_C_ERR(n)     (((n) & 3) << 27)
#define UHCI_TD_SPD          (1 << 29)
#define UHCI_TD_LEN_MASK     0x7FF

#define UHCI_TD_PID_OUT      0xE1
#define UHCI_TD_PID_IN       0x69
#define UHCI_TD_PID_SETUP    0x2D

#define UHCI_TD_TERM         (1u << 0)
#define UHCI_TD_LINK_QH      (1u << 1)
#define UHCI_TD_LINK_VF      (1u << 2)

/* ============================================================================
 * EHCI register definitions (USB Enhanced Host Controller, USB 2.0)
 * ============================================================================ */
#define EHCI_CAPLEN_OFF   0x00
#define EHCI_HCIVERSION   0x02
#define EHCI_HCSPARAMS    0x04
#define EHCI_HCCPARAMS    0x08
#define EHCI_HCSP_PORTROUTE 0x0C

#define EHCI_OP_USBCMD    0x00
#define EHCI_OP_USBSTS    0x04
#define EHCI_OP_USBINTR   0x08
#define EHCI_OP_FRINDEX   0x0C
#define EHCI_OP_CTRLDSSEG 0x10
#define EHCI_OP_PERIODICLISTBASE 0x14
#define EHCI_OP_ASYNCLISTADDR 0x18
#define EHCI_OP_CONFIG    0x40
#define EHCI_OP_PORTSC0   0x44

#define EHCI_CMD_RUN      (1 << 0)
#define EHCI_CMD_HCRESET  (1 << 1)
#define EHCI_CMD_PSE      (1 << 4)
#define EHCI_CMD_ASE      (1 << 5)
#define EHCI_CMD_IAAD     (1 << 6)
#define EHCI_STS_ASS      (1u << 15)   /* async schedule status */

#define EHCI_PORTSC_CCS   (1 << 0)
#define EHCI_PORTSC_CSC   (1 << 1)
#define EHCI_PORTSC_PE    (1 << 2)
#define EHCI_PORTSC_PEC   (1 << 3)
#define EHCI_PORTSC_OCC   (1 << 4)
#define EHCI_PORTSC_OCA   (1 << 5)
#define EHCI_PORTSC_PR    (1 << 8)
#define EHCI_PORTSC_PP    (1 << 12)
#define EHCI_PORTSC_PO    (1 << 13)

typedef volatile struct ALIGNED(32) {
    uint32_t next;
    uint32_t alt_next;
    uint32_t token;
    uint32_t bufp[5];
} ehci_qtd_t;

typedef volatile struct ALIGNED(32) {
    uint32_t hlink;
    uint32_t epchar;
    uint32_t epcap;
    uint32_t current_qtd;
    uint32_t next_qtd;
    uint32_t alt_qtd;
    uint32_t token;
    uint32_t bufp[5];
} ehci_qh_t;

#define EHCI_QTD_STATUS_ACTIVE  (1 << 7)
#define EHCI_QTD_STATUS_HALTED  (1 << 6)
#define EHCI_QTD_STATUS_DBE     (1 << 5)
#define EHCI_QTD_STATUS_BABBLE  (1 << 4)
#define EHCI_QTD_STATUS_XACT    (1 << 3)
#define EHCI_QTD_STATUS_MMF     (1 << 2)
#define EHCI_QTD_STATUS_SXS     (1 << 1)
#define EHCI_QTD_STATUS_PING    (1 << 0)
#define EHCI_QTD_PID_OUT        (0 << 8)
#define EHCI_QTD_PID_IN         (1 << 8)
#define EHCI_QTD_PID_SETUP      (2 << 8)
#define EHCI_QTD_DT_DATA0       (0u << 31)
#define EHCI_QTD_DT_DATA1       (1u << 31)
#define EHCI_QTD_LEN(n)         (((uint32_t)(n) & 0x7FFF) << 16)
#define EHCI_QTD_CERR(n)        (((n) & 3) << 10)
#define EHCI_QTD_IOC            (1 << 15)
#define EHCI_QTD_TERM           (1u << 0)

/* ============================================================================
 * Statically-allocated DMA storage.  The kernel runs identity-paged so
 * physical == virtual; controllers can dereference these addresses
 * directly.  Everything is aligned to 4 KiB so EHCI requirements hold.
 * ============================================================================ */
static uint32_t  uhci_framelist[1024] ALIGNED(4096);
static uhci_qh_t uhci_qh_ctrl                  ALIGNED(16);
static uhci_qh_t uhci_qh_bulk                  ALIGNED(16);
static uhci_td_t uhci_td_pool[16]              ALIGNED(16);

static ehci_qh_t  ehci_qh_dummy                ALIGNED(32); /* H=1 reclaim head */
static ehci_qh_t  ehci_qh_xfer                 ALIGNED(32); /* H=0 transfer QH  */
static ehci_qtd_t ehci_qtd_pool[16]            ALIGNED(32);
static uint32_t   ehci_periodic[1024]          ALIGNED(4096);

/* Shared DMA bounce buffer (4 KiB).  All control / bulk payloads fit
 * comfortably below 4 KiB, so we can reuse a single buffer per call. */
static uint8_t usb_dma_buf[4096] ALIGNED(4096);

/* ============================================================================
 * UHCI helpers
 * ============================================================================ */
static void uhci_global_reset(uint16_t io) {
    /* Per UHCI §2.1.1: assert GRESET for at least 10 ms, then clear. */
    outw(io + UHCI_USBCMD, UHCI_CMD_GRESET);
    pit_sleep(50);
    outw(io + UHCI_USBCMD, 0);
    pit_sleep(10);
}

static void uhci_setup_schedule(usb_controller_t *c) {
    /* Build a degenerate framelist: every entry points at the bulk QH,
     * the bulk QH chains to the control QH, both currently terminate.
     * Transactions populate the QH's element pointer on demand. */
    memset((void *)uhci_td_pool, 0, sizeof(uhci_td_pool));

    uhci_qh_bulk.head_link = ((uint32_t)(uintptr_t)&uhci_qh_ctrl)
                            | UHCI_TD_LINK_QH;
    uhci_qh_bulk.elem_link = UHCI_TD_TERM;
    uhci_qh_ctrl.head_link = UHCI_TD_TERM;
    uhci_qh_ctrl.elem_link = UHCI_TD_TERM;

    uint32_t fl_entry = ((uint32_t)(uintptr_t)&uhci_qh_bulk) | UHCI_TD_LINK_QH;
    for (int i = 0; i < 1024; i++) uhci_framelist[i] = fl_entry;

    outl(c->io_base + UHCI_FRBASEADD, (uint32_t)(uintptr_t)uhci_framelist);
    outw(c->io_base + UHCI_FRNUM, 0);
    outw(c->io_base + UHCI_SOFMOD, 0x40);
    outw(c->io_base + UHCI_USBINTR, 0);
    outw(c->io_base + UHCI_USBSTS, 0xFFFF);
    outw(c->io_base + UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
}

/* ---------- Transfer timeout policy ------------------------------------- *
 * Normal (control / bulk / MSC) transfers wait up to 2 s on the PIT clock.
 * HID interrupt-IN polls run in "quiet" mode: an idle keyboard/mouse NAKs
 * forever, and on EHCI a NAK can't be told apart from "data coming" without
 * waiting, so we bound the poll with a spin-count budget (sub-millisecond
 * to a few ms) and suppress the timeout log so the serial console isn't
 * flooded.  This keeps polling cheap without reworking the schedule. */
static volatile bool     g_xfer_quiet = false;
static volatile uint32_t g_xfer_spin  = 400000u;

static int uhci_run_chain(usb_controller_t *c, uhci_td_t *head) {
    /* Stitch the chain head into the control QH and wait for IOC. */
    uhci_qh_ctrl.elem_link = ((uint32_t)(uintptr_t)head);
    uint32_t deadline = pit_ms() + 2000;
    uint32_t spin = 0;
    for (;;) {
        uint32_t link = uhci_qh_ctrl.elem_link;
        if (link & UHCI_TD_TERM) break;
        if (g_xfer_quiet) {
            /* Bail fast on NAK (device has nothing to report) or when the
             * spin budget is exhausted, quietly. */
            if (head->status & UHCI_TD_STS_NAK) {
                uhci_qh_ctrl.elem_link = UHCI_TD_TERM;
                return -3;
            }
            if (++spin > g_xfer_spin) {
                uhci_qh_ctrl.elem_link = UHCI_TD_TERM;
                return -3;
            }
        } else if (pit_ms() > deadline) {
            uhci_qh_ctrl.elem_link = UHCI_TD_TERM;
            debug_printf("[uhci] transfer timeout\n");
            return -1;
        }
        /* Scan the chain looking for an error. */
        uhci_td_t *t = head;
        while (t) {
            if (t->status & (UHCI_TD_STS_STALLED |
                             UHCI_TD_STS_BABBLE |
                             UHCI_TD_STS_CRCTO)) {
                uhci_qh_ctrl.elem_link = UHCI_TD_TERM;
                if (!g_xfer_quiet)
                    debug_printf("[uhci] TD error status=0x%08x\n", t->status);
                return -2;
            }
            if (!(t->sw[0])) break;
            t = (uhci_td_t *)(uintptr_t)t->sw[0];
        }
        (void)c;
    }
    return 0;
}

static int uhci_xfer(usb_device_t *dev, uint8_t pid, uint8_t ep,
                     uint8_t toggle_seed, void *buf, uint32_t len,
                     bool include_status) {
    usb_controller_t *c = &g_ctrls[dev->ctrl_idx];
    uint16_t mps = (pid == UHCI_TD_PID_IN) ? dev->ep_in_mps : dev->ep_out_mps;
    if (mps == 0) mps = 64;

    int n_td = 0;
    uint32_t remaining = len;
    uint8_t  toggle = toggle_seed;
    uint32_t bufp = (uint32_t)(uintptr_t)buf;

    /* Build the data-phase TDs. */
    while (remaining > 0 && n_td < 14) {
        uint32_t chunk = remaining > mps ? mps : remaining;
        uhci_td_t *t = &uhci_td_pool[n_td];
        memset((void *)t, 0, sizeof(*t));
        t->status = UHCI_TD_STS_ACTIVE | UHCI_TD_C_ERR(3);
        if (dev->speed == 0) t->status |= UHCI_TD_LS;
        t->token  = (uint32_t)pid
                  | ((uint32_t)dev->address << 8)
                  | ((uint32_t)ep << 15)
                  | ((uint32_t)toggle << 19)
                  | ((uint32_t)((chunk - 1) & UHCI_TD_LEN_MASK) << 21);
        t->buffer = bufp;
        bufp     += chunk;
        remaining -= chunk;
        toggle   ^= 1;
        n_td++;
    }
    if (include_status) {
        /* STATUS phase: opposite-direction zero-length packet with DATA1. */
        uhci_td_t *t = &uhci_td_pool[n_td];
        memset((void *)t, 0, sizeof(*t));
        t->status = UHCI_TD_STS_ACTIVE | UHCI_TD_C_ERR(3) | UHCI_TD_IOC;
        if (dev->speed == 0) t->status |= UHCI_TD_LS;
        uint8_t status_pid = (pid == UHCI_TD_PID_IN) ? UHCI_TD_PID_OUT
                                                    : UHCI_TD_PID_IN;
        t->token  = (uint32_t)status_pid
                  | ((uint32_t)dev->address << 8)
                  | ((uint32_t)ep << 15)
                  | (1u << 19)             /* DATA1 */
                  | (0x7FFu << 21);        /* length = 0 */
        t->buffer = 0;
        n_td++;
    }
    /* Link forward + tag the last TD as terminator. */
    for (int i = 0; i < n_td; i++) {
        if (i + 1 < n_td) {
            uhci_td_pool[i].link = ((uint32_t)(uintptr_t)&uhci_td_pool[i + 1])
                                  | UHCI_TD_LINK_VF;
            uhci_td_pool[i].sw[0] = (uint32_t)(uintptr_t)&uhci_td_pool[i + 1];
        } else {
            uhci_td_pool[i].link = UHCI_TD_TERM;
            uhci_td_pool[i].sw[0] = 0;
            uhci_td_pool[i].status |= UHCI_TD_IOC;
        }
    }
    return uhci_run_chain(c, &uhci_td_pool[0]);
}

/* ============================================================================
 * EHCI helpers
 * ============================================================================ */
static volatile uint8_t *ehci_op_base(usb_controller_t *c) {
    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)c->mmio_base;
    return cap + cap[EHCI_CAPLEN_OFF];
}

static void ehci_setup_schedule(usb_controller_t *c) {
    volatile uint8_t *op = ehci_op_base(c);
    /* Initialise periodic list with terminator entries. */
    for (int i = 0; i < 1024; i++) ehci_periodic[i] = 1;
    *(volatile uint32_t *)(op + EHCI_OP_PERIODICLISTBASE) =
        (uint32_t)(uintptr_t)ehci_periodic;

    /* Single H=1 QH for the async list.  Two-QH topologies confuse
     * QEMU's EHCI because it stops async-list traversal at the first
     * H=1 QH ("reclamation head") and never reaches a second QH.
     * A single self-pointing H=1 QH is the simplest valid topology:
     * the HC circles back to itself and picks up new qTDs each pass.   */
    memset((void *)&ehci_qh_dummy, 0, sizeof(ehci_qh_dummy));
    ehci_qh_dummy.hlink    = ((uint32_t)(uintptr_t)&ehci_qh_dummy) | (1u << 1);
    ehci_qh_dummy.epchar   = (1u << 15);   /* H-bit = reclamation head   */
    ehci_qh_dummy.epcap    = (1u << 30);   /* mult = 1                   */
    ehci_qh_dummy.next_qtd = EHCI_QTD_TERM;
    ehci_qh_dummy.alt_qtd  = EHCI_QTD_TERM;

    *(volatile uint32_t *)(op + EHCI_OP_ASYNCLISTADDR) =
        (uint32_t)(uintptr_t)&ehci_qh_dummy;

    /* Enable the async schedule. */
    uint32_t cmd = *(volatile uint32_t *)(op + EHCI_OP_USBCMD);
    cmd |= EHCI_CMD_ASE | EHCI_CMD_RUN;
    *(volatile uint32_t *)(op + EHCI_OP_USBCMD) = cmd;
}

static int ehci_run_qtds(usb_controller_t *c, ehci_qtd_t *head,
                         usb_device_t *dev, uint8_t ep,
                         uint16_t mps, bool is_in) {
    (void)is_in;

    /* Compose the QH endpoint characteristics.
     * H-bit (bit 15) is kept: ehci_qh_dummy is the single H=1 QH.    */
    uint32_t epchar = (uint32_t)dev->address
                    | ((uint32_t)ep << 8)
                    | (2u << 12)                    /* high speed (default) */
                    | ((uint32_t)mps << 16)
                    | (1u << 14)                    /* DT bit comes from qTD */
                    | (1u << 15)                    /* H = reclamation head  */
                    ;

    /* Fully reinitialise the QH overlay before arming.
     * The HC writes token+bufp[] during a transfer; stale HALT/error bits
     * would prevent the HC from starting the next transfer.
     * current_qtd is zeroed so QEMU's EST_EXECUTE sees token.Active=0 and
     * follows next_qtd to the new chain (a stale non-zero current_qtd
     * causes QEMU to re-fetch the old done qTD and may loop).
     * Write order: park first (TERM), then fields, then arm (new chain). */
    ehci_qh_dummy.next_qtd    = EHCI_QTD_TERM;      /* park */
    ehci_qh_dummy.alt_qtd     = EHCI_QTD_TERM;
    ehci_qh_dummy.token       = 0;
    ehci_qh_dummy.current_qtd = 0;
    for (int i = 0; i < 5; i++) ehci_qh_dummy.bufp[i] = 0;

    uint32_t epcap = (1u << 30);                 /* Mult = 1 */

    /* Split transaction: FS/LS device behind a HS hub on EHCI.
     * Overwrite EPS, set the Control-Endpoint flag, add HubAddr+PortNum. */
    if (dev->speed != 2 && dev->tt_hub_addr != 0) {
        uint8_t eps = (dev->speed == 0) ? 1u : 0u;  /* Low=01, Full=00 */
        epchar = (epchar & ~(3u << 12))
               | ((uint32_t)eps << 12)
               | ((ep == 0) ? (1u << 27) : 0u);     /* C: ctrl EP only */
        epcap |= ((uint32_t)dev->tt_hub_addr << 16)
               | ((uint32_t)dev->tt_hub_port << 23);
    }

    ehci_qh_dummy.epchar   = epchar;
    ehci_qh_dummy.epcap    = epcap;
    ehci_qh_dummy.next_qtd = (uint32_t)(uintptr_t)head;   /* arm */

    /* Ring IAAD doorbell.
     * QEMU quirk: if the IAA-interrupt bit is still set in USBSTS from the
     * previous transfer, QEMU ignores a new IAAD ring.  Clear IAA (W/C)
     * before setting IAAD so QEMU always wakes up the async schedule. */
    volatile uint8_t *op = ehci_op_base(c);
    *(volatile uint32_t *)(op + EHCI_OP_USBSTS)  = (1u << 5);  /* W/C IAA */
    *(volatile uint32_t *)(op + EHCI_OP_USBCMD) |= EHCI_CMD_IAAD;

    uint32_t deadline = pit_ms() + 2000;
    uint32_t spin = 0;
    for (;;) {
        if (g_xfer_quiet) {
            if (++spin > g_xfer_spin) return -3;   /* quiet "no data yet" */
        } else if (pit_ms() >= deadline) {
            break;
        }
        bool all_done = true;
        ehci_qtd_t *t = head;
        for (int i = 0; i < 16 && t; i++) {
            if (t->token & EHCI_QTD_STATUS_ACTIVE) { all_done = false; break; }
            if (t->token & (EHCI_QTD_STATUS_HALTED |
                            EHCI_QTD_STATUS_BABBLE |
                            EHCI_QTD_STATUS_DBE)) {
                if (!g_xfer_quiet)
                    debug_printf("[ehci] qTD halted: token=0x%08x\n", t->token);
                return -1;
            }
            if (t->next & EHCI_QTD_TERM) break;
            t = (ehci_qtd_t *)(uintptr_t)(t->next & ~0x1Fu);
        }
        if (all_done) return 0;
    }
    /* Dump controller + QH state to aid diagnosis. */
    if (!g_xfer_quiet)
        debug_printf("[ehci] transfer timeout "
                     "CMD=0x%08x STS=0x%08x ALIST=0x%08x "
                     "QH.tok=0x%08x QH.next=0x%08x qTD.tok=0x%08x\n",
                     *(volatile uint32_t *)(op + EHCI_OP_USBCMD),
                     *(volatile uint32_t *)(op + EHCI_OP_USBSTS),
                     *(volatile uint32_t *)(op + EHCI_OP_ASYNCLISTADDR),
                     ehci_qh_dummy.token, ehci_qh_dummy.next_qtd,
                     head->token);
    return -2;
}

static void ehci_build_qtd(ehci_qtd_t *q, uint32_t buffer, uint32_t length,
                           uint32_t pid, uint8_t toggle) {
    memset((void *)q, 0, sizeof(*q));
    q->token = ((toggle ? 1u : 0u) << 31)
             | EHCI_QTD_LEN(length)
             | EHCI_QTD_CERR(3)
             | pid
             | EHCI_QTD_STATUS_ACTIVE;
    q->bufp[0] = buffer;
    q->next     = EHCI_QTD_TERM;
    q->alt_next = EHCI_QTD_TERM;
}

static int ehci_xfer_chunked(usb_device_t *dev, uint32_t pid,
                             uint8_t ep, uint16_t mps, uint8_t toggle_seed,
                             void *buf, uint32_t len, bool include_status) {
    usb_controller_t *c = &g_ctrls[dev->ctrl_idx];
    int n_qtd = 0;
    uint32_t remaining = len;
    uint32_t addr = (uint32_t)(uintptr_t)buf;
    uint8_t toggle = toggle_seed;
    while (remaining > 0 && n_qtd < 14) {
        uint32_t chunk = remaining > mps ? mps : remaining;
        ehci_build_qtd(&ehci_qtd_pool[n_qtd], addr, chunk, pid, toggle);
        addr      += chunk;
        remaining -= chunk;
        toggle    ^= 1;
        n_qtd++;
    }
    if (include_status) {
        uint32_t status_pid = (pid == EHCI_QTD_PID_IN) ? EHCI_QTD_PID_OUT
                                                      : EHCI_QTD_PID_IN;
        ehci_build_qtd(&ehci_qtd_pool[n_qtd], 0, 0, status_pid, 1);
        n_qtd++;
    }
    /* Link forward. */
    for (int i = 0; i < n_qtd - 1; i++) {
        ehci_qtd_pool[i].next = (uint32_t)(uintptr_t)&ehci_qtd_pool[i + 1];
    }
    ehci_qtd_pool[n_qtd - 1].next = EHCI_QTD_TERM;
    ehci_qtd_pool[n_qtd - 1].token |= EHCI_QTD_IOC;
    return ehci_run_qtds(c, &ehci_qtd_pool[0], dev, ep, mps,
                         pid == EHCI_QTD_PID_IN);
}

/* ============================================================================
 * Root-hub bring-up
 * ============================================================================ */
static void uhci_reset(usb_controller_t *c) {
    if (!c->io_base) return;
    uhci_global_reset(c->io_base);

    outw(c->io_base + UHCI_USBCMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 50; i++) {
        if (!(inw(c->io_base + UHCI_USBCMD) & UHCI_CMD_HCRESET)) break;
        pit_sleep(1);
    }
    outw(c->io_base + UHCI_USBCMD, 0);
    c->num_ports = 2;
    for (int p = 0; p < 2; p++) {
        uint16_t psc = inw(c->io_base + UHCI_PORTSC1 + p * 2);
        c->port_attached[p] = (psc & UHCI_PORTSC_CCS) != 0;
        c->port_was_attached[p] = c->port_attached[p];
        if (c->port_attached[p]) {
            outw(c->io_base + UHCI_PORTSC1 + p * 2,
                 (uint16_t)(psc | UHCI_PORTSC_RESET));
            pit_sleep(50);
            outw(c->io_base + UHCI_PORTSC1 + p * 2,
                 (uint16_t)((psc & ~UHCI_PORTSC_RESET) | UHCI_PORTSC_PE));
            pit_sleep(10);
            debug_printf("[usb] UHCI port %d: device attached, reset done\n", p);
        }
    }
    uhci_setup_schedule(c);
}

static void ehci_reset(usb_controller_t *c) {
    if (!c->mmio_base) return;
    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)c->mmio_base;
    uint32_t hcsp = *(volatile uint32_t *)(cap + EHCI_HCSPARAMS);
    volatile uint8_t *op = ehci_op_base(c);

    *(volatile uint32_t *)(op + EHCI_OP_USBCMD) = 0;
    pit_sleep(2);
    *(volatile uint32_t *)(op + EHCI_OP_USBCMD) = EHCI_CMD_HCRESET;
    for (int i = 0; i < 1000; i++) {
        if (!(*(volatile uint32_t *)(op + EHCI_OP_USBCMD) & EHCI_CMD_HCRESET)) break;
        pit_sleep(1);
    }
    *(volatile uint32_t *)(op + EHCI_OP_USBINTR) = 0;
    *(volatile uint32_t *)(op + EHCI_OP_CTRLDSSEG) = 0;
    *(volatile uint32_t *)(op + EHCI_OP_CONFIG) = 1;
    ehci_setup_schedule(c);

    c->num_ports = hcsp & 0x0F;
    if (c->num_ports > USB_MAX_PORTS) c->num_ports = USB_MAX_PORTS;
    for (int p = 0; p < c->num_ports; p++) {
        uint32_t psc = *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4);
        psc |= EHCI_PORTSC_PP;
        *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4) = psc;
        pit_sleep(20);
        psc = *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4);
        c->port_attached[p] = (psc & EHCI_PORTSC_CCS) != 0;
        c->port_was_attached[p] = c->port_attached[p];
        if (c->port_attached[p]) {
            /* Assert port reset (clear PE first as spec requires). */
            uint32_t pr = *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4);
            pr = (pr & ~EHCI_PORTSC_PE) | EHCI_PORTSC_PR;
            *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4) = pr;
            pit_sleep(50);                      /* USB 2.0: ≥10 ms reset pulse */

            /* De-assert reset by re-reading (not using stale pr) and clearing PR. */
            pr = *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4);
            pr &= ~EHCI_PORTSC_PR;
            *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4) = pr;
            for (int i = 0; i < 50; i++) {     /* wait for HC to finish reset */
                if (!(*(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4) & EHCI_PORTSC_PR))
                    break;
                pit_sleep(1);
            }
            pit_sleep(10);                      /* USB 2.0 §7.1.7.5: recovery */

            /* If PE is not set the device is full/low-speed and was handed off
             * to a companion UHCI/OHCI controller; release port ownership.   */
            uint32_t psc_fin = *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4);
            if (!(psc_fin & EHCI_PORTSC_PE)) {
                debug_printf("[usb] EHCI port %d: FS/LS device, releasing to companion\n", p);
                *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4) =
                    psc_fin | EHCI_PORTSC_PO;
                c->port_attached[p]     = false;
                c->port_was_attached[p] = false;
            } else {
                debug_printf("[usb] EHCI port %d: HS device, reset done\n", p);
            }
        }
    }
}

/* ============================================================================
 * Device enumeration
 * ============================================================================ */
static usb_device_t *alloc_device(int ctrl_idx, int port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!g_devs[i].in_use) {
            memset(&g_devs[i], 0, sizeof(g_devs[i]));
            g_devs[i].in_use   = true;
            g_devs[i].ctrl_idx = ctrl_idx;
            g_devs[i].port     = port;
            g_devs[i].ep_in    = 0x81;
            g_devs[i].ep_out   = 0x02;
            g_devs[i].ep_in_mps  = 64;
            g_devs[i].ep_out_mps = 64;
            g_n_devs++;
            return &g_devs[i];
        }
    }
    return NULL;
}

static void free_device(usb_device_t *dev) {
    if (!dev || !dev->in_use) return;
    dev->in_use = false;
    g_n_devs--;
}

/* Forward declarations for xHCI and hub functions used by do_control/bulk paths */
static int  xhci_find_slot(const usb_device_t *dev);
static int  xhci_ctrl_xfer(int slot, uint8_t bmt, uint8_t bReq,
                            uint16_t wVal, uint16_t wIdx, void *buf, uint16_t len);
static int  xhci_bulk_xfer_in(int slot, void *buf, uint32_t len, uint32_t *got);
static int  xhci_bulk_xfer_out(int slot, const void *buf, uint32_t len);
static int  xhci_intr_in(int slot, void *buf, uint32_t len, uint32_t *got);
static bool xhci_reset(usb_controller_t *c);
static bool xhci_enumerate_port(usb_controller_t *c, int port);
static int  xhci_poll_ports(usb_controller_t *c);
static void hub_init(usb_device_t *hub);

static int do_control(usb_device_t *dev, uint8_t bmRequestType,
                      uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                      void *data, uint16_t length) {
    usb_setup_t *setup = (usb_setup_t *)usb_dma_buf;
    setup->bmRequestType = bmRequestType;
    setup->bRequest      = bRequest;
    setup->wValue        = wValue;
    setup->wIndex        = wIndex;
    setup->wLength       = length;

    usb_controller_t *c = &g_ctrls[dev->ctrl_idx];
    if (c->kind == USB_CTRL_UHCI) {
        /* SETUP stage: 8 bytes, DATA0. */
        uhci_td_t *t = &uhci_td_pool[0];
        memset((void *)t, 0, sizeof(*t));
        t->status = UHCI_TD_STS_ACTIVE | UHCI_TD_C_ERR(3);
        if (dev->speed == 0) t->status |= UHCI_TD_LS;
        t->token  = (uint32_t)UHCI_TD_PID_SETUP
                  | ((uint32_t)dev->address << 8)
                  | (0u << 15)
                  | (0u << 19)
                  | ((uint32_t)((8 - 1) & UHCI_TD_LEN_MASK) << 21);
        t->buffer = (uint32_t)(uintptr_t)setup;
        t->link   = UHCI_TD_TERM;
        t->status |= UHCI_TD_IOC;
        if (uhci_run_chain(c, t) != 0) return -1;

        if (length > 0) {
            uint8_t pid = (bmRequestType & 0x80) ? UHCI_TD_PID_IN : UHCI_TD_PID_OUT;
            /* DATA stage starts with DATA1. */
            if (uhci_xfer(dev, pid, 0, 1, data, length, false) != 0) return -1;
        }
        /* STATUS stage: opposite direction, DATA1, zero-length. */
        uhci_td_t *s = &uhci_td_pool[0];
        memset((void *)s, 0, sizeof(*s));
        s->status = UHCI_TD_STS_ACTIVE | UHCI_TD_C_ERR(3) | UHCI_TD_IOC;
        if (dev->speed == 0) s->status |= UHCI_TD_LS;
        uint8_t status_pid = (bmRequestType & 0x80) ? UHCI_TD_PID_OUT
                                                   : UHCI_TD_PID_IN;
        s->token  = (uint32_t)status_pid
                  | ((uint32_t)dev->address << 8)
                  | (0u << 15)
                  | (1u << 19)
                  | (0x7FFu << 21);
        s->buffer = 0;
        s->link   = UHCI_TD_TERM;
        if (uhci_run_chain(c, s) != 0) return -1;
        return 0;
    }
    if (c->kind == USB_CTRL_EHCI) {
        /* SETUP qTD */
        ehci_build_qtd(&ehci_qtd_pool[0], (uint32_t)(uintptr_t)setup, 8,
                       EHCI_QTD_PID_SETUP, 0);
        ehci_qtd_pool[0].next = (uint32_t)(uintptr_t)&ehci_qtd_pool[1];

        int n = 1;
        uint32_t pid = (bmRequestType & 0x80) ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT;
        uint8_t toggle = 1;
        uint32_t remaining = length;
        uint32_t addr = (uint32_t)(uintptr_t)data;
        while (remaining > 0 && n < 14) {
            uint32_t chunk = remaining > dev->ep_in_mps ? dev->ep_in_mps
                                                         : remaining;
            ehci_build_qtd(&ehci_qtd_pool[n], addr, chunk, pid, toggle);
            ehci_qtd_pool[n].next = (uint32_t)(uintptr_t)&ehci_qtd_pool[n + 1];
            addr      += chunk;
            remaining -= chunk;
            toggle    ^= 1;
            n++;
        }
        /* STATUS */
        uint32_t status_pid = (bmRequestType & 0x80) ? EHCI_QTD_PID_OUT
                                                     : EHCI_QTD_PID_IN;
        ehci_build_qtd(&ehci_qtd_pool[n], 0, 0, status_pid, 1);
        ehci_qtd_pool[n].next = EHCI_QTD_TERM;
        ehci_qtd_pool[n].token |= EHCI_QTD_IOC;
        n++;
        return ehci_run_qtds(c, &ehci_qtd_pool[0], dev, 0, dev->ep_in_mps,
                             (bmRequestType & 0x80) != 0);
    }
    if (c->kind == USB_CTRL_XHCI) {
        int slot = xhci_find_slot(dev);
        if (slot < 0) return -1;
        return xhci_ctrl_xfer(slot, bmRequestType, bRequest,
                              wValue, wIndex, data, length);
    }
    return -1;
}

int usb_control_transfer(usb_device_t *dev, const usb_setup_t *setup,
                         void *data, uint16_t length) {
    if (!dev || !setup) return -1;
    return do_control(dev, setup->bmRequestType, setup->bRequest,
                      setup->wValue, setup->wIndex, data, length);
}

int usb_bulk_out(usb_device_t *dev, const void *data, uint32_t len) {
    if (!dev) return -1;
    usb_controller_t *c = &g_ctrls[dev->ctrl_idx];
    /* Copy into bounce buffer if caller's buffer crosses a 4 KiB boundary,
     * which we cannot service in a single PRD entry on EHCI. */
    void *src = (void *)data;
    if (len > sizeof(usb_dma_buf)) return -1;
    memcpy(usb_dma_buf, src, len);
    int rc;
    uint8_t ep = dev->ep_out & 0x0F;
    if (c->kind == USB_CTRL_UHCI) {
        rc = uhci_xfer(dev, UHCI_TD_PID_OUT, ep, dev->toggle_out,
                       usb_dma_buf, len, false);
    } else if (c->kind == USB_CTRL_EHCI) {
        rc = ehci_xfer_chunked(dev, EHCI_QTD_PID_OUT, ep, dev->ep_out_mps,
                               dev->toggle_out, usb_dma_buf, len, false);
    } else if (c->kind == USB_CTRL_XHCI) {
        int slot = xhci_find_slot(dev);
        rc = (slot >= 0) ? xhci_bulk_xfer_out(slot, usb_dma_buf, len) : -1;
        return rc;  /* xHCI handles toggle internally */
    } else {
        return -1;
    }
    if (rc == 0) {
        /* Advance toggle by the number of packets we sent. */
        uint32_t mps = dev->ep_out_mps ? dev->ep_out_mps : 64;
        uint32_t pkts = (len + mps - 1) / mps;
        dev->toggle_out ^= (pkts & 1);
    }
    return rc;
}

int usb_bulk_in(usb_device_t *dev, void *data, uint32_t cap,
                uint32_t *out_len) {
    if (!dev) return -1;
    usb_controller_t *c = &g_ctrls[dev->ctrl_idx];
    if (cap > sizeof(usb_dma_buf)) cap = sizeof(usb_dma_buf);
    int rc;
    uint8_t ep = dev->ep_in & 0x0F;
    if (c->kind == USB_CTRL_UHCI) {
        rc = uhci_xfer(dev, UHCI_TD_PID_IN, ep, dev->toggle_in,
                       usb_dma_buf, cap, false);
    } else if (c->kind == USB_CTRL_EHCI) {
        rc = ehci_xfer_chunked(dev, EHCI_QTD_PID_IN, ep, dev->ep_in_mps,
                               dev->toggle_in, usb_dma_buf, cap, false);
    } else if (c->kind == USB_CTRL_XHCI) {
        int slot = xhci_find_slot(dev);
        if (slot < 0) { if (out_len) *out_len = 0; return -1; }
        uint32_t got = 0;
        rc = xhci_bulk_xfer_in(slot, usb_dma_buf, cap, &got);
        if (rc == 0) {
            memcpy(data, usb_dma_buf, got);
            if (out_len) *out_len = got;
        } else if (out_len) *out_len = 0;
        return rc;
    } else {
        if (out_len) *out_len = 0;
        return -1;
    }
    if (rc == 0) {
        memcpy(data, usb_dma_buf, cap);
        if (out_len) *out_len = cap;
        uint32_t mps = dev->ep_in_mps ? dev->ep_in_mps : 64;
        uint32_t pkts = (cap + mps - 1) / mps;
        dev->toggle_in ^= (pkts & 1);
    } else if (out_len) {
        *out_len = 0;
    }
    return rc;
}

/* Interrupt-IN poll for HID class drivers.  Identical to usb_bulk_in but
 * runs in "quiet, bounded-spin" mode so an idle endpoint (which NAKs)
 * returns within a sub-millisecond budget instead of stalling 2 s.
 * Returns 0 and *out_len>0 when a report was received, negative otherwise
 * (-3 = "no data ready", expected and silent). */
int usb_intr_in(usb_device_t *dev, void *data, uint32_t cap,
                uint32_t *out_len, uint32_t spin_budget) {
    if (!dev) return -1;
    usb_controller_t *c = &g_ctrls[dev->ctrl_idx];
    if (c->kind == USB_CTRL_XHCI) {
        /* xHCI HID: dedicated non-blocking interrupt-IN poll (an idle mouse
         * never completes a transfer, so the bulk/blocking path would stall). */
        int slot = xhci_find_slot(dev);
        if (slot < 0) { if (out_len) *out_len = 0; return -1; }
        uint32_t got = 0;
        int rc = xhci_intr_in(slot, usb_dma_buf, cap, &got);
        if (rc == 0) {
            memcpy(data, usb_dma_buf, got);
            if (out_len) *out_len = got;
        } else if (out_len) *out_len = 0;
        return rc;
    }
    g_xfer_quiet = true;
    if (spin_budget) g_xfer_spin = spin_budget;
    int rc = usb_bulk_in(dev, data, cap, out_len);
    g_xfer_quiet = false;
    g_xfer_spin  = 400000u;
    return rc;
}

/* Parse the configuration descriptor and pull out the right interface + its
 * data endpoint(s).
 *
 * A config can list SEVERAL interfaces.  Composite HID devices are common: a
 * boot keyboard/mouse exposes interface 0 (the boot report endpoint, almost
 * always EP1-IN) AND one or more extra interfaces (consumer-control, vendor,
 * NKRO) each with their OWN interrupt-IN endpoint (EP2-IN, EP3-IN...).  The
 * old code walked endpoints across every interface and kept the LAST one, so
 * `ep_in` ended up pointing at the consumer/vendor endpoint (e.g. 0x82) which
 * only fires on special keys — never on normal movement/typing.  On real HW
 * that endpoint just NAKs forever: the xHCI EP shows Running, the doorbell is
 * rung, but no transfer event ever arrives -> 0 HID reports.  (Proven by the
 * `usbnative` brutal dump: in=82 on BOTH the keyboard and the mouse.)
 *
 * Fix: pick ONE interface (preferring the HID boot interface), then capture
 * only THAT interface's first IN/OUT data endpoint. */
static void parse_config(usb_device_t *dev, const uint8_t *buf, int len) {
    int  off = 0;
    int  best_score = -1, best_off = -1;
    int  n_ifaces = 0;

    /* Pass 1 — choose the most appropriate interface (alt setting 0 wins on a
     * tie because we scan in order and only replace on a strictly higher
     * score). */
    while (off + 2 <= len) {
        uint8_t dlen = buf[off], dtype = buf[off + 1];
        if (dlen == 0 || off + dlen > len) break;
        if (dtype == USB_DESC_INTERFACE && off + 9 <= len) {
            const usb_interface_descriptor_t *id =
                (const usb_interface_descriptor_t *)(buf + off);
            n_ifaces++;
            int score;
            if      (id->bInterfaceClass == USB_CLASS_HID) score = 100;
            else if (id->bInterfaceClass == USB_CLASS_MSC) score = 50;
            else if (id->bInterfaceClass == USB_CLASS_HUB) score = 40;
            else                                            score = 10;
            if (id->bInterfaceClass == USB_CLASS_HID) {
                if (id->bInterfaceSubClass == 1) score += 10;   /* boot subclass */
                if (id->bInterfaceProtocol == 1 ||
                    id->bInterfaceProtocol == 2) score += 5;     /* kbd / mouse  */
            }
            if (id->bNumEndpoints == 0) score = -1;             /* no data EP    */
            if (score > best_score) { best_score = score; best_off = off; }
        }
        off += dlen;
    }
    dev->n_ifaces = (uint8_t)n_ifaces;
    if (best_off < 0) return;   /* nothing usable — keep the alloc-time defaults */

    const usb_interface_descriptor_t *id =
        (const usb_interface_descriptor_t *)(buf + best_off);
    dev->iface_num      = id->bInterfaceNumber;
    dev->iface_class    = id->bInterfaceClass;
    dev->iface_subclass = id->bInterfaceSubClass;
    dev->iface_protocol = id->bInterfaceProtocol;

    /* Pass 2 — capture the FIRST IN and OUT data endpoint that belong to the
     * chosen interface only (stop at the next interface descriptor). */
    dev->ep_in = 0; dev->ep_out = 0; dev->ep_in_interval = 0;
    off = best_off + buf[best_off];
    while (off + 2 <= len) {
        uint8_t dlen = buf[off], dtype = buf[off + 1];
        if (dlen == 0 || off + dlen > len) break;
        if (dtype == USB_DESC_INTERFACE) break;     /* next interface -> done */
        if (dtype == USB_DESC_ENDPOINT && off + 6 <= len) {
            const usb_endpoint_descriptor_t *ep =
                (const usb_endpoint_descriptor_t *)(buf + off);
            uint8_t addr = ep->bEndpointAddress;
            uint8_t type = ep->bmAttributes & 0x03;
            /* BULK (mass storage) or INTERRUPT (HID) data endpoints only. */
            if (type == 0x02 || type == 0x03) {
                if ((addr & 0x80) && !dev->ep_in) {
                    dev->ep_in          = addr;
                    dev->ep_in_mps      = ep->wMaxPacketSize & 0x07FF;
                    dev->ep_in_interval = ep->bInterval;
                } else if (!(addr & 0x80) && !dev->ep_out) {
                    dev->ep_out     = addr;
                    dev->ep_out_mps = ep->wMaxPacketSize & 0x07FF;
                }
            }
        }
        off += dlen;
    }
}

static bool enumerate_device(int ctrl_idx, int port,
                              uint8_t tt_hub_addr, uint8_t tt_hub_port,
                              uint8_t speed);

/* ============================================================================
 * xHCI (USB 3.0) host controller driver
 * Supports SuperSpeed (USB 3.x) and USB 2.0/1.x devices on xHCI root ports.
 * Architecture: Command Ring + Event Ring + per-device Transfer Rings.
 * ============================================================================ */

/* Capability register offsets (from mmio_base) */
#define XHCI_CAP_CAPLENGTH   0x00u
#define XHCI_CAP_HCSPARAMS1  0x04u
#define XHCI_CAP_HCCPARAMS1  0x10u
#define XHCI_CAP_DBOFF       0x14u
#define XHCI_CAP_RTSOFF      0x18u

/* Operational register offsets (from op_base = mmio_base + CAPLENGTH) */
#define XHCI_OP_USBCMD   0x00u
#define XHCI_OP_USBSTS   0x04u
#define XHCI_OP_DNCTRL   0x14u
#define XHCI_OP_CRCR     0x18u   /* 64-bit: Command Ring Control */
#define XHCI_OP_DCBAAP   0x30u   /* 64-bit: DCBAA pointer */
#define XHCI_OP_CONFIG   0x38u
#define XHCI_OP_PORTSC(n) (0x400u + (n) * 16u)

/* USBCMD / USBSTS bits */
#define XHCI_CMD_RUN   (1u << 0)
#define XHCI_CMD_HCRST (1u << 1)
#define XHCI_STS_HCH   (1u << 0)
#define XHCI_STS_CNR   (1u << 11)

/* PORTSC bits */
#define XHCI_PORTSC_CCS    (1u << 0)
#define XHCI_PORTSC_PED    (1u << 1)
#define XHCI_PORTSC_PR     (1u << 4)
#define XHCI_PORTSC_PP     (1u << 9)
#define XHCI_PORTSC_SPEED(v) (((v) >> 10) & 0xFu)
#define XHCI_PORTSC_CSC    (1u << 17)
#define XHCI_PORTSC_PRC    (1u << 21)
#define XHCI_PORTSC_W1C    (0x7Eu << 17)  /* write-1-to-clear status bits */

/* TRB type codes */
#define XHCI_TRB_NORMAL    1u
#define XHCI_TRB_SETUP    2u
#define XHCI_TRB_DATA     3u
#define XHCI_TRB_STATUS   4u
#define XHCI_TRB_LINK     6u
#define XHCI_TRB_EN_SLOT  9u
#define XHCI_TRB_ADDR_DEV 11u
#define XHCI_TRB_CFG_EP   12u
#define XHCI_TRB_TRANS_EVT 32u
#define XHCI_TRB_CMD_CMPL  33u
#define XHCI_TRB_PORT_STC  34u

/* TRB control field helpers */
#define XHCI_TC_CYCLE  (1u << 0)
#define XHCI_TC_TC     (1u << 1)   /* Toggle Cycle for Link TRB */
#define XHCI_TC_CH     (1u << 4)
#define XHCI_TC_IOC    (1u << 5)
#define XHCI_TC_IDT    (1u << 6)   /* Immediate Data */
#define XHCI_TC_BSR    (1u << 9)   /* Block Set Address Request */
#define XHCI_TC_DIR    (1u << 16)  /* Direction: 1=IN */
#define XHCI_TC_TYPE(t) ((uint32_t)(t) << 10)
#define XHCI_TC_SLOT(s) ((uint32_t)(s) << 24)
#define XHCI_TC_EP(e)   ((uint32_t)(e) << 16)
#define XHCI_TC_TRT(t)  ((uint32_t)(t) << 16)  /* Transfer Type in Setup TRB */

/* Completion codes (bits 31-24 of event TRB status) */
#define XHCI_CC(s)      (((s) >> 24) & 0xFFu)
#define XHCI_CC_SUCCESS  1u
#define XHCI_CC_STALL    6u
#define XHCI_CC_SHORT   13u

/* Context add/drop flag helpers (Linux convention: bit 0 = Slot, bit n = DCI n) */
#define XHCI_CF_SLOT    (1u << 0)
#define XHCI_CF_EP(dci) (1u << (dci))

/* xHCI port speed encoding (from PORTSC[13:10]) */
#define XHCI_SPD_FS 1u
#define XHCI_SPD_LS 2u
#define XHCI_SPD_HS 3u
#define XHCI_SPD_SS 4u

/* Ring sizes */
#define XHCI_CMDR_SZ  16
#define XHCI_EVTR_SZ  32
#define XHCI_TR_SZ    16
#define XHCI_NSLOTS    8

/* ---- Data structures ---- */
typedef struct PACKED { uint32_t p0,p1,status,ctrl; } xhci_trb_t; /* 16 bytes */

/* Slot Context (32 bytes, CSZ=0) */
typedef struct PACKED {
    uint32_t route_speed;  /* [19:0]=route, [23:20]=speed, [31:27]=ctx_ents */
    uint32_t lat_port;     /* [23:16]=root hub port */
    uint32_t int_addr;     /* [7:0]=dev_addr, [31:22]=interrupter */
    uint32_t state;
    uint32_t rsvd[4];
} xhci_slot_ctx_t;         /* 32 bytes */

/* Endpoint Context (32 bytes) */
typedef struct PACKED {
    uint32_t ep_state;
    uint32_t ep_info;     /* [5:3]=type,[10:8]=max_burst,[31:16]=max_pkt */
    uint32_t tr_deq_lo;   /* dequeue pointer low + DCS in bit 0 */
    uint32_t tr_deq_hi;
    uint32_t avg_len;
    uint32_t rsvd[3];
} xhci_ep_ctx_t;           /* 32 bytes */

/* Input Control Context (32 bytes) */
typedef struct PACKED { uint32_t drop,add,rsvd[6]; } xhci_icc_t;

/* Input Context (1 ICC + 1 Slot + 31 EP = 33 × 32 = 1056 bytes) */
typedef struct PACKED {
    xhci_icc_t       icc;
    xhci_slot_ctx_t  slot;
    xhci_ep_ctx_t    ep[31];
} xhci_input_ctx_t;        /* 1056 bytes */

/* ERST entry (16 bytes) */
typedef struct PACKED { uint64_t base; uint32_t size; uint32_t rsvd; } xhci_erst_t;

/* ---- Per-controller instance (rings + runtime state) --------------------
 * HISTORICALLY all of this was a SINGLE static set: resetting a second xHCI
 * repointed the shared rings + register bases at the newest controller and
 * left every previously-reset xHCI deaf (its DCBAA/CRCR still pointed at our
 * rings, but X->op/X->db/X->rt and the producer/consumer cycles now belonged
 * to the other chip).  On boards with two xHCIs (P8Z77: Intel + ASMedia)
 * that meant pendrives on the first controller's ports were NEVER seen —
 * reproduced in QEMU with two qemu-xhci controllers.  Everything lives in
 * xhci_hc_t now; X points at the instance being operated on. */
#define XHCI_MAX_HC 2

typedef struct ALIGNED(64) {
    /* DMA areas — every member here must stay 64-byte aligned; they are
     * grouped as multiples of 64 so the offsets work out. */
    uint64_t         dcbaa[256];                              /* 2048 B */
    xhci_trb_t       cmdr[XHCI_CMDR_SZ];                      /*  256 B */
    xhci_trb_t       evtr[XHCI_EVTR_SZ];                      /*  512 B */
    xhci_trb_t       ep0r [XHCI_NSLOTS][XHCI_TR_SZ];          /* 2048 B */
    xhci_trb_t       epir [XHCI_NSLOTS][XHCI_TR_SZ];          /* 2048 B */
    xhci_trb_t       epor [XHCI_NSLOTS][XHCI_TR_SZ];          /* 2048 B */
    uint8_t          devctx[XHCI_NSLOTS][512];                /* 4096 B */
    xhci_erst_t      erst[4];      /* entry 0 used; padded to 64 B      */
    xhci_input_ctx_t inpctx;       /* 1056 B (starts 64-aligned)        */

    /* Register bases */
    uint32_t op;                   /* operational register base          */
    uint32_t db;                   /* doorbell register base             */
    uint32_t rt;                   /* runtime register base              */

    /* Ring cycle/index state */
    uint8_t  cpc;  int cpi;        /* cmd ring producer cycle / index    */
    uint8_t  ecc;  int eci;        /* evt ring consumer cycle / index    */
    uint8_t  ep0c[XHCI_NSLOTS], eic[XHCI_NSLOTS], eoc[XHCI_NSLOTS];
    int      ep0i[XHCI_NSLOTS], eii[XHCI_NSLOTS], eoi[XHCI_NSLOTS];
    /* Map from xHCI slot (1..8) to g_devs index; -1 = free */
    int8_t   slot_dev[XHCI_NSLOTS + 1];

    /* Interrupt-IN (HID) poll state: a single TRB armed per slot, checked
     * non-blockingly (an idle mouse never completes). */
    bool     iarm[XHCI_NSLOTS + 1];
    bool     idone[XHCI_NSLOTS + 1];
    int      icc [XHCI_NSLOTS + 1];
    uint32_t ires[XHCI_NSLOTS + 1];

    bool     in_use;
} xhci_hc_t;

static xhci_hc_t  g_xhc[XHCI_MAX_HC];
static xhci_hc_t *X = &g_xhc[0];   /* instance being operated on */

/* Bind X to the hc that owns controller c (no-op if c never reset). */
static void xhci_bind(const usb_controller_t *c) {
    if (c && c->xhci_hc >= 0 && c->xhci_hc < XHCI_MAX_HC)
        X = &g_xhc[c->xhci_hc];
}
static uint8_t  g_xhci_last_epcc = 0;   /* last CONFIGURE_ENDPOINT cc (diag) */
/* ---- Brutal-logging counters for the interrupt-IN path (usbnative diag) ---- */
static uint32_t g_xint_doorbells = 0;   /* # doorbell rings issued            */
static uint32_t g_xint_events    = 0;   /* # events drained from the ring     */
static uint32_t g_xint_xfer_evts = 0;   /* # transfer events specifically     */
static uint32_t g_xint_last_ctrl = 0;   /* last event TRB ctrl word           */
static uint32_t g_xint_last_sts  = 0;   /* last event TRB status word         */
static uint32_t g_xint_last_p0   = 0;   /* last event TRB pointer low         */

/* ---- Register helpers ---- */
static uint32_t xop_rd(uint32_t off)  { return *(volatile uint32_t*)(uintptr_t)(X->op+off); }
static void     xop_wr(uint32_t off, uint32_t v)
                { *(volatile uint32_t*)(uintptr_t)(X->op+off) = v; }
static void     xop_wr64(uint32_t off, uint64_t v) {
    *(volatile uint32_t*)(uintptr_t)(X->op+off)   = (uint32_t)v;
    *(volatile uint32_t*)(uintptr_t)(X->op+off+4) = (uint32_t)(v>>32);
}
static void     xrt_wr(uint32_t off, uint32_t v)
                { *(volatile uint32_t*)(uintptr_t)(X->rt+off) = v; }
static void     xrt_wr64(uint32_t off, uint64_t v) {
    *(volatile uint32_t*)(uintptr_t)(X->rt+off)   = (uint32_t)v;
    *(volatile uint32_t*)(uintptr_t)(X->rt+off+4) = (uint32_t)(v>>32);
}
static void     xdb_wr(uint8_t slot, uint8_t ep)
                { *(volatile uint32_t*)(uintptr_t)(X->db+(uint32_t)slot*4) = ep; }

/* EP DCI from USB endpoint address */
static uint8_t xhci_dci(uint8_t ep_addr) {
    if (!ep_addr) return 1;
    uint8_t n = ep_addr & 0x0Fu;
    uint8_t d = (ep_addr & 0x80u) ? 1u : 0u;
    return (uint8_t)(2*n + d);
}

/* ---- Command ring ---- */
/* Place one TRB in the command ring and ring the HC doorbell.
 * Returns 0 on success (non-success completion code in *cc). */
static int xhci_submit_cmd(const xhci_trb_t *trb, uint8_t *cc) {
    /* Copy + stamp cycle bit */
    xhci_trb_t t = *trb;
    t.ctrl = (t.ctrl & ~XHCI_TC_CYCLE) | (X->cpc ? XHCI_TC_CYCLE : 0u);
    X->cmdr[X->cpi] = t;

    X->cpi++;
    if (X->cpi >= XHCI_CMDR_SZ - 1) {
        /* Wrap with Link TRB */
        xhci_trb_t lnk;
        memset(&lnk, 0, sizeof(lnk));
        lnk.p0   = (uint32_t)(uintptr_t)X->cmdr;
        lnk.ctrl = XHCI_TC_TYPE(XHCI_TRB_LINK) | XHCI_TC_TC |
                   (X->cpc ? XHCI_TC_CYCLE : 0u);
        X->cmdr[X->cpi] = lnk;
        X->cpi  = 0;
        X->cpc ^= 1;
    }

    xdb_wr(0, 0);   /* ring host controller doorbell */

    /* Poll event ring for command completion (timeout 500 ms) */
    uint32_t deadline = pit_ms() + 500;
    while (pit_ms() < deadline) {
        xhci_trb_t *ev = &X->evtr[X->eci];
        uint8_t type = (uint8_t)((ev->ctrl >> 10) & 0x3Fu);
        uint8_t cyc  = (uint8_t)(ev->ctrl & 1u);
        if (cyc != X->ecc) { /* no event yet */ continue; }
        /* Consume this event */
        X->eci++;
        if (X->eci >= XHCI_EVTR_SZ) { X->eci = 0; X->ecc ^= 1; }
        /* Update ERDP (clear EHB bit 3, point to dequeue ptr) */
        xrt_wr64(0x20 + 0x18,
                 (uint64_t)(uintptr_t)&X->evtr[X->eci]);

        if (type == XHCI_TRB_CMD_CMPL) {
            if (cc) *cc = XHCI_CC(ev->status);
            return (XHCI_CC(ev->status) == XHCI_CC_SUCCESS) ? 0 : -1;
        }
        /* Skip transfer events, port status events, etc. */
    }
    debug_printf("[xhci] command timeout\n");
    return -2;
}

/* ---- Transfer ring helpers ---- */
static int xhci_ring_trb(xhci_trb_t *ring, int *idx, uint8_t *cyc,
                          const xhci_trb_t *trb) {
    xhci_trb_t t = *trb;
    t.ctrl = (t.ctrl & ~XHCI_TC_CYCLE) | (*cyc ? XHCI_TC_CYCLE : 0u);
    ring[*idx] = t;
    (*idx)++;
    if (*idx >= XHCI_TR_SZ - 1) {
        /* Ring Link TRB at last slot */
        xhci_trb_t lnk;
        memset(&lnk, 0, sizeof(lnk));
        lnk.p0   = (uint32_t)(uintptr_t)ring;
        lnk.ctrl = XHCI_TC_TYPE(XHCI_TRB_LINK) | XHCI_TC_TC |
                   (*cyc ? XHCI_TC_CYCLE : 0u);
        ring[*idx] = lnk;
        *idx  = 0;
        *cyc ^= 1;
    }
    return 0;
}

/* Wait for a Transfer Event on the event ring (timeout 2000 ms).
 * Returns completion code; *actual_len filled from short-pkt events. */
static int xhci_wait_xfer(uint32_t *actual_len) {
    uint32_t deadline = pit_ms() + 2000;
    while (pit_ms() < deadline) {
        xhci_trb_t *ev = &X->evtr[X->eci];
        uint8_t cyc  = (uint8_t)(ev->ctrl & 1u);
        if (cyc != X->ecc) continue;
        uint8_t type = (uint8_t)((ev->ctrl >> 10) & 0x3Fu);
        X->eci++;
        if (X->eci >= XHCI_EVTR_SZ) { X->eci = 0; X->ecc ^= 1; }
        xrt_wr64(0x20 + 0x18, (uint64_t)(uintptr_t)&X->evtr[X->eci]);
        if (type == XHCI_TRB_TRANS_EVT) {
            uint8_t cc = XHCI_CC(ev->status);
            if (actual_len) *actual_len = ev->status & 0xFFFFu;  /* residue */
            if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT) return (int)cc;
            debug_printf("[xhci] transfer event cc=%u\n", cc);
            return -(int)cc;
        }
        /* skip command completions that arrived late */
    }
    debug_printf("[xhci] transfer timeout\n");
    return -99;
}

/* Non-blocking: drain every transfer event currently on the event ring and
 * record completion (cc, residue) for the matching slot.  Used by the HID
 * interrupt-poll path so an idle endpoint never blocks. */
static void xhci_pump_events(void) {
    /* Acknowledge any pending interrupt status BEFORE reading the ring: the
     * brutal dump showed USBSTS.EINT and IMAN.IP stuck at 1.  These are RW1C
     * and real Intel/ASMedia xHCI will not signal NEW events to the ring
     * handler until they are cleared (QEMU's NEC model is lax, which is why it
     * "worked" in emulation).  USBSTS write touches only EINT (bit 3); IMAN
     * write clears IP (bit 0) and leaves IE=0 (we poll, no IRQ). */
    if (X->op) {
        uint32_t sts = *(volatile uint32_t *)(uintptr_t)(X->op + XHCI_OP_USBSTS);
        if (sts & (1u << 3))
            *(volatile uint32_t *)(uintptr_t)(X->op + XHCI_OP_USBSTS) = (1u << 3);
        uint32_t iman = *(volatile uint32_t *)(uintptr_t)(X->rt + 0x20 + 0x00);
        if (iman & 1u)
            *(volatile uint32_t *)(uintptr_t)(X->rt + 0x20 + 0x00) = iman | 1u;
    }
    for (int guard = 0; guard < XHCI_EVTR_SZ * 2; guard++) {
        xhci_trb_t *ev = &X->evtr[X->eci];
        if ((uint8_t)(ev->ctrl & 1u) != X->ecc) return;   /* no new event */
        uint8_t type = (uint8_t)((ev->ctrl >> 10) & 0x3Fu);
        uint8_t evslot = (uint8_t)((ev->ctrl >> 24) & 0xFFu);
        g_xint_events++;
        g_xint_last_ctrl = ev->ctrl;
        g_xint_last_sts  = ev->status;
        g_xint_last_p0   = ev->p0;
        X->eci++;
        if (X->eci >= XHCI_EVTR_SZ) { X->eci = 0; X->ecc ^= 1; }
        /* ack with the Event Handler Busy bit set (real HW wants this). */
        xrt_wr64(0x20 + 0x18, (uint64_t)(uintptr_t)&X->evtr[X->eci] | (1u << 3));
        if (type == XHCI_TRB_TRANS_EVT) {
            g_xint_xfer_evts++;
            if (evslot >= 1 && evslot <= XHCI_NSLOTS) {
                X->icc  [evslot] = (int)XHCI_CC(ev->status);
                X->ires [evslot] = ev->status & 0xFFFFu;
                X->idone[evslot] = true;
            }
        }
    }
}

/* Interrupt-IN poll for a HID endpoint.  Arms one TRB if none is outstanding,
 * then checks (non-blocking) whether a report has arrived.  Returns 0 with
 * *got set on a report, -3 when there's nothing yet (idle), negative on error. */
static int xhci_intr_in(int slot, void *buf, uint32_t len, uint32_t *got) {
    if (slot < 1 || slot > XHCI_NSLOTS) { if (got) *got = 0; return -1; }
    int8_t di = X->slot_dev[slot];
    uint8_t dci = 3;   /* default EP1 IN */
    if (di >= 0 && g_devs[di].in_use && g_devs[di].ep_in)
        dci = xhci_dci(g_devs[di].ep_in);

    if (!X->iarm[slot]) {
        xhci_trb_t t;
        memset(&t, 0, sizeof(t));
        t.p0     = (uint32_t)(uintptr_t)buf;
        t.status = len;
        t.ctrl   = XHCI_TC_TYPE(XHCI_TRB_NORMAL) | XHCI_TC_IOC;
        xhci_ring_trb(X->epir[slot-1], &X->eii[slot-1],
                      &X->eic[slot-1], &t);
        xdb_wr((uint8_t)slot, dci);
        g_xint_doorbells++;
        X->iarm[slot] = true;
    }

    xhci_pump_events();
    if (!X->idone[slot]) { if (got) *got = 0; return -3; }  /* nothing yet */

    X->idone [slot] = false;
    X->iarm[slot] = false;    /* re-arm on the next poll */
    int cc = X->icc[slot];
    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT) {
        if (got) *got = len - X->ires[slot];
        return 0;
    }
    if (got) *got = 0;
    return -cc;
}

/* ============================================================================
 * Brutal xHCI state dump — surfaced by `usbnative` so a single photo shows the
 * controller's real state when interrupt reports don't flow.  No guessing.
 * ============================================================================ */
int xhci_brutal_dump(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    size_t n = 0;
    if (!X->op) return ksnprintf(out, cap, "xHCI: not initialised (X->op=0)\n");

    uint32_t cmd = xop_rd(XHCI_OP_USBCMD);
    uint32_t sts = xop_rd(XHCI_OP_USBSTS);
    n += ksnprintf(out + n, cap - n,
        "xHCI USBCMD=%08x RUN=%d  USBSTS=%08x HCH=%d HSE=%d EINT=%d PCD=%d HCE=%d CNR=%d\n",
        cmd, (cmd & XHCI_CMD_RUN) ? 1 : 0,
        sts, (sts >> 0) & 1, (sts >> 2) & 1, (sts >> 3) & 1, (sts >> 4) & 1,
        (sts >> 12) & 1, (sts >> 11) & 1);

    uint32_t erdp = *(volatile uint32_t *)(uintptr_t)(X->rt + 0x20 + 0x18);
    uint32_t iman = *(volatile uint32_t *)(uintptr_t)(X->rt + 0x20 + 0x00);
    n += ksnprintf(out + n, cap - n,
        "EVT idx=%d cyc=%d ERDP=%08x IMAN=%08x  doorbells=%u events=%u xferEvt=%u\n",
        X->eci, X->ecc, erdp, iman,
        g_xint_doorbells, g_xint_events, g_xint_xfer_evts);
    n += ksnprintf(out + n, cap - n,
        "LAST-EVT ctrl=%08x sts=%08x p0=%08x  type=%u cc=%u slot=%u\n",
        g_xint_last_ctrl, g_xint_last_sts, g_xint_last_p0,
        (g_xint_last_ctrl >> 10) & 0x3F, (g_xint_last_sts >> 24) & 0xFF,
        (g_xint_last_ctrl >> 24) & 0xFF);

    for (int s = 1; s <= XHCI_NSLOTS; s++) {
        int8_t di = X->slot_dev[s];
        if (di < 0 || !g_devs[di].in_use) continue;
        usb_device_t *d = &g_devs[di];
        uint8_t dci = d->ep_in ? xhci_dci(d->ep_in) : 0;
        uint32_t epd0 = 0, epinfo = 0, deqlo = 0;
        if (dci) {
            volatile uint8_t *ec = X->devctx[s-1] + (uint32_t)dci * 32u;
            epd0   = *(volatile uint32_t *)(ec + 0);
            epinfo = *(volatile uint32_t *)(ec + 4);
            deqlo  = *(volatile uint32_t *)(ec + 8);
        }
        n += ksnprintf(out + n, cap - n,
            "slot%d in=%02x proto=%u iv=%u ifaces=%u dci=%u epState=%u epType=%u mps=%u deq=%08x armed=%d done=%d cc=%d\n",
            s, d->ep_in, d->iface_protocol, d->ep_in_interval, d->n_ifaces,
            dci, epd0 & 0x7, (epinfo >> 3) & 0x7,
            (epinfo >> 16) & 0xFFFF, deqlo,
            X->iarm[s], X->idone[s], X->icc[s]);
    }

    /* PORTSC for connected ports */
    for (int p = 0; p < 16; p++) {
        uint32_t psc = *(volatile uint32_t *)(uintptr_t)(X->op + XHCI_OP_PORTSC(p));
        if (!(psc & XHCI_PORTSC_CCS)) continue;
        n += ksnprintf(out + n, cap - n,
            "PORTSC[%d]=%08x CCS=%d PED=%d spd=%u\n",
            p, psc, (psc >> 0) & 1, (psc >> 1) & 1, XHCI_PORTSC_SPEED(psc));
    }

    /* Raw event-ring window (to spot stuck/wrong-cycle events) */
    n += ksnprintf(out + n, cap - n, "EVTRING from idx %d:\n", X->eci);
    for (int i = 0; i < 6 && n < cap - 1; i++) {
        int idx = (X->eci + i) % XHCI_EVTR_SZ;
        xhci_trb_t *e = &X->evtr[idx];
        n += ksnprintf(out + n, cap - n, "  [%2d] p0=%08x st=%08x ct=%08x\n",
                       idx, e->p0, e->status, e->ctrl);
    }
    return (int)n;
}

/* ---- Slot management ---- */
static int xhci_enable_slot(void) {
    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.ctrl = XHCI_TC_TYPE(XHCI_TRB_EN_SLOT);
    uint8_t cc = 0;
    if (xhci_submit_cmd(&cmd, &cc) != 0) {
        debug_printf("[xhci] Enable Slot failed cc=%u\n", cc);
        return -1;
    }
    /* Slot ID is in bits [31:24] of the command completion event p1 field.
     * We get it from the last event consumed — X->eci was already advanced.
     * Re-read the slot from the just-consumed event (X->eci-1 or EVTR_SZ-1). */
    int last = (X->eci == 0) ? XHCI_EVTR_SZ - 1 : X->eci - 1;
    int slot = (int)((X->evtr[last].ctrl >> 24) & 0xFFu);
    if (slot < 1 || slot > XHCI_NSLOTS) {
        debug_printf("[xhci] Enable Slot returned bad slot=%d\n", slot);
        return -1;
    }
    return slot;
}

static int xhci_address_device(int slot, int port, uint8_t xspeed) {
    /* Build Input Context for Address Device command */
    memset(&X->inpctx, 0, sizeof(X->inpctx));
    X->inpctx.icc.add = XHCI_CF_SLOT | XHCI_CF_EP(1); /* slot + EP0 */

    /* Slot context */
    xhci_slot_ctx_t *sl = &X->inpctx.slot;
    sl->route_speed = ((uint32_t)xspeed << 20) | (1u << 27); /* speed, 2 ctx entries */
    sl->lat_port    = (uint32_t)(port + 1) << 16;             /* root hub port num */

    /* EP0 context (DCI 1 = ep[0]) */
    xhci_ep_ctx_t *ep0 = &X->inpctx.ep[0];
    uint16_t mps = (xspeed == XHCI_SPD_SS) ? 512 : (xspeed == XHCI_SPD_HS) ? 64 : 8;
    ep0->ep_info  = (4u << 3) | (0u << 8) | ((uint32_t)mps << 16); /* type=CTRL,burst=0 */
    ep0->tr_deq_lo = (uint32_t)(uintptr_t)X->ep0r[slot-1] | 1u; /* DCS=1 */
    ep0->avg_len   = 8;

    /* Register DCBAA entry for this slot */
    X->dcbaa[slot] = (uint64_t)(uintptr_t)X->devctx[slot-1];

    /* Address Device command */
    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.p0   = (uint32_t)(uintptr_t)&X->inpctx;
    cmd.ctrl = XHCI_TC_TYPE(XHCI_TRB_ADDR_DEV) | XHCI_TC_SLOT(slot);
    /* BSR=0: xHCI sends SET_ADDRESS to device automatically */
    uint8_t cc = 0;
    if (xhci_submit_cmd(&cmd, &cc) != 0) {
        debug_printf("[xhci] Address Device slot=%d failed cc=%u\n", slot, cc);
        return -1;
    }
    return 0;
}

/* intr=true configures the IN endpoint as INTERRUPT-IN (HID keyboards/mice);
 * otherwise BULK-IN/OUT (mass storage).  ep_out_addr==0 => no OUT endpoint
 * (the common HID case), so only the IN context is added. */
static int xhci_configure_eps(int slot, uint8_t ep_in_addr, uint16_t in_mps,
                               uint8_t ep_out_addr, uint16_t out_mps, bool intr) {
    memset(&X->inpctx, 0, sizeof(X->inpctx));
    uint8_t dci_in  = xhci_dci(ep_in_addr);
    uint8_t dci_out = ep_out_addr ? xhci_dci(ep_out_addr) : 0;

    /* Per xHCI spec §4.6.6: EP0 Add flag (bit 1) must be 0 for CFG_EP */
    X->inpctx.icc.add = XHCI_CF_SLOT | XHCI_CF_EP(dci_in)
                         | (dci_out ? XHCI_CF_EP(dci_out) : 0u);

    /* Copy current slot context and update Context Entries (= index of last DCI) */
    X->inpctx.slot = *(xhci_slot_ctx_t *)X->devctx[slot-1];
    uint8_t max_dci = (dci_in > dci_out) ? dci_in : dci_out;
    X->inpctx.slot.route_speed =
        (X->inpctx.slot.route_speed & 0x07FFFFFFu)
        | ((uint32_t)max_dci << 27);

    /* IN endpoint: Interrupt-IN (type 7) for HID, else Bulk-IN (type 6). */
    xhci_ep_ctx_t *ei = &X->inpctx.ep[dci_in - 1];
    uint32_t in_type = intr ? 7u : 6u;
    ei->ep_info  = (in_type << 3) | (0u << 8) | ((uint32_t)in_mps << 16);
    /* Interrupt endpoints need a polling Interval (ep_state[23:16]); ~8 ms is
     * fine for a boot mouse/keyboard.  CErr=3 in ep_info[2:1]. */
    ei->ep_state = intr ? (6u << 16) : 0u;
    ei->ep_info |= (3u << 1);   /* CErr = 3 */
    ei->tr_deq_lo = (uint32_t)(uintptr_t)X->epir[slot-1] | 1u;
    /* dword4: [15:0]=Average TRB Length, [31:16]=Max ESIT Payload Lo.  For a
     * PERIODIC (interrupt) endpoint the controller allocates bus bandwidth
     * from Max ESIT Payload; leaving it 0 means "no service time" and the
     * endpoint is NEVER polled on real (Intel/ASMedia) xHCI -> 0 reports,
     * even though QEMU's lax model worked.  Set it to the max packet size. */
    ei->avg_len = intr ? (((uint32_t)in_mps << 16) | in_mps) : in_mps;

    /* OUT endpoint (mass storage): Bulk-OUT (type 2).  Skipped for HID. */
    if (dci_out) {
        xhci_ep_ctx_t *eo = &X->inpctx.ep[dci_out - 1];
        eo->ep_info  = (2u << 3) | (0u << 8) | ((uint32_t)out_mps << 16);
        eo->ep_info |= (3u << 1);
        eo->tr_deq_lo = (uint32_t)(uintptr_t)X->epor[slot-1] | 1u;
        eo->avg_len   = out_mps;
    }

    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.p0   = (uint32_t)(uintptr_t)&X->inpctx;
    cmd.ctrl = XHCI_TC_TYPE(XHCI_TRB_CFG_EP) | XHCI_TC_SLOT(slot);
    uint8_t cc = 0;
    g_xhci_last_epcc = 0;
    if (xhci_submit_cmd(&cmd, &cc) != 0) {
        debug_printf("[xhci] Configure EP slot=%d failed cc=%u\n", slot, cc);
        g_xhci_last_epcc = cc ? cc : 0xFE;   /* remember failure for diag */
        return -1;
    }
    g_xhci_last_epcc = 1;   /* success */
    return 0;
}

/* ---- Control transfer via EP0 transfer ring ---- */
static int xhci_ctrl_xfer(int slot, uint8_t bmt, uint8_t bReq,
                           uint16_t wVal, uint16_t wIdx, void *buf, uint16_t len) {
    int *idx = &X->ep0i[slot-1];
    uint8_t *cyc = &X->ep0c[slot-1];
    xhci_trb_t *ring = X->ep0r[slot-1];

    /* Setup TRB (immediate data: 8-byte SETUP packet in p0/p1) */
    xhci_trb_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.p0 = (uint32_t)bmt | ((uint32_t)bReq << 8)
             | ((uint32_t)wVal << 16);
    setup.p1 = (uint32_t)wIdx | ((uint32_t)len << 16);
    setup.status = 8; /* TRB Transfer Length = 8 */
    uint8_t trt = (len == 0) ? 0u : (bmt & 0x80u) ? 3u : 2u; /* 3=IN, 2=OUT, 0=No */
    setup.ctrl = XHCI_TC_TYPE(XHCI_TRB_SETUP) | XHCI_TC_IDT | XHCI_TC_TRT(trt);
    xhci_ring_trb(ring, idx, cyc, &setup);

    /* Data TRB (if any) */
    if (len > 0 && buf) {
        xhci_trb_t data;
        memset(&data, 0, sizeof(data));
        data.p0    = (uint32_t)(uintptr_t)buf;
        data.status = (uint32_t)len;
        data.ctrl   = XHCI_TC_TYPE(XHCI_TRB_DATA)
                    | ((bmt & 0x80u) ? XHCI_TC_DIR : 0u)
                    | XHCI_TC_IOC;
        xhci_ring_trb(ring, idx, cyc, &data);
    }

    /* Status TRB */
    xhci_trb_t status;
    memset(&status, 0, sizeof(status));
    /* Status direction: opposite of data direction.  If no data: IN direction. */
    uint32_t sdir = (len == 0 || !(bmt & 0x80u)) ? XHCI_TC_DIR : 0u;
    status.ctrl = XHCI_TC_TYPE(XHCI_TRB_STATUS) | XHCI_TC_IOC | sdir;
    xhci_ring_trb(ring, idx, cyc, &status);

    /* Ring EP0 doorbell (DCI = 1) */
    xdb_wr((uint8_t)slot, 1);

    /* Wait for transfer completion (or timeout) */
    int r = xhci_wait_xfer(NULL);
    /* Setup TRB doesn't generate an event; we get one for Data + Status.
     * With IOC on both Data and Status we may get two events.
     * Consume the second one if needed. */
    if (len > 0 && buf) {
        uint32_t dummy;
        xhci_wait_xfer(&dummy);  /* consume Status event; ignore result */
    }
    return (r >= 0) ? 0 : r;
}

/* ---- Bulk transfer ---- */
static int xhci_bulk_xfer_in(int slot, void *buf, uint32_t len, uint32_t *got) {
    xhci_trb_t *ring = X->epir[slot-1];
    int *idx = &X->eii[slot-1];
    uint8_t *cyc = &X->eic[slot-1];

    xhci_trb_t t;
    memset(&t, 0, sizeof(t));
    t.p0     = (uint32_t)(uintptr_t)buf;
    t.status = len;
    t.ctrl   = XHCI_TC_TYPE(XHCI_TRB_NORMAL) | XHCI_TC_IOC;
    xhci_ring_trb(ring, idx, cyc, &t);

    uint8_t dci = 3; /* default: EP1 IN = DCI 3 */
    int8_t di = X->slot_dev[slot];
    if (di >= 0 && g_devs[di].in_use && g_devs[di].ep_in)
        dci = xhci_dci(g_devs[di].ep_in);
    xdb_wr((uint8_t)slot, dci);

    uint32_t residue = 0;
    int r = xhci_wait_xfer(&residue);
    if (got) *got = (r >= 0) ? (len - residue) : 0;
    return (r >= 0) ? 0 : r;
}

static int xhci_bulk_xfer_out(int slot, const void *buf, uint32_t len) {
    xhci_trb_t *ring = X->epor[slot-1];
    int *idx = &X->eoi[slot-1];
    uint8_t *cyc = &X->eoc[slot-1];

    xhci_trb_t t;
    memset(&t, 0, sizeof(t));
    t.p0     = (uint32_t)(uintptr_t)buf;
    t.status = len;
    t.ctrl   = XHCI_TC_TYPE(XHCI_TRB_NORMAL) | XHCI_TC_IOC;
    xhci_ring_trb(ring, idx, cyc, &t);

    uint8_t dci = 4; /* default: EP2 OUT = DCI 4 */
    int8_t di = X->slot_dev[slot];
    if (di >= 0 && g_devs[di].in_use && g_devs[di].ep_out)
        dci = xhci_dci(g_devs[di].ep_out);
    xdb_wr((uint8_t)slot, dci);

    int r = xhci_wait_xfer(NULL);
    return (r >= 0) ? 0 : r;
}

/* ---- Controller init ---- */
static bool xhci_reset(usb_controller_t *c) {
    uint32_t base = c->mmio_base;
    uint8_t  caplen = *(volatile uint8_t *)(uintptr_t)base;
    if (caplen < 0x10 || caplen > 0x80) {
        debug_printf("[xhci] bad CAPLENGTH=%u\n", caplen);
        return false;
    }

    /* Claim (or re-claim) a per-controller instance.  Re-resetting the same
     * controller reuses its slot; a board with more xHCIs than instances
     * logs and degrades to the old single-instance behaviour for extras. */
    if (c->xhci_hc < 0) {
        for (int i = 0; i < XHCI_MAX_HC; i++) {
            if (!g_xhc[i].in_use) { c->xhci_hc = (int8_t)i; break; }
        }
        if (c->xhci_hc < 0) {
            debug_printf("[xhci] no free hc instance (max %d) for %02x:%02x.%d\n",
                         XHCI_MAX_HC, c->bus, c->dev, c->fn);
            return false;
        }
    }
    X = &g_xhc[c->xhci_hc];
    X->in_use = true;
    debug_printf("[xhci] controller %02x:%02x.%d -> hc instance %d\n",
                 c->bus, c->dev, c->fn, c->xhci_hc);

    X->op = base + caplen;
    uint32_t dboff  = *(volatile uint32_t *)(uintptr_t)(base + XHCI_CAP_DBOFF);
    uint32_t rtsoff = *(volatile uint32_t *)(uintptr_t)(base + XHCI_CAP_RTSOFF);
    X->db = base + (dboff  & 0xFFFFFFF8u);
    X->rt = base + (rtsoff & 0xFFFFFFE0u);

    uint32_t hcsparams1 = *(volatile uint32_t *)(uintptr_t)(base + XHCI_CAP_HCSPARAMS1);
    uint8_t  max_ports  = (uint8_t)(hcsparams1 >> 24);
    uint8_t  max_slots  = (uint8_t)(hcsparams1 & 0xFFu);
    if (max_slots > XHCI_NSLOTS) max_slots = XHCI_NSLOTS;
    if (max_ports > USB_MAX_PORTS) max_ports = USB_MAX_PORTS;

    /* Stop the controller if running */
    uint32_t cmd = xop_rd(XHCI_OP_USBCMD);
    if (cmd & XHCI_CMD_RUN) {
        xop_wr(XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RUN);
        uint32_t t0 = pit_ms() + 100;
        while (pit_ms() < t0 && !(xop_rd(XHCI_OP_USBSTS) & XHCI_STS_HCH));
    }

    /* Reset */
    xop_wr(XHCI_OP_USBCMD, xop_rd(XHCI_OP_USBCMD) | XHCI_CMD_HCRST);
    uint32_t t0 = pit_ms() + 500;
    while (pit_ms() < t0) {
        if (!(xop_rd(XHCI_OP_USBCMD) & XHCI_CMD_HCRST) &&
            !(xop_rd(XHCI_OP_USBSTS) & XHCI_STS_CNR)) break;
    }
    if ((xop_rd(XHCI_OP_USBCMD) & XHCI_CMD_HCRST) ||
        (xop_rd(XHCI_OP_USBSTS) & XHCI_STS_CNR)) {
        debug_printf("[xhci] reset timeout\n");
        return false;
    }

    /* Initialize data structures */
    memset(X->dcbaa,     0, sizeof(X->dcbaa));
    memset(X->cmdr,  0, sizeof(X->cmdr));
    memset(X->evtr,  0, sizeof(X->evtr));
    memset(X->ep0r,  0, sizeof(X->ep0r));
    memset(X->epir, 0, sizeof(X->epir));
    memset(X->epor,0, sizeof(X->epor));
    memset(X->devctx,   0, sizeof(X->devctx));
    memset(X->slot_dev,   -1, sizeof(X->slot_dev));

    X->cpc = 1; X->cpi = 0;
    X->ecc = 1; X->eci = 0;
    memset(X->ep0c, 1, XHCI_NSLOTS);
    memset(X->eic,  1, XHCI_NSLOTS);
    memset(X->eoc,  1, XHCI_NSLOTS);
    memset(X->ep0i, 0, sizeof(X->ep0i));
    memset(X->eii,  0, sizeof(X->eii));
    memset(X->eoi,  0, sizeof(X->eoi));
    memset(X->iarm, 0, sizeof(X->iarm));
    memset(X->idone,  0, sizeof(X->idone));

    /* Set max slots */
    xop_wr(XHCI_OP_CONFIG, max_slots);

    /* Set DCBAA pointer */
    xop_wr64(XHCI_OP_DCBAAP, (uint64_t)(uintptr_t)X->dcbaa);

    /* Command Ring: set CRCR (RCS=1, pointer with cycle state) */
    xop_wr64(XHCI_OP_CRCR, (uint64_t)(uintptr_t)X->cmdr | 1u /* RCS=1 */);

    /* Event Ring: single segment */
    X->erst[0].base = (uint64_t)(uintptr_t)X->evtr;
    X->erst[0].size = XHCI_EVTR_SZ;
    X->erst[0].rsvd = 0;
    /* Interrupter 0 at RT+0x20 */
    xrt_wr(0x20 + 0x08, 1);   /* ERSTSZ = 1 segment */
    xrt_wr64(0x20 + 0x10, (uint64_t)(uintptr_t)X->erst);  /* ERSTBA */
    xrt_wr64(0x20 + 0x18, (uint64_t)(uintptr_t)X->evtr);   /* ERDP */

    /* Enable device notification for function remote wake */
    xop_wr(XHCI_OP_DNCTRL, 0x0002u);

    /* Power all ports and start controller */
    xop_wr(XHCI_OP_USBCMD, xop_rd(XHCI_OP_USBCMD) | XHCI_CMD_RUN);
    pit_sleep(50);

    /* Power all ports (PP bit) */
    for (int p = 0; p < max_ports; p++) {
        uint32_t psc = *(volatile uint32_t*)(uintptr_t)(X->op + XHCI_OP_PORTSC(p));
        if (!(psc & XHCI_PORTSC_PP))
            *(volatile uint32_t*)(uintptr_t)(X->op + XHCI_OP_PORTSC(p)) =
                (psc & ~XHCI_PORTSC_W1C) | XHCI_PORTSC_PP;
    }
    pit_sleep(100);  /* port power stable time */

    c->num_ports = max_ports;

    /* Record port attachment state */
    for (int p = 0; p < max_ports && p < USB_MAX_PORTS; p++) {
        uint32_t psc = *(volatile uint32_t*)(uintptr_t)(X->op + XHCI_OP_PORTSC(p));
        c->port_attached[p] = (psc & XHCI_PORTSC_CCS) != 0;
        c->port_was_attached[p] = c->port_attached[p];
    }

    debug_printf("[xhci] initialized: %d ports, %d slots\n", max_ports, max_slots);
    return true;
}

/* Enumerate a device on an xHCI root port.
 * Returns true on success.  Uses enumerate_device() for common logic. */
static bool xhci_enumerate_port(usb_controller_t *c, int port) {
    int ci = (int)(c - g_ctrls);
    if (c->xhci_hc < 0) return false;
    xhci_bind(c);
    uint32_t psc = *(volatile uint32_t*)(uintptr_t)(X->op + XHCI_OP_PORTSC(port));

    /* Reset the port */
    *(volatile uint32_t*)(uintptr_t)(X->op + XHCI_OP_PORTSC(port)) =
        (psc & ~XHCI_PORTSC_W1C) | XHCI_PORTSC_PR;
    uint32_t dl = pit_ms() + 200;
    while (pit_ms() < dl) {
        psc = *(volatile uint32_t*)(uintptr_t)(X->op + XHCI_OP_PORTSC(port));
        if (!(psc & XHCI_PORTSC_PR)) break;
    }
    pit_sleep(10);
    psc = *(volatile uint32_t*)(uintptr_t)(X->op + XHCI_OP_PORTSC(port));
    /* Clear PRC */
    *(volatile uint32_t*)(uintptr_t)(X->op + XHCI_OP_PORTSC(port)) =
        (psc & ~XHCI_PORTSC_W1C) | XHCI_PORTSC_PRC;

    /* Poll for the port to enable (PED).  A USB-2.0 (FS/LS/HS) device enables
     * automatically a short time after the reset completes; checking once can
     * race and wrongly report "not enabled". */
    uint32_t pdl = pit_ms() + 150;
    do {
        psc = *(volatile uint32_t*)(uintptr_t)(X->op + XHCI_OP_PORTSC(port));
        if (psc & XHCI_PORTSC_PED) break;
        pit_sleep(2);
    } while (pit_ms() < pdl);

    if (!(psc & XHCI_PORTSC_PED)) {
        debug_printf("[xhci] port %d not enabled after reset (psc=0x%08x)\n",
                     port, psc);
        usb_diag_add("p%d:noPED ", port);
        return false;
    }

    /* Speed: xHCI encoding → our 0=LS,1=FS,2=HS convention */
    uint8_t xspd = (uint8_t)XHCI_PORTSC_SPEED(psc);
    if (xspd < 1 || xspd > 4) xspd = XHCI_SPD_HS;  /* invalid read -> assume HS */
    uint8_t speed;
    if      (xspd == XHCI_SPD_LS) speed = 0;
    else if (xspd == XHCI_SPD_FS) speed = 1;
    else if (xspd == XHCI_SPD_HS) speed = 2;
    else                          speed = 2;   /* SS, treat as HS for now */

    debug_printf("[xhci] port %d: xspd=%u -> speed=%u\n", port, xspd, speed);

    /* Enable slot */
    int slot = xhci_enable_slot();
    if (slot < 0) { usb_diag_add("p%d:noSLOT ", port); return false; }

    /* Address device (xHCI sends SET_ADDRESS internally) */
    if (xhci_address_device(slot, port, xspd) != 0) {
        usb_diag_add("p%d(sp%u):ADDR-fail ", port, xspd);
        return false;
    }

    /* From here, use enumerate_device() which calls do_control() →
     * xhci_ctrl_xfer() for EHCI kind... but we need to route xHCI
     * control transfers through xhci_ctrl_xfer(slot, ...).
     * We temporarily mark the device at address 0 with the slot info
     * so do_control() can dispatch to xHCI. */

    /* Allocate a device entry manually */
    usb_device_t *dev = alloc_device(ci, port);
    if (!dev) { debug_printf("[xhci] device table full\n"); return false; }
    dev->speed       = speed;
    dev->tt_hub_addr = 0;
    dev->tt_hub_port = 0;
    dev->ep_in_mps   = (speed == 2) ? 64 : (speed == 1 ? 64 : 8);
    dev->ep_out_mps  = dev->ep_in_mps;
    /* Give xHCI device a slot-derived address */
    dev->address     = (uint8_t)slot;   /* slot == USB address in xHCI */
    X->slot_dev[slot] = (int8_t)(dev - g_devs);

    /* GET_DEVICE_DESCRIPTOR (8 bytes) */
    uint8_t desc[64];
    memset(desc, 0, sizeof(desc));
    if (xhci_ctrl_xfer(slot, 0x80, USB_REQ_GET_DESCRIPTOR,
                       (USB_DESC_DEVICE << 8), 0, desc, 8) != 0) {
        debug_printf("[xhci] initial GET_DESCRIPTOR failed\n");
        usb_diag_add("p%d(sp%u):GETDESC-fail ", port, xspd);
        free_device(dev); X->slot_dev[slot] = -1; return false;
    }
    dev->ep_in_mps  = desc[7] ? desc[7] : dev->ep_in_mps;
    dev->ep_out_mps = dev->ep_in_mps;

    /* GET_DEVICE_DESCRIPTOR full */
    if (xhci_ctrl_xfer(slot, 0x80, USB_REQ_GET_DESCRIPTOR,
                       (USB_DESC_DEVICE << 8), 0, desc, 18) != 0) {
        debug_printf("[xhci] full GET_DESCRIPTOR failed\n");
        free_device(dev); X->slot_dev[slot] = -1; return false;
    }
    const usb_device_descriptor_t *dd = (const usb_device_descriptor_t *)desc;
    dev->dev_class    = dd->bDeviceClass;
    dev->dev_subclass = dd->bDeviceSubClass;
    dev->dev_protocol = dd->bDeviceProtocol;
    dev->vendor_id    = dd->idVendor;
    dev->product_id   = dd->idProduct;

    /* GET_CONFIG_DESCRIPTOR — Full-Speed devices on xHCI occasionally return a
     * truncated/garbled config descriptor on the first try right after the
     * port reset.  On real HW this showed up as the mouse enumerating with
     * class=00 / 0 interfaces / "Unknown" (and then no HID reports), while the
     * very next run came up correctly (class=03 / 2 interfaces).  Retry the
     * read and only accept a config that parses to at least one interface,
     * rather than committing a bogus 0-interface device. */
    uint8_t cfg[256];
    bool cfg_ok = false;
    for (int attempt = 0; attempt < 4 && !cfg_ok; attempt++) {
        if (attempt) pit_sleep(10);
        memset(cfg, 0, sizeof(cfg));
        if (xhci_ctrl_xfer(slot, 0x80, USB_REQ_GET_DESCRIPTOR,
                           (USB_DESC_CONFIG << 8), 0, cfg, 9) != 0)
            continue;
        if (cfg[1] != USB_DESC_CONFIG) continue;     /* not a config descriptor */
        uint16_t total = ((uint16_t)cfg[3] << 8) | cfg[2];
        if (total < 9) continue;                     /* impossibly short      */
        if (total > sizeof(cfg)) total = sizeof(cfg);
        if (xhci_ctrl_xfer(slot, 0x80, USB_REQ_GET_DESCRIPTOR,
                           (USB_DESC_CONFIG << 8), 0, cfg, total) != 0)
            continue;
        parse_config(dev, cfg, total);
        if (dev->n_ifaces > 0) cfg_ok = true;
    }
    if (!cfg_ok) {
        debug_printf("[xhci] GET_CONFIG failed/garbled after retries\n");
        usb_diag_add("p%d(sp%u):CFG-retry-fail ", port, xspd);
        free_device(dev); X->slot_dev[slot] = -1; return false;
    }

    /* Configure the data endpoint(s).  HID (keyboard/mouse) has a single
     * interrupt-IN endpoint and no OUT; mass storage has bulk IN+OUT. */
    if (dev->ep_in) {
        bool intr = (dev->iface_class == USB_CLASS_HID);
        xhci_configure_eps(slot, dev->ep_in, dev->ep_in_mps,
                           dev->ep_out, dev->ep_out_mps, intr);
    }

    /* SET_CONFIGURATION */
    uint8_t cfg_val = cfg[5];   /* bConfigurationValue at offset 5 */
    xhci_ctrl_xfer(slot, 0x00, USB_REQ_SET_CONFIG, cfg_val, 0, NULL, 0);
    pit_sleep(2);

    /* Build product string */
    if (dev->iface_class == USB_CLASS_MSC)      memcpy(dev->product_str, "USB Mass Storage", 17);
    else if (dev->iface_class == USB_CLASS_HID) memcpy(dev->product_str, "USB HID Device",  15);
    else                                         memcpy(dev->product_str, "USB Device",      11);

    debug_printf("[xhci] enumerated slot=%d addr=%d class=0x%02x/0x%02x/0x%02x "
                 "vid=0x%04x pid=0x%04x ep_in=0x%02x mps=%u ep_out=0x%02x mps=%u\n",
                 slot, dev->address, dev->iface_class, dev->iface_subclass,
                 dev->iface_protocol, dev->vendor_id, dev->product_id,
                 dev->ep_in, dev->ep_in_mps, dev->ep_out, dev->ep_out_mps);

    /* Read the IN endpoint's state from the controller-owned Device Context
     * (32-byte contexts: EP context for DCI n is at offset n*32, dword0[2:0]
     * = EP State; 1 = Running).  If it isn't Running the controller will never
     * service the endpoint -> 0 interrupt reports. */
    uint8_t ep_state = 0;
    if (dev->ep_in) {
        uint8_t dci = xhci_dci(dev->ep_in);
        ep_state = (uint8_t)(*(volatile uint32_t *)
                       (X->devctx[slot-1] + (uint32_t)dci * 32u) & 0x7u);
    }
    usb_diag_add("p%d:OK(c%02x,pr%u,epcc%u,in%02x,iv%u,if%u,sp%u,m%u,st%u) ",
                 port, dev->iface_class, dev->iface_protocol, g_xhci_last_epcc,
                 dev->ep_in, dev->ep_in_interval, dev->n_ifaces,
                 dev->speed, dev->ep_in_mps, ep_state);
    if (g_hotplug) g_hotplug(dev, true);
    if (dev->iface_class == USB_CLASS_HUB || dev->dev_class == USB_CLASS_HUB)
        hub_init(dev);

    return true;
}

/* ---- Transfer dispatch for devices enumerated via xHCI ---- */
/* Called by do_control() / usb_bulk_* when c->kind == USB_CTRL_XHCI.
 * We look up the slot for the device and call the xHCI transfer functions. */
static int xhci_find_slot(const usb_device_t *dev) {
    /* Bind X to the hc that owns this device's controller: every slot-based
     * transfer (ctrl/bulk/intr) is immediately preceded by find_slot, so
     * this is the single choke point that keeps X correct per device. */
    if (dev->ctrl_idx >= 0 && dev->ctrl_idx < g_n_ctrls)
        xhci_bind(&g_ctrls[dev->ctrl_idx]);
    int di = (int)(dev - g_devs);
    for (int s = 1; s <= XHCI_NSLOTS; s++)
        if (X->slot_dev[s] == di) return s;
    return -1;
}

/* xHCI port polling: check PORTSC CSC bits for hot-plug events */
static int xhci_poll_ports(usb_controller_t *c) {
    if (c->kind != USB_CTRL_XHCI || !c->present || c->xhci_hc < 0) return 0;
    xhci_bind(c);
    int changes = 0;
    for (int p = 0; p < c->num_ports && p < USB_MAX_PORTS; p++) {
        volatile uint32_t *pscr = (volatile uint32_t*)(uintptr_t)(X->op + XHCI_OP_PORTSC(p));
        uint32_t psc = *pscr;
        if (!(psc & XHCI_PORTSC_CSC)) continue;
        /* Clear CSC */
        *pscr = (psc & ~XHCI_PORTSC_W1C) | XHCI_PORTSC_CSC;
        bool now = (psc & XHCI_PORTSC_CCS) != 0;
        if (now == c->port_was_attached[p]) continue;
        c->port_was_attached[p] = now;
        c->port_attached[p]     = now;
        changes++;
        if (now) {
            debug_printf("[xhci] port %d: device attached\n", p);
            xhci_enumerate_port(c, p);
        } else {
            debug_printf("[xhci] port %d: device detached\n", p);
            int ci = (int)(c - g_ctrls);
            for (int j = 0; j < USB_MAX_DEVICES; j++) {
                usb_device_t *d = &g_devs[j];
                if (d->in_use && d->ctrl_idx == ci && d->port == p) {
                    if (g_hotplug) g_hotplug(d, false);
                    int s = xhci_find_slot(d);
                    if (s >= 0) X->slot_dev[s] = -1;
                    free_device(d);
                }
            }
        }
    }
    return changes;
}

/* ---------- USB hub class driver --------------------------------------- *
 * Enumerates devices behind an external USB hub.  Many motherboards (and
 * QEMU) attach the 2nd+ USB device behind a hub rather than a root port, so
 * without this a mouse / pendrive next to a keyboard would be invisible.
 * Once a downstream device is powered + reset via the hub class requests it
 * sits at address 0 and the host controller talks to it DIRECTLY by address,
 * so the existing transfer paths work unchanged (no split-transaction TT
 * support yet — full-speed-behind-UHCI-hub and high-speed-behind-EHCI-hub,
 * the common cases, are covered). */
#define USB_DESC_HUB        0x29
#define HUB_REQ_GET_STATUS  0x00
#define HUB_REQ_CLR_FEATURE 0x01
#define HUB_REQ_SET_FEATURE 0x03
#define HUB_FEAT_PORT_RESET  4
#define HUB_FEAT_PORT_POWER  8
#define HUB_FEAT_C_RESET     20
#define HUB_FEAT_C_CONNECT   16   /* C_PORT_CONNECTION change feature */

/* Per-hub state used for hot-plug polling of downstream ports.
 * Hub 0 uses downstream port encoding 100..115, hub 1 uses 116..131, etc. */
#define USB_HUB_MAX    4
#define HUB_PORT_BASE  100
#define HUB_PORT_SPAN   16

typedef struct {
    bool    in_use;
    int     ctrl_idx;
    uint8_t hub_addr;
    int     nports;
    int     port_base;              /* first downstream port number */
    bool    port_was[HUB_PORT_SPAN];/* [1..15] last-known connected state */
} usb_hub_t;
static usb_hub_t  g_hub_tbl[USB_HUB_MAX];
static int g_hub_depth = 0;

static void hub_init(usb_device_t *hub) {
    if (!hub || g_hub_depth >= 5) return;
    g_hub_depth++;

    uint8_t hd[16];
    memset(hd, 0, sizeof(hd));
    if (do_control(hub, 0xA0, USB_REQ_GET_DESCRIPTOR,
                   (USB_DESC_HUB << 8), 0, hd, 9) != 0) {
        debug_printf("[usbhub] addr=%d: GET hub descriptor failed\n", hub->address);
        g_hub_depth--;
        return;
    }
    int nports = hd[2];
    if (nports < 1) nports = 1;
    if (nports > HUB_PORT_SPAN - 1) nports = HUB_PORT_SPAN - 1;
    uint32_t pwr_ms = (uint32_t)hd[5] * 2u + 20u;
    debug_printf("[usbhub] addr=%d: %d downstream port(s)\n", hub->address, nports);

    /* Claim a slot in the hub table so downstream ports get unique encodings. */
    int hi = 0;
    for (int h = 0; h < USB_HUB_MAX; h++) {
        if (!g_hub_tbl[h].in_use) { hi = h; break; }
    }
    int port_base = HUB_PORT_BASE + hi * HUB_PORT_SPAN;

    /* Power every port, wait, then enumerate what's connected. */
    for (int p = 1; p <= nports; p++)
        do_control(hub, 0x23, HUB_REQ_SET_FEATURE, HUB_FEAT_PORT_POWER, p, NULL, 0);
    pit_sleep(pwr_ms > 200 ? 200 : pwr_ms);

    for (int p = 1; p <= nports; p++) {
        uint8_t st[4] = {0,0,0,0};
        if (do_control(hub, 0xA3, HUB_REQ_GET_STATUS, 0, p, st, 4) != 0) continue;
        bool connected = (st[0] & 0x01) != 0;
        g_hub_tbl[hi].port_was[p] = connected;
        if (!connected) continue;

        do_control(hub, 0x23, HUB_REQ_SET_FEATURE, HUB_FEAT_PORT_RESET, p, NULL, 0);
        pit_sleep(50);
        do_control(hub, 0x23, HUB_REQ_CLR_FEATURE, HUB_FEAT_C_RESET,    p, NULL, 0);
        pit_sleep(10);

        memset(st, 0, sizeof(st));
        if (do_control(hub, 0xA3, HUB_REQ_GET_STATUS, 0, p, st, 4) != 0) continue;
        if (!(st[0] & 0x02)) continue;                 /* PORT_ENABLE */

        /* Detect device speed from port status (USB 2.0 §11.24.2.7):
         * wPortStatus byte 1 bit 1 = PORT_LOW_SPEED, bit 2 = PORT_HIGH_SPEED */
        uint8_t pspeed;
        if      (st[1] & 0x02) pspeed = 0;  /* Low */
        else if (st[1] & 0x04) pspeed = 2;  /* High */
        else                   pspeed = 1;  /* Full */

        /* TT is needed for FS/LS behind an EHCI hub only. */
        usb_controller_t *hctrl = &g_ctrls[hub->ctrl_idx];
        uint8_t tt_addr = (hctrl->kind == USB_CTRL_EHCI && pspeed != 2)
                          ? hub->address : 0;
        uint8_t tt_port = tt_addr ? (uint8_t)p : 0;

        debug_printf("[usbhub] addr=%d port %d: device present, speed=%d%s\n",
                     hub->address, p, pspeed,
                     tt_addr ? " (split-TT)" : "");
        enumerate_device(hub->ctrl_idx, port_base + p, tt_addr, tt_port, pspeed);
    }

    /* Register this hub for downstream hot-plug polling. */
    g_hub_tbl[hi].in_use    = true;
    g_hub_tbl[hi].ctrl_idx  = hub->ctrl_idx;
    g_hub_tbl[hi].hub_addr  = hub->address;
    g_hub_tbl[hi].nports    = nports;
    g_hub_tbl[hi].port_base = port_base;
    g_hub_depth--;
}

static bool enumerate_device(int ctrl_idx, int port,
                              uint8_t tt_hub_addr, uint8_t tt_hub_port,
                              uint8_t speed) {
    usb_device_t *dev = alloc_device(ctrl_idx, port);
    if (!dev) {
        debug_printf("[usb] device table full, ignoring new device\n");
        return false;
    }
    dev->speed        = speed;
    dev->tt_hub_addr  = tt_hub_addr;
    dev->tt_hub_port  = tt_hub_port;
    dev->ep_in_mps  = 64;
    dev->ep_out_mps = 64;

    /* GET_DEVICE_DESCRIPTOR (first 8 bytes) at address 0. */
    uint8_t desc[64];
    memset(desc, 0, sizeof(desc));
    if (do_control(dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                   (USB_DESC_DEVICE << 8), 0, desc, 8) != 0) {
        debug_printf("[usb] enumeration: initial GET_DESCRIPTOR failed\n");
        free_device(dev);
        return false;
    }
    dev->ep_in_mps  = desc[7] ? desc[7] : 64;
    dev->ep_out_mps = dev->ep_in_mps;

    /* SET_ADDRESS to a fresh address. */
    uint8_t addr = g_next_address++;
    if (do_control(dev, 0x00, USB_REQ_SET_ADDRESS, addr, 0, NULL, 0) != 0) {
        debug_printf("[usb] enumeration: SET_ADDRESS %u failed\n", addr);
        free_device(dev);
        return false;
    }
    dev->address = addr;
    pit_sleep(2);

    /* GET_DEVICE_DESCRIPTOR in full. */
    if (do_control(dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                   (USB_DESC_DEVICE << 8), 0, desc, 18) != 0) {
        debug_printf("[usb] enumeration: full device descriptor failed\n");
        free_device(dev);
        return false;
    }
    const usb_device_descriptor_t *dd = (const usb_device_descriptor_t *)desc;
    dev->dev_class    = dd->bDeviceClass;
    dev->dev_subclass = dd->bDeviceSubClass;
    dev->dev_protocol = dd->bDeviceProtocol;
    dev->vendor_id    = dd->idVendor;
    dev->product_id   = dd->idProduct;

    /* GET_CONFIG_DESCRIPTOR (9-byte header then the full thing). */
    uint8_t cfg[256];
    memset(cfg, 0, sizeof(cfg));
    if (do_control(dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                   (USB_DESC_CONFIG << 8), 0, cfg, 9) != 0) {
        debug_printf("[usb] enumeration: config descriptor header failed\n");
        free_device(dev);
        return false;
    }
    const usb_config_descriptor_t *cd = (const usb_config_descriptor_t *)cfg;
    uint16_t total = cd->wTotalLength;
    if (total > sizeof(cfg)) total = sizeof(cfg);
    if (do_control(dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                   (USB_DESC_CONFIG << 8), 0, cfg, total) != 0) {
        debug_printf("[usb] enumeration: full config descriptor failed\n");
        free_device(dev);
        return false;
    }
    parse_config(dev, cfg, total);

    /* SET_CONFIGURATION 1. */
    if (do_control(dev, 0x00, USB_REQ_SET_CONFIG,
                   cd->bConfigurationValue, 0, NULL, 0) != 0) {
        debug_printf("[usb] enumeration: SET_CONFIGURATION failed\n");
        free_device(dev);
        return false;
    }
    pit_sleep(2);

    /* Build a printable name. */
    if (dev->iface_class == USB_CLASS_MSC)       memcpy(dev->product_str, "USB Mass Storage", 17);
    else if (dev->iface_class == USB_CLASS_HID)  memcpy(dev->product_str, "USB HID Device", 15);
    else                                         memcpy(dev->product_str, "USB Device", 11);

    debug_printf("[usb] enumerated dev addr=%d class=0x%02x/0x%02x/0x%02x "
                 "vid=0x%04x pid=0x%04x ep_in=0x%02x mps=%u ep_out=0x%02x mps=%u\n",
                 dev->address, dev->iface_class, dev->iface_subclass,
                 dev->iface_protocol, dev->vendor_id, dev->product_id,
                 dev->ep_in, dev->ep_in_mps, dev->ep_out, dev->ep_out_mps);

    if (g_hotplug) g_hotplug(dev, true);

    /* If this device is a hub, bring up everything plugged into it. */
    if (dev->iface_class == USB_CLASS_HUB || dev->dev_class == USB_CLASS_HUB)
        hub_init(dev);
    return true;
}

/* ============================================================================
 * Port polling
 * ============================================================================ */
static bool uhci_port_attached(usb_controller_t *c, int p) {
    uint16_t psc = inw(c->io_base + UHCI_PORTSC1 + p * 2);
    return (psc & UHCI_PORTSC_CCS) != 0;
}
static bool ehci_port_attached(usb_controller_t *c, int p) {
    volatile uint8_t *op = ehci_op_base(c);
    uint32_t psc = *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4);
    return (psc & EHCI_PORTSC_CCS) != 0;
}

int usb_poll(void) {
    int changes = 0;

    /* In firmware-legacy preserve mode the controllers are owned by BIOS SMM;
     * touching their PORTSC/reset paths would break the emulated keyboard.
     * Stay completely hands-off. */
    if (g_preserve_legacy) return 0;

    /* ---- xHCI hot-plug (handled by its own poll) ---- */
    for (int ci = 0; ci < g_n_ctrls; ci++) {
        usb_controller_t *c = &g_ctrls[ci];
        if (c->kind == USB_CTRL_XHCI) changes += xhci_poll_ports(c);
    }

    /* ---- Root-port hot-plug (UHCI/EHCI) ---- */
    for (int ci = 0; ci < g_n_ctrls; ci++) {
        usb_controller_t *c = &g_ctrls[ci];
        if (c->kind != USB_CTRL_UHCI && c->kind != USB_CTRL_EHCI) continue;
        for (int p = 0; p < c->num_ports; p++) {
            bool now = (c->kind == USB_CTRL_UHCI)
                       ? uhci_port_attached(c, p)
                       : ehci_port_attached(c, p);
            if (now == c->port_was_attached[p]) continue;
            c->port_was_attached[p] = now;
            c->port_attached[p]     = now;
            changes++;
            debug_printf("[usb] hotplug ctrl=%d port=%d %s\n",
                         ci, p, now ? "attached" : "detached");
            if (now) {
                if (c->kind == USB_CTRL_UHCI) {
                    uint16_t psc = inw(c->io_base + UHCI_PORTSC1 + p * 2);
                    outw(c->io_base + UHCI_PORTSC1 + p * 2,
                         (uint16_t)(psc | UHCI_PORTSC_RESET));
                    pit_sleep(50);
                    outw(c->io_base + UHCI_PORTSC1 + p * 2,
                         (uint16_t)((psc & ~UHCI_PORTSC_RESET) | UHCI_PORTSC_PE));
                    pit_sleep(10);
                } else {
                    volatile uint8_t *op = ehci_op_base(c);
                    uint32_t psc = *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4);
                    *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4) =
                        (psc & ~EHCI_PORTSC_PE) | EHCI_PORTSC_PR;
                    pit_sleep(50);
                    *(volatile uint32_t *)(op + EHCI_OP_PORTSC0 + p * 4) =
                        psc & ~EHCI_PORTSC_PR;
                    pit_sleep(10);
                }
                /* Root-port: EHCI=HS, UHCI=FS; no TT needed */
                uint8_t rspeed = (c->kind == USB_CTRL_UHCI) ? 1u : 2u;
                enumerate_device(ci, p, 0, 0, rspeed);
            } else {
                /* Detach: free root-port device; if it was a hub, cascade. */
                for (int i = 0; i < USB_MAX_DEVICES; i++) {
                    usb_device_t *dev = &g_devs[i];
                    if (!dev->in_use || dev->ctrl_idx != ci || dev->port != p) continue;
                    if (dev->iface_class == USB_CLASS_HUB ||
                        dev->dev_class  == USB_CLASS_HUB) {
                        for (int hi = 0; hi < USB_HUB_MAX; hi++) {
                            if (!g_hub_tbl[hi].in_use) continue;
                            if (g_hub_tbl[hi].ctrl_idx != ci ||
                                g_hub_tbl[hi].hub_addr  != dev->address) continue;
                            int pb = g_hub_tbl[hi].port_base;
                            for (int j = 0; j < USB_MAX_DEVICES; j++) {
                                usb_device_t *d = &g_devs[j];
                                if (d->in_use && d->ctrl_idx == ci &&
                                    d->port >= pb && d->port < pb + HUB_PORT_SPAN) {
                                    if (g_hotplug) g_hotplug(d, false);
                                    free_device(d);
                                }
                            }
                            g_hub_tbl[hi].in_use = false;
                            break;
                        }
                    }
                    if (g_hotplug) g_hotplug(dev, false);
                    free_device(dev);
                }
            }
        }
    }

    /* ---- Hub downstream port polling ---- */
    for (int hi = 0; hi < USB_HUB_MAX; hi++) {
        usb_hub_t *hs = &g_hub_tbl[hi];
        if (!hs->in_use) continue;

        usb_device_t *hub_dev = NULL;
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            if (g_devs[i].in_use && g_devs[i].ctrl_idx == hs->ctrl_idx &&
                g_devs[i].address == hs->hub_addr) {
                hub_dev = &g_devs[i];
                break;
            }
        }
        if (!hub_dev) { hs->in_use = false; continue; }

        for (int p = 1; p <= hs->nports; p++) {
            uint8_t st[4] = {0,0,0,0};
            if (do_control(hub_dev, 0xA3, HUB_REQ_GET_STATUS, 0, p, st, 4) != 0) continue;
            bool connected = (st[0] & 0x01) != 0;
            bool changed   = (st[2] & 0x01) != 0;   /* C_PORT_CONNECTION */
            if (!changed) continue;

            do_control(hub_dev, 0x23, HUB_REQ_CLR_FEATURE,
                       HUB_FEAT_C_CONNECT, p, NULL, 0);

            if (connected == hs->port_was[p]) continue;
            hs->port_was[p] = connected;
            changes++;

            int dev_port = hs->port_base + p;
            if (connected) {
                debug_printf("[usbhub] addr=%d port %d: hotplug attach\n",
                             hs->hub_addr, p);
                do_control(hub_dev, 0x23, HUB_REQ_SET_FEATURE,
                           HUB_FEAT_PORT_RESET, p, NULL, 0);
                pit_sleep(50);
                do_control(hub_dev, 0x23, HUB_REQ_CLR_FEATURE,
                           HUB_FEAT_C_RESET, p, NULL, 0);
                pit_sleep(10);
                memset(st, 0, sizeof(st));
                if (do_control(hub_dev, 0xA3, HUB_REQ_GET_STATUS, 0, p, st, 4) == 0
                    && (st[0] & 0x02)) {
                    uint8_t pspeed;
                    if      (st[1] & 0x02) pspeed = 0;
                    else if (st[1] & 0x04) pspeed = 2;
                    else                   pspeed = 1;
                    usb_controller_t *hctrl2 = &g_ctrls[hs->ctrl_idx];
                    uint8_t tt2 = (hctrl2->kind == USB_CTRL_EHCI && pspeed != 2)
                                  ? hs->hub_addr : 0;
                    enumerate_device(hs->ctrl_idx, dev_port,
                                     tt2, tt2 ? (uint8_t)p : 0, pspeed);
                }
            } else {
                debug_printf("[usbhub] addr=%d port %d: hotplug detach\n",
                             hs->hub_addr, p);
                for (int j = 0; j < USB_MAX_DEVICES; j++) {
                    usb_device_t *d = &g_devs[j];
                    if (d->in_use && d->ctrl_idx == hs->ctrl_idx &&
                        d->port == dev_port) {
                        if (g_hotplug) g_hotplug(d, false);
                        free_device(d);
                    }
                }
            }
        }
    }

    return changes;
}

void usb_set_hotplug_cb(usb_hotplug_cb_t cb) { g_hotplug = cb; }

/* ============================================================================
 * BIOS USB-legacy ownership detection
 * ----------------------------------------------------------------------------
 * On a CSM / "Legacy USB Support = Enabled" machine the firmware SMM handler
 * owns the EHCI (or UHCI) controller and emulates the USB keyboard + mouse as
 * a PS/2 device on ports 0x60/0x64.  NexxoN's PS/2 driver consumes that
 * emulation, which is precisely why the keyboard works in the BIOS, in GRUB
 * and during early boot.
 *
 * If the OS issues HCRESET on that controller WITHOUT first taking ownership
 * through the USBLEGSUP semaphore, the SMM emulation is yanked out from under
 * the firmware while the OS never cleanly takes over -> keyboard/mouse go dead
 * and downstream ports can drop VBUS.  This is the observed ASUS P8Z77 symptom
 * (USB-3.0-only ports, xHCI = Smart Auto, USB legacy enabled): the moment the
 * desktop comes up the hub/pendrive LEDs go dark and input stops responding.
 *
 * Detecting firmware ownership lets us PRESERVE the working input path instead
 * of half-resetting it into a dead state.
 * ============================================================================ */
static bool ehci_bios_owns(const usb_controller_t *c) {
    if (c->kind != USB_CTRL_EHCI || !c->mmio_base) return false;
    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)c->mmio_base;
    uint32_t hccp = *(volatile uint32_t *)(cap + EHCI_HCCPARAMS);
    uint8_t  eecp = (uint8_t)((hccp >> 8) & 0xFF);
    uint8_t  hop  = 0;
    /* EECP points into PCI config space; valid capabilities live at >= 0x40. */
    while (eecp >= 0x40 && hop < 16) {
        uint32_t v = pci_read32(c->bus, c->dev, c->fn, eecp);
        if ((v & 0xFF) == 0x01)                  /* USBLEGSUP capability id   */
            return (v & (1u << 16)) != 0;        /* HC BIOS Owned Semaphore   */
        eecp = (uint8_t)((v >> 8) & 0xFF);
        hop++;
    }
    return false;
}

static bool uhci_bios_owns(const usb_controller_t *c) {
    if (c->kind != USB_CTRL_UHCI) return false;
    /* UHCI legacy support reg (PCI 0xC0): the 60h/64h trap-enable bits being
     * set means the firmware is actively trapping USB for PS/2 emulation. */
    uint32_t lk = pci_read32(c->bus, c->dev, c->fn, 0xC0);
    return (lk & 0x00F0u) != 0;
}

/* True when ANY detected controller is still firmware-owned for legacy input. */
static bool usb_bios_legacy_active(void) {
    for (int i = 0; i < g_n_ctrls; i++) {
        if (ehci_bios_owns(&g_ctrls[i]) || uhci_bios_owns(&g_ctrls[i]))
            return true;
    }
    return false;
}

/* xHCI firmware-ownership probe (USB Legacy Support Capability in the MMIO
 * extended-capability list).  On Panther Point with "xHCI Mode = Enabled" the
 * USB-3.0 ports — and the keyboard plugged into them — are driven by the xHCI
 * controller under BIOS SMM legacy.  We never DRIVE xHCI here (it stays
 * skipped), but knowing the firmware owns it lets the preserve policy keep
 * every controller hands-off so the emulated keyboard survives.  Read-only. */
static bool xhci_mmio_bios_owns(uint32_t mmio_base) {
    if (!mmio_base) return false;
    uint32_t hccp1 = *(volatile uint32_t *)(uintptr_t)(mmio_base + 0x10);
    uint32_t xecp  = (hccp1 >> 16) & 0xFFFFu;     /* dword offset, 0 = none */
    uint32_t hop   = 0;
    while (xecp && hop < 64) {
        uintptr_t pa = (uintptr_t)mmio_base + (uintptr_t)xecp * 4u;
        uint32_t v = *(volatile uint32_t *)pa;
        if ((v & 0xFFu) == 0x01u)                 /* USB Legacy Support cap */
            return (v & (1u << 16)) != 0;         /* HC BIOS Owned Semaphore */
        uint32_t next = (v >> 8) & 0xFFu;         /* next cap, dword offset */
        if (!next) break;
        xecp += next;
        hop++;
    }
    return false;
}

/* xHCI BIOS->OS hand-off: walk the MMIO extended-capability list to the USB
 * Legacy Support cap, set the OS-Owned semaphore, wait for the BIOS-Owned bit
 * to clear, then disable all SMI sources.  This is what makes the firmware
 * stop emulating the keyboard so we can drive the USB-3.0 devices natively. */
static void xhci_take_ownership(usb_controller_t *c) {
    if (c->kind != USB_CTRL_XHCI || !c->mmio_base) return;
    uint32_t hccp1 = *(volatile uint32_t *)(uintptr_t)(c->mmio_base + 0x10);
    uint32_t xecp  = (hccp1 >> 16) & 0xFFFFu;
    uint32_t hop   = 0;
    while (xecp && hop < 64) {
        uintptr_t pa = (uintptr_t)c->mmio_base + (uintptr_t)xecp * 4u;
        volatile uint32_t *cap = (volatile uint32_t *)pa;
        if ((*cap & 0xFFu) == 0x01u) {            /* USB Legacy Support cap */
            *cap |= (1u << 24);                   /* HC OS Owned Semaphore  */
            for (int t = 0; t < 100; t++) {       /* wait up to ~100ms      */
                if (!(*cap & (1u << 16))) break;  /* BIOS-owned cleared     */
                pit_sleep(1);
            }
            /* USBLEGCTLSTS at cap+1 dword: clear SMI enables, ack statuses. */
            volatile uint32_t *ctlsts = (volatile uint32_t *)(pa + 4u);
            *ctlsts = (*ctlsts & ~0x000E0000u) | 0xE0000000u;
            break;
        }
        uint32_t next = (*cap >> 8) & 0xFFu;
        if (!next) break;
        xecp += next;
        hop++;
    }
}

/* Take controller ownership from BIOS *before* we reset it (spec-correct
 * ordering).  Only used on the native bring-up path, when firmware is NOT
 * actively emulating legacy input on this box. */
static void ehci_take_ownership(usb_controller_t *c) {
    if (c->kind != USB_CTRL_EHCI || !c->mmio_base) return;
    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)c->mmio_base;
    uint32_t hccp = *(volatile uint32_t *)(cap + EHCI_HCCPARAMS);
    uint8_t  eecp = (uint8_t)((hccp >> 8) & 0xFF);
    uint8_t  hop  = 0;
    while (eecp >= 0x40 && hop < 16) {
        uint32_t v = pci_read32(c->bus, c->dev, c->fn, eecp);
        if ((v & 0xFF) == 0x01) {                /* USBLEGSUP */
            pci_write32(c->bus, c->dev, c->fn, eecp, v | (1u << 24)); /* OS owned */
            for (int t = 0; t < 100; t++) {      /* wait up to ~100ms for BIOS  */
                uint32_t w = pci_read32(c->bus, c->dev, c->fn, eecp);
                if (!(w & (1u << 16))) break;    /* BIOS-owned cleared          */
                pit_sleep(1);
            }
            /* Disable all SMI sources in USBLEGCTLSTS (eecp+4). */
            pci_write32(c->bus, c->dev, c->fn, (uint8_t)(eecp + 4), 0);
            break;
        }
        eecp = (uint8_t)((v >> 8) & 0xFF);
        hop++;
    }
}

/* Hand every detected controller off from BIOS, reset it, and enumerate all
 * ports — including devices behind the chipset rate-matching hub (the hub
 * class driver + EHCI split transactions handle FS/LS HID).  Shared by
 * usb_init()'s native path and the runtime usb_force_native() takeover.
 * include_xhci drives the xHCI controller too (USB-3.0 ports) — kept opt-in so
 * early boot doesn't wedge on a quirky controller, but always on for the
 * runtime takeover where a hang is reboot-recoverable. */
/* Take ownership of one xHCI, reset it into a fresh per-controller instance,
 * report its ports into the takeover diag and enumerate whatever is already
 * attached.  Shared by the main bringup pass and the chipset second pass. */
static void xhci_claim_and_enumerate(usb_controller_t *c) {
    size_t dl = strlen(g_usb_diag);
    xhci_take_ownership(c);     /* xECP USBLEGSUP hand-off before reset */
    if (!xhci_reset(c)) {
        ksnprintf(g_usb_diag + dl, sizeof(g_usb_diag) - dl, "xhci:RESET-FAILED ");
        return;
    }
    /* Per-port connection + speed (read after reset; X->op is this HC). */
    dl += ksnprintf(g_usb_diag + dl, sizeof(g_usb_diag) - dl,
                    "xhci[%dp]:", c->num_ports);
    for (int p = 0; p < c->num_ports && p < USB_MAX_PORTS; p++) {
        uint32_t psc = *(volatile uint32_t*)(uintptr_t)
                           (X->op + XHCI_OP_PORTSC(p));
        if (!(psc & XHCI_PORTSC_CCS)) continue;
        uint8_t sp = (uint8_t)XHCI_PORTSC_SPEED(psc);
        const char *ss = (sp == XHCI_SPD_SS) ? "SS" : (sp == XHCI_SPD_HS) ? "HS"
                       : (sp == XHCI_SPD_FS) ? "FS" : (sp == XHCI_SPD_LS) ? "LS" : "?";
        dl += ksnprintf(g_usb_diag + dl, sizeof(g_usb_diag) - dl,
                        "p%d=%s%s ", p, ss,
                        (psc & XHCI_PORTSC_PED) ? "+en" : "");
    }
    for (int p = 0; p < c->num_ports && p < USB_MAX_PORTS; p++) {
        if (c->port_attached[p])
            xhci_enumerate_port(c, p);
    }
}

static void usb_native_bringup(bool include_xhci) {
    /* Every xHCI now owns its own instance (rings/contexts/register bases in
     * xhci_hc_t), so multiple controllers can run concurrently — resetting a
     * second one no longer wipes the first one's slots.  Capacity is
     * XHCI_MAX_HC; extras log RESET-FAILED and are skipped. */

    /* Prefer add-in xHCI host controllers (on a non-zero PCI bus) over the
     * chipset's own xHCI (bus 0).  On the target board the keyboard + mouse
     * live on an ASMedia add-in card, while the chipset xHCI hosts the boot
     * pendrive + external hub AND is the controller the firmware uses for
     * legacy/SMM input.  Resetting or halting that chipset controller during
     * the native takeover is what made the hub/pendrive go dark and killed the
     * keyboard.  When an add-in xHCI is present we leave the chipset xHCI
     * completely untouched (boot device + SMM keyboard survive); we only fall
     * back to it if no add-in xHCI exists. */
    bool have_addin_xhci = false;
    for (int ci = 0; ci < g_n_ctrls; ci++)
        if (g_ctrls[ci].kind == USB_CTRL_XHCI && g_ctrls[ci].bus != 0)
            have_addin_xhci = true;

    for (int ci = 0; ci < g_n_ctrls; ci++) {
        usb_controller_t *c = &g_ctrls[ci];
        if (c->kind == USB_CTRL_XHCI && !include_xhci) continue;
        if (have_addin_xhci && c->bus == 0) {
            /* The HID mouse/keyboard live on an add-in xHCI (non-zero PCI
             * bus).  Leave the ENTIRE chipset USB subsystem (bus 0: xHCI +
             * EHCI + UHCI) on the firmware: the external hub + the boot
             * pendrive sit on the chipset EHCI USB-2 ports, and the firmware
             * drives SMM legacy input through the chipset USB.  Resetting any
             * of it is what makes the hub + pendrive go dark after usbnative.
             * We only take over the add-in controller that has the mouse. */
            const char *cn = (c->kind == USB_CTRL_EHCI) ? "ehci"
                           : (c->kind == USB_CTRL_UHCI) ? "uhci"
                           : (c->kind == USB_CTRL_XHCI) ? "xhci" : "?";
            usb_diag_add("%s[chipset:left-on-SMM] ", cn);
            continue;
        }
        /* Enable I/O + MMIO + bus-master (BIOS may leave add-in cards off). */
        uint32_t cmd = pci_read32(c->bus, c->dev, c->fn, 0x04);
        pci_write32(c->bus, c->dev, c->fn, 0x04,
                    cmd | (1u << 0) | (1u << 1) | (1u << 2));
        const char *kn = (c->kind == USB_CTRL_EHCI) ? "ehci"
                       : (c->kind == USB_CTRL_UHCI) ? "uhci"
                       : (c->kind == USB_CTRL_XHCI) ? "xhci" : "?";
        size_t dl = strlen(g_usb_diag);
        /* Spec-correct ordering: take ownership from BIOS BEFORE HCRESET. */
        if (c->kind == USB_CTRL_EHCI) { ehci_take_ownership(c); ehci_reset(c); }
        if (c->kind == USB_CTRL_UHCI) {
            pci_write32(c->bus, c->dev, c->fn, 0xC0, 0x00008F00u); /* drop UHCI legacy */
            uhci_reset(c);
        }
        if (c->kind == USB_CTRL_XHCI) {
            /* Each claimed xHCI gets its OWN instance (event ring, command
             * ring, DCBAA, slot state) via xhci_reset, so multiple
             * controllers coexist; a BIOS-owned controller left running
             * writes into ITS firmware ring, never ours. */
            xhci_claim_and_enumerate(c);
        } else if (c->kind == USB_CTRL_EHCI || c->kind == USB_CTRL_UHCI) {
            int att = 0;
            for (int p = 0; p < c->num_ports && p < USB_MAX_PORTS; p++)
                if (c->port_attached[p]) att++;
            dl += ksnprintf(g_usb_diag + dl, sizeof(g_usb_diag) - dl,
                            "%s[%dp,%dconn] ", kn, c->num_ports, att);
        }
    }

    /* ---- Second pass: the chipset xHCI (bus 0) -------------------------
     * The main pass leaves ALL bus-0 USB on firmware SMM while an add-in
     * xHCI exists (input safety: until the add-in HIDs are natively
     * enumerated, the SMM keyboard is the only input path).  Once the
     * add-in pass HAS yielded a native HID device, the SMM input path is
     * redundant — claiming the chipset xHCI is then safe and unlocks
     * hot-plug on ALL of its USB3 ports (the majority of the board's
     * ports; pendrives there were previously invisible BY DESIGN).  The
     * chipset EHCI/UHCI (USB2) stay untouched as the conservative
     * fallback.  The boot medium is safe either way: the LIVE RAMFS is
     * fully memory-resident after boot. */
    if (include_xhci && have_addin_xhci) {
        bool native_hid = false;
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            if (g_devs[i].in_use && g_devs[i].iface_class == USB_CLASS_HID) {
                native_hid = true;
                break;
            }
        }
        for (int ci = 0; ci < g_n_ctrls; ci++) {
            usb_controller_t *c = &g_ctrls[ci];
            if (c->kind != USB_CTRL_XHCI || c->bus != 0 || c->xhci_hc >= 0)
                continue;
            if (!native_hid) {
                usb_diag_add("xhci[chipset:kept-on-SMM,no-native-hid] ");
                debug_printf("[usb] chipset xHCI left on SMM "
                             "(no native HID yet — input safety)\n");
                continue;
            }
            uint32_t cmd = pci_read32(c->bus, c->dev, c->fn, 0x04);
            pci_write32(c->bus, c->dev, c->fn, 0x04,
                        cmd | (1u << 0) | (1u << 1) | (1u << 2));
            usb_diag_add("xhci[chipset:claimed] ");
            debug_printf("[usb] native HID up — claiming chipset xHCI "
                         "%02x:%02x.%d for hot-plug\n", c->bus, c->dev, c->fn);
            xhci_claim_and_enumerate(c);
        }
    }
    debug_ok("usb: enumeration done");

    /* Enumerate every port that came up attached during the initial reset. */
    for (int ci = 0; ci < g_n_ctrls; ci++) {
        usb_controller_t *c = &g_ctrls[ci];
        if (c->kind == USB_CTRL_XHCI) continue; /* xHCI enumerated inside xhci_reset */
        if (c->kind != USB_CTRL_UHCI && c->kind != USB_CTRL_EHCI) continue;
        if (have_addin_xhci && c->bus == 0) continue; /* chipset left on SMM */
        for (int p = 0; p < c->num_ports; p++) {
            if (c->port_attached[p]) {
                uint8_t ispeed = (c->kind == USB_CTRL_UHCI) ? 1u : 2u;
                enumerate_device(ci, p, 0, 0, ispeed);
            }
        }
    }
}

/* ============================================================================
 * usb_init() — public entry point
 * ============================================================================ */
int usb_init(void) {
    debug_step("usb: scanning PCI for host controllers");
    pci_device_t devs[64];
    int n = pci_enumerate(devs, 64);
    g_n_ctrls = 0;
    memset(g_devs,    0, sizeof(g_devs));
    memset(g_hub_tbl, 0, sizeof(g_hub_tbl));
    g_n_devs = 0;
    bool xhci_fw_owned = false;   /* firmware owns an (un-driven) xHCI for legacy */
    for (int i = 0; i < n && g_n_ctrls < USB_MAX_CTRL; i++) {
        if (devs[i].class_code != 0x0C || devs[i].subclass != 0x03) continue;
        usb_controller_t *c = &g_ctrls[g_n_ctrls];
        memset(c, 0, sizeof(*c));
        c->bus  = devs[i].bus;
        c->dev  = devs[i].device;
        c->fn   = devs[i].function;
        c->vendor_id = devs[i].vendor_id;
        c->device_id = devs[i].device_id;
        c->irq  = (uint8_t)(pci_read32(c->bus, c->dev, c->fn, 0x3C) & 0xFF);
        switch (devs[i].prog_if) {
            case 0x00:
                c->kind = USB_CTRL_UHCI;
                c->io_base = (uint16_t)(devs[i].bar[4] & 0xFFFCu);
                debug_printf("[usb] UHCI  %04x:%04x  I/O base 0x%04x irq %d\n",
                             c->vendor_id, c->device_id, c->io_base, c->irq);
                break;
            case 0x10:
                c->kind = USB_CTRL_OHCI;
                c->mmio_base = devs[i].bar[0] & 0xFFFFFFF0u;
                debug_printf("[usb] OHCI  %04x:%04x  MMIO 0x%08x irq %d\n",
                             c->vendor_id, c->device_id, c->mmio_base, c->irq);
                break;
            case 0x20:
                c->kind = USB_CTRL_EHCI;
                c->mmio_base = devs[i].bar[0] & 0xFFFFFFF0u;
                debug_printf("[usb] EHCI  %04x:%04x  MMIO 0x%08x irq %d\n",
                             c->vendor_id, c->device_id, c->mmio_base, c->irq);
                break;
            case 0x30:
                /* Record the xHCI controller so the runtime `usbnative`
                 * takeover can drive it (the USB-3.0 ports — and the keyboard/
                 * mouse plugged into them under "xHCI Mode = Smart Auto/Enabled"
                 * — live here).  We do NOT reset it during early boot (it can
                 * wedge some Intel controllers); usb_native_bringup() only
                 * touches it when explicitly asked. */
                c->kind = USB_CTRL_XHCI;
                c->mmio_base = devs[i].bar[0] & 0xFFFFFFF0u;
                {
                    uint32_t xc = pci_read32(c->bus, c->dev, c->fn, 0x04);
                    pci_write32(c->bus, c->dev, c->fn, 0x04, xc | (1u << 1));
                    if (xhci_mmio_bios_owns(c->mmio_base)) xhci_fw_owned = true;
                }
                debug_printf("[usb] xHCI  %04x:%04x  MMIO 0x%08x irq %d "
                             "(runtime-only: use `usbnative`)\n",
                             c->vendor_id, c->device_id, c->mmio_base, c->irq);
                break;
            default:
                c->kind = USB_CTRL_UNKNOWN;
                debug_printf("[usb] unknown USB prog_if 0x%02x at %04x:%04x\n",
                             devs[i].prog_if, c->vendor_id, c->device_id);
                continue;
        }
        c->present = true;
        c->xhci_hc = -1;   /* no xHCI instance claimed yet */
        /* Enable memory + I/O decode (NOT bus-master, NOT reset) so the
         * ownership probe below can read the controller's registers.  This is
         * non-destructive and does not disturb firmware SMM legacy. */
        uint32_t cmd = pci_read32(c->bus, c->dev, c->fn, 0x04);
        pci_write32(c->bus, c->dev, c->fn, 0x04, cmd | (1u << 0) | (1u << 1));
        g_n_ctrls++;
    }
    if (g_n_ctrls == 0) {
        debug_printf("[usb] no USB host controllers found\n");
        return 0;
    }

    /* ---- Input-stability policy gate (bare-metal safety) ----
     * If the firmware still owns a controller for USB-legacy PS/2 emulation,
     * resetting it would kill the only working keyboard/mouse path.  Preserve
     * it: detect the controllers (so lsusb still lists them) but do NOT reset,
     * take ownership, or enumerate.  The PS/2 driver keeps consuming the SMM
     * emulation and the desktop stays controllable. */
    g_preserve_legacy = usb_bios_legacy_active() || xhci_fw_owned;
    if (g_preserve_legacy) {
        debug_printf("[usb] firmware USB-legacy active -> preserving BIOS "
                     "input path; skipping native controller reset\n");
        debug_ok("usb: BIOS legacy input preserved (no native reset)");
        return g_n_ctrls;
    }

    /* ---- Native bring-up (no firmware legacy in the way) ---- */
    usb_native_bringup(USB_ENABLE_XHCI_EARLY);
    return g_n_ctrls;
}

/* Runtime takeover: leave BIOS-legacy preserve mode and bring the USB stack up
 * NATIVELY -- hand the controllers off from firmware SMM, reset them, and
 * enumerate every device (keyboard, mouse, storage), including HID behind the
 * chipset rate-matching hub via the hub driver + split transactions.  This is
 * the same thing Windows/Linux do after boot, and the ONLY way to get a native
 * USB mouse on a box whose firmware emulates only the legacy keyboard.  Safe to
 * trigger from the shell: if anything goes wrong a reboot returns to the
 * working SMM keyboard.  Returns the number of USB devices now enumerated. */
int usb_force_native(void) {
    debug_printf("[usb] runtime native takeover requested\n");
    /* Drop any devices/hubs left over from a previous attempt. */
    memset(g_devs,    0, sizeof(g_devs));
    memset(g_hub_tbl, 0, sizeof(g_hub_tbl));
    g_n_devs = 0;
    g_usb_diag[0] = 0;
    g_preserve_legacy = false;
    usb_native_bringup(true);   /* runtime takeover drives xHCI too */
    debug_printf("[usb] native takeover done: %d device(s)\n", g_n_devs);
    return g_n_devs;
}

/* Non-destructive pre-flight for the automatic boot-time native takeover.
 * See usb.h.  Reads xHCI CAPLENGTH + HCSPARAMS1 + PORTSC straight from MMIO —
 * no writes, no ownership change, no reset — so it cannot disturb the firmware
 * SMM input path or the boot device.  Only an add-in xHCI (PCI bus != 0)
 * carrying a Full/Low-Speed device makes a takeover advisable: that keeps the
 * chipset USB (boot pendrive + the SMM keyboard it emulates) completely
 * untouched and guarantees there is a HID worth driving natively. */
int usb_native_takeover_advisable(char *why, size_t cap) {
    int  addin_xhci = 0;       /* add-in xHCI controllers (non-zero bus)   */
    int  fsls       = 0;       /* FS/LS connections on add-in xHCI (HID)   */
    int  hsss       = 0;       /* HS/SS connections (storage/hub, not HID) */
    for (int ci = 0; ci < g_n_ctrls; ci++) {
        usb_controller_t *c = &g_ctrls[ci];
        if (c->kind != USB_CTRL_XHCI || c->bus == 0) continue;
        addin_xhci++;
        uint32_t base   = c->mmio_base;
        if (!base) continue;
        uint8_t  caplen = *(volatile uint8_t *)(uintptr_t)base;
        if (caplen < 0x10 || caplen > 0x80) continue;     /* implausible cap */
        uint32_t op   = base + caplen;
        uint32_t hcs1 = *(volatile uint32_t *)(uintptr_t)(base + XHCI_CAP_HCSPARAMS1);
        int nports = (int)(uint8_t)(hcs1 >> 24);
        if (nports > USB_MAX_PORTS) nports = USB_MAX_PORTS;
        for (int p = 0; p < nports; p++) {
            uint32_t psc = *(volatile uint32_t *)(uintptr_t)(op + XHCI_OP_PORTSC(p));
            if (!(psc & XHCI_PORTSC_CCS)) continue;        /* nothing attached */
            uint8_t sp = (uint8_t)XHCI_PORTSC_SPEED(psc);
            if (sp == XHCI_SPD_FS || sp == XHCI_SPD_LS) fsls++;
            else                                        hsss++;
        }
    }
    if (addin_xhci == 0) {
        if (why) ksnprintf(why, cap,
            "no add-in xHCI (chipset-only USB) — leaving BIOS SMM input in place");
        return 0;
    }
    if (fsls == 0) {
        if (why) ksnprintf(why, cap,
            "add-in xHCI present but no Full/Low-Speed HID on its ports "
            "(%d high-speed device(s))", hsss);
        return 0;
    }
    if (why) ksnprintf(why, cap,
        "add-in xHCI carries %d HID-speed device(s) — safe to drive natively "
        "(chipset USB + boot device untouched)", fsls);
    return fsls;
}

/* On-screen per-controller port summary from the last takeover. */
const char *usb_takeover_diag(void) { return g_usb_diag[0] ? g_usb_diag : "(no ports reported)"; }

int  usb_controller_count(void) { return g_n_ctrls; }

/* True when usb_init() chose to preserve firmware USB-legacy input instead of
 * resetting the controllers (bare-metal CSM safety path). */
bool usb_legacy_preserved(void) { return g_preserve_legacy; }

bool usb_get_controller(int idx, usb_controller_t *out) {
    if (idx < 0 || idx >= g_n_ctrls || !out) return false;
    *out = g_ctrls[idx];
    return true;
}

int usb_device_count(void) { return g_n_devs; }

usb_device_t *usb_get_device(int idx) {
    if (idx < 0) return NULL;
    int seen = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!g_devs[i].in_use) continue;
        if (seen == idx) return &g_devs[i];
        seen++;
    }
    return NULL;
}

usb_device_t *usb_device_by_address(uint8_t address) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (g_devs[i].in_use && g_devs[i].address == address) return &g_devs[i];
    }
    return NULL;
}

bool usb_disable_legacy(void) {
    bool any = false;
    for (int i = 0; i < g_n_ctrls; i++) {
        usb_controller_t *c = &g_ctrls[i];
        if (c->kind == USB_CTRL_UHCI) {
            pci_write32(c->bus, c->dev, c->fn, 0xC0, 0x00008F00u);
            debug_printf("[usb] UHCI %04x:%04x: legacy emulation disabled\n",
                         c->vendor_id, c->device_id);
            any = true;
        }
        if (c->kind == USB_CTRL_EHCI) {
            /* Walk the EHCI extended capability list to find USBLEGSUP. */
            volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)c->mmio_base;
            uint32_t hccp = *(volatile uint32_t *)(cap + EHCI_HCCPARAMS);
            uint8_t eecp = (hccp >> 8) & 0xFF;
            uint8_t hop  = 0;
            while (eecp && hop < 16) {
                uint32_t v = pci_read32(c->bus, c->dev, c->fn, eecp);
                if ((v & 0xFF) == 0x01) {        /* USBLEGSUP */
                    pci_write32(c->bus, c->dev, c->fn, eecp,
                                (v | (1u << 24)) & ~(1u << 16));
                    debug_printf("[usb] EHCI %04x:%04x: legacy semaphore taken\n",
                                 c->vendor_id, c->device_id);
                    break;
                }
                eecp = (v >> 8) & 0xFF;
                hop++;
            }
            any = true;
        }
    }
    return any;
}

int usb_describe(const usb_device_t *dev, char *out, size_t cap) {
    if (!dev || !out || cap == 0) return 0;
    const char *class_name = "Unknown";
    if (dev->iface_class == USB_CLASS_HID)      class_name = "HID";
    else if (dev->iface_class == USB_CLASS_MSC) class_name = "Mass Storage";
    else if (dev->iface_class == USB_CLASS_HUB) class_name = "Hub";
    return ksnprintf(out, cap,
                     "addr=%-3d vid=%04x pid=%04x  %-13s  %s",
                     dev->address, dev->vendor_id, dev->product_id,
                     class_name, dev->product_str);
}

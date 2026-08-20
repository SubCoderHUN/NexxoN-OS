/* ============================================================================
 * NexxoN OS - AHCI 1.x SATA driver
 * ----------------------------------------------------------------------------
 *
 * MEMORY LAYOUT (per port - we only bring up one):
 *
 *   1024-byte command list  : 32 command-list headers (we only use slot 0).
 *    256-byte FIS receive   : where the HBA writes incoming SATA FISes.
 *    256-byte command table : Command-FIS (64 B) + ATAPI (16 B) + 48 B
 *                             reserved + one 16-byte PRDT entry that points
 *                             at the bounce buffer below.
 *   4 KiB DMA bounce buffer : reused for every transfer.  Large enough that
 *                             we could DMA up to 8 sectors in one go, but
 *                             our public API is single-sector for simplicity.
 *
 *  Everything lives in BSS with hard alignment (GCC __attribute__((aligned)))
 *  so the controller's physical-address registers can take the address of
 *  the array verbatim.  No paging means physical == virtual.
 *
 * I/O FLOW (read / write):
 *
 *   1. Build a register H2D FIS in the command table (LBA48, count=1).
 *   2. Configure cmd-list header 0: command FIS length, write/read flag,
 *      PRDT length = 1.
 *   3. Build a single PRDT entry pointing at g_dma_buf with DBC = 511 (one
 *      sector minus one).
 *   4. Spin until PxTFD.BSY/DRQ are clear, then issue by writing 1 into
 *      PxCI (command slot 0).
 *   5. Poll PxCI until bit 0 clears.  Bail with AHCI_ERR_IO if PxIS.TFES
 *      asserts (task-file error).
 *   6. For reads, memcpy from g_dma_buf -> caller buffer.
 *
 * Logged extensively over COM1 - every probe, IDENTIFY result, error, and
 * I/O timeout shows up so bare-metal post-mortem is doable on real hardware.
 * ============================================================================ */
#include "ahci.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "debug.h"
#include "panic.h"
#include "pit.h"

/* ---------- HBA memory layout (offsets from ABAR) ----------------------- */
typedef volatile struct PACKED {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
} hba_mem_t;

#define HBA_PORTS_OFFSET    0x100
#define HBA_PORT_STRIDE     0x80

typedef volatile struct PACKED {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t reserved0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t reserved1[11];
    uint32_t vendor[4];
} hba_port_t;

/* ---------- In-RAM AHCI data structures --------------------------------- */
typedef struct PACKED {
    uint16_t flags;          /* CFL[4:0] | A | W | P | R | B | C | rsv | PMP[3:0] */
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} ahci_cmd_header_t;

typedef struct PACKED {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_i;          /* bytes-1 in bits [21:0], I bit = (1<<31) */
} ahci_prdt_entry_t;

typedef struct PACKED {
    uint8_t           cfis[64];
    uint8_t           atapi[16];
    uint8_t           reserved[48];
    ahci_prdt_entry_t prdt[1];   /* we only ever use one entry */
} ahci_cmd_table_t;

typedef struct PACKED {
    uint8_t  fis_type;       /* 0x27 - Register H2D */
    uint8_t  pmport_c;       /* bit 7 = command (1) / control (0) */
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;
    uint8_t  lba3, lba4, lba5;
    uint8_t  featureh;
    uint8_t  countl, counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  reserved[4];
} fis_h2d_t;

/* ---------- AHCI register bit definitions ------------------------------- */
#define GHC_HR              (1u << 0)
#define GHC_IE              (1u << 1)
#define GHC_AE              (1u << 31)

/* GHC.HR self-clears once the HBA reset completes.  Per spec it must clear
 * within 1 second - on QEMU/VirtualBox it's effectively immediate. */
#define HBA_RESET_TIMEOUT   2000000u

#define PORT_CMD_ST         (1u << 0)
#define PORT_CMD_FRE        (1u << 4)
#define PORT_CMD_FR         (1u << 14)
#define PORT_CMD_CR         (1u << 15)

#define PORT_TFD_BSY        0x80
#define PORT_TFD_DRQ        0x08
#define PORT_TFD_ERR        0x01

#define PORT_IS_TFES        (1u << 30)

#define SATA_SIG_ATA        0x00000101
#define HBA_PORT_DET_PRESENT 3
#define HBA_PORT_IPM_ACTIVE  1

#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35

#define POLL_TIMEOUT        2000000u

/* ---------- Static, aligned in-memory work area -------------------------
 * AHCI 1.x mandates very specific alignments for the HBA-visible DMA
 * regions.  The compiler attribute below makes the linker (placing these
 * in BSS) honour them at load time:
 *   - g_cmd_list   : 1024-byte aligned  (PxCLB[9:0] == 0  required)
 *   - g_fis_recv   :  256-byte aligned  (PxFB [7:0] == 0  required)
 *   - g_cmd_table  :  128-byte aligned  (CTBA[6:0] == 0   required)
 *   - g_dma_buf    :    2-byte aligned in spec; we use 512 so any sector
 *                       boundary is naturally hit.
 * Compile-time _Static_assert guards the alignment values so a future tweak
 * to the constants cannot silently re-introduce an alignment bug. */
ALIGNED(1024) static uint8_t g_cmd_list   [1024];      /* 32 headers */
ALIGNED(256)  static uint8_t g_fis_recv   [256];
ALIGNED(128)  static uint8_t g_cmd_table  [256];       /* one slot's table */
ALIGNED(512)  static uint8_t g_dma_buf    [4096];

_Static_assert((sizeof(g_cmd_list)  & 0x3FF) == 0, "cmd list size must be 1024-byte multiple");
_Static_assert((sizeof(g_fis_recv)  & 0x0FF) == 0, "fis recv size must be 256-byte multiple");
_Static_assert((sizeof(g_cmd_table) & 0x07F) == 0, "cmd table size must be 128-byte multiple");

/* ---------- Driver state ------------------------------------------------ */
static hba_mem_t  *g_hba       = NULL;
static hba_port_t *g_port      = NULL;
static int         g_port_idx  = -1;
static uint32_t    g_sectors   = 0;
static bool        g_present   = false;

/* Compact one-line probe summary.  All detailed AHCI logging goes to COM1 via
 * debug_printf(), but on a modern board the user can only photograph the
 * framebuffer, never the serial port -- so we also accumulate the few
 * breadcrumbs that matter (controller location, CAP/PI, why no disk bound)
 * into this buffer and let kernel.c surface it ON SCREEN.  Keeps bare-metal
 * "AHCI sees nothing" post-mortem possible from a single photo. */
static char        g_diag[200] = {0};
static void diag_reset(void) { g_diag[0] = 0; }
static void diag_add(const char *fmt, ...) {
    size_t len = strlen(g_diag);
    if (len >= sizeof(g_diag) - 1) return;
    va_list ap; va_start(ap, fmt);
    kvsnprintf(g_diag + len, sizeof(g_diag) - len, fmt, ap);
    va_end(ap);
}
const char *ahci_diag(void) { return g_diag[0] ? g_diag : "ahci: not probed"; }

/* ---------- Helpers ----------------------------------------------------- */
static inline hba_port_t *port_at(int idx) {
    return (hba_port_t *)((uint8_t *)g_hba + HBA_PORTS_OFFSET + idx * HBA_PORT_STRIDE);
}

static int wait_clear(volatile uint32_t *reg, uint32_t mask) {
    for (uint32_t i = 0; i < POLL_TIMEOUT; i++) {
        if ((*reg & mask) == 0) return AHCI_OK;
    }
    return AHCI_ERR_TIMEOUT;
}

/* Stop the port engines: clear ST then FRE, wait CR and FR. */
static int port_stop(hba_port_t *p) {
    p->cmd &= ~PORT_CMD_ST;
    if (wait_clear(&p->cmd, PORT_CMD_CR) != AHCI_OK) return AHCI_ERR_TIMEOUT;
    p->cmd &= ~PORT_CMD_FRE;
    if (wait_clear(&p->cmd, PORT_CMD_FR) != AHCI_OK) return AHCI_ERR_TIMEOUT;
    return AHCI_OK;
}

/* Start the port engines: FRE first, then ST. */
static int port_start(hba_port_t *p) {
    if (wait_clear(&p->cmd, PORT_CMD_CR) != AHCI_OK) return AHCI_ERR_TIMEOUT;
    p->cmd |= PORT_CMD_FRE;
    p->cmd |= PORT_CMD_ST;
    return AHCI_OK;
}

/* Wire the static command list / FIS / command-table memory into the port
 * registers, zero the storage, and seed the single command-list header so
 * it always points at g_cmd_table. */
static void port_rebase(hba_port_t *p) {
    port_stop(p);

    memset(g_cmd_list,  0, sizeof(g_cmd_list));
    memset(g_fis_recv,  0, sizeof(g_fis_recv));
    memset(g_cmd_table, 0, sizeof(g_cmd_table));

    /* Sanity: every base address we hand to the HBA must satisfy AHCI's
     * alignment rules.  ALIGNED(...) on the BSS arrays should already make
     * this hold, but a stray linker flag or future malloc-backed rewrite
     * could break it - so log a loud failure rather than corrupt the disk. */
    uint32_t clb_pa  = (uint32_t)(uintptr_t)g_cmd_list;
    uint32_t fb_pa   = (uint32_t)(uintptr_t)g_fis_recv;
    uint32_t ctba_pa = (uint32_t)(uintptr_t)g_cmd_table;
    if (clb_pa  & 0x3FFu) debug_printf("[ahci] WARN clb  not 1024-aligned: 0x%08x\n", clb_pa);
    if (fb_pa   & 0x0FFu) debug_printf("[ahci] WARN fb   not  256-aligned: 0x%08x\n", fb_pa);
    if (ctba_pa & 0x07Fu) debug_printf("[ahci] WARN ctba not  128-aligned: 0x%08x\n", ctba_pa);

    p->clb  = clb_pa;
    p->clbu = 0;
    p->fb   = fb_pa;
    p->fbu  = 0;

    ahci_cmd_header_t *hdr = (ahci_cmd_header_t *)g_cmd_list;
    hdr[0].ctba  = ctba_pa;
    hdr[0].ctbau = 0;

    /* Disable per-port interrupts (we poll), then ack any sticky bits the
     * BIOS / previous OS may have left set in SERR and IS.  Without these
     * three writes the HBA can latch an old TFES from a stale state and
     * make our next IDENTIFY or WRITE look like it failed instantly. */
    p->ie   = 0;
    p->serr = 0xFFFFFFFFu;
    p->is   = 0xFFFFFFFFu;

    port_start(p);
}

/* Recover a port after a command failure (TFES or stuck CI).  Stops the
 * engines, clears sticky error bits, and brings the engines back up.  After
 * this returns the next xfer_one() can issue a fresh command safely. */
static void port_recover(hba_port_t *p) {
    port_stop(p);
    p->serr = 0xFFFFFFFFu;
    p->is   = 0xFFFFFFFFu;
    port_start(p);
}

/* Submit a prepared command (slot 0) and poll PxCI until completion. */
static int issue_and_wait(hba_port_t *p) {
    /* Clear any stale IS bits left from a previous command so the TFES
     * check below cannot trigger off old state. */
    p->is = 0xFFFFFFFFu;

    /* Wait until any in-flight task settles (BSY=0, DRQ=0). */
    for (uint32_t i = 0; i < POLL_TIMEOUT; i++) {
        if (!(p->tfd & (PORT_TFD_BSY | PORT_TFD_DRQ))) goto ready;
    }
    debug_fail("ahci", "task file stuck BSY/DRQ before issue");
    return AHCI_ERR_TIMEOUT;
ready:
    p->ci = 1;     /* slot 0 */

    for (uint32_t i = 0; i < POLL_TIMEOUT; i++) {
        uint32_t ci = p->ci;
        if ((ci & 1) == 0) {
            if (p->is & PORT_IS_TFES) {
                debug_printf("[ahci] TFES asserted (tfd=0x%08x is=0x%08x serr=0x%08x)\n",
                             p->tfd, p->is, p->serr);
                port_recover(p);
                return AHCI_ERR_IO;
            }
            return AHCI_OK;
        }
        if (p->is & PORT_IS_TFES) {
            debug_printf("[ahci] TFES during poll (tfd=0x%08x serr=0x%08x)\n",
                         p->tfd, p->serr);
            port_recover(p);
            return AHCI_ERR_IO;
        }
    }
    debug_fail("ahci", "command timeout (PxCI stayed set)");
    /* Recover so a subsequent xfer_one() isn't permanently jammed by the
     * still-set CI bit from this timed-out slot. */
    port_recover(p);
    /* Driver watchdog: a stuck PxCI usually means the disk is wedged
     * (cable yanked, firmware bug, host bridge stall).  Surface this
     * through the recoverable-panic GUI so the user sees a clear error
     * instead of an opaque AHCI_ERR_TIMEOUT propagating up to fail
     * whatever command (installsys, rfile) initiated the I/O.  If
     * recovery isn't armed yet (pre-shell init), recoverable_panic
     * forwards to the hard panic anyway. */
    recoverable_panic(
        "AHCI watchdog: command on port %d timed out "
        "(PxCI stuck set).  Disk may be unresponsive.",
        g_port_idx);
    /* unreachable */
    return AHCI_ERR_TIMEOUT;
}

/* Common path: build LBA48 H2D FIS + 1-entry PRDT for a single sector
 * transfer, then submit it. */
static int xfer_one(uint32_t lba, uint8_t ata_cmd, bool write) {
    if (!g_present || !g_port) return AHCI_ERR_NO_DEV;
    if (g_sectors && lba >= g_sectors) return AHCI_ERR_BOUNDS;

    ahci_cmd_header_t *hdr = (ahci_cmd_header_t *)g_cmd_list;
    /* CFL = register-H2D FIS length in dwords (5 of them = 20 bytes / 4). */
    uint16_t flags = (uint16_t)(sizeof(fis_h2d_t) / 4);
    if (write) flags |= (1u << 6);                  /* W bit */
    hdr[0].flags = flags;
    hdr[0].prdtl = 1;
    hdr[0].prdbc = 0;

    ahci_cmd_table_t *tbl = (ahci_cmd_table_t *)g_cmd_table;
    memset(tbl, 0, sizeof(*tbl));

    tbl->prdt[0].dba   = (uint32_t)(uintptr_t)g_dma_buf;
    tbl->prdt[0].dbau  = 0;
    tbl->prdt[0].dbc_i = (512u - 1u);              /* 512 bytes - 1; no INT */

    fis_h2d_t *cfis = (fis_h2d_t *)tbl->cfis;
    cfis->fis_type = 0x27;
    cfis->pmport_c = (1u << 7);
    cfis->command  = ata_cmd;
    cfis->device   = (1u << 6);                    /* LBA mode */
    cfis->lba0     = (uint8_t)(lba & 0xFF);
    cfis->lba1     = (uint8_t)((lba >> 8)  & 0xFF);
    cfis->lba2     = (uint8_t)((lba >> 16) & 0xFF);
    cfis->lba3     = (uint8_t)((lba >> 24) & 0xFF);
    cfis->lba4     = 0;
    cfis->lba5     = 0;
    cfis->countl   = 1;
    cfis->counth   = 0;

    return issue_and_wait(g_port);
}

/* ---------- IDENTIFY DEVICE -------------------------------------------- */
static int port_identify(hba_port_t *p) {
    ahci_cmd_header_t *hdr = (ahci_cmd_header_t *)g_cmd_list;
    hdr[0].flags = (uint16_t)(sizeof(fis_h2d_t) / 4);   /* read */
    hdr[0].prdtl = 1;
    hdr[0].prdbc = 0;

    ahci_cmd_table_t *tbl = (ahci_cmd_table_t *)g_cmd_table;
    memset(tbl, 0, sizeof(*tbl));
    tbl->prdt[0].dba   = (uint32_t)(uintptr_t)g_dma_buf;
    tbl->prdt[0].dbau  = 0;
    tbl->prdt[0].dbc_i = (512u - 1u);

    fis_h2d_t *cfis = (fis_h2d_t *)tbl->cfis;
    cfis->fis_type = 0x27;
    cfis->pmport_c = (1u << 7);
    cfis->command  = ATA_CMD_IDENTIFY;
    /* No LBA, no count.  Identify returns 512 B of data. */

    int r = issue_and_wait(p);
    if (r != AHCI_OK) return r;

    /* Pull total LBA48 sector count from words 100..103. */
    const uint16_t *w = (const uint16_t *)g_dma_buf;
    uint64_t lba48 =  (uint64_t)w[100]
                   | ((uint64_t)w[101] << 16)
                   | ((uint64_t)w[102] << 32)
                   | ((uint64_t)w[103] << 48);
    if (lba48 == 0) {
        /* Fallback: 28-bit total at words 60..61. */
        lba48 = (uint32_t)w[60] | ((uint32_t)w[61] << 16);
    }
    /* Clamp to 32-bit: we only address an LBA28-friendly subset anyway. */
    if (lba48 > 0xFFFFFFFFull) lba48 = 0xFFFFFFFFull;
    g_sectors = (uint32_t)lba48;

    /* Pretty-print the model string (words 27..46, byte-swapped). */
    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)((w[27 + i] >> 8) & 0xFF);
        model[i * 2 + 1] = (char)(w[27 + i] & 0xFF);
    }
    model[40] = 0;
    /* trim trailing spaces */
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;
    debug_printf("[ahci] disk model: '%s'  sectors=%u (%u MiB)\n",
                 model, g_sectors, g_sectors / 2048);
    return AHCI_OK;
}

/* TASK 15: explicit COMRESET sequence used during bring-up of real
 * hardware ports.  Spec-compliant procedure per AHCI 1.3 section 10.4.2:
 *   1. Set PxSCTL.DET = 1 (request COMRESET)
 *   2. Wait at least 1 ms with the bit asserted
 *   3. Clear PxSCTL.DET = 0
 *   4. Poll PxSSTS.DET = 3 (device + PHY communication established) with
 *      a generous spin-up budget (real SATA drives can take ~3 seconds
 *      to negotiate the PHY)
 *
 * On a port that has nothing attached, step 4 times out and we report
 * back without binding. */
static int port_comreset(hba_port_t *p, int idx) {
    uint32_t sctl = p->sctl;
    p->sctl = (sctl & ~0x0Fu) | 0x01u;       /* DET = 1 */
    /* Sleep ≥1 ms with COMRESET held.  We use 5 ms to give physical
     * drives plenty of margin (some quirky controllers ignore 1 ms). */
    pit_sleep(5);
    p->sctl = (p->sctl & ~0x0Fu);            /* DET = 0 */

    /* Poll PxSSTS.DET == 3 for up to 3 seconds.  Real drives commonly
     * negotiate within 500 ms; SSDs are near-instant; some HDD spin-up
     * latencies pile up at boot, hence the wide budget. */
    for (uint32_t ms = 0; ms < 3000; ms++) {
        uint32_t ssts = p->ssts;
        uint8_t det = (uint8_t)(ssts & 0x0F);
        if (det == HBA_PORT_DET_PRESENT) {
            debug_printf("[ahci]   port %d: COMRESET completed after ~%u ms "
                         "(ssts=0x%08x)\n", idx, ms, ssts);
            /* Clear any sticky SERR bits the negotiation set. */
            p->serr = 0xFFFFFFFFu;
            return AHCI_OK;
        }
        pit_sleep(1);
    }
    debug_printf("[ahci]   port %d: COMRESET TIMEOUT (ssts=0x%08x)\n",
                 idx, p->ssts);
    return AHCI_ERR_TIMEOUT;
}

/* ---------- Probe ports for the first SATA disk ------------------------
 * After GHC.HR the per-port signature register is cleared back to
 * 0xFFFFFFFF and only gets repopulated once the attached device sends its
 * Register-D2H FIS through the port's FIS-receive area.  That FIS won't be
 * accepted until PxCMD.FRE is on, so we MUST port_rebase() the port first
 * and then wait for the signature to settle before trying to read it. */
static int find_and_bind_sata_port(void) {
    uint32_t pi = g_hba->pi;
    debug_printf("[ahci] ports implemented bitmap = 0x%08x\n", pi);

    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        hba_port_t *p = port_at(i);
        uint32_t ssts = p->ssts;
        uint8_t  det  = (uint8_t)(ssts & 0x0F);
        uint8_t  ipm  = (uint8_t)((ssts >> 8) & 0x0F);

        debug_printf("[ahci]   port %d: ssts=0x%08x sig=0x%08x det=%u ipm=%u\n",
                     i, ssts, p->sig, det, ipm);
        diag_add(" p%d:d%ui%u", i, det, ipm);

        /* TASK 15: on real hardware some ports return det=0 immediately
         * after HBA reset because the COMRESET handshake hasn't been driven
         * yet.  Intel 6/7-series chipsets can also show DET=3 while IPM is
         * still partial/slumber during early boot; if we simply skip non-
         * ACTIVE IPM here, a perfectly good AHCI disk is reported as absent.
         * Kick either condition with COMRESET and then trust DET=3 enough to
         * attempt IDENTIFY -- the command path has its own BSY/DRQ timeout and
         * will safely reject a port that never wakes. */
        if (det != HBA_PORT_DET_PRESENT || ipm != HBA_PORT_IPM_ACTIVE) {
            debug_printf("[ahci]   port %d: DET/IPM not ready (det=%u ipm=%u), issuing COMRESET\n",
                         i, det, ipm);
            if (port_comreset(p, i) != AHCI_OK) continue;
            ssts = p->ssts;
            det  = (uint8_t)(ssts & 0x0F);
            ipm  = (uint8_t)((ssts >> 8) & 0x0F);
            debug_printf("[ahci]   port %d: after COMRESET det=%u ipm=%u ssts=0x%08x\n",
                         i, det, ipm, ssts);
            if (det != HBA_PORT_DET_PRESENT) continue;
        }

        /* Bring the port engines up before reading sig - the FIS that
         * populates PxSIG only arrives once FRE is set. */
        g_port     = p;
        g_port_idx = i;
        port_rebase(p);

        /* Wait for the device to send its Register-D2H FIS:
         *   - PxTFD.BSY and PxTFD.DRQ should drop
         *   - PxSIG should leave 0xFFFFFFFF
         * Either condition alone is not enough on every emulator. */
        uint32_t waited = 0;
        for (; waited < POLL_TIMEOUT; waited++) {
            if (!(p->tfd & (PORT_TFD_BSY | PORT_TFD_DRQ)) &&
                p->sig != 0xFFFFFFFFu) break;
        }
        uint32_t sig = p->sig;
        debug_printf("[ahci]   port %d settled after %u polls: sig=0x%08x tfd=0x%08x\n",
                     i, waited, sig, p->tfd);

        /* TASK 15 hardening: extra spin-up grace.  Some HDDs need up to
         * ~5 seconds before they stop reporting BSY+DRQ across
         * COMRESET.  If the polling loop above bailed early we retry
         * with millisecond pacing rather than tight-loop polling. */
        if (sig == 0xFFFFFFFFu || (p->tfd & (PORT_TFD_BSY | PORT_TFD_DRQ))) {
            debug_printf("[ahci]   port %d: extending settle window to ~5s\n", i);
            for (uint32_t ms = 0; ms < 5000; ms++) {
                if (!(p->tfd & (PORT_TFD_BSY | PORT_TFD_DRQ)) &&
                    p->sig != 0xFFFFFFFFu) {
                    sig = p->sig;
                    debug_printf("[ahci]   port %d: ready after %u extra ms "
                                 "sig=0x%08x tfd=0x%08x\n",
                                 i, ms, sig, p->tfd);
                    break;
                }
                pit_sleep(1);
            }
            sig = p->sig;
        }

        /* Accept the standard SATA-ATA signature AND the older 0x00000000
         * that some controllers report mid-negotiation.  Anything else
         * (ATAPI 0xEB140101, port multiplier, SEMB, ...) is skipped. */
        if (sig != SATA_SIG_ATA && sig != 0x00000000u) {
            debug_printf("[ahci]   port %d: non-SATA signature 0x%08x, skipping\n",
                         i, sig);
            g_port     = NULL;
            g_port_idx = -1;
            continue;
        }

        debug_printf("[ahci] binding to port %d (standard SATA signature)\n", i);

        if (port_identify(p) != AHCI_OK) {
            debug_fail("ahci", "IDENTIFY failed - skipping port");
            diag_add("(p%d IDENT-fail)", i);
            g_port     = NULL;
            g_port_idx = -1;
            continue;
        }
        return AHCI_OK;
    }
    return AHCI_ERR_NO_DEV;
}

/* ---------- Top-level init --------------------------------------------- */
bool ahci_init(void) {
    debug_step("ahci: searching PCI for an AHCI 1.0 controller");
    diag_reset();

    /* Bulletproof discovery first: scan every PCI slot for any Class-01
     * device, log them all, and pick the most AHCI-like one.  This covers
     * VirtualBox (standard SATA/AHCI), QEMU ich9-ahci, and real-hardware
     * RAID-mode chipsets that still expose AHCI via BAR5. */
    pci_device_t dev;
    bool got = pci_find_storage(&dev);
    if (!got) {
        /* Fall back to the strict exact-match probe in case the bulletproof
         * scan was too conservative (e.g. it declined a candidate with a
         * zero-looking BAR5 that the BIOS will populate after we enable
         * MMIO + busmaster).  This call also doubles as a sanity check on
         * the legacy code path. */
        got = pci_find(PCI_CLASS_MASS_STORAGE, PCI_SUB_SATA, PCI_PROG_IF_AHCI, &dev);
    }
    if (!got) {
        debug_fail("ahci", "no AHCI / SATA controller on PCI bus");
        diag_add("no class-01/06/01 AHCI controller on PCI "
                 "(SATA mode = IDE? set BIOS SATA = AHCI)");
        return false;
    }
    diag_add("ctrl %02x:%02x.%x cls=%02x/%02x/%02x", dev.bus, dev.device,
             dev.function, dev.class_code, dev.subclass, dev.prog_if);

    /* Some controllers leave MMIO + busmaster disabled at power-on; re-
     * reading BAR5 after the enable also catches the case where the BIOS
     * is using a sticky-on-MEM model. */
    pci_enable_busmaster(&dev);
    pci_refresh_bars(&dev);

    /* BAR5 = ABAR.  Lower nibble is type flags, mask them off. */
    uint32_t abar = dev.bar[5] & 0xFFFFFFF0u;
    if (abar == 0) {
        debug_fail("ahci", "BAR5 reads back as 0 - BIOS did not assign MMIO");
        diag_add(" ABAR=0 (no MMIO assigned)");
        return false;
    }
    diag_add(" ABAR=%08x", abar);
    debug_printf("[ahci] selected %02x:%02x.%x class=%02x sub=%02x pi=%02x ABAR=0x%08x\n",
                 dev.bus, dev.device, dev.function,
                 dev.class_code, dev.subclass, dev.prog_if, abar);
    g_hba = (hba_mem_t *)(uintptr_t)abar;

    /* Step A: ensure AHCI Enable is set BEFORE attempting HBA reset.  The
     * AHCI spec (10.4.3) says GHC.HR is ignored unless GHC.AE = 1. */
    g_hba->ghc |= GHC_AE;

    /* Step B: HBA reset.  Hands the controller from BIOS / previous OS into
     * a known good state.  VirtualBox's AHCI emulation in particular leaves
     * a port in a half-configured state after BIOS boot, which is why a
     * fresh kernel can IDENTIFY (a read-only command) but crashes on the
     * first WRITE - the reset clears all per-port stickiness. */
    g_hba->ghc |= GHC_HR;
    uint32_t hr_waited = 0;
    while ((g_hba->ghc & GHC_HR) && hr_waited < HBA_RESET_TIMEOUT) hr_waited++;
    if (g_hba->ghc & GHC_HR) {
        debug_fail("ahci", "HBA reset never self-cleared GHC.HR");
        diag_add(" HBA-reset stuck");
        return false;
    }
    debug_printf("[ahci] HBA reset complete (GHC.HR cleared after %u poll iters)\n",
                 hr_waited);

    /* Step C: re-assert AE since some implementations clear it on HR. */
    g_hba->ghc |= GHC_AE;
    /* Leave GHC.IE off - we drive all I/O via polling. */

    debug_printf("[ahci] CAP=0x%08x PI=0x%08x VS=0x%08x\n",
                 g_hba->cap, g_hba->pi, g_hba->vs);
    diag_add(" PI=%08x", g_hba->pi);

    /* Probe ports until we find a SATA disk that IDENTIFYs cleanly. */
    if (find_and_bind_sata_port() != AHCI_OK) {
        debug_fail("ahci", "no SATA disk found on any implemented port");
        diag_add(" -> no disk on any port");
        return false;
    }
    diag_add(" -> port %d OK", g_port_idx);

    g_present = true;
    debug_printf("[ OK ] ahci: SATA ready on port %d, %u sectors (%u MiB)\n",
                 g_port_idx, g_sectors, g_sectors / 2048);
    return true;
}

bool     ahci_present     (void) { return g_present; }
uint32_t ahci_sector_count(void) { return g_sectors; }
uint16_t ahci_port_index  (void) { return (uint16_t)(g_port_idx & 0xFFFF); }

/* ---------- Public single-sector API ------------------------------------ */
int ahci_read_sector(uint32_t lba, void *buf) {
    int r = xfer_one(lba, ATA_CMD_READ_DMA_EXT, false);
    if (r != AHCI_OK) return r;
    memcpy(buf, g_dma_buf, 512);
    return AHCI_OK;
}

int ahci_write_sector(uint32_t lba, const void *buf) {
    memcpy(g_dma_buf, buf, 512);
    return xfer_one(lba, ATA_CMD_WRITE_DMA_EXT, true);
}

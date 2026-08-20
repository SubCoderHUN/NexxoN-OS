/* ============================================================================
 * NexxoN OS - Minimal ACPI power management
 * ----------------------------------------------------------------------------
 * Goal: find the ACPI PM1a control register and the S5 sleep-type values so
 * we can issue a proper ACPI power-off on real hardware and QEMU.
 *
 * Algorithm:
 *   1. Scan EBDA + BIOS-ROM for "RSD PTR " (RSDP).
 *   2. Follow RSDP.RsdtAddress to find the RSDT.
 *   3. Walk RSDT entries looking for the "FACP" (FADT) table.
 *   4. Read PM1a_CNT_BLK from FADT (I/O port for the power-control register).
 *   5. Scan DSDT byte-by-byte for "_S5_" and extract SLP_TYPa (3-bit field).
 *   6. Shutdown = outw(pm1a_cnt, (slp_typa << 10) | SLP_EN).
 *
 * Fallbacks (for VMs that don't expose clean ACPI or when RSDP is missing):
 *   QEMU        outw(0x0604, 0x2000)
 *   Bochs       outw(0xB004, 0x2000)
 * ============================================================================ */
#include "acpi.h"
#include "io.h"
#include "string.h"
#include "debug.h"

/* ---------- ACPI table structures (packed to match firmware layout) ------- */

typedef struct {
    char     sig[8];       /* "RSD PTR " */
    uint8_t  cksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;    /* physical address of RSDT */
} __attribute__((packed)) rsdp_t;

typedef struct {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  cksum;
    char     oem_id[6];
    char     oem_table[8];
    uint32_t oem_rev;
    uint32_t creator_id;
    uint32_t creator_rev;
} __attribute__((packed)) sdth_t;   /* standard ACPI SDT header, 36 bytes */

/* FADT — only the fields we actually need, up through PM1a_CNT_BLK. */
typedef struct {
    sdth_t   h;              /* 36-byte header */
    uint32_t fw_ctrl;        /* +36 */
    uint32_t dsdt;           /* +40: physical address of DSDT */
    uint8_t  int_model;      /* +44 */
    uint8_t  pm_profile;     /* +45 */
    uint16_t sci_int;        /* +46 */
    uint32_t smi_cmd;        /* +48 */
    uint8_t  acpi_enable;    /* +52 */
    uint8_t  acpi_disable;   /* +53 */
    uint8_t  s4bios_req;     /* +54 */
    uint8_t  pstate_cnt;     /* +55 */
    uint32_t pm1a_evt_blk;   /* +56 */
    uint32_t pm1b_evt_blk;   /* +60 */
    uint32_t pm1a_cnt_blk;   /* +64  ← I/O port for PM1a control register */
} __attribute__((packed)) fadt_t;

#define SLP_EN  0x2000u     /* bit 13 of PM1_CNT: sleep-enable */

/* ---------- Module state -------------------------------------------------- */
static uint32_t g_pm1a_cnt = 0;
static uint16_t g_slp_typa = 0;   /* S5 sleep type (shutdown) */
static uint16_t g_slp_typ3 = 0;   /* S3 sleep type (suspend-to-RAM) */
static bool     g_s3_valid = false;
static bool     g_acpi_ok  = false;

/* ---------- RSDP scan ----------------------------------------------------- */
static rsdp_t *rsdp_scan(uint8_t *base, uint32_t len) {
    for (uint32_t i = 0; i + sizeof(rsdp_t) <= len; i += 16) {
        if (memcmp(base + i, "RSD PTR ", 8) == 0)
            return (rsdp_t *)(base + i);
    }
    return NULL;
}

static rsdp_t *find_rsdp(void) {
    /* 1. EBDA (first 1 KiB of the Extended BIOS Data Area). */
    uint16_t ebda_seg = *(volatile uint16_t *)0x040E;
    if (ebda_seg) {
        rsdp_t *r = rsdp_scan((uint8_t *)((uint32_t)ebda_seg << 4), 0x400);
        if (r) return r;
    }
    /* 2. BIOS ROM (0xE0000 – 0xFFFFF). */
    return rsdp_scan((uint8_t *)0xE0000, 0x20000);
}

/* ---------- DSDT _S5_ heuristic ------------------------------------------ */
/* AML encodes _Sx_ as a Name opcode + Package.  We scan for the four bytes
 * "_Sx_" and then look forward up to 24 bytes for the BytePrefix (0x0A)
 * followed by the SLP_TYPa value. */
static uint16_t extract_sx_slp_typ(uint8_t *dsdt, uint32_t len, char sx) {
    for (uint32_t i = 0; i + 20 < len; i++) {
        if (dsdt[i]   != '_') continue;
        if (dsdt[i+1] != 'S') continue;
        if (dsdt[i+2] != sx)  continue;
        if (dsdt[i+3] != '_') continue;
        for (uint32_t j = i + 4; j < i + 24 && j + 1 < len; j++) {
            if (dsdt[j] == 0x0A) {   /* BytePrefix opcode */
                uint8_t raw = dsdt[j + 1];
                return (uint16_t)((raw & 0x07u) << 10);
            }
        }
    }
    return 0xFFFFu;   /* not found */
}

static uint16_t extract_s5_slp_typ(uint8_t *dsdt, uint32_t len) {
    uint16_t v = extract_sx_slp_typ(dsdt, len, '5');
    return (v == 0xFFFFu) ? 0 : v;
}

/* ---------- Public API ---------------------------------------------------- */
void acpi_init(void) {
    debug_step("acpi: scanning for RSDP (EBDA + BIOS ROM)");

    rsdp_t *rsdp = find_rsdp();
    if (!rsdp) {
        debug_printf("[acpi] RSDP not found - will use VM port fallbacks\n");
        return;
    }
    debug_printf("[acpi] RSDP @ %p  RSDT=0x%x\n",
                 (uint32_t)(uintptr_t)rsdp, rsdp->rsdt_addr);

    sdth_t *rsdt = (sdth_t *)(uintptr_t)rsdp->rsdt_addr;
    if (memcmp(rsdt->sig, "RSDT", 4) != 0) {
        debug_printf("[acpi] RSDT signature mismatch\n");
        return;
    }

    uint32_t n_entries = (rsdt->length - sizeof(sdth_t)) / 4;
    uint32_t *entries  = (uint32_t *)(rsdt + 1);

    for (uint32_t i = 0; i < n_entries; i++) {
        sdth_t *h = (sdth_t *)(uintptr_t)entries[i];
        if (memcmp(h->sig, "FACP", 4) != 0) continue;

        fadt_t  *fadt     = (fadt_t *)h;
        g_pm1a_cnt        = fadt->pm1a_cnt_blk;

        sdth_t  *dsdt_hdr  = (sdth_t *)(uintptr_t)fadt->dsdt;
        uint8_t *dsdt_data = (uint8_t *)(dsdt_hdr + 1);
        uint32_t dsdt_len  = (dsdt_hdr->length > sizeof(sdth_t))
                             ? dsdt_hdr->length - (uint32_t)sizeof(sdth_t)
                             : 0;

        g_slp_typa = extract_s5_slp_typ(dsdt_data, dsdt_len);
        uint16_t s3 = extract_sx_slp_typ(dsdt_data, dsdt_len, '3');
        if (s3 != 0xFFFFu) { g_slp_typ3 = s3; g_s3_valid = true; }
        g_acpi_ok  = true;

        debug_printf("[acpi] FADT found  PM1a_CNT=0x%x  SLP_TYPa(S5)=0x%x  SLP_TYP3=%s\n",
                     g_pm1a_cnt, g_slp_typa >> 10, g_s3_valid ? "yes" : "no");
        debug_ok("acpi: ACPI power management initialised");
        return;
    }
    debug_printf("[acpi] FADT not found in RSDT\n");
}

/* Short bounded busy-wait so we never enter a "wait forever for the chipset
 * to acknowledge" path (the previous reboot bug).  Uses a volatile loop
 * counter rather than pit_sleep() so it works after `cli` has been issued. */
static void acpi_pause_busy(void) {
    for (volatile uint32_t i = 0; i < 800000u; i++) {
        __asm__ volatile ("pause");
    }
}

void acpi_shutdown(void) {
    /* Disable interrupts up-front: a stray IRQ during shutdown could
     * re-enter terminal/wm code and paint over the framebuffer mid-power-
     * off, leaving an ugly artefact on hardware that supports late
     * S5 cancellation. */
    __asm__ volatile ("cli");

    /* Preferred: ACPI via FADT. */
    if (g_acpi_ok && g_pm1a_cnt) {
        debug_printf("[acpi] ACPI shutdown: outw(0x%x, 0x%x)\n",
                     g_pm1a_cnt, g_slp_typa | SLP_EN);
        outw((uint16_t)g_pm1a_cnt, (uint16_t)(g_slp_typa | SLP_EN));
        acpi_pause_busy();
    }

    /* Fallback 1: QEMU ACPI / isa-debug-exit power port. */
    outw(0x0604, 0x2000);
    acpi_pause_busy();

    /* Fallback 2: Bochs / old QEMU. */
    outw(0xB004, 0x2000);
    acpi_pause_busy();

    /* Fallback 3: VirtualBox-specific ACPI shutdown port. */
    outw(0x4004, 0x3400);
    acpi_pause_busy();

    /* Should be unreachable on any emulator; halt defensively with
     * interrupts disabled so a stuck shutdown never devolves into a
     * fault loop that pegs the host CPU. */
    debug_printf("[acpi] shutdown commands sent but machine still running - halting\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

void acpi_reboot(void) {
    debug_step("acpi: starting safe reboot sequence");

    /* CRITICAL: Disable interrupts FIRST.  Every method below talks to
     * hardware in a way that an IRQ-driven repaint could race with, and
     * any fault during the sequence must NOT recurse through the panic
     * path (the original bug was a fault-loop after the 8042 reset
     * timed out, which made VirtualBox grow to 10+ GiB of host RAM as
     * it logged each frame). */
    __asm__ volatile ("cli");

    /* --- Method 1: PCI Reset Control Register (Intel ICH RCR) ----------
     * Standard on every modern PCH; QEMU, Bochs and VirtualBox all
     * honour it.  Bit 1 = RST_CPU, bit 2 = SYS_RST; writing 0x06 fires
     * a full system reset.  We arm with 0x02 first so chipsets that
     * latch the bits separately see the rising edge.                    */
    debug_printf("[reboot] method 1: PCI reset (port 0xCF9)\n");
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    acpi_pause_busy();

    /* --- Method 2: 8042 keyboard controller reset ----------------------
     * BOUNDED drain of the input-buffer-full bit.  The previous code
     * had `while (inb(0x64) & 0x02) {}` which is unbounded - if some
     * VM kept that bit stuck high we'd spin forever, which is exactly
     * what was happening on VirtualBox.                                  */
    debug_printf("[reboot] method 2: 8042 (port 0x64)\n");
    for (int i = 0; i < 10000; i++) {
        if (!(inb(0x64) & 0x02)) break;
        io_wait();
    }
    outb(0x64, 0xFE);
    acpi_pause_busy();

    /* --- Method 3: Fast Reset via System Control Port A ---------------
     * Setting bit 0 of port 0x92 is the classic "fast reset" used by
     * BIOSes since the AT.  Most VMs accept it; real chipsets clear bit
     * 0 themselves before the reset commits, so we don't risk leaving
     * A20 stuck on.                                                      */
    debug_printf("[reboot] method 3: Fast Reset (port 0x92)\n");
    {
        uint8_t a = inb(0x92);
        outb(0x92, (uint8_t)(a | 0x01));
        acpi_pause_busy();
    }

    /* --- Method 4: Triple Fault via null IDT --------------------------
     * Final resort.  Load IDTR with limit=0, then any interrupt vector
     * (INT3 here) is out-of-bounds.  CPU raises #GP → can't dispatch →
     * #DF → can't dispatch → triple fault → reset.                       */
    debug_printf("[reboot] method 4: triple fault\n");
    {
        struct {
            uint16_t limit;
            uint32_t base;
        } __attribute__((packed)) null_idt = { 0, 0 };
        __asm__ volatile ("lidt %0" :: "m"(null_idt));
        __asm__ volatile ("int $0x03");
    }

    /* Unreachable in theory.  If somehow we end up here (VM
     * configured to pause on triple-fault, or hypervisor swallowed
     * the fault), halt forever with interrupts disabled - that's
     * strictly better than a fault storm that bloats the host. */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* ============================================================================
 * TASK 27: MADT walker for SMP enumeration
 * ============================================================================ */
int acpi_madt_lapics(uint8_t *out, int max) {
    rsdp_t *rsdp = find_rsdp();
    if (!rsdp) return 0;
    sdth_t *rsdt = (sdth_t *)(uintptr_t)rsdp->rsdt_addr;
    if (memcmp(rsdt->sig, "RSDT", 4) != 0) return 0;
    uint32_t n_entries = (rsdt->length - sizeof(sdth_t)) / 4;
    uint32_t *entries  = (uint32_t *)(rsdt + 1);
    for (uint32_t i = 0; i < n_entries; i++) {
        sdth_t *h = (sdth_t *)(uintptr_t)entries[i];
        if (memcmp(h->sig, "APIC", 4) != 0) continue;
        /* MADT header: 36 bytes inherited + 4 bytes LAPIC base + 4 flags. */
        uint8_t *p = (uint8_t *)(h + 1) + 8;
        uint8_t *e = (uint8_t *)h + h->length;
        int n = 0;
        while (p < e && n < max) {
            uint8_t type = p[0];
            uint8_t len  = p[1];
            if (len < 2) break;
            if (type == 0) {
                /* Processor Local APIC. */
                uint8_t apic_id = p[3];
                uint32_t flags  = *(uint32_t *)(p + 4);
                if ((flags & 1) && n + 1 < max) {
                    /* BSP itself has APIC ID matching CPUID; we don't
                     * filter for that here - the SMP layer drops the
                     * matching entry to avoid double-waking. */
                    out[n++] = apic_id;
                }
            }
            p += len;
        }
        return n;
    }
    return 0;
}

/* ============================================================================
 * S3 suspend-to-RAM
 * ============================================================================ */
bool acpi_suspend_s3(void) {
    if (!g_acpi_ok || !g_s3_valid || g_pm1a_cnt == 0) {
        debug_printf("[acpi] S3 not supported (acpi_ok=%d s3_valid=%d)\n",
                     g_acpi_ok, g_s3_valid);
        return false;
    }
    debug_printf("[acpi] entering S3 suspend (PM1a_CNT=0x%x typ=0x%x)\n",
                 g_pm1a_cnt, g_slp_typ3 >> 10);

    /* Set WAK_STS bit in PM1a_EVT_BLK if it was set from a previous wake. */
    /* Write SLP_TYP=3 | SLP_EN to PM1a_CNT to trigger S3. */
    __asm__ volatile ("cli");
    outw((uint16_t)g_pm1a_cnt, (uint16_t)(g_slp_typ3 | SLP_EN));
    /* Execution resumes here after BIOS wakeup (on QEMU the S3 wakeup is
     * immediate; on real hardware the CPU halts until the wake event). */
    for (volatile uint32_t i = 0; i < 200000u; i++)
        __asm__ volatile ("pause");
    __asm__ volatile ("sti");
    debug_printf("[acpi] resumed from S3\n");
    return true;
}

/* ============================================================================
 * CPU Performance / P-state control
 * ============================================================================ */

#define MSR_IA32_PERF_CTL    0x00000199u
#define MSR_IA32_PERF_STATUS 0x00000198u

static bool g_perf_probed = false;
static bool g_perf_ok     = false;
static uint8_t g_perf_min = 0;
static uint8_t g_perf_max = 0;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static void perf_probe(void) {
    if (g_perf_probed) return;
    g_perf_probed = true;

    /* Check CPUID.1:ECX bit 7 (EST = Enhanced SpeedStep) or
     * CPUID.6 bit 0 (Turbo/P-state support). */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(1u));
    if (!(ecx & (1u << 7))) {
        debug_printf("[cpufreq] Enhanced SpeedStep not reported by CPUID\n");
        /* Try anyway — some VMs expose the MSR without advertising EST. */
    }

    /* Read current P-state from PERF_STATUS. */
    uint64_t status = rdmsr(MSR_IA32_PERF_STATUS);
    uint8_t cur = (uint8_t)((status >> 8) & 0xFFu);
    if (cur == 0) cur = 8;   /* fallback: assume P8 */

    /* We treat max=cur (whatever the BIOS set) and min=1 for simplicity.
     * A real driver would walk ACPI _PSS objects. */
    g_perf_max = cur;
    g_perf_min = 1;
    g_perf_ok  = true;
    debug_printf("[cpufreq] P-state range %u..%u (current=%u)\n",
                 g_perf_min, g_perf_max, cur);
    (void)eax; (void)ebx; (void)edx;
}

bool cpu_perf_supported(void) {
    perf_probe();
    return g_perf_ok;
}

int cpu_perf_get(void) {
    perf_probe();
    if (!g_perf_ok) return -1;
    uint64_t status = rdmsr(MSR_IA32_PERF_STATUS);
    uint8_t cur = (uint8_t)((status >> 8) & 0xFFu);
    if (g_perf_max == g_perf_min) return 50;
    int pct = (int)(((cur - g_perf_min) * 100u) / (g_perf_max - g_perf_min));
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

void cpu_perf_set(int pct) {
    perf_probe();
    if (!g_perf_ok) return;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    uint8_t target = (uint8_t)(g_perf_min +
        (uint32_t)(pct) * (g_perf_max - g_perf_min) / 100u);
    if (target < g_perf_min) target = g_perf_min;
    if (target > g_perf_max) target = g_perf_max;
    uint64_t ctl = ((uint64_t)target << 8);
    wrmsr(MSR_IA32_PERF_CTL, ctl);
    debug_printf("[cpufreq] P-state set to %u (%d%%)\n", target, pct);
}

void cpu_perf_desc(char *buf, int sz) {
    perf_probe();
    if (!g_perf_ok) {
        ksnprintf((char *)buf, (uint32_t)sz, "N/A");
        return;
    }
    uint64_t status = rdmsr(MSR_IA32_PERF_STATUS);
    uint8_t cur = (uint8_t)((status >> 8) & 0xFFu);
    /* Bus ratio * 100 MHz is approximate but common on Intel. */
    uint32_t mhz = (uint32_t)cur * 100u;
    ksnprintf((char *)buf, (uint32_t)sz, "P%u (~%u MHz)", cur, mhz);
}

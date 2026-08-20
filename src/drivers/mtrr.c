/* ============================================================================
 * NexxoN OS - MTRR variable-range programming
 * ----------------------------------------------------------------------------
 * Intel SDM Vol. 3A, chapter 11.11 - the variable-range MTRRs.  Each one is
 * a pair of 64-bit MSRs:
 *     IA32_MTRR_PHYSBASE_n  (0x200 + 2*n)  = base address | mem type
 *     IA32_MTRR_PHYSMASK_n  (0x201 + 2*n)  = mask | valid bit (bit 11)
 *
 * A region is matched when (addr & mask) == (base & mask).  The mask
 * encodes a power-of-two-aligned span: size must be a power of 2 and base
 * must be size-aligned.  We round inputs into the nearest power-of-two
 * envelope so a typical 1024x768x4 = 3 MiB framebuffer registers as 4 MiB.
 *
 * The whole module is wrapped in feature checks: CPUID.01h:EDX.MTRR plus
 * IA32_MTRRCAP.VCNT must be > 0.  On a CPU that lacks MTRRs we silently
 * return false and the compositor still works (just without the
 * write-combining speedup).
 * ============================================================================ */
#include "mtrr.h"
#include "debug.h"
#include "string.h"

#define MSR_IA32_MTRRCAP        0xFE
#define MSR_IA32_MTRR_DEFTYPE   0x2FF
#define MSR_IA32_MTRR_PHYSBASE  0x200
#define MSR_IA32_MTRR_PHYSMASK  0x201

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d) {
    uint32_t ra, rb, rc, rd;
    __asm__ volatile ("cpuid"
                      : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                      : "a"(leaf));
    if (a) *a = ra;
    if (b) *b = rb;
    if (c) *c = rc;
    if (d) *d = rd;
}

static inline void rdmsr64(uint32_t msr, uint32_t *lo, uint32_t *hi) {
    uint32_t a, d;
    __asm__ volatile ("rdmsr" : "=a"(a), "=d"(d) : "c"(msr));
    if (lo) *lo = a;
    if (hi) *hi = d;
}

static inline void wrmsr64(uint32_t msr, uint32_t lo, uint32_t hi) {
    __asm__ volatile ("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

bool mtrr_supported(void) {
    /* CPUID.01h:EDX bit 12 = MTRR. */
    uint32_t edx = 0;
    cpuid(1, NULL, NULL, NULL, &edx);
    if (!(edx & (1u << 12))) return false;
    /* IA32_MTRRCAP.VCNT (bits 7:0) = variable-range MTRR count. */
    uint32_t cap_lo = 0;
    rdmsr64(MSR_IA32_MTRRCAP, &cap_lo, NULL);
    return (cap_lo & 0xFFu) > 0;
}

/* Round a size up to the next power of two. */
static uint32_t roundup_pow2_u32(uint32_t v) {
    if (v == 0) return 1u;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1u;
}

bool mtrr_set_write_combining(uint32_t base, uint32_t size) {
    if (!mtrr_supported()) {
        debug_printf("[mtrr] CPU has no MTRR support - skipping WC config\n");
        return false;
    }
    uint32_t cap_lo = 0;
    rdmsr64(MSR_IA32_MTRRCAP, &cap_lo, NULL);
    uint32_t vcnt = cap_lo & 0xFFu;
    bool     wc_supported = (cap_lo & (1u << 10)) != 0;
    if (!wc_supported) {
        debug_printf("[mtrr] CPU does not advertise WC memtype\n");
        return false;
    }

    /* Snap base down to the size alignment and size up to a power of two. */
    uint32_t sz = roundup_pow2_u32(size);
    uint32_t aligned_base = base & ~(sz - 1);

    /* Construct mask: top bits == ~(size-1), bit 11 = valid. */
    uint32_t mask_lo = (~(sz - 1)) | (1u << 11);
    uint32_t base_lo = (aligned_base & 0xFFFFF000u) | MTRR_MEMTYPE_WC;

    /* Find a free variable-MTRR slot.  "Free" means valid bit 11 is 0. */
    int slot = -1;
    for (uint32_t i = 0; i < vcnt; i++) {
        uint32_t m_lo = 0;
        rdmsr64(MSR_IA32_MTRR_PHYSMASK + i * 2, &m_lo, NULL);
        if (!(m_lo & (1u << 11))) { slot = (int)i; break; }
    }
    if (slot < 0) {
        debug_printf("[mtrr] all %u variable MTRRs already in use\n", vcnt);
        return false;
    }

    /* Cache-policy reprogramming dance per SDM 11.11.8:
     *   1. CLI + disable MTRRs (IA32_MTRR_DEFTYPE.E=0)
     *   2. write the slot's PHYSBASE + PHYSMASK
     *   3. flush caches (wbinvd), invalidate TLB, restore CR0.CD/NW
     *   4. re-enable MTRRs                                                */
    uintptr_t flags;
#if defined(__x86_64__)
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags));
#else
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
#endif

    uint32_t def_lo, def_hi;
    rdmsr64(MSR_IA32_MTRR_DEFTYPE, &def_lo, &def_hi);
    uint32_t new_def = def_lo & ~(1u << 11);   /* clear E bit */
    wrmsr64(MSR_IA32_MTRR_DEFTYPE, new_def, def_hi);

    /* CR0.CD=1, NW=0 then wbinvd to flush.  Reload CR3 to flush TLB.
     * uintptr_t temporaries so the mov matches the native CR width. */
    uintptr_t cr0, cr3;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uintptr_t cr0_off = (cr0 | ((uintptr_t)1u << 30)) & ~((uintptr_t)1u << 29);
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0_off));
    __asm__ volatile ("wbinvd");
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3));

    wrmsr64(MSR_IA32_MTRR_PHYSBASE + slot * 2, base_lo, 0);
    wrmsr64(MSR_IA32_MTRR_PHYSMASK + slot * 2, mask_lo, 0);

    __asm__ volatile ("wbinvd");
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3));
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    wrmsr64(MSR_IA32_MTRR_DEFTYPE, def_lo | (1u << 11), def_hi);

    if (flags & 0x200u) __asm__ volatile ("sti");

    debug_printf("[mtrr] slot %d: WC enabled for 0x%08x..0x%08x (%u KiB)\n",
                 slot, aligned_base, aligned_base + sz, sz / 1024);
    return true;
}

static const char *memtype_name(uint8_t t) {
    switch (t) {
        case 0x00: return "UC";
        case 0x01: return "WC";
        case 0x04: return "WT";
        case 0x05: return "WP";
        case 0x06: return "WB";
        default:   return "??";
    }
}

/* Compute the effective MTRR memory type the CPU resolves for a physical
 * address: variable ranges win over the default type, and on overlap UC wins,
 * then WT over WB.  This is exactly what determines whether our framebuffer is
 * actually write-combining or got clobbered back to UC by a BIOS range. */
int mtrr_effective_type(uint32_t addr) {
    if (!mtrr_supported()) return -1;
    uint32_t def_lo = 0;
    rdmsr64(MSR_IA32_MTRR_DEFTYPE, &def_lo, NULL);
    if (!(def_lo & (1u << 11))) return -1;           /* MTRRs disabled */
    uint32_t cap_lo = 0; rdmsr64(MSR_IA32_MTRRCAP, &cap_lo, NULL);
    uint32_t vcnt = cap_lo & 0xFFu;
    int eff = -1;
    for (uint32_t i = 0; i < vcnt; i++) {
        uint32_t b_lo = 0, m_lo = 0;
        rdmsr64(MSR_IA32_MTRR_PHYSBASE + i * 2, &b_lo, NULL);
        rdmsr64(MSR_IA32_MTRR_PHYSMASK + i * 2, &m_lo, NULL);
        if (!(m_lo & (1u << 11))) continue;          /* slot not valid */
        uint32_t mask = m_lo & 0xFFFFF000u;
        if ((addr & mask) == (b_lo & mask)) {
            uint8_t t = (uint8_t)(b_lo & 0xFF);
            if (eff < 0) eff = t;
            else if (t == 0x00 || eff == 0x00) eff = 0x00;  /* UC wins */
            else if (t == 0x04 || eff == 0x04) eff = 0x04;  /* WT over WB */
        }
    }
    if (eff < 0) eff = (int)(def_lo & 0xFF);         /* fall back to default */
    return eff;
}

/* Multi-line human-readable MTRR dump for the `sysrep` diagnostic command.
 * Returns bytes written (excluding NUL). */
int mtrr_report(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!mtrr_supported()) {
        return ksnprintf(out, cap, " MTRR        : not supported by CPU\n");
    }
    uint32_t cap_lo = 0; rdmsr64(MSR_IA32_MTRRCAP, &cap_lo, NULL);
    uint32_t def_lo = 0; rdmsr64(MSR_IA32_MTRR_DEFTYPE, &def_lo, NULL);
    uint32_t vcnt = cap_lo & 0xFFu;
    size_t n = 0;
    n += ksnprintf(out + n, cap - n,
                   " MTRR        : %u var slots, WC-cap=%d, MTRRs %s, default=%s\n",
                   vcnt, (cap_lo & (1u << 10)) ? 1 : 0,
                   (def_lo & (1u << 11)) ? "ON" : "OFF",
                   memtype_name((uint8_t)(def_lo & 0xFF)));
    for (uint32_t i = 0; i < vcnt && n < cap - 1; i++) {
        uint32_t b_lo = 0, m_lo = 0;
        rdmsr64(MSR_IA32_MTRR_PHYSBASE + i * 2, &b_lo, NULL);
        rdmsr64(MSR_IA32_MTRR_PHYSMASK + i * 2, &m_lo, NULL);
        if (!(m_lo & (1u << 11))) continue;          /* skip invalid slots */
        uint32_t mask = m_lo & 0xFFFFF000u;
        /* size = (~mask + 1) over the low 4 GiB window */
        uint32_t span = (~mask) + 1u;
        n += ksnprintf(out + n, cap - n,
                       "   var%u: base=0x%08x size=%uMiB type=%s\n",
                       i, b_lo & 0xFFFFF000u, span / (1024u * 1024u),
                       memtype_name((uint8_t)(b_lo & 0xFF)));
    }
    return (int)n;
}

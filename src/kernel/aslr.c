/* ============================================================================
 * NexxoN OS - Modern exploit mitigations (NX + ASLR)
 * ----------------------------------------------------------------------------
 * Boot-time:
 *
 *   1. CPUID(0x80000001).EDX bit 20 - probe for NX support.  Most
 *      x86 implementations since 2003 have it; absence is logged and
 *      we skip the NX enabling step.
 *   2. RDMSR EFER (0xC0000080) -> set bit 11 (NXE) -> WRMSR back.
 *      This makes PTE bit 63 the no-execute bit.
 *   3. crypto_seed() the PRNG with pit_ticks() ^ rtc_seconds() and
 *      cache a 32-bit boot-time ASLR offset.
 *
 * The kernel doesn't yet have a per-process page table - paging is
 * still identity-mapped via the bootloader-installed CR3 - so the
 * NXE bit takes effect the moment we set up our own pages in a
 * future patch.  Until then, aslr_init() configures everything that
 * doesn't require a page table rebuild.
 * ============================================================================ */
#include "aslr.h"
#include "crypto.h"
#include "pit.h"
#include "rtc.h"
#include "debug.h"

static bool     g_nx_enabled = false;
static uint32_t g_aslr_offset = 0;

/* Read CPUID leaf. */
static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf));
}

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t v) {
    uint32_t lo = (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32);
    __asm__ volatile ("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

bool aslr_init(void) {
    /* NX detection via extended CPUID. */
    uint32_t a, b, c, d;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, &a, &b, &c, &d);
        if (d & (1u << 20)) {
            uint64_t efer = rdmsr(0xC0000080);
            efer |= (1ull << 11);
            wrmsr(0xC0000080, efer);
            g_nx_enabled = true;
            debug_printf("[aslr] NX bit enabled in EFER (CPUID 0x80000001.EDX[20] set)\n");
        } else {
            debug_printf("[aslr] CPU has no NX bit - DEP unavailable on this hardware\n");
        }
    }
    /* Seed PRNG with boot-time entropy. */
    crypto_seed(pit_ticks() ^ rtc_seconds() * 0x9E3779B1u);
    /* Cache an ASLR offset.  Keep it small to fit in the user-space
     * address window (top 1 MiB is conventionally avoided by .text
     * placement).  We aim for 0..0x40000 of 16-byte aligned shifts. */
    uint32_t r;
    crypto_random(&r, 4);
    g_aslr_offset = (r & 0x3FFF0u);
    debug_printf("[aslr] randomized base offset = 0x%05x\n", g_aslr_offset);
    return true;
}

bool     aslr_nx_enabled(void) { return g_nx_enabled; }
uint32_t aslr_offset    (void) { return g_aslr_offset; }
uint32_t aslr_random    (void) {
    uint32_t v;
    crypto_random(&v, 4);
    return v;
}
uint32_t aslr_random_canary(void) {
    uint32_t v = aslr_random();
    /* Canary value can't be a string-termination byte, so set MSB. */
    return v | 0x80000000u;
}

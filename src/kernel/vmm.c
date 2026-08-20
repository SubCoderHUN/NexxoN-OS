/* ============================================================================
 * NexxoN OS - Virtual Memory Manager (per-process page tables)
 * ----------------------------------------------------------------------------
 * Adds an isolated address space per Ring 3 task on top of the
 * bootloader-installed identity mapping.  Implementation summary:
 *
 *   - 48 MiB physical-frame pool in BSS, bitmap-tracked.
 *   - Up to 16 process paging roots (vmm_pd_t).  x86_64 roots deep-copy
 *     the low 4 GiB identity hierarchy with supervisor-only kernel leaves,
 *     then split private 2 MiB PDEs only where USER pages are requested.
 *   - vmm_map_page() pins a page-table frame on demand and writes the
 *     PTE with the requested flag set (USER, RW, NX, ...).
 *   - vmm_switch() reloads CR3.  The bootloader CR3 stays valid as the
 *     "kernel" address space.
 *   - vmm_resolve() walks PD+PT for a virtual address.
 *
 * Wired into execve via posix.c once that lands, and consumed by
 * aslr.c when relocating user binaries.
 * ============================================================================ */
#include "vmm.h"
#include "smp.h"
#include "string.h"
#include "debug.h"

#if defined(__x86_64__)
/* ============================================================================
 * x86_64 - 4-level paging (PML4 -> PDPT -> PD -> PT)
 * ----------------------------------------------------------------------------
 * The boot64 stub already entered long mode, identity-mapped the low 4 GiB
 * with 2 MiB pages, and loaded CR3 with the PML4 it built.  We ADOPT that
 * live PML4 as the kernel address space (vmm_init) and add 4 KiB-granular
 * mappings on demand (vmm_map_page) by walking/creating PDPT/PD/PT levels
 * from a physical frame pool.  Fresh tables live in the identity-mapped pool
 * (< 4 GiB), so their physical address is directly dereferenceable.
 *
 * This is the proven boot64/vmm64.c machinery promoted behind the SAME public
 * API the 32-bit kernel uses (vmm_map_page / switch / resolve / clone / ...),
 * so the rest of the kernel ports across unchanged.
 * ============================================================================ */
#define PG_PRESENT  0x1ULL
#define PG_RW       0x2ULL
#define PG_USER     0x4ULL
#define PG_PS       0x80ULL
#define ADDR_MASK   0x000FFFFFFFFFF000ULL   /* 52-bit phys, 4 KiB aligned */

/* 48 MiB physical-frame pool: backs private process page tables, ELF pages,
 * stacks, brk and mmap.  Identity-mapped (phys == virt < 4 GiB), so kernel
 * code can initialise a guest frame without switching into the guest CR3. */
#define VMM_TOTAL_FRAMES    12288
static uint8_t  g_frame_pool[VMM_TOTAL_FRAMES * VMM_PAGE_SIZE] ALIGNED(4096);
static uint8_t  g_frame_bitmap[VMM_TOTAL_FRAMES / 8];
static uint32_t g_used_frames = 0;
static bool     g_nx_active = false;

/* Up to 16 cloned address spaces (each PML4 is exactly one page). */
#define VMM_MAX_PDS         16
static vmm_pd_t  g_pd_table[VMM_MAX_PDS] ALIGNED(4096);
static bool      g_pd_used [VMM_MAX_PDS];
static vmm_pd_t *g_kernel_pd_ptr = NULL;

/* Single-CPU critical section (no SMP on x86_64 yet): save/clear/restore IF.
 * Replaces the 32-bit kspin path so vmm.c stays free of an smp.c dependency
 * in the 64-bit build. */
static inline uintptr_t irq_save(void) {
    uintptr_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uintptr_t f) {
    __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory", "cc");
}
static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void invlpg(uintptr_t v) {
    __asm__ volatile ("invlpg (%0)" :: "r"(v) : "memory");
}

uint32_t vmm_frame_alloc(void) {
    uintptr_t fl = irq_save();
    for (uint32_t i = 0; i < VMM_TOTAL_FRAMES; i++) {
        uint32_t bi = i >> 3; uint8_t bm = (uint8_t)(1u << (i & 7));
        if (!(g_frame_bitmap[bi] & bm)) {
            g_frame_bitmap[bi] |= bm; g_used_frames++;
            uint32_t phys = (uint32_t)(uintptr_t)&g_frame_pool[i * VMM_PAGE_SIZE];
            irq_restore(fl);
            uint64_t *z = (uint64_t *)(uintptr_t)phys;    /* zero it (identity) */
            for (uint32_t j = 0; j < VMM_PAGE_SIZE / 8; j++) z[j] = 0;
            return phys;
        }
    }
    irq_restore(fl);
    return 0;
}

void vmm_frame_free(uint32_t phys) {
    uint32_t base = (uint32_t)(uintptr_t)g_frame_pool;
    if (phys < base) return;
    uint32_t off = phys - base;
    if (off >= VMM_TOTAL_FRAMES * VMM_PAGE_SIZE) return;
    uint32_t idx = off / VMM_PAGE_SIZE;
    uint32_t bi = idx >> 3; uint8_t bm = (uint8_t)(1u << (idx & 7));
    uintptr_t fl = irq_save();
    if (g_frame_bitmap[bi] & bm) { g_frame_bitmap[bi] &= (uint8_t)~bm; g_used_frames--; }
    irq_restore(fl);
}

int vmm_frame_stats(uint32_t *used, uint32_t *total) {
    if (used)  *used  = g_used_frames * VMM_PAGE_SIZE;
    if (total) *total = VMM_TOTAL_FRAMES * VMM_PAGE_SIZE;
    return 0;
}

static bool frame_owned(uintptr_t phys) {
    uintptr_t base = (uintptr_t)g_frame_pool;
    return phys >= base &&
           phys < base + (uintptr_t)VMM_TOTAL_FRAMES * VMM_PAGE_SIZE &&
           (phys & (VMM_PAGE_SIZE - 1u)) == 0;
}

bool vmm_init(void) {
    memset(g_frame_bitmap, 0, sizeof(g_frame_bitmap));
    memset(g_pd_used, 0, sizeof(g_pd_used));
    g_used_frames = 0;
    {
        uint32_t lo, hi;
        __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi)
                          : "c"(0xC0000080u));
        g_nx_active = ((((uint64_t)hi << 32) | lo) & (1ULL << 11)) != 0;
    }
    /* Adopt the live PML4 the boot stub built and loaded into CR3. */
    g_kernel_pd_ptr = (vmm_pd_t *)(uintptr_t)(read_cr3() & ADDR_MASK);
    debug_printf("[vmm] x86_64 4-level paging: adopted boot PML4 at %p "
                 "(%u KiB frame pool)\n", (void *)g_kernel_pd_ptr,
                 (unsigned)(VMM_TOTAL_FRAMES * VMM_PAGE_SIZE / 1024u));
    return g_kernel_pd_ptr != NULL;
}

vmm_pd_t *vmm_kernel_pd(void) { return g_kernel_pd_ptr; }

/* Paging is already active (boot stub turned on CR0.PG + CR4.PAE + EFER.LME);
 * nothing to enable. */
void vmm_enable_paging(void) { }

/* ---------------------------------------------------------------------------
 * Page Attribute Table (PAT) write-combining for the framebuffer (64-bit).
 *
 * Same rationale as the 32-bit path (see the #else branch): firmware often
 * pins a UC variable-MTRR over the LFB (P8Z77: var3 @0xe0000000 512MiB) and
 * UC beats any WC MTRR of ours — every pixel write becomes an uncached PCIe
 * transaction, which is why bare-metal rendering crawls and gets WORSE with
 * resolution.  Per SDM Vol.3 Table 11-7, PAT=WC + MTRR=UC resolves to WC, so
 * we program PAT slot PA1 = WC and flip the identity-map 2 MiB PDEs covering
 * the LFB to PAT index 1 (PWT=1, PCD=0, PAT-bit=0).  For a 2 MiB PDE the PAT
 * bit is bit 12 — same position as the 32-bit 4 MiB-PSE page.
 * ------------------------------------------------------------------------- */
#define MSR_IA32_PAT      0x277u
#define PAT_TYPE_WC       0x01u
#define PDE_PAT_BIT       (1ULL << 12)  /* 2 MiB-page PAT bit */

bool vmm_set_pat_write_combining(uint32_t phys, uint32_t size) {
    if (!g_kernel_pd_ptr) return false;
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u));
    if (!(d & (1u << 16))) return false;        /* CPUID.01h:EDX.PAT */

    /* SDM 11.12.4 safe memory-type-change sequence. */
    uintptr_t rflags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile ("cli");

    uintptr_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    /* CD=1 (bit30), NW=0 (bit29). */
    __asm__ volatile ("mov %0, %%cr0"
                      :: "r"((cr0 & ~(1UL << 29)) | (1UL << 30)));
    __asm__ volatile ("wbinvd");
    uintptr_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    /* PA1 -> WC (touch only byte 1 of the low dword). */
    uint32_t pat_lo, pat_hi;
    __asm__ volatile ("rdmsr" : "=a"(pat_lo), "=d"(pat_hi) : "c"(MSR_IA32_PAT));
    pat_lo = (pat_lo & ~0x0000FF00u) | (PAT_TYPE_WC << 8);
    __asm__ volatile ("wrmsr" :: "a"(pat_lo), "d"(pat_hi), "c"(MSR_IA32_PAT));

    /* Point every 2 MiB PDE covering the range at PAT index 1.  Walk the
     * LIVE kernel PML4 (adopted from the boot stub, identity-mapped). */
    uint64_t start = (uint64_t)phys & ~0x1FFFFFULL;
    uint64_t end   = ((uint64_t)phys + size + 0x1FFFFFULL) & ~0x1FFFFFULL;
    int flipped = 0;
    for (uint64_t ad = start; ad < end; ad += 0x200000ULL) {
        uint64_t *pml4 = g_kernel_pd_ptr->entries;
        uint64_t pml4e = pml4[(ad >> 39) & 0x1FF];
        if (!(pml4e & PG_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & ADDR_MASK);
        uint64_t pdpte = pdpt[(ad >> 30) & 0x1FF];
        if (!(pdpte & PG_PRESENT) || (pdpte & PG_PS)) continue; /* no 1G here */
        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & ADDR_MASK);
        int pi = (int)((ad >> 21) & 0x1FF);
        uint64_t e = pd[pi];
        if (!(e & PG_PRESENT) || !(e & PG_PS)) continue;
        e &= ~((uint64_t)VMM_FLAG_PCD | PDE_PAT_BIT);  /* PCD=0, PAT=0 */
        e |=  (uint64_t)VMM_FLAG_PWT;                  /* PWT=1 -> index 1 = WC */
        pd[pi] = e;
        flipped++;
    }

    __asm__ volatile ("wbinvd");
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0 & ~(1UL << 30)));   /* CD=0 */

    if (rflags & (1u << 9)) __asm__ volatile ("sti");
    debug_printf("[vmm] PAT write-combining enabled for 0x%08x..0x%08x "
                 "(%d x 2MiB PDE)\n", (uint32_t)start, (uint32_t)end, flipped);
    return flipped > 0;
}

/* Resolve the effective PAT memory type for the 2 MiB page covering `phys`
 * (0=UC,1=WC,4=WT,5=WP,6=WB,7=UC-), or -1 if unavailable.  `sysrep` uses it
 * so the reported framebuffer cache type reflects PAT, not just the MTRR. */
int vmm_fb_pat_type(uint32_t phys) {
    if (!g_kernel_pd_ptr) return -1;
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u));
    if (!(d & (1u << 16))) return -1;
    uint64_t ad = phys;
    uint64_t *pml4 = g_kernel_pd_ptr->entries;
    uint64_t pml4e = pml4[(ad >> 39) & 0x1FF];
    if (!(pml4e & PG_PRESENT)) return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & ADDR_MASK);
    uint64_t pdpte = pdpt[(ad >> 30) & 0x1FF];
    if (!(pdpte & PG_PRESENT) || (pdpte & PG_PS)) return -1;
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & ADDR_MASK);
    uint64_t e = pd[(ad >> 21) & 0x1FF];
    if (!(e & PG_PRESENT) || !(e & PG_PS)) return -1;
    int idx = 0;
    if (e & VMM_FLAG_PWT) idx |= 1;
    if (e & VMM_FLAG_PCD) idx |= 2;
    if (e & PDE_PAT_BIT)  idx |= 4;
    uint32_t pat_lo, pat_hi;
    __asm__ volatile ("rdmsr" : "=a"(pat_lo), "=d"(pat_hi) : "c"(MSR_IA32_PAT));
    uint32_t val = (idx < 4) ? (pat_lo >> (idx * 8)) : (pat_hi >> ((idx - 4) * 8));
    return (int)(val & 0x7u);
}

/* Return the next-level table referenced by tbl[idx], creating it (present|rw,
 * plus any `extra` privilege bits) when absent.  Tables live below 4 GiB so
 * their physical address is directly dereferenceable via the identity map. */
static uint64_t *next_table(uint64_t *tbl, int idx, uintptr_t extra) {
    if (!(tbl[idx] & PG_PRESENT)) {
        uint32_t pa = vmm_frame_alloc();
        if (!pa) return NULL;
        tbl[idx] = (uint64_t)pa | PG_PRESENT | PG_RW | extra;
    } else if (tbl[idx] & PG_PS) {
        return NULL;
    } else {
        tbl[idx] |= extra;                 /* promote privilege if requested */
    }
    return (uint64_t *)(uintptr_t)(tbl[idx] & ADDR_MASK);
}

/* Replace one private address-space 2 MiB identity PDE with a private 4 KiB
 * PT.  The 511 untouched leaves stay supervisor-only; vmm_map_page then
 * replaces only the requested leaf with a USER mapping. */
static uint64_t *split_2m_page(uint64_t *pdir, int idx, uintptr_t user) {
    uint64_t old = pdir[idx];
    if (!(old & PG_PRESENT) || !(old & PG_PS)) return NULL;
    uint32_t pt_pa = vmm_frame_alloc();
    if (!pt_pa) return NULL;
    uint64_t *pt = (uint64_t *)(uintptr_t)pt_pa;
    uint64_t base = old & 0x000FFFFFFFE00000ULL;
    uint64_t leaf_flags = PG_PRESENT;
    if (old & PG_RW) leaf_flags |= PG_RW;
    if (old & VMM_FLAG_PWT) leaf_flags |= VMM_FLAG_PWT;
    if (old & VMM_FLAG_PCD) leaf_flags |= VMM_FLAG_PCD;
    if (old & VMM_FLAG_NX)  leaf_flags |= VMM_FLAG_NX;
    for (int i = 0; i < VMM_TABLE_ENTRIES; i++)
        pt[i] = base + (uint64_t)i * VMM_PAGE_SIZE + leaf_flags;
    pdir[idx] = (uint64_t)pt_pa | PG_PRESENT | PG_RW | user;
    return pt;
}

int vmm_map_page(vmm_pd_t *pd, uintptr_t virt, uintptr_t phys, uintptr_t flags) {
    if (!pd) return -1;
    uintptr_t user = (flags & VMM_FLAG_USER) ? PG_USER : 0;
    uintptr_t fl = irq_save();
    uint64_t *pml4 = pd->entries;
    uint64_t *pdpt = next_table(pml4, (int)((virt >> 39) & 0x1FF), user);
    if (!pdpt) { irq_restore(fl); return -1; }
    uint64_t *pdir = next_table(pdpt, (int)((virt >> 30) & 0x1FF), user);
    if (!pdir) { irq_restore(fl); return -1; }
    int pdi = (int)((virt >> 21) & 0x1FF);
    uint64_t *pt;
    if ((pdir[pdi] & (PG_PRESENT | PG_PS)) == (PG_PRESENT | PG_PS))
        pt = split_2m_page(pdir, pdi, user);
    else
        pt = next_table(pdir, pdi, user);
    if (!pt) { irq_restore(fl); return -1; }
    pt[(int)((virt >> 12) & 0x1FF)] =
        ((uint64_t)phys & ADDR_MASK) | (flags & 0xFFFULL) | PG_PRESENT
        | (g_nx_active ? ((uint64_t)flags & VMM_FLAG_NX) : 0);
    irq_restore(fl);
    invlpg(virt);
    return 0;
}

int vmm_unmap_page(vmm_pd_t *pd, uintptr_t virt) {
    if (!pd) return -1;
    static const int sh[3] = {39, 30, 21};
    uintptr_t fl = irq_save();
    uint64_t *t = pd->entries;
    for (int lvl = 0; lvl < 3; lvl++) {
        int idx = (int)((virt >> sh[lvl]) & 0x1FF);
        if (!(t[idx] & PG_PRESENT) || (t[idx] & PG_PS)) {
            irq_restore(fl);
            return -1;
        }
        t = (uint64_t *)(uintptr_t)(t[idx] & ADDR_MASK);
    }
    int idx = (int)((virt >> 12) & 0x1FF);
    uint64_t old = t[idx];
    if (!(old & PG_PRESENT)) { irq_restore(fl); return -1; }
    t[idx] = 0;
    irq_restore(fl);
    invlpg(virt);
    if ((old & PG_USER) && frame_owned((uintptr_t)(old & ADDR_MASK)))
        vmm_frame_free((uint32_t)(old & ADDR_MASK));
    return 0;
}

uintptr_t vmm_resolve(vmm_pd_t *pd, uintptr_t virt) {
    if (!pd) return 0;
    static const int sh[3] = {39, 30, 21};
    uint64_t *t = pd->entries;
    for (int lvl = 0; lvl < 3; lvl++) {
        int idx = (int)((virt >> sh[lvl]) & 0x1FF);
        if (!(t[idx] & PG_PRESENT)) return 0;
        if (t[idx] & PG_PS) {              /* large page terminates the walk */
            uint64_t mask = (lvl == 1) ? 0x3FFFFFFFULL : 0x1FFFFFULL;
            return (uintptr_t)((t[idx] & ADDR_MASK & ~mask) | (virt & mask));
        }
        t = (uint64_t *)(uintptr_t)(t[idx] & ADDR_MASK);
    }
    int idx = (int)((virt >> 12) & 0x1FF);
    if (!(t[idx] & PG_PRESENT)) return 0;
    return (uintptr_t)((t[idx] & ADDR_MASK) | (virt & 0xFFFULL));
}

int vmm_query_page(vmm_pd_t *pd, uintptr_t virt,
                   uintptr_t *phys, uintptr_t *flags) {
    if (!pd) return -1;
    static const int sh[3] = {39, 30, 21};
    uint64_t *t = pd->entries;
    bool user = true, rw = true;
    bool nx = false;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint64_t e = t[(virt >> sh[lvl]) & 0x1FF];
        if (!(e & PG_PRESENT)) return -1;
        user = user && ((e & PG_USER) != 0);
        rw = rw && ((e & PG_RW) != 0);
        nx = nx || ((e & VMM_FLAG_NX) != 0);
        if (e & PG_PS) {
            uint64_t mask = (lvl == 1) ? 0x3FFFFFFFULL : 0x1FFFFFULL;
            if (phys) *phys = (uintptr_t)((e & ADDR_MASK & ~mask) |
                                         (virt & mask));
            if (flags) {
                uintptr_t f = VMM_FLAG_PRESENT | (rw ? VMM_FLAG_RW : 0) |
                              (user ? VMM_FLAG_USER : 0) |
                              (nx ? VMM_FLAG_NX : 0) | VMM_FLAG_PS;
                *flags = f;
            }
            return 0;
        }
        t = (uint64_t *)(uintptr_t)(e & ADDR_MASK);
    }
    uint64_t e = t[(virt >> 12) & 0x1FF];
    if (!(e & PG_PRESENT)) return -1;
    user = user && ((e & PG_USER) != 0);
    rw = rw && ((e & PG_RW) != 0);
    nx = nx || ((e & VMM_FLAG_NX) != 0);
    if (phys) *phys = (uintptr_t)((e & ADDR_MASK) | (virt & 0xFFFULL));
    if (flags) {
        uintptr_t f = VMM_FLAG_PRESENT | (rw ? VMM_FLAG_RW : 0) |
                      (user ? VMM_FLAG_USER : 0) |
                      (nx ? VMM_FLAG_NX : 0);
        *flags = f;
    }
    return 0;
}

bool vmm_user_range_ok(vmm_pd_t *pd, uintptr_t virt, size_t len, bool write) {
    if (!pd) return false;
    if (len == 0) return true;
    if (virt + len < virt) return false;
    uintptr_t page = virt & ~(uintptr_t)(VMM_PAGE_SIZE - 1u);
    uintptr_t last = (virt + len - 1u) & ~(uintptr_t)(VMM_PAGE_SIZE - 1u);
    for (;;) {
        uintptr_t flags;
        if (vmm_query_page(pd, page, NULL, &flags) != 0 ||
            !(flags & VMM_FLAG_USER) ||
            (write && !(flags & VMM_FLAG_RW)))
            return false;
        if (page == last) break;
        page += VMM_PAGE_SIZE;
    }
    return true;
}

int vmm_protect_page(vmm_pd_t *pd, uintptr_t virt, uintptr_t flags) {
    if (!pd) return -1;
    uint64_t *t = pd->entries;
    static const int sh[3] = {39, 30, 21};
    uintptr_t fl = irq_save();
    for (int lvl = 0; lvl < 3; lvl++) {
        uint64_t e = t[(virt >> sh[lvl]) & 0x1FF];
        if (!(e & PG_PRESENT) || (e & PG_PS)) {
            irq_restore(fl);
            return -1;
        }
        t = (uint64_t *)(uintptr_t)(e & ADDR_MASK);
    }
    int idx = (int)((virt >> 12) & 0x1FF);
    uint64_t old = t[idx];
    if (!(old & PG_PRESENT) || !(old & PG_USER)) {
        irq_restore(fl);
        return -1;
    }
    old &= ~(PG_RW | VMM_FLAG_NX);
    old |= flags & VMM_FLAG_RW;
    if (g_nx_active) old |= flags & VMM_FLAG_NX;
    t[idx] = old;
    irq_restore(fl);
    invlpg(virt);
    return 0;
}

vmm_pd_t *vmm_clone_for_exec(void) {
    uintptr_t fl = irq_save();
    vmm_pd_t *out = NULL;
    for (int i = 0; i < VMM_MAX_PDS; i++) {
        if (!g_pd_used[i]) {
            g_pd_used[i] = true;
            out = &g_pd_table[i];
            memset(out, 0, sizeof(*out));
            break;
        }
    }
    irq_restore(fl);
    if (!out) return NULL;

    /* High roots are not currently used by the low-identity kernel.  Preserve
     * any future supervisor mappings without granting USER privilege. */
    for (int i = 1; i < VMM_TABLE_ENTRIES; i++)
        out->entries[i] = g_kernel_pd_ptr->entries[i] & ~PG_USER;

    uint64_t root = g_kernel_pd_ptr->entries[0];
    if (!(root & PG_PRESENT) || (root & PG_PS)) {
        vmm_release(out);
        return NULL;
    }
    uint32_t pdpt_pa = vmm_frame_alloc();
    if (!pdpt_pa) { vmm_release(out); return NULL; }
    uint64_t *dst_pdpt = (uint64_t *)(uintptr_t)pdpt_pa;
    uint64_t *src_pdpt = (uint64_t *)(uintptr_t)(root & ADDR_MASK);
    out->entries[0] = (uint64_t)pdpt_pa |
                      ((root & 0xFFFULL) & ~PG_USER);

    for (int j = 0; j < VMM_TABLE_ENTRIES; j++) {
        uint64_t pe = src_pdpt[j];
        if (!(pe & PG_PRESENT)) continue;
        if (pe & PG_PS) {
            dst_pdpt[j] = pe & ~PG_USER;
            continue;
        }
        uint32_t pd_pa = vmm_frame_alloc();
        if (!pd_pa) { vmm_release(out); return NULL; }
        uint64_t *dst_pd = (uint64_t *)(uintptr_t)pd_pa;
        uint64_t *src_pd = (uint64_t *)(uintptr_t)(pe & ADDR_MASK);
        dst_pdpt[j] = (uint64_t)pd_pa | ((pe & 0xFFFULL) & ~PG_USER);
        for (int k = 0; k < VMM_TABLE_ENTRIES; k++) {
            uint64_t de = src_pd[k];
            if (!(de & PG_PRESENT)) continue;
            if (de & PG_PS) {
                dst_pd[k] = de & ~PG_USER;
                continue;
            }
            uint32_t pt_pa = vmm_frame_alloc();
            if (!pt_pa) { vmm_release(out); return NULL; }
            uint64_t *dst_pt = (uint64_t *)(uintptr_t)pt_pa;
            uint64_t *src_pt = (uint64_t *)(uintptr_t)(de & ADDR_MASK);
            dst_pd[k] = (uint64_t)pt_pa | ((de & 0xFFFULL) & ~PG_USER);
            for (int q = 0; q < VMM_TABLE_ENTRIES; q++)
                dst_pt[q] = src_pt[q] & ~PG_USER;
        }
    }
    debug_printf("[vmm] private PML4 %p cloned with supervisor-only kernel map\n",
                 (void *)out);
    return out;
}

void vmm_switch(vmm_pd_t *pd) {
    if (!pd) return;
    __asm__ volatile ("mov %0, %%cr3" :: "r"((uint64_t)(uintptr_t)pd) : "memory");
}

void vmm_release(vmm_pd_t *pd) {
    if (!pd || pd == g_kernel_pd_ptr) return;
    int pdi = (int)(pd - g_pd_table);
    if (pdi < 0 || pdi >= VMM_MAX_PDS || !g_pd_used[pdi]) return;

    /* Root 0 is a fully private clone.  USER leaves own their physical data
     * frames; supervisor identity leaves do not.  Every lower table itself is
     * private and comes from g_frame_pool. */
    uint64_t re = pd->entries[0];
    if ((re & PG_PRESENT) && !(re & PG_PS) &&
        frame_owned((uintptr_t)(re & ADDR_MASK))) {
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(re & ADDR_MASK);
        for (int j = 0; j < VMM_TABLE_ENTRIES; j++) {
            uint64_t pe = pdpt[j];
            if (!(pe & PG_PRESENT) || (pe & PG_PS)) continue;
            uint64_t *pdir = (uint64_t *)(uintptr_t)(pe & ADDR_MASK);
            for (int k = 0; k < VMM_TABLE_ENTRIES; k++) {
                uint64_t de = pdir[k];
                if (!(de & PG_PRESENT) || (de & PG_PS)) continue;
                uint64_t *pt = (uint64_t *)(uintptr_t)(de & ADDR_MASK);
                for (int q = 0; q < VMM_TABLE_ENTRIES; q++) {
                    uint64_t leaf = pt[q];
                    uintptr_t pa = (uintptr_t)(leaf & ADDR_MASK);
                    if ((leaf & (PG_PRESENT | PG_USER)) ==
                        (PG_PRESENT | PG_USER) && frame_owned(pa))
                        vmm_frame_free((uint32_t)pa);
                }
                if (frame_owned((uintptr_t)(de & ADDR_MASK)))
                    vmm_frame_free((uint32_t)(de & ADDR_MASK));
            }
            if (frame_owned((uintptr_t)(pe & ADDR_MASK)))
                vmm_frame_free((uint32_t)(pe & ADDR_MASK));
        }
        vmm_frame_free((uint32_t)(re & ADDR_MASK));
    }
    memset(pd, 0, sizeof(*pd));
    uintptr_t fl = irq_save();
    g_pd_used[pdi] = false;
    irq_restore(fl);
    debug_printf("[vmm] private PML4 %p released (%u/%u KiB used)\n",
                 (void *)pd, g_used_frames * 4u,
                 VMM_TOTAL_FRAMES * 4u);
}

#else  /* !__x86_64__ : original 32-bit 2-level PSE implementation (unchanged) */

/* 4 MiB physical-frame pool (1024 x 4 KiB).  The previous 16 MiB reservation
 * was wildly oversized: the identity map is built from 4 MiB PSE large-page
 * PDEs (zero PT frames), and the only dynamic consumers are a handful of
 * on-demand page tables plus pthread stacks.  Trimming it frees 12 MiB of
 * always-resident BSS, which matters on small VMs where the old footprint
 * pegged the task-manager memory readout near 100%. */
#define VMM_TOTAL_FRAMES    1024        /* 4 MiB / 4 KiB */
#define VMM_MAX_PDS         16
#define VMM_MAX_PTS         64

static uint8_t  g_frame_pool[VMM_TOTAL_FRAMES * VMM_PAGE_SIZE] ALIGNED(4096);
static uint8_t  g_frame_bitmap[VMM_TOTAL_FRAMES / 8];
static uint32_t g_used_frames = 0;

static vmm_pd_t g_pd_table[VMM_MAX_PDS] ALIGNED(4096);
static bool     g_pd_used [VMM_MAX_PDS];
static vmm_pd_t *g_kernel_pd_ptr = NULL;

/* Pre-allocated PT pool to keep map_page heapless. */
static vmm_pt_t g_pt_table[VMM_MAX_PTS] ALIGNED(4096);
static bool     g_pt_used [VMM_MAX_PTS];

/* SMP-safe global allocator lock.  All mutations of g_pd_used, g_pt_used
 * and g_frame_bitmap go through this so an AP enqueuing into a per-CPU
 * runqueue can safely fault in a new user mapping without racing the
 * BSP's setup code. */
static kspin_t g_vmm_lock = 0;

static int alloc_pt_locked(void) {
    for (int i = 0; i < VMM_MAX_PTS; i++) {
        if (!g_pt_used[i]) {
            g_pt_used[i] = true;
            memset(&g_pt_table[i], 0, sizeof(g_pt_table[i]));
            return i;
        }
    }
    return -1;
}

static int pt_index_of(const vmm_pt_t *pt) {
    int n = (int)(pt - g_pt_table);
    return (n >= 0 && n < VMM_MAX_PTS) ? n : -1;
}

static void invlpg(uint32_t virt) {
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

uint32_t vmm_frame_alloc(void) {
    kspin_lock(&g_vmm_lock);
    for (uint32_t i = 0; i < VMM_TOTAL_FRAMES; i++) {
        uint32_t bi = i >> 3;
        uint8_t  bm = (uint8_t)(1u << (i & 7));
        if (!(g_frame_bitmap[bi] & bm)) {
            g_frame_bitmap[bi] |= bm;
            g_used_frames++;
            uint32_t phys = (uint32_t)(uintptr_t)&g_frame_pool[i * VMM_PAGE_SIZE];
            kspin_unlock(&g_vmm_lock);
            return phys;
        }
    }
    kspin_unlock(&g_vmm_lock);
    return 0;
}

void vmm_frame_free(uint32_t phys) {
    uint32_t base = (uint32_t)(uintptr_t)g_frame_pool;
    if (phys < base) return;
    uint32_t off = phys - base;
    if (off >= VMM_TOTAL_FRAMES * VMM_PAGE_SIZE) return;
    uint32_t idx = off / VMM_PAGE_SIZE;
    uint32_t bi = idx >> 3;
    uint8_t  bm = (uint8_t)(1u << (idx & 7));
    kspin_lock(&g_vmm_lock);
    if (g_frame_bitmap[bi] & bm) {
        g_frame_bitmap[bi] &= (uint8_t)~bm;
        g_used_frames--;
    }
    kspin_unlock(&g_vmm_lock);
}

int vmm_frame_stats(uint32_t *used, uint32_t *total) {
    if (used)  *used  = g_used_frames * VMM_PAGE_SIZE;
    if (total) *total = VMM_TOTAL_FRAMES * VMM_PAGE_SIZE;
    return 0;
}

bool vmm_init(void) {
    memset(g_frame_bitmap, 0, sizeof(g_frame_bitmap));
    memset(g_pd_used,      0, sizeof(g_pd_used));
    memset(g_pt_used,      0, sizeof(g_pt_used));
    g_used_frames = 0;
    kspin_init(&g_vmm_lock);

    /* Build the kernel template: identity-map the lower 4 MiB so the
     * existing kernel keeps working when we switch to a user PD.  The
     * lower-4-MiB PDE is supervisor-only - user pages live above. */
    kspin_lock(&g_vmm_lock);
    g_pd_used[0] = true;
    g_kernel_pd_ptr = &g_pd_table[0];
    memset(g_kernel_pd_ptr, 0, sizeof(*g_kernel_pd_ptr));

    int pti = alloc_pt_locked();
    if (pti < 0) { kspin_unlock(&g_vmm_lock); return false; }
    vmm_pt_t *pt = &g_pt_table[pti];
    for (int i = 0; i < VMM_PT_ENTRIES; i++) {
        uint32_t phys = (uint32_t)(i * VMM_PAGE_SIZE);
        pt->entries[i] = phys | VMM_FLAG_PRESENT | VMM_FLAG_RW;
    }
    g_kernel_pd_ptr->entries[0] =
        ((uint32_t)(uintptr_t)pt) | VMM_FLAG_PRESENT | VMM_FLAG_RW;
    kspin_unlock(&g_vmm_lock);

    debug_printf("[vmm] kernel PD ready (%u KiB frame pool, %d PT slots, "
                 "%d PD slots)\n",
                 (unsigned)(VMM_TOTAL_FRAMES * VMM_PAGE_SIZE / 1024u),
                 VMM_MAX_PTS, VMM_MAX_PDS);
    return true;
}

vmm_pd_t *vmm_kernel_pd(void) { return g_kernel_pd_ptr; }

/* Enable hardware paging with a 4 MiB PSE identity-map covering all 4 GiB.
 *
 * We overwrite every PDE in the kernel template with a 4 MiB "large page"
 * entry (PS=1 in CR4.PSE mode).  Each PDE maps a 4 MiB physical region at
 * the same virtual address (identity), supervisor-only, read-write.
 *
 * Why 4 GiB?  The kernel stack, frame pool, and BSS can extend beyond
 * the original 4 MiB stub mapping.  The VESA framebuffer sits near
 * 0xFD000000.  LAPIC MMIO lives at 0xFEE00000.  A full identity map avoids
 * needing to enumerate and special-case every device region.
 *
 * vmm_map_page() handles PSE PDEs: if it encounters a PS-flagged PDE it
 * allocates a fresh 4 KiB PT and pre-populates it with the same identity
 * mapping before replacing the large-page entry.  This keeps vmm_map_page()
 * correct for future user-space memory allocation. */
void vmm_enable_paging(void) {
    if (!g_kernel_pd_ptr) return;

    /* Fill all 1024 PDEs with 4 MiB identity-map entries. */
    for (uint32_t pde = 0; pde < (uint32_t)VMM_PD_ENTRIES; pde++) {
        uint32_t phys = pde * 0x400000u;   /* pde * 4 MiB */
        g_kernel_pd_ptr->entries[pde] =
            phys | VMM_FLAG_PS | VMM_FLAG_RW | VMM_FLAG_USER | VMM_FLAG_PRESENT;
    }

    /* Enable PSE in CR4 so the CPU honours bit 7 (PS) in PDEs. */
    uint32_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 4);   /* CR4.PSE */
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));

    /* Point CR3 at the kernel page directory. */
    __asm__ volatile ("mov %0, %%cr3" :: "r"((uint32_t)(uintptr_t)g_kernel_pd_ptr));

    /* Set CR0.PG to activate paging. */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1u << 31);  /* CR0.PG */
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    debug_printf("[vmm] paging enabled: CR0.PG=1, CR4.PSE=1, "
                 "4 GiB identity-mapped with 4 MiB pages\n");
}

/* ---------------------------------------------------------------------------
 * Page Attribute Table (PAT) write-combining for the framebuffer.
 *
 * On boards whose firmware pins a huge UC variable-MTRR over the LFB (e.g. the
 * ASUS P8Z77's `var3 @0xe0000000 512MiB`), our WC MTRR loses: when two MTRRs
 * overlap and one is UC, UC wins, so every blit crawls over uncached PCIe
 * writes.  PAT overrides that: per Intel SDM Vol.3 Table 11-7, a PAT type of
 * WC combined with an MTRR type of UC resolves to WC.  So we reprogram one PAT
 * slot to WC and flip the cache bits of the 4 MiB identity PDEs covering the
 * framebuffer to select it.  This is exactly how Linux's ioremap_wc() beats a
 * UC MTRR.  Returns false (and changes nothing) if the CPU lacks PAT.
 *
 * PAT index for a PDE = (PAT<<2)|(PCD<<1)|PWT, where for a 4 MiB page the PAT
 * bit is PDE bit 12.  We use index 1 (PWT=1, PCD=0, PAT=0) and set PA1 = WC.
 * ------------------------------------------------------------------------- */
#define MSR_IA32_PAT      0x277u
#define PAT_TYPE_WC       0x01u
#define PDE_PAT_BIT       (1u << 12)   /* 4 MiB-page PAT bit */

bool vmm_set_pat_write_combining(uint32_t phys, uint32_t size) {
    if (!g_kernel_pd_ptr) return false;
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u));
    if (!(d & (1u << 16))) return false;        /* CPUID.01h:EDX.PAT */

    /* SDM 11.12.4 safe memory-type-change sequence. */
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli");

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    /* CD=1 (bit30), NW=0 (bit29). */
    __asm__ volatile ("mov %0, %%cr0" :: "r"((cr0 & ~(1u << 29)) | (1u << 30)));
    __asm__ volatile ("wbinvd");
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    /* PA1 -> WC (touch only byte 1 of the low dword). */
    uint32_t pat_lo, pat_hi;
    __asm__ volatile ("rdmsr" : "=a"(pat_lo), "=d"(pat_hi) : "c"(MSR_IA32_PAT));
    pat_lo = (pat_lo & ~0x0000FF00u) | (PAT_TYPE_WC << 8);
    __asm__ volatile ("wrmsr" :: "a"(pat_lo), "d"(pat_hi), "c"(MSR_IA32_PAT));

    /* Point every 4 MiB PDE covering the range at PAT index 1. */
    uint32_t start = phys & ~0x3FFFFFu;
    uint32_t end   = (phys + size + 0x3FFFFFu) & ~0x3FFFFFu;
    for (uint32_t ad = start; ad < end; ad += 0x400000u) {
        uint32_t pi = ad >> 22;
        if (pi >= (uint32_t)VMM_PD_ENTRIES) break;
        uint32_t e = g_kernel_pd_ptr->entries[pi];
        if (!(e & VMM_FLAG_PRESENT) || !(e & VMM_FLAG_PS)) continue;
        e &= ~(VMM_FLAG_PCD | PDE_PAT_BIT);   /* PCD=0, PAT=0 */
        e |=  VMM_FLAG_PWT;                    /* PWT=1 -> index 1 = WC */
        g_kernel_pd_ptr->entries[pi] = e;
    }

    __asm__ volatile ("wbinvd");
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0 & ~(1u << 30)));   /* CD=0 */

    if (eflags & (1u << 9)) __asm__ volatile ("sti");
    debug_printf("[vmm] PAT write-combining enabled for 0x%08x..0x%08x\n",
                 start, end);
    return true;
}

/* Resolve the effective PAT memory type the CPU will use for a 4 MiB page
 * covering `phys` (0=UC,1=WC,4=WT,5=WP,6=WB,7=UC-), or -1 if PAT is absent /
 * the page isn't a present large page.  Used by `sysrep` so the reported
 * framebuffer cache type reflects PAT, not just the (BIOS-clobbered) MTRR. */
int vmm_fb_pat_type(uint32_t phys) {
    if (!g_kernel_pd_ptr) return -1;
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u));
    if (!(d & (1u << 16))) return -1;
    uint32_t pi = phys >> 22;
    if (pi >= (uint32_t)VMM_PD_ENTRIES) return -1;
    uint32_t e = g_kernel_pd_ptr->entries[pi];
    if (!(e & VMM_FLAG_PRESENT) || !(e & VMM_FLAG_PS)) return -1;
    int idx = 0;
    if (e & VMM_FLAG_PWT) idx |= 1;
    if (e & VMM_FLAG_PCD) idx |= 2;
    if (e & PDE_PAT_BIT)  idx |= 4;
    uint32_t pat_lo, pat_hi;
    __asm__ volatile ("rdmsr" : "=a"(pat_lo), "=d"(pat_hi) : "c"(MSR_IA32_PAT));
    uint32_t val = (idx < 4) ? (pat_lo >> (idx * 8)) : (pat_hi >> ((idx - 4) * 8));
    return (int)(val & 0x7u);
}

vmm_pd_t *vmm_clone_for_exec(void) {
    kspin_lock(&g_vmm_lock);
    for (int i = 0; i < VMM_MAX_PDS; i++) {
        if (!g_pd_used[i]) {
            g_pd_used[i] = true;
            memcpy(&g_pd_table[i], g_kernel_pd_ptr, sizeof(vmm_pd_t));
            kspin_unlock(&g_vmm_lock);
            return &g_pd_table[i];
        }
    }
    kspin_unlock(&g_vmm_lock);
    return NULL;
}

int vmm_map_page(vmm_pd_t *pd, uint32_t virt, uint32_t phys, uint32_t flags) {
    if (!pd) return -1;
    uint32_t pde = virt >> 22;
    uint32_t pte = (virt >> 12) & 0x3FF;
    /* Carry USER into the PDE so the CPU's privilege check honours the
     * caller's intent - kernel-only mappings get supervisor PDEs. */
    uint32_t pde_user = (flags & VMM_FLAG_USER) ? VMM_FLAG_USER : 0;
    vmm_pt_t *pt;
    kspin_lock(&g_vmm_lock);

    /* Allocate a new PT when:
     *   (a) the PDE is not present at all, OR
     *   (b) the PDE is a 4 MiB PSE large-page entry (PS bit set) -- we must
     *       replace it with a 4 KiB PT, pre-populated with the same identity
     *       mapping so existing kernel access through this PDE is preserved. */
    bool is_large = (pd->entries[pde] & VMM_FLAG_PRESENT) &&
                    (pd->entries[pde] & VMM_FLAG_PS);
    if (!(pd->entries[pde] & VMM_FLAG_PRESENT) || is_large) {
        int pti = alloc_pt_locked();
        if (pti < 0) { kspin_unlock(&g_vmm_lock); return -1; }
        pt = &g_pt_table[pti];
        if (is_large) {
            /* Reconstruct identity mapping for the 1024 x 4 KiB pages that
             * the large page covered, so kernel code keeps working after the
             * PDE is replaced. */
            uint32_t base_phys = pde * 0x400000u;
            for (int j = 0; j < VMM_PT_ENTRIES; j++) {
                pt->entries[j] = (base_phys + (uint32_t)(j * VMM_PAGE_SIZE))
                                 | VMM_FLAG_PRESENT | VMM_FLAG_RW;
            }
        }
        pd->entries[pde] = ((uint32_t)(uintptr_t)pt) | VMM_FLAG_PRESENT |
                           VMM_FLAG_RW | pde_user;
    } else {
        pt = (vmm_pt_t *)(uintptr_t)(pd->entries[pde] & ~0xFFFu);
        /* If a new mapping needs USER access through a previously
         * supervisor-only PDE, promote it.  USER is a "max privilege"
         * gate, not a per-page assertion. */
        if (pde_user && !(pd->entries[pde] & VMM_FLAG_USER)) {
            pd->entries[pde] |= VMM_FLAG_USER;
        }
    }
    pt->entries[pte] = (phys & ~0xFFFu) | (flags & 0xFFFu) | VMM_FLAG_PRESENT;
    kspin_unlock(&g_vmm_lock);
    invlpg(virt);
    return 0;
}

int vmm_unmap_page(vmm_pd_t *pd, uint32_t virt) {
    if (!pd) return -1;
    uint32_t pde = virt >> 22;
    uint32_t pte = (virt >> 12) & 0x3FF;
    kspin_lock(&g_vmm_lock);
    if (!(pd->entries[pde] & VMM_FLAG_PRESENT)) {
        kspin_unlock(&g_vmm_lock);
        return -1;
    }
    vmm_pt_t *pt = (vmm_pt_t *)(uintptr_t)(pd->entries[pde] & ~0xFFFu);
    pt->entries[pte] = 0;
    /* If the PT now has no live entries, recycle the slot.  Avoids
     * silently leaking the 64-slot pool over many exec() cycles. */
    bool empty = true;
    for (int i = 0; i < VMM_PT_ENTRIES; i++) {
        if (pt->entries[i] & VMM_FLAG_PRESENT) { empty = false; break; }
    }
    if (empty) {
        int idx = pt_index_of(pt);
        if (idx >= 0) g_pt_used[idx] = false;
        pd->entries[pde] = 0;
    }
    kspin_unlock(&g_vmm_lock);
    invlpg(virt);
    return 0;
}

uint32_t vmm_resolve(vmm_pd_t *pd, uint32_t virt) {
    if (!pd) return 0;
    uint32_t pde = virt >> 22;
    uint32_t pte = (virt >> 12) & 0x3FF;
    if (!(pd->entries[pde] & VMM_FLAG_PRESENT)) return 0;
    vmm_pt_t *pt = (vmm_pt_t *)(uintptr_t)(pd->entries[pde] & ~0xFFFu);
    if (!(pt->entries[pte] & VMM_FLAG_PRESENT)) return 0;
    return (pt->entries[pte] & ~0xFFFu) | (virt & 0xFFFu);
}

void vmm_switch(vmm_pd_t *pd) {
    if (!pd) return;
    __asm__ volatile ("mov %0, %%cr3" :: "r"((uint32_t)(uintptr_t)pd));
}

/* Tear down a user PD: walk every user-space PDE, return its PT slot to
 * the pool, and mark the PD slot free.  The kernel template PD (slot 0)
 * is permanent and never released. */
void vmm_release(vmm_pd_t *pd) {
    if (!pd || pd == g_kernel_pd_ptr) return;
    kspin_lock(&g_vmm_lock);
    for (int pde = 1; pde < VMM_PD_ENTRIES; pde++) {
        if (!(pd->entries[pde] & VMM_FLAG_PRESENT)) continue;
        vmm_pt_t *pt = (vmm_pt_t *)(uintptr_t)(pd->entries[pde] & ~0xFFFu);
        int idx = pt_index_of(pt);
        if (idx >= 0) g_pt_used[idx] = false;
        pd->entries[pde] = 0;
    }
    int pdi = (int)(pd - g_pd_table);
    if (pdi > 0 && pdi < VMM_MAX_PDS) g_pd_used[pdi] = false;
    kspin_unlock(&g_vmm_lock);
}

#endif /* __x86_64__ */

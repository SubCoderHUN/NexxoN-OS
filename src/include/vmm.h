/* ============================================================================
 * NexxoN OS - Per-process Virtual Memory Manager  (v1.0)
 * ----------------------------------------------------------------------------
 * Each Ring 3 task gets its own paging root.  On x86_64 the low kernel/MMIO
 * identity map is deep-cloned supervisor-only and independent USER PTEs back
 * code/data/stack.  ASLR + NX + per-process isolation depend on this layer.
 *
 *     vmm_init()              - allocates the kernel template PD
 *     vmm_clone_for_exec()    - builds a fresh PD for a new image
 *     vmm_map_page()          - maps a single 4 KiB page with flags
 *     vmm_switch(pd)          - loads a new CR3 (per-CPU)
 *
 * Backing physical-page allocator is the swap.c pool extended to track
 * raw frames; user code never sees raw frames directly.
 * ============================================================================ */
#ifndef NEXXON_VMM_H
#define NEXXON_VMM_H

#include "types.h"

#define VMM_PAGE_SIZE     4096u

#define VMM_FLAG_PRESENT  (1u << 0)
#define VMM_FLAG_RW       (1u << 1)
#define VMM_FLAG_USER     (1u << 2)
#define VMM_FLAG_PWT      (1u << 3)
#define VMM_FLAG_PCD      (1u << 4)
#define VMM_FLAG_ACCESSED (1u << 5)
#define VMM_FLAG_DIRTY    (1u << 6)
#define VMM_FLAG_PS       (1u << 7)    /* PDE: large page (4 MiB 32-bit / 2 MiB 64-bit) */
#define VMM_FLAG_NX       (1ULL << 63) /* PTE bit 63 (PAE / IA-32e no-execute)  */

/* ----------------------------------------------------------------------------
 * Page-table geometry is architecture-dependent.  The 32-bit kernel uses a
 * classic 2-level directory (1024 x 32-bit entries / table).  The x86_64 port
 * uses 4-level paging (PML4 -> PDPT -> PD -> PT) where EVERY level is a page
 * of 512 x 64-bit entries.  `vmm_pd_t` is the address-space ROOT table in both
 * worlds (a PD in 32-bit, the PML4 in 64-bit) and `vmm_pt_t` is a generic
 * lower-level table; the public API below is identical for both.
 * ------------------------------------------------------------------------- */
#if defined(__x86_64__)
#define VMM_TABLE_ENTRIES 512
#define VMM_PD_ENTRIES    VMM_TABLE_ENTRIES
#define VMM_PT_ENTRIES    VMM_TABLE_ENTRIES
typedef struct vmm_pd {
    uint64_t entries[VMM_TABLE_ENTRIES] ALIGNED(4096);
} vmm_pd_t;
typedef struct vmm_pt {
    uint64_t entries[VMM_TABLE_ENTRIES] ALIGNED(4096);
} vmm_pt_t;
#else
#define VMM_PD_ENTRIES    1024
#define VMM_PT_ENTRIES    1024
typedef struct vmm_pd {
    uint32_t entries[VMM_PD_ENTRIES] ALIGNED(4096);
} vmm_pd_t;
typedef struct vmm_pt {
    uint32_t entries[VMM_PT_ENTRIES] ALIGNED(4096);
} vmm_pt_t;
#endif

bool       vmm_init             (void);
void       vmm_enable_paging    (void);  /* PSE identity-map 4 GiB, CR0.PG=1 */
/* Mark the framebuffer range Write-Combining via PAT (overrides a BIOS UC
 * MTRR; SDM Vol.3 Table 11-7).  Returns false if the CPU lacks PAT. */
bool       vmm_set_pat_write_combining(uint32_t phys, uint32_t size);
/* PAT-resolved memory type for the 4 MiB page at `phys` (0=UC,1=WC,...,6=WB),
 * or -1 if PAT is unavailable.  For diagnostics. */
int        vmm_fb_pat_type      (uint32_t phys);
vmm_pd_t  *vmm_kernel_pd        (void);
vmm_pd_t  *vmm_clone_for_exec   (void);
/* Addresses are uintptr_t: 32-bit under -m32 (identical ABI to the old
 * uint32_t signatures), 64-bit under -m64 so full canonical addresses fit. */
int        vmm_map_page         (vmm_pd_t *pd, uintptr_t virt, uintptr_t phys,
                                 uintptr_t flags);
int        vmm_unmap_page       (vmm_pd_t *pd, uintptr_t virt);
uintptr_t  vmm_resolve          (vmm_pd_t *pd, uintptr_t virt);
/* Query the leaf mapping and effective page flags.  Returns 0 when mapped. */
int        vmm_query_page       (vmm_pd_t *pd, uintptr_t virt,
                                 uintptr_t *phys, uintptr_t *flags);
/* True only when every page in the range is present and user-accessible. */
bool       vmm_user_range_ok    (vmm_pd_t *pd, uintptr_t virt, size_t len,
                                 bool write);
/* Replace RW/NX/USER permission bits on one existing 4 KiB user page. */
int        vmm_protect_page     (vmm_pd_t *pd, uintptr_t virt,
                                 uintptr_t flags);
void       vmm_switch           (vmm_pd_t *pd);
void       vmm_release          (vmm_pd_t *pd);

/* Physical frame allocator (4 KiB granularity, 16 MiB total on x86_64). */
uint32_t   vmm_frame_alloc      (void);
void       vmm_frame_free       (uint32_t phys);
int        vmm_frame_stats      (uint32_t *used, uint32_t *total);

#endif /* NEXXON_VMM_H */

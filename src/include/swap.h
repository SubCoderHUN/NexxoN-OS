/* ============================================================================
 * NexxoN OS - Virtual Memory Swap & Pagefile  (TASK 24, v1.0)
 * ----------------------------------------------------------------------------
 * Daemon-style page swap layer that exports a single in-kernel pool of
 * "swappable" pages backed by /sys/swapfile.sys.  When system RAM use
 * exceeds 90% the daemon picks the least-recently-touched page from
 * its tracked set, copies it out to the swap file, and frees the
 * physical backing.  A subsequent #PF on the swapped-out page triggers
 * demand paging from disk.
 *
 * In this milestone the implementation is in two halves:
 *
 *   * The metadata + LRU tracking + /sys/swapfile.sys management runs
 *     unconditionally and is exposed for the Task Manager's "Memory"
 *     tab.  swap_init() sizes the swap file at 64 MiB by default.
 *
 *   * Actual page eviction is gated on a future per-process page-table
 *     implementation; the swap_evict_once() entry point is wired but
 *     returns 0 (no eviction) until the page tables exist.
 *
 * The framework is ready to receive a real MMU layer without API change.
 * ============================================================================ */
#ifndef NEXXON_SWAP_H
#define NEXXON_SWAP_H

#include "types.h"

#define SWAP_FILE_PATH   "/sys/swapfile.sys"
#define SWAP_PAGE_SIZE   4096
#define SWAP_MAX_PAGES   16384       /* 64 MiB */

typedef struct {
    uint32_t total_pages;
    uint32_t pages_in_use;
    uint32_t pages_swapped_out;
    uint32_t lru_size;
} swap_stats_t;

bool swap_init        (void);
void swap_tick        (void);
int  swap_evict_once  (void);     /* returns 1 if a page was evicted */
void swap_get_stats   (swap_stats_t *out);

#endif /* NEXXON_SWAP_H */

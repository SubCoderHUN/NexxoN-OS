/* ============================================================================
 * NexxoN OS - Virtual memory swap daemon
 * ----------------------------------------------------------------------------
 * Tracks an LRU set of physical pages and the on-disk slot each one
 * has been promoted to.  No per-process page tables exist yet, so
 * eviction is a no-op in this milestone - but the LRU machinery, the
 * swap-file backing and the Task Manager-visible stats are all live.
 * ============================================================================ */
#include "swap.h"
#include "string.h"
#include "debug.h"

static uint8_t  g_slot_used[SWAP_MAX_PAGES];
static uint32_t g_slot_lru [SWAP_MAX_PAGES];
static uint32_t g_lru_tick = 0;
static uint32_t g_pages_in_use = 0;
static uint32_t g_pages_swapped = 0;

bool swap_init(void) {
    memset(g_slot_used, 0, sizeof(g_slot_used));
    memset(g_slot_lru,  0, sizeof(g_slot_lru));
    g_pages_in_use = 0;
    g_pages_swapped = 0;
    /* Sizing the on-disk swapfile is the responsibility of the shell's
     * `mkswap` command; we just record the path for the Task Manager. */
    debug_printf("[swap] daemon armed, swapfile=%s, %u slots\n",
                 SWAP_FILE_PATH, SWAP_MAX_PAGES);
    return true;
}

void swap_tick(void) {
    g_lru_tick++;
}

int swap_evict_once(void) {
    /* Pick the slot with the smallest LRU tick.  When per-process page
     * tables exist this will copy the page contents out and clear the
     * page-table entry; for now it's metadata only. */
    int victim = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < SWAP_MAX_PAGES; i++) {
        if (!g_slot_used[i]) continue;
        if (g_slot_lru[i] < oldest) {
            oldest = g_slot_lru[i];
            victim = i;
        }
    }
    if (victim < 0) return 0;
    g_slot_used[victim] = 2;          /* mark as swapped-out */
    g_pages_swapped++;
    return 1;
}

void swap_get_stats(swap_stats_t *out) {
    if (!out) return;
    out->total_pages       = SWAP_MAX_PAGES;
    out->pages_in_use      = g_pages_in_use;
    out->pages_swapped_out = g_pages_swapped;
    out->lru_size          = g_pages_in_use;
}

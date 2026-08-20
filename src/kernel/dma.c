/* ============================================================================
 * NexxoN OS - Generic DMA buffer manager (scatter-gather aware)
 * ----------------------------------------------------------------------------
 * BSS-resident pool of DMA-aligned buffers.  Kernel runs identity-paged,
 * so virtual == physical for every kernel-space pointer (the AHCI /
 * E1000 controllers can ingest the address verbatim).
 *
 * dma_build_sgl() splits a buffer into ≤16 entries each no larger than
 * 4 KiB - the conservative limit that fits inside one EHCI PRD slot
 * and one AHCI PRDT entry alike.  Callers iterate the returned SGL
 * when programming their descriptor rings.
 * ============================================================================ */
#include "dma.h"
#include "string.h"
#include "debug.h"

static dma_buf_t g_bufs[DMA_MAX_BUFS];
static uint8_t   g_pool[DMA_MAX_BUFS][DMA_BUF_SIZE] ALIGNED(4096);

void dma_init(void) {
    for (int i = 0; i < DMA_MAX_BUFS; i++) {
        g_bufs[i].in_use = false;
        g_bufs[i].virt   = g_pool[i];
        g_bufs[i].phys   = (uint32_t)(uintptr_t)g_pool[i];
        g_bufs[i].size   = 0;
    }
}

dma_buf_t *dma_alloc(uint32_t size) {
    if (size > DMA_BUF_SIZE) return NULL;
    for (int i = 0; i < DMA_MAX_BUFS; i++) {
        if (!g_bufs[i].in_use) {
            g_bufs[i].in_use = true;
            g_bufs[i].size   = size;
            return &g_bufs[i];
        }
    }
    return NULL;
}

int dma_free(dma_buf_t *b) {
    if (!b || !b->in_use) return -1;
    b->in_use = false;
    return 0;
}

int dma_build_sgl(const void *buf, uint32_t len, dma_sgl_t *out) {
    if (!buf || !out || len == 0) return -1;
    memset(out, 0, sizeof(*out));
    uint32_t addr = (uint32_t)(uintptr_t)buf;
    while (len > 0 && out->n < DMA_MAX_SG) {
        /* Split at 4 KiB boundaries - widely supported by SATA / NIC DMA. */
        uint32_t page_end = (addr & ~0xFFFu) + 0x1000;
        uint32_t avail    = page_end - addr;
        uint32_t take     = (len < avail) ? len : avail;
        out->entries[out->n].phys = addr;
        out->entries[out->n].len  = take;
        out->n++;
        addr += take;
        len  -= take;
    }
    return len == 0 ? out->n : -1;
}

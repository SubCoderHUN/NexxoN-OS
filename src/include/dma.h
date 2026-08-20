/* ============================================================================
 * NexxoN OS - Generic scatter-gather DMA framework  (TASK 34, v1.0)
 * ----------------------------------------------------------------------------
 * AHCI and the E1000 already build descriptor lists (PRDT, TX/RX rings)
 * - this module is the shared book-keeping layer that hides the
 * descriptor-format differences behind a single API:
 *
 *   dma_alloc_buffer(size, align)   - returns a pointer + physical address
 *   dma_build_sgl(buf, len, sgl)    - splits a buffer into ≤16 SG entries
 *   dma_free(buf)                   - releases the buffer
 *
 * Backed by a single static pool because the kernel still doesn't
 * have a heap.  Plenty for a single in-flight transfer per device.
 * ============================================================================ */
#ifndef NEXXON_DMA_H
#define NEXXON_DMA_H

#include "types.h"

#define DMA_MAX_BUFS     8
#define DMA_BUF_SIZE     (64 * 1024)
#define DMA_MAX_SG       16

typedef struct {
    uint32_t phys;
    uint32_t len;
} dma_sg_entry_t;

typedef struct {
    int             n;
    dma_sg_entry_t  entries[DMA_MAX_SG];
} dma_sgl_t;

typedef struct {
    bool      in_use;
    uint32_t  size;
    uint8_t  *virt;
    uint32_t  phys;
} dma_buf_t;

void       dma_init      (void);
dma_buf_t *dma_alloc     (uint32_t size);
int        dma_free      (dma_buf_t *b);
int        dma_build_sgl (const void *buf, uint32_t len, dma_sgl_t *out);

#endif /* NEXXON_DMA_H */

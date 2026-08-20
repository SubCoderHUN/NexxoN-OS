/* ============================================================================
 * NexxoN OS - VBE mode table (boot-time enumerated, kernel-readable)
 * ----------------------------------------------------------------------------
 * The stage-2 bootloader enumerates every VBE mode at boot time and stores
 * a compact table at physical address VBE_TABLE_PHYS.  The kernel reads
 * this table to discover which resolutions are available for runtime
 * switching without needing to call INT 10h again.
 * ============================================================================ */
#ifndef NEXXON_VBE_TABLE_H
#define NEXXON_VBE_TABLE_H

#include "types.h"

#define VBE_TABLE_PHYS      0x00013000
#define VBE_TABLE_MAX_MODES 64

typedef struct PACKED {
    uint16_t mode_number;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint32_t phys_base;
    uint8_t  bpp;
    uint8_t  _reserved[3];
} vbe_mode_entry_t;

typedef struct PACKED {
    uint16_t count;
    uint16_t _pad;
    vbe_mode_entry_t modes[VBE_TABLE_MAX_MODES];
} vbe_mode_table_t;

int              vbe_table_count(void);
const vbe_mode_entry_t *vbe_table_get(int idx);
const vbe_mode_entry_t *vbe_table_find(uint16_t w, uint16_t h);

#endif /* NEXXON_VBE_TABLE_H */

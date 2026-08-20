/* ============================================================================
 * NexxoN OS - Unified system block-disk abstraction
 * ----------------------------------------------------------------------------
 * Picks the first working internal disk backend at boot:
 *   1. AHCI  - SATA controller running in AHCI mode (the modern default);
 *   2. IDE   - legacy ATA PIO, which also covers a SATA controller left in
 *              IDE / "Compatibility" mode in the BIOS (common on older
 *              boards, and the reason "no SATA disk found" appeared on real
 *              hardware even though a disk was present and bootable).
 *
 * Every read/write returns 0 on success, negative on error - matching both
 * AHCI_OK and ATA_OK so existing callers keep working unchanged.
 * ============================================================================ */
#ifndef NEXXON_SYSDISK_H
#define NEXXON_SYSDISK_H

#include "types.h"

/* Probe AHCI, then legacy IDE/ATA.  Returns true if a disk was found. */
bool        sysdisk_init(void);
bool        sysdisk_present(void);
uint32_t    sysdisk_sector_count(void);
int         sysdisk_read_sector (uint32_t lba, void *buf);
int         sysdisk_write_sector(uint32_t lba, const void *buf);
const char *sysdisk_kind(void);     /* "AHCI", "IDE", or "none" */

#endif /* NEXXON_SYSDISK_H */

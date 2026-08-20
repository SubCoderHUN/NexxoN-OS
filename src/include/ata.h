/* ============================================================================
 * NexxoN OS - ATA / IDE PIO disk driver
 * ----------------------------------------------------------------------------
 * Implements LBA28 PIO transfer mode for legacy IDE-compatible targets.
 * At boot it scans primary/secondary channels and master/slave positions so
 * BIOS/CSM SATA compatibility mappings on real PCs are detected reliably.
 * No DMA, no IRQ-driven completion - we busy-poll the status register because
 * that is portable across every BIOS/QEMU/VirtualBox combination.
 *
 * Sector size is fixed at the standard 512 bytes.  Address space is 128 GiB
 * (28-bit LBA limit), which is more than any of our reasonable disk images.
 * ============================================================================ */
#ifndef NEXXON_ATA_H
#define NEXXON_ATA_H

#include "types.h"

#define ATA_SECTOR_SIZE   512

#define ATA_OK             0
#define ATA_ERR_TIMEOUT   -1
#define ATA_ERR_DRIVE     -2
#define ATA_ERR_BUSY      -3
#define ATA_ERR_BOUNDS    -4

/* Returns true if a usable disk was detected on any legacy IDE target
 * during init.  When false, all ata_read_sector / ata_write_sector calls
 * will return ATA_ERR_DRIVE. */
bool ata_init(void);

bool     ata_present  (void);
uint32_t ata_sector_count(void);          /* 0 if no disk */

int  ata_read_sector  (uint32_t lba, void *buf);
int  ata_write_sector (uint32_t lba, const void *buf);

int  ata_read_sectors (uint32_t lba, uint32_t n, void *buf);
int  ata_write_sectors(uint32_t lba, uint32_t n, const void *buf);

#endif /* NEXXON_ATA_H */

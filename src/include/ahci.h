/* ============================================================================
 * NexxoN OS - Minimalist AHCI (SATA) driver
 * ----------------------------------------------------------------------------
 * Speaks just enough of the AHCI 1.x spec to:
 *   * Find the HBA via PCI (class 01h:06h:01h),
 *   * Bring one SATA port online,
 *   * IDENTIFY DEVICE to learn the LBA48 sector count,
 *   * Issue single-sector READ DMA EXT (0x25) and WRITE DMA EXT (0x35)
 *     commands through command slot 0 with a 1-entry PRDT pointing at a
 *     reusable sector-aligned DMA bounce buffer.
 *
 * Multi-slot queueing, NCQ, hot-plug, port multipliers, and IRQ-driven
 * completion are intentionally omitted - polling is enough for our shell-
 * driven workload and keeps the code free of paging/locking complexity.
 *
 * All raw I/O calls take a physical-disk LBA.  The NXFS layer is responsible
 * for adding the partition offset (LBA 2048) when it wants to address into
 * the data partition.
 * ============================================================================ */
#ifndef NEXXON_AHCI_H
#define NEXXON_AHCI_H

#include "types.h"

#define AHCI_OK             0
#define AHCI_ERR_NO_DEV    -1
#define AHCI_ERR_TIMEOUT   -2
#define AHCI_ERR_IO        -3
#define AHCI_ERR_BOUNDS    -4

/* Locate the controller via PCI, set up command list / FIS area /
 * command tables, IDENTIFY the first available SATA port.  Returns true
 * iff a usable SATA disk is ready to accept I/O. */
bool ahci_init(void);

bool     ahci_present     (void);
uint32_t ahci_sector_count(void);          /* total disk size in 512-B sectors */
uint16_t ahci_port_index  (void);          /* which port we bound to (0..31)   */
/* Compact one-line probe summary (controller location, CAP/PI, why no disk).
 * Surfaced ON SCREEN by kernel.c so bare-metal "no disk" is diagnosable from a
 * photo when no serial console is available. */
const char *ahci_diag     (void);

/* Single-sector PIO-style API.  Internally these always use DMA + the
 * statically allocated bounce buffer, but the caller sees one-sector
 * synchronous semantics. */
int ahci_read_sector  (uint32_t lba, void *buf);
int ahci_write_sector (uint32_t lba, const void *buf);

#endif /* NEXXON_AHCI_H */

/* ============================================================================
 * NexxoN OS - Unified system block-disk abstraction (AHCI -> IDE fallback)
 * ----------------------------------------------------------------------------
 * See sysdisk.h.  sysdisk_init() is the single storage bring-up entry point:
 * it tries the AHCI driver first and, if no AHCI disk is present (the SATA
 * controller is in IDE/Compatibility mode, or it's a PATA disk), falls back
 * to the legacy ATA PIO driver.  The rest of the OS (NXFS, the installer,
 * the shell `installsys`, Settings) talks only to this layer so a disk shows
 * up regardless of how the chipset presents it.
 * ============================================================================ */
#include "sysdisk.h"
#include "ahci.h"
#include "ata.h"
#include "debug.h"

enum { SD_NONE = 0, SD_AHCI, SD_ATA };
static int g_kind = SD_NONE;

bool sysdisk_init(void) {
    if (ahci_init()) {
        g_kind = SD_AHCI;
        debug_ok("sysdisk: AHCI/SATA disk online");
        return true;
    }
    /* No AHCI controller/disk - try legacy IDE/ATA.  This catches a SATA
     * controller left in IDE mode by the firmware, which is exactly why a
     * present, bootable disk read back as "not found" on bare metal. */
    if (ata_init()) {
        g_kind = SD_ATA;
        debug_ok("sysdisk: legacy IDE/ATA disk online (no AHCI)");
        return true;
    }
    g_kind = SD_NONE;
    return false;
}

bool sysdisk_present(void) {
    switch (g_kind) {
        case SD_AHCI: return ahci_present();
        case SD_ATA:  return ata_present();
        default:      return false;
    }
}

uint32_t sysdisk_sector_count(void) {
    switch (g_kind) {
        case SD_AHCI: return ahci_sector_count();
        case SD_ATA:  return ata_sector_count();
        default:      return 0;
    }
}

int sysdisk_read_sector(uint32_t lba, void *buf) {
    switch (g_kind) {
        case SD_AHCI: return ahci_read_sector(lba, buf);
        case SD_ATA:  return ata_read_sector(lba, buf);
        default:      return -1;
    }
}

int sysdisk_write_sector(uint32_t lba, const void *buf) {
    switch (g_kind) {
        case SD_AHCI: return ahci_write_sector(lba, buf);
        case SD_ATA:  return ata_write_sector(lba, buf);
        default:      return -1;
    }
}

const char *sysdisk_kind(void) {
    switch (g_kind) {
        case SD_AHCI: return "AHCI";
        case SD_ATA:  return "IDE";
        default:      return "none";
    }
}

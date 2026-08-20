/* ============================================================================
 * NexxoN OS - Minimal PCI configuration-space enumerator
 * ----------------------------------------------------------------------------
 * Speaks the legacy 0xCF8 / 0xCFC port pair so it works on every BIOS-era
 * machine plus QEMU's i440FX/Q35 PCI fabric.  We do not implement the MMCFG
 * (ECAM) shortcut - the few cycles saved aren't worth the code complexity
 * for a one-shot bus scan at boot.
 *
 * The only user of this module is the AHCI driver, which calls pci_find()
 * with (class=0x01, subclass=0x06, prog_if=0x01) to locate the SATA AHCI
 * controller.
 * ============================================================================ */
#ifndef NEXXON_PCI_H
#define NEXXON_PCI_H

#include "types.h"

#define PCI_CLASS_MASS_STORAGE  0x01
#define PCI_SUB_SATA            0x06
#define PCI_PROG_IF_AHCI        0x01

typedef struct {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint32_t bar[6];
} pci_device_t;

/* Raw config-space access. */
uint32_t pci_read32 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);

/* Walk the entire bus space and fill `out` with the first device whose
 * (class, subclass, prog_if) triplet matches the arguments.  Returns
 * true if a match was found.  Logs the enumeration on COM1. */
bool pci_find(uint8_t cls, uint8_t sub, uint8_t pi, pci_device_t *out);

/* Bulletproof storage discovery for real-hardware variability.  Scans every
 * PCI slot, logs every Class 01 (mass-storage) device it sees, and returns
 * the best candidate for an AHCI driver in `out`.  Preference order:
 *   1. SATA + AHCI 1.0  (sub=0x06, pi=0x01)         -> ideal
 *   2. SATA + any other prog_if                      -> still try AHCI MMIO
 *   3. RAID controllers exposing AHCI                -> last resort
 * Returns true only when a usable AHCI-class candidate is found AND its
 * BAR5 is non-zero (so the caller can map the ABAR immediately).         */
bool pci_find_storage(pci_device_t *out);

/* Re-read a device's BARs - used after we enable bus-master + MMIO bits
 * via the command register in pci_enable_busmaster(). */
void pci_refresh_bars(pci_device_t *dev);

/* Set bits 1 (MMIO enable) + 2 (bus-master) of the device command word. */
void pci_enable_busmaster(const pci_device_t *dev);

/* Fill `out` with every device on every bus.  Returns the number of
 * devices actually written (capped at `max`).  Used by the Device
 * Manager GUI window to render the full hardware inventory. */
int pci_enumerate(pci_device_t *out, int max);

/* Translate a (class, subclass, prog_if) triplet into a human-readable
 * device class name.  Always returns a non-NULL string. */
const char *pci_class_name(uint8_t cls, uint8_t sub, uint8_t pi);

#endif /* NEXXON_PCI_H */

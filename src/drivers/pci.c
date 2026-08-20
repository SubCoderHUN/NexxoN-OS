/* ============================================================================
 * NexxoN OS - PCI configuration-space enumerator (legacy I/O ports)
 * ============================================================================ */
#include "pci.h"
#include "io.h"
#include "debug.h"

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

static inline uint32_t make_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)(dev & 0x1F) << 11)
         | ((uint32_t)(fn  & 0x07) << 8)
         | ((uint32_t)off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(PCI_CONFIG_ADDRESS, make_addr(bus, dev, fn, off));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(PCI_CONFIG_ADDRESS, make_addr(bus, dev, fn, off));
    outl(PCI_CONFIG_DATA, v);
}

static void fill_device(pci_device_t *out, uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t r0 = pci_read32(bus, dev, fn, 0x00);
    uint32_t r2 = pci_read32(bus, dev, fn, 0x08);
    uint32_t r3 = pci_read32(bus, dev, fn, 0x0C);

    out->bus         = bus;
    out->device      = dev;
    out->function    = fn;
    out->vendor_id   = (uint16_t)(r0 & 0xFFFF);
    out->device_id   = (uint16_t)((r0 >> 16) & 0xFFFF);
    out->revision    = (uint8_t)(r2 & 0xFF);
    out->prog_if     = (uint8_t)((r2 >> 8)  & 0xFF);
    out->subclass    = (uint8_t)((r2 >> 16) & 0xFF);
    out->class_code  = (uint8_t)((r2 >> 24) & 0xFF);
    out->header_type = (uint8_t)((r3 >> 16) & 0xFF);
    for (int i = 0; i < 6; i++) {
        out->bar[i] = pci_read32(bus, dev, fn, (uint8_t)(0x10 + i * 4));
    }
}

bool pci_find(uint8_t cls, uint8_t sub, uint8_t pi, pci_device_t *out) {
    debug_printf("[pci] scanning for class=%02x sub=%02x pi=%02x ...\n",
                 cls, sub, pi);

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t r0 = pci_read32((uint8_t)bus, dev, fn, 0x00);
                uint16_t vendor = (uint16_t)(r0 & 0xFFFF);
                if (vendor == 0xFFFF) {
                    if (fn == 0) break;       /* no device here at all */
                    continue;
                }
                uint32_t r2 = pci_read32((uint8_t)bus, dev, fn, 0x08);
                uint8_t  class_code = (uint8_t)((r2 >> 24) & 0xFF);
                uint8_t  subclass   = (uint8_t)((r2 >> 16) & 0xFF);
                uint8_t  prog_if    = (uint8_t)((r2 >> 8)  & 0xFF);

                if (class_code == cls && subclass == sub && prog_if == pi) {
                    fill_device(out, (uint8_t)bus, dev, fn);
                    debug_printf("[pci] match: %02x:%02x.%x  vendor=%04x device=%04x  "
                                 "BAR5=0x%08x\n",
                                 (uint8_t)bus, dev, fn,
                                 out->vendor_id, out->device_id, out->bar[5]);
                    return true;
                }

                /* Single-function devices skip fn=1..7. */
                if (fn == 0) {
                    uint32_t r3 = pci_read32((uint8_t)bus, dev, fn, 0x0C);
                    uint8_t  ht = (uint8_t)((r3 >> 16) & 0xFF);
                    if (!(ht & 0x80)) break;
                }
            }
        }
    }
    return false;
}

/* Bulletproof Class-01 scan.  Walks the full bus/dev/fn space exactly like
 * pci_find() but, instead of demanding an exact (class,sub,pi) match, it
 * scores every Class 01 device it finds, logs them all on COM1, and picks
 * the best AHCI candidate.  The scoring is intentionally generous so real-
 * world quirks (a SATA controller stuck in RAID mode, an HBA reporting a
 * non-standard prog_if, ...) still bring up the disk on actual hardware. */
bool pci_find_storage(pci_device_t *out) {
    if (!out) return false;
    debug_printf("[pci] bulletproof storage scan starting ...\n");

    pci_device_t best;
    int  best_score = 0;
    bool found      = false;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t r0 = pci_read32((uint8_t)bus, dev, fn, 0x00);
                uint16_t vendor = (uint16_t)(r0 & 0xFFFF);
                if (vendor == 0xFFFF) {
                    if (fn == 0) break;
                    continue;
                }

                uint32_t r2 = pci_read32((uint8_t)bus, dev, fn, 0x08);
                uint8_t class_code = (uint8_t)((r2 >> 24) & 0xFF);
                uint8_t subclass   = (uint8_t)((r2 >> 16) & 0xFF);
                uint8_t prog_if    = (uint8_t)((r2 >> 8)  & 0xFF);

                if (class_code == PCI_CLASS_MASS_STORAGE) {
                    pci_device_t cand;
                    fill_device(&cand, (uint8_t)bus, dev, fn);

                    int score = 0;
                    const char *kind = "storage";
                    if (subclass == PCI_SUB_SATA && prog_if == PCI_PROG_IF_AHCI) {
                        score = 100; kind = "SATA/AHCI 1.0";
                    } else if (subclass == PCI_SUB_SATA) {
                        score = 60;  kind = "SATA (non-standard prog_if)";
                    } else if (subclass == 0x04) {
                        score = 30;  kind = "RAID (may expose AHCI)";
                    } else if (subclass == 0x01) {
                        score = 5;   kind = "IDE";
                    } else if (subclass == 0x08) {
                        score = 20;  kind = "NVMe";
                    }

                    /* A zero ABAR (BAR5) means the BIOS never assigned MMIO
                     * for this device - we can't drive it.  Penalise hard. */
                    if ((cand.bar[5] & 0xFFFFFFF0u) == 0) score /= 4;

                    debug_printf("[pci]   %02x:%02x.%x  vendor=%04x device=%04x  "
                                 "class=%02x sub=%02x pi=%02x  BAR5=0x%08x  "
                                 "(%s, score=%d)\n",
                                 (uint8_t)bus, dev, fn,
                                 cand.vendor_id, cand.device_id,
                                 class_code, subclass, prog_if, cand.bar[5],
                                 kind, score);

                    if (score > best_score) {
                        best       = cand;
                        best_score = score;
                        found      = true;
                    }
                }

                if (fn == 0) {
                    uint32_t r3 = pci_read32((uint8_t)bus, dev, fn, 0x0C);
                    uint8_t  ht = (uint8_t)((r3 >> 16) & 0xFF);
                    if (!(ht & 0x80)) break;
                }
            }
        }
    }

    if (!found) {
        debug_printf("[pci] bulletproof storage scan: no Class-01 devices.\n");
        return false;
    }
    /* We only return success when the candidate looks like something an
     * AHCI driver can talk to (SATA family OR a RAID controller exposing
     * MMIO BAR5).  IDE-only controllers are reported but not selected. */
    if (best.class_code == PCI_CLASS_MASS_STORAGE &&
        (best.subclass == PCI_SUB_SATA || best.subclass == 0x04) &&
        (best.bar[5] & 0xFFFFFFF0u) != 0) {
        *out = best;
        debug_printf("[pci] selected %02x:%02x.%x for AHCI bring-up "
                     "(class=%02x sub=%02x pi=%02x BAR5=0x%08x)\n",
                     best.bus, best.device, best.function,
                     best.class_code, best.subclass, best.prog_if,
                     best.bar[5]);
        return true;
    }
    debug_printf("[pci] best candidate is not AHCI-compatible "
                 "(class=%02x sub=%02x pi=%02x BAR5=0x%08x) - declining\n",
                 best.class_code, best.subclass, best.prog_if, best.bar[5]);
    return false;
}

void pci_refresh_bars(pci_device_t *dev) {
    for (int i = 0; i < 6; i++) {
        dev->bar[i] = pci_read32(dev->bus, dev->device, dev->function,
                                 (uint8_t)(0x10 + i * 4));
    }
}

void pci_enable_busmaster(const pci_device_t *dev) {
    uint32_t cmd = pci_read32(dev->bus, dev->device, dev->function, 0x04);
    /* Bit 0 = I/O space enable, bit 1 = memory (MMIO) space enable,
     * bit 2 = bus master enable.  I/O space MUST be on for controllers
     * that expose an I/O BAR (e.g. UHCI) — without it, register access
     * silently goes nowhere and transfers stall.  The firmware enables
     * these for on-board controllers but not always for add-in ones, so
     * we set all three to drive any controller regardless of BIOS state. */
    cmd |= (1u << 0) | (1u << 1) | (1u << 2);
    pci_write32(dev->bus, dev->device, dev->function, 0x04, cmd);
}

int pci_enumerate(pci_device_t *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (uint16_t bus = 0; bus < 256 && n < max; bus++) {
        for (uint8_t dev = 0; dev < 32 && n < max; dev++) {
            for (uint8_t fn = 0; fn < 8 && n < max; fn++) {
                uint32_t r0 = pci_read32((uint8_t)bus, dev, fn, 0x00);
                uint16_t vendor = (uint16_t)(r0 & 0xFFFF);
                if (vendor == 0xFFFF) {
                    if (fn == 0) break;
                    continue;
                }
                fill_device(&out[n], (uint8_t)bus, dev, fn);
                n++;
                if (fn == 0) {
                    uint32_t r3 = pci_read32((uint8_t)bus, dev, fn, 0x0C);
                    uint8_t  ht = (uint8_t)((r3 >> 16) & 0xFF);
                    if (!(ht & 0x80)) break;
                }
            }
        }
    }
    return n;
}

const char *pci_class_name(uint8_t cls, uint8_t sub, uint8_t pi) {
    (void)pi;
    switch (cls) {
        case 0x00: return "Unclassified";
        case 0x01:
            switch (sub) {
                case 0x00: return "SCSI controller";
                case 0x01: return "IDE controller";
                case 0x02: return "Floppy controller";
                case 0x05: return "ATA controller";
                case 0x06: return "SATA/AHCI controller";
                case 0x07: return "Serial-attached SCSI";
                case 0x08: return "NVMe controller";
                default:   return "Mass storage";
            }
        case 0x02:
            switch (sub) {
                case 0x00: return "Ethernet controller";
                case 0x80: return "Network controller";
                default:   return "Network device";
            }
        case 0x03:
            switch (sub) {
                case 0x00: return "VGA display controller";
                case 0x01: return "XGA display controller";
                case 0x02: return "3D display controller";
                default:   return "Display controller";
            }
        case 0x04: return "Multimedia controller";
        case 0x05: return "Memory controller";
        case 0x06:
            switch (sub) {
                case 0x00: return "Host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI-PCI bridge";
                default:   return "Bridge device";
            }
        case 0x07: return "Simple comm. controller";
        case 0x08:
            switch (sub) {
                case 0x00: return "Interrupt controller (PIC)";
                case 0x01: return "DMA controller";
                case 0x02: return "Timer";
                case 0x03: return "RTC";
                default:   return "System peripheral";
            }
        case 0x09:
            switch (sub) {
                case 0x00: return "Keyboard controller";
                case 0x02: return "Mouse controller";
                default:   return "Input device";
            }
        case 0x0A: return "Docking station";
        case 0x0B: return "Processor";
        case 0x0C:
            switch (sub) {
                case 0x00: return "FireWire (1394) controller";
                case 0x03: return "USB controller";
                case 0x05: return "SMBus controller";
                default:   return "Serial bus controller";
            }
        case 0x0D: return "Wireless controller";
        case 0x10: return "Encryption controller";
        case 0x11: return "Signal processing device";
        default:   return "Unknown PCI device";
    }
}

/* ============================================================================
 * NexxoN OS - USB Mass Storage Class (BOT + SCSI)  (v1.0)
 * ----------------------------------------------------------------------------
 * Implements the Bulk-Only Transport protocol (USB MSC §3) on top of the
 * generic usb_bulk_in / usb_bulk_out primitives, plus the minimal SCSI
 * command subset (INQUIRY, TEST UNIT READY, READ CAPACITY (10),
 * READ (10), WRITE (10)) needed to drive a pendrive as a 512-byte-sector
 * block device.
 *
 * The public surface is a single block-device handle: msc_get_volume(idx)
 * returns a msc_volume_t with a sector-read function pointer that plugs
 * directly into the partition parser / FAT driver.
 * ============================================================================ */
#ifndef NEXXON_MSC_H
#define NEXXON_MSC_H

#include "types.h"
#include "usb.h"

#define MSC_MAX_VOLUMES   4

/* Command Block Wrapper, USB MSC §5.1. */
typedef struct PACKED {
    uint32_t dCBWSignature;       /* 'USBC' = 0x43425355 */
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;          /* bit7 = direction (1 = IN) */
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;        /* 1..16 */
    uint8_t  CBWCB[16];
} msc_cbw_t;

#define MSC_CBW_SIGNATURE   0x43425355u
#define MSC_CSW_SIGNATURE   0x53425355u

/* Command Status Wrapper, USB MSC §5.2. */
typedef struct PACKED {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;          /* 0 = OK, 1 = failed, 2 = phase error */
} msc_csw_t;

typedef struct {
    bool         present;
    usb_device_t *dev;
    uint8_t      max_lun;
    uint32_t     sector_count;    /* logical block count from READ CAPACITY */
    uint32_t     sector_size;
    char         vendor[9];
    char         product[17];
    char         revision[5];
    uint32_t     next_tag;
} msc_volume_t;

/* Lifecycle - called by the PnP daemon when a USB device with class 0x08
 * (Mass Storage) is enumerated.  Returns true if the device was claimed. */
bool msc_attach    (usb_device_t *dev);
void msc_detach    (usb_device_t *dev);

int  msc_volume_count(void);
msc_volume_t *msc_get_volume(int idx);

/* Synchronous, 512-byte-sector read/write API (uses SCSI READ/WRITE (10)). */
int  msc_read_sector (msc_volume_t *vol, uint32_t lba, void *buf);
int  msc_write_sector(msc_volume_t *vol, uint32_t lba, const void *buf);

#endif /* NEXXON_MSC_H */

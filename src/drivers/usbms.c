/* ============================================================================
 * NexxoN OS - USB Mass Storage Class driver (BOT + minimal SCSI)
 * ----------------------------------------------------------------------------
 * Implements the Bulk-Only Transport protocol on top of the generic USB
 * bulk transfer primitives provided by usb.c, and speaks just enough SCSI
 * to turn a USB pendrive into a 512-byte sector block device.  Commands
 * implemented:
 *
 *    0x12  INQUIRY              - vendor / product / revision strings
 *    0x00  TEST UNIT READY      - confirm the LUN is online
 *    0x25  READ CAPACITY (10)   - learn the disk geometry
 *    0x28  READ (10)            - single-sector read
 *    0x2A  WRITE (10)           - single-sector write
 *    0x03  REQUEST SENSE        - drain stale errors after a STALL
 *
 * No request queueing, no NCQ, no asynchronous IO.  Each call to
 * msc_read_sector / msc_write_sector is a synchronous CBW -> DATA -> CSW
 * round trip.  Pendrives are slow enough that batching SECTORs into 4 KiB
 * BoT transfers would be a nice optimisation, but the current single-
 * sector API is what the FAT / NTFS parsers want anyway.
 * ============================================================================ */
#include "msc.h"
#include "usb.h"
#include "string.h"
#include "debug.h"
#include "pit.h"

static msc_volume_t g_vols[MSC_MAX_VOLUMES];
static int          g_n_vols = 0;

#define MSC_REQ_RESET     0xFF
#define MSC_REQ_GET_MAX_LUN 0xFE

static int bot_send_cbw(msc_volume_t *vol, const uint8_t *cdb, uint8_t cdb_len,
                        uint32_t data_len, bool is_in, uint32_t *tag_out) {
    msc_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = MSC_CBW_SIGNATURE;
    cbw.dCBWTag       = ++vol->next_tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags    = is_in ? 0x80 : 0x00;
    cbw.bCBWLUN       = 0;
    cbw.bCBWCBLength  = cdb_len;
    if (cdb_len > 16) cdb_len = 16;
    memcpy(cbw.CBWCB, cdb, cdb_len);
    if (tag_out) *tag_out = cbw.dCBWTag;
    return usb_bulk_out(vol->dev, &cbw, sizeof(cbw));
}

static int bot_recv_csw(msc_volume_t *vol, uint32_t expected_tag,
                        msc_csw_t *out) {
    msc_csw_t csw;
    memset(&csw, 0, sizeof(csw));
    uint32_t got = 0;
    int rc = usb_bulk_in(vol->dev, &csw, sizeof(csw), &got);
    if (rc != 0) return rc;
    if (csw.dCSWSignature != MSC_CSW_SIGNATURE) {
        debug_printf("[msc] bad CSW signature 0x%08x\n", csw.dCSWSignature);
        return -1;
    }
    if (csw.dCSWTag != expected_tag) {
        debug_printf("[msc] CSW tag mismatch (got 0x%08x want 0x%08x)\n",
                     csw.dCSWTag, expected_tag);
        return -1;
    }
    if (out) *out = csw;
    return csw.bCSWStatus == 0 ? 0 : -1;
}

/* Generic BOT request: send CDB, transfer optional data, read CSW. */
static int bot_request(msc_volume_t *vol,
                       const uint8_t *cdb, uint8_t cdb_len,
                       void *data, uint32_t data_len, bool is_in) {
    uint32_t tag = 0;
    int rc = bot_send_cbw(vol, cdb, cdb_len, data_len, is_in, &tag);
    if (rc != 0) return rc;
    if (data_len > 0) {
        uint32_t actual = 0;
        if (is_in) {
            rc = usb_bulk_in(vol->dev, data, data_len, &actual);
        } else {
            rc = usb_bulk_out(vol->dev, data, data_len);
            actual = data_len;
        }
        (void)actual;
        if (rc != 0) return rc;
    }
    return bot_recv_csw(vol, tag, NULL);
}

static int scsi_inquiry(msc_volume_t *vol) {
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t buf[36];
    memset(buf, 0, sizeof(buf));
    if (bot_request(vol, cdb, sizeof(cdb), buf, sizeof(buf), true) != 0) return -1;
    memcpy(vol->vendor, buf + 8, 8);   vol->vendor[8] = 0;
    memcpy(vol->product, buf + 16, 16); vol->product[16] = 0;
    memcpy(vol->revision, buf + 32, 4); vol->revision[4] = 0;
    /* Trim trailing spaces. */
    for (int i = 7; i >= 0 && vol->vendor[i] == ' '; i--) vol->vendor[i] = 0;
    for (int i = 15; i >= 0 && vol->product[i] == ' '; i--) vol->product[i] = 0;
    for (int i = 3; i >= 0 && vol->revision[i] == ' '; i--) vol->revision[i] = 0;
    debug_printf("[msc] INQUIRY: '%s' / '%s' / '%s'\n",
                 vol->vendor, vol->product, vol->revision);
    return 0;
}

static int scsi_test_unit_ready(msc_volume_t *vol) {
    uint8_t cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
    return bot_request(vol, cdb, sizeof(cdb), NULL, 0, true);
}

static int scsi_request_sense(msc_volume_t *vol) {
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    memset(sense, 0, sizeof(sense));
    int rc = bot_request(vol, cdb, sizeof(cdb), sense, sizeof(sense), true);
    if (rc == 0) {
        debug_printf("[msc] sense key=0x%02x asc=0x%02x ascq=0x%02x\n",
                     sense[2] & 0x0F, sense[12], sense[13]);
    }
    return rc;
}

static int scsi_read_capacity(msc_volume_t *vol) {
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t cap[8];
    memset(cap, 0, sizeof(cap));
    if (bot_request(vol, cdb, sizeof(cdb), cap, sizeof(cap), true) != 0) return -1;
    /* Big-endian. */
    uint32_t last_lba = ((uint32_t)cap[0] << 24)
                      | ((uint32_t)cap[1] << 16)
                      | ((uint32_t)cap[2] << 8)
                      |  (uint32_t)cap[3];
    uint32_t bsize    = ((uint32_t)cap[4] << 24)
                      | ((uint32_t)cap[5] << 16)
                      | ((uint32_t)cap[6] << 8)
                      |  (uint32_t)cap[7];
    vol->sector_count = last_lba + 1;
    vol->sector_size  = bsize ? bsize : 512;
    /* The whole block/FS stack (partition, FAT, exFAT) is built around
     * 512-byte logical sectors and hands msc_read_sector() 512-byte
     * buffers.  A device reporting a larger sector size would overflow
     * those buffers (and the 4 KiB DMA bounce buffer), which on real
     * hardware can wedge or crash the host USB stack.  Refuse to attach
     * such a device rather than risk it — virtually every USB stick is
     * 512-byte anyway. */
    if (vol->sector_size != 512) {
        debug_printf("[msc] unsupported sector size %u (need 512) - not attaching\n",
                     vol->sector_size);
        return -1;
    }
    debug_printf("[msc] READ CAPACITY: %u sectors x %u bytes = %u MiB\n",
                 vol->sector_count, vol->sector_size,
                 (uint32_t)(((uint64_t)vol->sector_count *
                             vol->sector_size) >> 20));
    return 0;
}

int msc_read_sector(msc_volume_t *vol, uint32_t lba, void *buf) {
    if (!vol || !vol->present) return -1;
    uint8_t cdb[10] = { 0x28, 0,
                        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                        (uint8_t)(lba >> 8),  (uint8_t)(lba),
                        0, 0, 1, 0 };
    return bot_request(vol, cdb, sizeof(cdb), buf, vol->sector_size, true);
}

int msc_write_sector(msc_volume_t *vol, uint32_t lba, const void *buf) {
    if (!vol || !vol->present) return -1;
    uint8_t cdb[10] = { 0x2A, 0,
                        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                        (uint8_t)(lba >> 8),  (uint8_t)(lba),
                        0, 0, 1, 0 };
    /* Cast away const for the bulk-out transfer; the API only reads it. */
    return bot_request(vol, cdb, sizeof(cdb), (void *)buf,
                       vol->sector_size, false);
}

static msc_volume_t *alloc_volume(usb_device_t *dev) {
    for (int i = 0; i < MSC_MAX_VOLUMES; i++) {
        if (!g_vols[i].present) {
            memset(&g_vols[i], 0, sizeof(g_vols[i]));
            g_vols[i].present     = true;
            g_vols[i].dev         = dev;
            g_vols[i].sector_size = 512;
            g_vols[i].next_tag    = 0xC0DEBEEF;
            dev->private_data     = &g_vols[i];
            g_n_vols++;
            return &g_vols[i];
        }
    }
    return NULL;
}

static void free_volume(msc_volume_t *vol) {
    if (!vol || !vol->present) return;
    vol->present = false;
    if (vol->dev) vol->dev->private_data = NULL;
    g_n_vols--;
}

bool msc_attach(usb_device_t *dev) {
    if (!dev) return false;
    if (dev->iface_class != USB_CLASS_MSC) return false;
    /* SubClass 0x06 = SCSI Transparent, Protocol 0x50 = BOT.  Be lenient
     * with anything that says SCSI - many real devices report subclass
     * 0x06 with protocol 0x50, but a few firmware bugs swap them. */
    if (dev->iface_protocol != 0x50) {
        debug_printf("[msc] device addr=%d uses non-BOT protocol 0x%02x, skipping\n",
                     dev->address, dev->iface_protocol);
        return false;
    }
    msc_volume_t *vol = alloc_volume(dev);
    if (!vol) return false;

    /* Class-specific GET_MAX_LUN request: returns one byte. */
    usb_setup_t setup;
    setup.bmRequestType = 0xA1;
    setup.bRequest      = MSC_REQ_GET_MAX_LUN;
    setup.wValue        = 0;
    setup.wIndex        = dev->iface_num;
    setup.wLength       = 1;
    uint8_t maxlun = 0;
    if (usb_control_transfer(dev, &setup, &maxlun, 1) != 0) maxlun = 0;
    vol->max_lun = maxlun;

    if (scsi_inquiry(vol) != 0) {
        debug_printf("[msc] INQUIRY failed; releasing volume\n");
        free_volume(vol);
        return false;
    }
    /* Some pendrives need a TEST UNIT READY retry loop after enumeration. */
    for (int i = 0; i < 5; i++) {
        if (scsi_test_unit_ready(vol) == 0) break;
        scsi_request_sense(vol);
        pit_sleep(50);
    }
    if (scsi_read_capacity(vol) != 0) {
        debug_printf("[msc] READ CAPACITY failed; releasing volume\n");
        free_volume(vol);
        return false;
    }
    dev->bot_ready = true;
    debug_printf("[msc] attached '%s %s' as msc%d (%u sectors)\n",
                 vol->vendor, vol->product, g_n_vols - 1, vol->sector_count);
    return true;
}

void msc_detach(usb_device_t *dev) {
    if (!dev) return;
    msc_volume_t *vol = (msc_volume_t *)dev->private_data;
    if (vol) free_volume(vol);
}

int           msc_volume_count(void)      { return g_n_vols; }
msc_volume_t *msc_get_volume   (int idx) {
    int seen = 0;
    for (int i = 0; i < MSC_MAX_VOLUMES; i++) {
        if (!g_vols[i].present) continue;
        if (seen == idx) return &g_vols[i];
        seen++;
    }
    return NULL;
}

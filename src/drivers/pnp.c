/* ============================================================================
 * NexxoN OS - Plug-and-Play daemon (USB orchestration)
 * ----------------------------------------------------------------------------
 * Subscribes to usb_set_hotplug_cb() and decides what to do with each
 * arriving / departing USB device.
 *
 *   * Mass-storage devices -> claim with msc_attach(), call partition_parse
 *     on LBA 0, and for every recognised FAT / NTFS partition install a
 *     vfs_mount() at "/usbN" so the file manager can browse it.
 *
 *   * HID devices -> reserved for future native polling driver.  Logged
 *     and otherwise ignored (the PS/2 emulation path keeps working).
 *
 *   * Anything else -> logged and ignored.
 *
 * pnp_tick() must be called periodically (the kernel scheduler does this
 * from its idle loop).  It runs usb_poll() then iterates the resulting
 * device list looking for newly bound mass-storage volumes that haven't
 * been mounted yet.
 *
 * The daemon is intentionally idempotent: callable as often as you like,
 * a single hot-plug event is processed exactly once.
 * ============================================================================ */
#include "pnp.h"
#include "usb.h"
#include "usbhid.h"
#include "msc.h"
#include "partition.h"
#include "fat.h"
#include "exfat.h"
#include "ntfs.h"
#include "vfs.h"
#include "string.h"
#include "debug.h"
#include "notify.h"
#include "i18n.h"
#include "explorer.h"  /* explorer_on_mounts_changed(): live sidebar refresh */

/* ---------- Adapter: MSC volume -> blockdev callbacks -------------------- */
static int msc_block_read(void *user, uint32_t lba, void *buf) {
    msc_volume_t *vol = (msc_volume_t *)user;
    return msc_read_sector(vol, lba, buf);
}

static int msc_block_write(void *user, uint32_t lba, const void *buf) {
    msc_volume_t *vol = (msc_volume_t *)user;
    return msc_write_sector(vol, lba, buf);
}

/* ---------- Per-mount state ---------------------------------------------- */
typedef enum {
    PNP_FS_NONE  = 0,
    PNP_FS_FAT   = 1,
    PNP_FS_NTFS  = 2,
    PNP_FS_EXFAT = 3,
} pnp_fs_kind_t;

typedef struct {
    bool           in_use;
    pnp_fs_kind_t  fs;
    msc_volume_t  *vol;
    uint32_t       partition_lba;
    char           mountpoint[VFS_NAME_MAX];
    fat_volume_t   fat;
    exfat_volume_t exfat;
    ntfs_volume_t  ntfs;
} pnp_mount_state_t;

#define PNP_MAX_MOUNTS  8
static pnp_mount_state_t g_pnp_mounts[PNP_MAX_MOUNTS];

/* Pick the LOWEST free /usbN name.  A bare counter grew forever across
 * hot-swap cycles (unplug + replug turned /usb0 into /usb1, /usb2, ...),
 * which broke any path the user had just typed or remembered. */
static void pnp_pick_mountpoint(char *out, size_t cap) {
    for (int n = 0; n < PNP_MAX_MOUNTS * 2; n++) {
        ksnprintf(out, cap, "usb%d", n);
        if (!vfs_find_mount(out)) return;
    }
}

/* One i18n toast per mount/unmount so a plugged stick is VISIBLE without
 * watching COM1; plus a poke so an open Explorer refreshes its sidebar. */
static void pnp_announce(bool mounted, const char *label, const char *mp) {
    char body[96], what[64];
    ksnprintf(what, sizeof(what), "%s (/%s)",
              (label && label[0]) ? label : "?", mp);
    ksnprintf(body, sizeof(body),
              L(mounted ? STR_USB_MOUNTED : STR_USB_REMOVED), what);
    notify_post(mounted ? NOTIFY_SUCCESS : NOTIFY_INFO,
                L(STR_USB_TITLE), body);
    explorer_on_mounts_changed();
}

/* ---------- VFS adapters ------------------------------------------------- */
static int fat_vfs_list(vfs_mount_t *m, const char *path,
                        vfs_entry_t *out, int max) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    fat_entry_t fe[64];
    int n = fat_list(&st->fat, path ? path : "/", fe, 64);
    if (n < 0) return n;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        strncpy(out[i].name, fe[i].name, sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = 0;
        out[i].size   = fe[i].size;
        out[i].is_dir = fe[i].is_dir;
    }
    return n;
}

static int fat_vfs_read(vfs_mount_t *m, const char *path,
                        void *buf, uint32_t cap) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return fat_read(&st->fat, path ? path : "", buf, cap);
}

static int fat_vfs_info(vfs_mount_t *m, char *out, size_t cap) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return ksnprintf(out, cap, "FAT%d  label='%s'  bytes/sec=%u  cluster_sz=%u",
                     (int)st->fat.kind, st->fat.label,
                     st->fat.bytes_per_sector,
                     st->fat.bytes_per_sector * st->fat.sectors_per_cluster);
}

static int fat_vfs_write(vfs_mount_t *m, const char *path,
                         const void *data, uint32_t len) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return fat_write_file(&st->fat, path ? path : "", data, len);
}

static int fat_vfs_mkdir(vfs_mount_t *m, const char *path) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return fat_mkdir(&st->fat, path ? path : "");
}

static int fat_vfs_delete(vfs_mount_t *m, const char *path) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return fat_delete(&st->fat, path ? path : "");
}

static int fat_vfs_rename(vfs_mount_t *m, const char *path,
                          const char *new_name) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    int r = fat_rename(&st->fat, path ? path : "", new_name);
    if (r == -2) return VFS_RENAME_EBADNAME;
    if (r == -3) return VFS_RENAME_EEXISTS;
    return r;
}

static int fat_vfs_read_at(vfs_mount_t *m, const char *path,
                           uint32_t offset, void *buf, uint32_t len) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return fat_read_at(&st->fat, path ? path : "", offset, buf, len);
}

static int fat_vfs_wopen(vfs_mount_t *m, const char *path) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return fat_write_open(&st->fat, path ? path : "");
}

static int fat_vfs_wappend(vfs_mount_t *m, const void *data, uint32_t len) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return fat_write_append(&st->fat, data, len);
}

static int fat_vfs_wclose(vfs_mount_t *m, bool commit) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return fat_write_close(&st->fat, commit);
}

static void fat_vfs_unmount(vfs_mount_t *m) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    if (!st) return;
    st->in_use = false;
}

/* ---------- exFAT VFS adapters ------------------------------------------ */
static int exfat_vfs_list(vfs_mount_t *m, const char *path,
                          vfs_entry_t *out, int max) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    fat_entry_t fe[64];
    int n = exfat_list(&st->exfat, path ? path : "/", fe, 64);
    if (n < 0) return n;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        strncpy(out[i].name, fe[i].name, sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = 0;
        out[i].size   = fe[i].size;
        out[i].is_dir = fe[i].is_dir;
    }
    return n;
}

static int exfat_vfs_read(vfs_mount_t *m, const char *path,
                          void *buf, uint32_t cap) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return exfat_read(&st->exfat, path ? path : "", buf, cap);
}

static int exfat_vfs_write(vfs_mount_t *m, const char *path,
                           const void *data, uint32_t len) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return exfat_write_file(&st->exfat, path ? path : "", data, len);
}

static int exfat_vfs_mkdir(vfs_mount_t *m, const char *path) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return exfat_mkdir(&st->exfat, path ? path : "");
}

static int exfat_vfs_delete(vfs_mount_t *m, const char *path) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return exfat_delete(&st->exfat, path ? path : "");
}

static int exfat_vfs_rename(vfs_mount_t *m, const char *path,
                            const char *new_name) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    int r = exfat_rename(&st->exfat, path ? path : "", new_name);
    if (r == -2) return VFS_RENAME_EBADNAME;
    if (r == -3) return VFS_RENAME_EEXISTS;
    return r;
}

static int exfat_vfs_read_at(vfs_mount_t *m, const char *path,
                             uint32_t offset, void *buf, uint32_t len) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return exfat_read_at(&st->exfat, path ? path : "", offset, buf, len);
}

static int exfat_vfs_wopen(vfs_mount_t *m, const char *path) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return exfat_write_open(&st->exfat, path ? path : "");
}

static int exfat_vfs_wappend(vfs_mount_t *m, const void *data, uint32_t len) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return exfat_write_append(&st->exfat, data, len);
}

static int exfat_vfs_wclose(vfs_mount_t *m, bool commit) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return exfat_write_close(&st->exfat, commit);
}

static int exfat_vfs_info(vfs_mount_t *m, char *out, size_t cap) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return ksnprintf(out, cap, "exFAT  label='%s'  cluster_sz=%u",
                     st->exfat.label,
                     st->exfat.sec_per_clus * 512u);
}

static void exfat_vfs_unmount(vfs_mount_t *m) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    if (st) st->in_use = false;
}

static int ntfs_vfs_list(vfs_mount_t *m, const char *path,
                         vfs_entry_t *out, int max) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    ntfs_entry_t ne[64];
    int n = ntfs_list(&st->ntfs, path ? path : "/", ne, 64);
    if (n < 0) return n;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        strncpy(out[i].name, ne[i].name, sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = 0;
        out[i].size   = (uint32_t)ne[i].size;
        out[i].is_dir = ne[i].is_dir;
    }
    return n;
}

static int ntfs_vfs_read(vfs_mount_t *m, const char *path,
                         void *buf, uint32_t cap) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return ntfs_read(&st->ntfs, path ? path : "", buf, cap);
}

static int ntfs_vfs_info(vfs_mount_t *m, char *out, size_t cap) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    return ksnprintf(out, cap, "NTFS  label='%s'  cluster_sz=%u",
                     st->ntfs.label,
                     st->ntfs.bytes_per_sector * st->ntfs.sectors_per_cluster);
}

static void ntfs_vfs_unmount(vfs_mount_t *m) {
    pnp_mount_state_t *st = (pnp_mount_state_t *)m->driver_state;
    if (!st) return;
    st->in_use = false;
}

/* ---------- Mount allocator ---------------------------------------------- */
static pnp_mount_state_t *alloc_state(void) {
    for (int i = 0; i < PNP_MAX_MOUNTS; i++) {
        if (!g_pnp_mounts[i].in_use) {
            memset(&g_pnp_mounts[i], 0, sizeof(g_pnp_mounts[i]));
            g_pnp_mounts[i].in_use = true;
            return &g_pnp_mounts[i];
        }
    }
    return NULL;
}

/* Try mounting the partition at `start_lba` of `vol` first as FAT, then
 * as NTFS, then bail.  Returns the VFS mount or NULL. */
static vfs_mount_t *try_mount_partition(msc_volume_t *vol, uint32_t start_lba,
                                        const char *suggested_label) {
    pnp_mount_state_t *st = alloc_state();
    if (!st) return NULL;
    st->vol            = vol;
    st->partition_lba  = start_lba;

    char mountpoint[VFS_NAME_MAX];
    pnp_pick_mountpoint(mountpoint, sizeof(mountpoint));
    strncpy(st->mountpoint, mountpoint, sizeof(st->mountpoint) - 1);

    /* FAT first. */
    if (fat_mount(&st->fat, msc_block_read, vol, start_lba)) {
        st->fat.write = msc_block_write;
        st->fs = PNP_FS_FAT;
        const char *label = st->fat.label[0] ? st->fat.label
                                              : (suggested_label ? suggested_label
                                                                  : "USB-VOLUME");
        char fsname[16];
        ksnprintf(fsname, sizeof(fsname), "FAT%d", (int)st->fat.kind);
        vfs_mount_t *m = vfs_mount(mountpoint, label, fsname,
                                   st, fat_vfs_list, fat_vfs_read,
                                   fat_vfs_info, fat_vfs_unmount);
        if (m) {
            m->write   = fat_vfs_write;
            m->mkdir   = fat_vfs_mkdir;
            m->del     = fat_vfs_delete;
            m->rename  = fat_vfs_rename;
            m->read_at = fat_vfs_read_at;
            m->wopen   = fat_vfs_wopen;
            m->wappend = fat_vfs_wappend;
            m->wclose  = fat_vfs_wclose;
            pnp_announce(true, label, mountpoint);
        }
        else   st->in_use = false;
        return m;
    }
    /* exFAT (modern USB sticks > 32 GiB). */
    if (exfat_mount(&st->exfat, msc_block_read, vol, start_lba)) {
        st->exfat.write = msc_block_write;
        st->fs = PNP_FS_EXFAT;
        const char *label = st->exfat.label[0] ? st->exfat.label
                          : (suggested_label ? suggested_label : "USB-EXFAT");
        vfs_mount_t *m = vfs_mount(mountpoint, label, "exFAT",
                                   st, exfat_vfs_list, exfat_vfs_read,
                                   exfat_vfs_info, exfat_vfs_unmount);
        if (m) {
            m->write   = exfat_vfs_write;
            m->mkdir   = exfat_vfs_mkdir;
            m->del     = exfat_vfs_delete;
            m->rename  = exfat_vfs_rename;
            m->read_at = exfat_vfs_read_at;
            m->wopen   = exfat_vfs_wopen;
            m->wappend = exfat_vfs_wappend;
            m->wclose  = exfat_vfs_wclose;
            pnp_announce(true, label, mountpoint);
        } else st->in_use = false;
        return m;
    }
    /* Fall back to NTFS. */
    if (ntfs_mount(&st->ntfs, msc_block_read, vol, start_lba)) {
        st->fs = PNP_FS_NTFS;
        const char *label = st->ntfs.label[0] ? st->ntfs.label
                                               : (suggested_label ? suggested_label
                                                                   : "USB-VOLUME");
        vfs_mount_t *m = vfs_mount(mountpoint, label, "NTFS",
                                   st, ntfs_vfs_list, ntfs_vfs_read,
                                   ntfs_vfs_info, ntfs_vfs_unmount);
        if (m) pnp_announce(true, label, mountpoint);
        else   st->in_use = false;
        return m;
    }
    st->in_use = false;
    debug_printf("[pnp] partition at LBA %u unrecognised (not FAT, not NTFS)\n",
                 start_lba);
    return NULL;
}

static void on_msc_attached(usb_device_t *dev) {
    if (!msc_attach(dev)) return;
    msc_volume_t *vol = (msc_volume_t *)dev->private_data;
    if (!vol) return;

    /* Parse partition table. */
    partition_table_t pt;
    int n = partition_parse(msc_block_read, vol, &pt);
    if (n <= 0) {
        debug_printf("[pnp] no partitions on USB device, mounting LBA 0 raw\n");
        try_mount_partition(vol, 0, vol->product[0] ? vol->product : "USB");
        return;
    }
    int mounted = 0;
    for (int i = 0; i < pt.count; i++) {
        if (!pt.entries[i].present) continue;
        /* Only attempt FS-bearing partitions. */
        uint8_t t = pt.entries[i].type_byte;
        if (t == PART_KIND_FAT12 || t == PART_KIND_FAT16 ||
            t == PART_KIND_FAT32 || t == PART_KIND_FAT32_LBA ||
            t == PART_KIND_NTFS  || t == PART_KIND_GPT_PARTITION ||
            t == PART_KIND_UNKNOWN || pt.is_gpt) {
            vfs_mount_t *m = try_mount_partition(vol, pt.entries[i].start_lba,
                                                 pt.entries[i].name);
            if (m) mounted++;
        }
    }
    debug_printf("[pnp] mounted %d partition(s) from USB device addr=%d\n",
                 mounted, dev->address);
}

static void on_msc_detached(usb_device_t *dev) {
    /* Unmount everything pointing at this device's volume. */
    msc_volume_t *vol = (msc_volume_t *)dev->private_data;
    if (!vol) return;
    for (int i = 0; i < PNP_MAX_MOUNTS; i++) {
        pnp_mount_state_t *st = &g_pnp_mounts[i];
        if (st->in_use && st->vol == vol) {
            const char *label =
                  (st->fs == PNP_FS_FAT)   ? st->fat.label
                : (st->fs == PNP_FS_EXFAT) ? st->exfat.label
                                           : st->ntfs.label;
            char mp[VFS_NAME_MAX];
            strncpy(mp, st->mountpoint, sizeof(mp) - 1);
            mp[sizeof(mp) - 1] = 0;
            vfs_unmount_by_name(st->mountpoint);
            st->in_use = false;
            pnp_announce(false, label, mp);
        }
    }
    msc_detach(dev);
}

static void hotplug_cb(usb_device_t *dev, bool attached) {
    if (!dev) return;
    if (attached) {
        if (dev->iface_class == USB_CLASS_MSC) {
            on_msc_attached(dev);
        } else if (dev->iface_class == USB_CLASS_HID) {
            /* Native HID boot driver: USB keyboard/mouse work even after the
             * BIOS USB-Legacy PS/2 emulation is torn down by usb_init(). */
            usbhid_attach(dev);
        } else {
            debug_printf("[pnp] unhandled class 0x%02x on USB device addr=%d\n",
                         dev->iface_class, dev->address);
        }
    } else {
        if (dev->iface_class == USB_CLASS_MSC) {
            on_msc_detached(dev);
        } else if (dev->iface_class == USB_CLASS_HID) {
            usbhid_detach(dev);
        }
    }
}

void pnp_init(void) {
    memset(g_pnp_mounts, 0, sizeof(g_pnp_mounts));
    usb_set_hotplug_cb(hotplug_cb);
    /* Fire the callback for everything already enumerated by usb_init. */
    int n = usb_device_count();
    for (int i = 0; i < n; i++) {
        usb_device_t *dev = usb_get_device(i);
        if (dev) hotplug_cb(dev, true);
    }
    debug_ok("pnp: USB hot-plug daemon armed");
}

void pnp_tick(void) {
    usb_poll();      /* detect hot-plug attach/detach */
    usbhid_poll();   /* pump USB keyboard/mouse reports */
}

int pnp_mount_count(void) {
    int n = 0;
    for (int i = 0; i < PNP_MAX_MOUNTS; i++) if (g_pnp_mounts[i].in_use) n++;
    return n;
}

int pnp_mount_describe(int idx, char *out, size_t cap) {
    int seen = 0;
    for (int i = 0; i < PNP_MAX_MOUNTS; i++) {
        if (!g_pnp_mounts[i].in_use) continue;
        if (seen == idx) {
            pnp_mount_state_t *st = &g_pnp_mounts[i];
            uint64_t bytes = (uint64_t)st->vol->sector_count *
                             (uint64_t)st->vol->sector_size;
            uint32_t mib = (uint32_t)(bytes >> 20);
            const char *fs = (st->fs == PNP_FS_FAT)   ? "FAT"
                           : (st->fs == PNP_FS_EXFAT) ? "exFAT" : "NTFS";
            const char *label = (st->fs == PNP_FS_FAT)   ? st->fat.label
                              : (st->fs == PNP_FS_EXFAT) ? st->exfat.label
                                                         : st->ntfs.label;
            return ksnprintf(out, cap, "/%s  %u MiB  %s  [%s]",
                             st->mountpoint, mib, fs,
                             label[0] ? label : "(no label)");
        }
        seen++;
    }
    return -1;
}

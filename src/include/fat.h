/* ============================================================================
 * NexxoN OS - FAT12/16/32 + NTFS-skeleton reader  (v1.0)
 * ----------------------------------------------------------------------------
 * Sits on top of a generic block-device callback (sector_read function
 * pointer + handle) so the same parser can drive an MBR-partitioned USB
 * mass-storage volume OR a raw on-disk image.  Provides:
 *
 *   fat_mount(...)       - probe BPB at LBA 0, decide variant (12/16/32),
 *                          cache root-dir + FAT geometry.
 *   fat_list(path, cb)   - directory listing.
 *   fat_read(path, ...)  - read file by absolute path.
 *
 * NTFS support in this milestone is stubbed at probe-only depth: detect
 * the NTFS magic, log the volume name, and return "read-only directory
 * unsupported".  Full MFT walking (resident vs non-resident attribute
 * decoding, data runs, $MFT bitmap) is planned for the next patch.
 *
 * This is read-only: no file create / write / rename, in line with the
 * USB-pendrive-as-data-source role.
 * ============================================================================ */
#ifndef NEXXON_FAT_H
#define NEXXON_FAT_H

#include "types.h"

typedef int (*blockdev_read_t) (void *user, uint32_t lba, void *buf);
typedef int (*blockdev_write_t)(void *user, uint32_t lba, const void *buf);

typedef enum {
    FS_NONE   = 0,
    FS_FAT12  = 12,
    FS_FAT16  = 16,
    FS_FAT32  = 32,
    FS_NTFS   = 99,
} fs_kind_t;

typedef struct {
    bool         mounted;
    fs_kind_t    kind;
    uint32_t     lba_start;
    uint16_t     bytes_per_sector;
    uint8_t      sectors_per_cluster;
    uint16_t     reserved_sectors;
    uint8_t      num_fats;
    uint32_t     sectors_per_fat;
    uint32_t     root_cluster;        /* FAT32 only */
    uint16_t     root_entries;        /* FAT12/16 only */
    uint32_t     fat_lba;
    uint32_t     data_lba;
    uint32_t     fsinfo_lba;          /* FAT32: FSInfo sector, 0 = none   */
    bool         fsinfo_blanked;      /* free-count set to "unknown" yet? */
    char         label[12];
    blockdev_read_t  read;
    blockdev_write_t write;
    void        *user;

    /* Next-free hint for the cluster allocator: streaming writes used to
     * rescan the FAT from cluster 2 for EVERY allocation (O(n^2) sector
     * reads - a 2.5 MiB copy onto a 512-byte-cluster volume stalled for
     * minutes).  The hint starts each search where the last one ended. */
    uint32_t     alloc_hint;

    /* Sequential read cursor for fat_read_at: remembers the cluster that
     * contains the last byte served so chunked copies of big files stay
     * O(1) per chunk instead of re-walking the chain from the start. */
    char         rc_path[280];
    uint32_t     rc_first_cluster;
    uint32_t     rc_size;
    uint32_t     rc_cluster;          /* cluster covering rc_pos          */
    uint32_t     rc_pos;              /* byte offset of rc_cluster start  */

    /* Streaming write state (one open stream per volume; the Explorer /
     * shell copy path is strictly single-threaded). */
    bool         ws_active;
    uint32_t     ws_dir_cluster;
    char         ws_name[64];
    uint32_t     ws_first, ws_prev;
    uint32_t     ws_len;
    uint32_t     ws_buffered;         /* bytes pending in the cluster buf */
} fat_volume_t;

typedef struct {
    char       name[64];
    uint32_t   size;
    bool       is_dir;
} fat_entry_t;

bool fat_mount(fat_volume_t *vol, blockdev_read_t rd, void *user,
               uint32_t partition_lba);
int  fat_list (fat_volume_t *vol, const char *path,
               fat_entry_t *out, int max);
int  fat_read (fat_volume_t *vol, const char *path,
               void *buf, uint32_t cap);

int  fat_write_file(fat_volume_t *vol, const char *path,
                    const void *data, uint32_t len);
int  fat_mkdir     (fat_volume_t *vol, const char *path);
int  fat_delete    (fat_volume_t *vol, const char *path);
/* Same-directory rename.  Long targets get a synthesised LFN run + a
 * unique NAME~N short entry; plain 8.3 names rewrite the short entry in
 * place.  0 = ok, -2 = name not representable, -3 = already exists. */
int  fat_rename    (fat_volume_t *vol, const char *path,
                    const char *new_name);

/* Chunked read: copy up to `len` bytes starting at byte `offset` of the
 * file.  Returns bytes copied (0 at/after EOF), -1 on error.  Sequential
 * offsets reuse a per-volume cursor, so big-file copies stay linear. */
int  fat_read_at   (fat_volume_t *vol, const char *path, uint32_t offset,
                    void *buf, uint32_t len);

/* Streaming write: open (create/truncate), append chunks, close.
 * close(commit=false) aborts and frees whatever was allocated.  The
 * directory entry (incl. LFN when needed) appears only on commit. */
int  fat_write_open  (fat_volume_t *vol, const char *path);
int  fat_write_append(fat_volume_t *vol, const void *data, uint32_t len);
int  fat_write_close (fat_volume_t *vol, bool commit);

#endif /* NEXXON_FAT_H */

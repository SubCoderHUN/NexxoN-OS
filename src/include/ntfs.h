/* ============================================================================
 * NexxoN OS - NTFS MFT walker (read-only, v1.0)
 * ----------------------------------------------------------------------------
 * Parses the Master File Table of an NTFS volume just deeply enough to:
 *
 *   * Locate $MFT (always at the cluster numbered in the boot sector).
 *   * Decode FILE record headers + STANDARD_INFORMATION + FILE_NAME +
 *     DATA attributes (both resident and non-resident).
 *   * Walk the non-resident DATA attribute's data runs to map a logical
 *     file offset to a physical LBA.
 *   * Render a root-directory listing by following the $INDEX_ROOT +
 *     $INDEX_ALLOCATION attributes of MFT record 5.
 *
 * Writes, journaling (USN/LogFile), encrypted/compressed attributes and
 * extended attribute lists are intentionally out of scope - this driver
 * exists so a NexxoN user can plug in an NTFS-formatted USB stick and
 * read files off of it, nothing more.
 * ============================================================================ */
#ifndef NEXXON_NTFS_H
#define NEXXON_NTFS_H

#include "types.h"

typedef int (*ntfs_blockdev_read_t)(void *user, uint32_t lba, void *buf);

typedef struct {
    bool                 mounted;
    uint32_t             lba_start;
    uint16_t             bytes_per_sector;
    uint8_t              sectors_per_cluster;
    uint64_t             total_sectors;
    uint64_t             mft_lcn;        /* MFT's first cluster (rel. to part)*/
    uint16_t             file_record_size;  /* in bytes (clamped to 1024)     */
    uint16_t             index_record_size;
    char                 label[36];
    ntfs_blockdev_read_t read;
    void                *user;
} ntfs_volume_t;

typedef struct {
    char     name[64];
    uint64_t size;
    bool     is_dir;
    uint64_t mft_record;
} ntfs_entry_t;

bool ntfs_mount  (ntfs_volume_t *vol, ntfs_blockdev_read_t rd, void *user,
                  uint32_t partition_lba);
int  ntfs_list   (ntfs_volume_t *vol, const char *path,
                  ntfs_entry_t *out, int max);
int  ntfs_read   (ntfs_volume_t *vol, const char *path,
                  void *buf, uint32_t cap);

#endif /* NEXXON_NTFS_H */

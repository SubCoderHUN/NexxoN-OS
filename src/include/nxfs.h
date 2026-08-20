/* ============================================================================
 * NexxoN OS - NXFS v3 hierarchical filesystem
 * ----------------------------------------------------------------------------
 * v3 removes the 16 KiB file-size ceiling and adds crash consistency:
 *
 *   * ext2-style block mapping: 12 direct + single + double + triple
 *     indirect pointers over 4 KiB blocks (1024-entry fan-out).  Design
 *     ceilings: 48 KiB direct, +4 MiB single, +4 GiB double, +4 TiB
 *     triple - any offset is reachable in <= 3 indirect-block reads, no
 *     scanning.  Volume ceiling is 2 TiB (32-bit sector LBAs in the
 *     sysdisk layer).
 *   * Write-ahead metadata journal: every structural change (inode,
 *     bitmap, indirect block, parent dir) is committed as a transaction
 *     in a dedicated journal region and replayed on mount.  File DATA is
 *     written before the transaction that references it ("ordered"
 *     mode): a crash can leak blocks (nxfs_check reclaims them) but can
 *     never produce a corrupt structure.
 *   * Free-space bitmaps sized at format time, cached per sector with a
 *     next-free allocation hint; free counters live in the superblock so
 *     statistics never scan.
 *   * Streaming API (write_begin/append/end + read_at with 64-bit
 *     offsets) so multi-GiB files never need a whole-file buffer.  The
 *     legacy whole-file calls remain as wrappers.
 *
 * Directories still store up to NXFS_MAX_CHILDREN inline child inode IDs
 * (the v3 effort targets FILE size; dirent scaling is a future epoch).
 * The public nxfs_inode_t keeps the v2 field names that UI code iterates
 * (type/name/size/children/child_count), so callers compile unchanged.
 *
 * On-disk layout (sector-relative to the NXFS partition, 512-B sectors):
 *
 *      8                superblock (geometry, free counters, label)
 *      jrnl_start..     journal region (header + payload + commit)
 *      ibm_start..      inode bitmap
 *      bbm_start..      block bitmap (1 bit per 4 KiB block)
 *      itab_start..     inode table (1 sector per inode)
 *      data_start..     data area, block k = data_start + k*8 sectors
 * ============================================================================ */
#ifndef NEXXON_NXFS_H
#define NEXXON_NXFS_H

#include "types.h"

#define NXFS_MAGIC          0x4E58465Bu      /* 'NXF[' little-endian        */
#define NXFS_VERSION        3

#define NXFS_SECTOR_SIZE    512
#define NXFS_BLOCK_SECTORS  8               /* 4 KiB allocation unit        */
#define NXFS_BLOCK_BYTES    (NXFS_BLOCK_SECTORS * NXFS_SECTOR_SIZE)
#define NXFS_PTRS_PER_BLOCK (NXFS_BLOCK_BYTES / 4)      /* indirect fanout */
#define NXFS_DIRECT         12

#define NXFS_MAX_CHILDREN   64              /* per-directory inline child IDs */
#define NXFS_NAME_MAX       60              /* incl. NUL terminator        */

/* Legacy whole-file buffer sizing used by small apps (editor, cat,
 * scripts).  v3 files can be far larger - those apps simply cap what
 * they load; bulk paths use the streaming API instead. */
#define NXFS_MAX_BLOCKS     32
#define NXFS_LEGACY_BUF     (NXFS_MAX_BLOCKS * NXFS_SECTOR_SIZE)   /* 16 KiB */

#define NXFS_LBA_SUPERBLOCK   8

#define NXFS_TYPE_FREE      0
#define NXFS_TYPE_FILE      1
#define NXFS_TYPE_DIR       2

/* Partition offset hard-coded by the v4 boot spec: the NXFS image sits in
 * the single primary partition that starts at LBA 16384 of the physical
 * disk (8 MiB headroom for the kernel ELF that lives at LBA 64..16383).
 * RAMFS (Live Mode) ignores this and treats its first sector as
 * NXFS-LBA-0. */
#define NXFS_PARTITION_OFFSET   16384u

/* ---------- Mount mode reported by nxfs_mode() -------------------------- */
typedef enum {
    NXFS_MODE_NONE = 0,      /* not mounted                       */
    NXFS_MODE_LIVE = 1,      /* mounted from in-memory RAMFS      */
    NXFS_MODE_SATA = 2,      /* mounted from AHCI SATA partition  */
} nxfs_mode_t;

#define NXFS_OK             0
#define NXFS_ERR_NOSPACE   -1
#define NXFS_ERR_NOTFOUND  -2
#define NXFS_ERR_EXISTS    -3
#define NXFS_ERR_NOTDIR    -4
#define NXFS_ERR_NOTFILE   -5
#define NXFS_ERR_NAME      -6
#define NXFS_ERR_FULL      -7
#define NXFS_ERR_IO        -8
#define NXFS_ERR_TOOBIG    -9
#define NXFS_ERR_BUSY      -10

#define NXFS_INSTALL_SIGNATURE   0x494E5354u    /* 'INST' little-endian      */

typedef struct PACKED nxfs_superblock {
    uint32_t magic;
    uint32_t version;            /* NXFS_VERSION (3)                       */
    uint32_t total_sectors;      /* of the NXFS area                       */
    uint32_t block_sectors;      /* NXFS_BLOCK_SECTORS                     */
    uint32_t nblocks;            /* data blocks (4 KiB each)               */
    uint32_t inode_count;
    uint32_t jrnl_start;         /* journal region (sectors)               */
    uint32_t jrnl_sectors;
    uint32_t ibm_start;          /* inode bitmap                           */
    uint32_t ibm_sectors;
    uint32_t bbm_start;          /* block bitmap                           */
    uint32_t bbm_sectors;
    uint32_t itab_start;         /* inode table (1 sector per inode)       */
    uint32_t data_start;         /* first data sector (8-aligned)          */
    uint32_t free_blocks;        /* maintained transactionally             */
    uint32_t free_inodes;
    uint32_t root_inode;         /* always 0                               */
    uint8_t  label[32];
    uint32_t install_sig;        /* NXFS_INSTALL_SIGNATURE when installed  */
    uint32_t install_time;
    uint8_t  reserved[512 - 17*4 - 32 - 2*4];
} nxfs_superblock_t;

/* One inode = one sector (atomic write; the journal relies on this).
 * Field names match v2 where UI code touches them. */
typedef struct PACKED nxfs_inode {
    uint32_t magic;              /* NXFS_MAGIC                             */
    uint32_t type;               /* NXFS_TYPE_*                            */
    char     name[NXFS_NAME_MAX];
    uint32_t parent_inode;
    uint32_t size;               /* file size LOW 32 bits / dir: #children */
    uint32_t size_hi;            /* file size HIGH 32 bits                 */
    uint32_t block_count;        /* allocated data blocks (incl. indirect) */
    uint32_t direct[NXFS_DIRECT];/* block numbers; 0 = not allocated       */
    uint32_t ind1;               /* single-indirect block                  */
    uint32_t ind2;               /* double-indirect block                  */
    uint32_t ind3;               /* triple-indirect block                  */
    uint32_t child_count;        /* dir only                               */
    uint32_t children[NXFS_MAX_CHILDREN];
    uint8_t  reserved[512 - 4*2 - NXFS_NAME_MAX - 4*4 - 4*NXFS_DIRECT
                          - 4*3 - 4 - 4*NXFS_MAX_CHILDREN];
} nxfs_inode_t;

/* ---------- Lifecycle ----------------------------------------------------- */
int           nxfs_mount     (void);    /* try SATA, fall back to RAMFS  */
int           nxfs_format    (const char *label);  /* format active backend */
bool          nxfs_is_mounted(void);
nxfs_mode_t   nxfs_mode      (void);
const char   *nxfs_backend_name(void);  /* "SATA(AHCI)" or "RAMFS(live)"   */
bool          nxfs_is_installed(void);
int  nxfs_install_to_sata(void);
/* Diagnostic / rescue: force-mount the SATA NXFS partition as the
 * active FS from a LIVE boot (QEMU cannot reach the stage2 HDD-boot
 * path; this gives the SATA mount + journal replay a forced entry). */
int  nxfs_remount_sata(void);

/* ---------- Geometry / statistics ---------------------------------------- */
void     nxfs_stats        (uint32_t *inodes_used, uint32_t *blocks_used);
uint32_t nxfs_total_sectors(void);      /* size of the mounted NXFS area  */
uint32_t nxfs_total_blocks (void);
uint32_t nxfs_free_blocks  (void);
uint32_t nxfs_data_start   (void);      /* first data sector (diagnostics)*/

/* Consistency check.  Walks the tree, validates structure, and (when
 * `repair` and the volume fits the in-RAM shadow bitmap) rebuilds the
 * block/inode bitmaps from the reachable tree - reclaiming any blocks a
 * crash between data and metadata commits may have leaked.  Returns the
 * number of problems found (0 = clean), negative on I/O error.  `out`
 * (optional) receives a short human-readable report. */
int nxfs_check(bool repair, char *out, uint32_t out_cap);

/* ---------- Directory navigation ----------------------------------------- */
uint32_t nxfs_cwd      (void);
int      nxfs_set_cwd  (uint32_t inode);
int      nxfs_pwd_path (char *out, size_t out_sz);
int      nxfs_resolve  (uint32_t cwd, const char *name, uint32_t *out_inode);
/* Resolve an absolute "/a/b/c" path from the root.  Single components
 * (no slash) resolve against the root as well. */
int      nxfs_resolve_path(const char *path, uint32_t *out_inode);

/* ---------- Inode read/write --------------------------------------------- */
int      nxfs_read_inode (uint32_t inode, nxfs_inode_t *out);
int      nxfs_write_inode(uint32_t inode, const nxfs_inode_t *in);

/* ---------- Listing ------------------------------------------------------ */
typedef void (*nxfs_list_cb_t)(const nxfs_inode_t *node, void *user);
int nxfs_list(uint32_t dir_inode, nxfs_list_cb_t cb, void *user);

/* ---------- File operations ---------------------------------------------- */
int nxfs_create_file (uint32_t parent, const char *name, uint32_t *out_inode);
int nxfs_delete_file (uint32_t parent, const char *name);
/* Legacy whole-buffer write (truncate + write).  Wraps the stream API. */
int nxfs_write_file  (uint32_t inode, const void *data, uint32_t len);
int nxfs_read_file   (uint32_t inode, void *out, uint32_t out_sz, uint32_t *bytes_read);

/* Streaming write: truncate-on-begin, append chunks, close.  One open
 * stream at a time (single-threaded FS layer).  end(commit=false)
 * truncates back to empty.  All metadata journaled in batches - every
 * intermediate commit leaves a consistent (shorter) file. */
int nxfs_write_begin (uint32_t inode);
int nxfs_write_append(const void *data, uint32_t len);
int nxfs_write_end   (bool commit);

/* Chunked read at a 64-bit byte offset.  Returns bytes copied (0 at or
 * past EOF), negative on error. */
int nxfs_read_at     (uint32_t inode, uint64_t offset, void *out, uint32_t len);
/* 64-bit file size of an inode (files; dirs return 0). */
uint64_t nxfs_file_size(uint32_t inode);

/* ---------- Directory operations ----------------------------------------- */
int nxfs_create_dir  (uint32_t parent, const char *name, uint32_t *out_inode);
int nxfs_delete_dir  (uint32_t parent, const char *name);
int nxfs_rename_dir  (uint32_t parent, const char *old_name, const char *new_name);
int nxfs_rename      (uint32_t parent, const char *old_name, const char *new_name);

#endif /* NEXXON_NXFS_H */

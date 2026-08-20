/* ============================================================================
 * NexxoN OS - exFAT read/write driver
 * ----------------------------------------------------------------------------
 * Mounts exFAT volumes (512-byte logical sectors), provides directory listing,
 * file read, and now full write support: create/overwrite files, mkdir, delete.
 * Write path: FAT-chain allocation + allocation-bitmap management +
 * directory entry-set construction (0x85+0xC0+0xC1) with set checksum.
 * ============================================================================ */
#ifndef NEXXON_EXFAT_H
#define NEXXON_EXFAT_H

#include "types.h"
#include "fat.h"        /* blockdev_read_t, fat_entry_t */

typedef struct {
    bool             mounted;
    blockdev_read_t  read;
    blockdev_write_t write;           /* NULL = read-only                  */
    void            *user;
    uint32_t         part_lba;        /* partition start LBA               */
    uint32_t         fat_lba;         /* part_lba + FatOffset              */
    uint32_t         heap_lba;        /* part_lba + ClusterHeapOffset      */
    uint32_t         cluster_count;
    uint32_t         root_cluster;
    uint8_t          spc_shift;       /* SectorsPerClusterShift            */
    uint32_t         sec_per_clus;    /* 1 << spc_shift                    */
    uint32_t         bitmap_cluster;  /* first cluster of allocation bitmap */
    uint64_t         bitmap_bytes;    /* byte size of the bitmap            */
    char             label[16];

    uint32_t         alloc_hint;      /* next-free cluster search hint      */

    /* Sequential read cursor (exfat_read_at). */
    char             rc_path[280];
    uint32_t         rc_first;
    uint64_t         rc_size;
    bool             rc_contig;
    uint32_t         rc_cluster;
    uint32_t         rc_pos;          /* byte offset of rc_cluster start    */

    /* Streaming write state (one open stream per volume). */
    bool             ws_active;
    char             ws_path[280];
    uint32_t         ws_first, ws_prev;
    uint32_t         ws_len;
    uint32_t         ws_buffered;
} exfat_volume_t;

/* Probe the exFAT boot sector at `partition_lba`.  Returns false if it is
 * not an exFAT volume (caller can then try FAT / NTFS). */
bool exfat_mount(exfat_volume_t *vol, blockdev_read_t rd, void *user,
                 uint32_t partition_lba);

/* Directory listing (path is "/" or "/sub/dir"). */
int  exfat_list(exfat_volume_t *vol, const char *path,
                fat_entry_t *out, int max);

/* Read a file by absolute path.  Returns the file size (bytes copied capped
 * at `cap`), or -1 on error. */
int  exfat_read(exfat_volume_t *vol, const char *path, void *buf, uint32_t cap);

/* Write support — vol->write must be set before calling these. */

/* Create or overwrite a file at `path` with `len` bytes from `data`.
 * Parent directory must already exist.  Returns 0 on success, -1 on error. */
int  exfat_write_file(exfat_volume_t *vol, const char *path,
                      const void *data, uint32_t len);

/* Create a directory at `path`.  Parent must already exist. */
int  exfat_mkdir(exfat_volume_t *vol, const char *path);

/* Delete a file or a directory (directories are emptied iteratively,
 * deepest leaf first, so no clusters leak). */
int  exfat_delete(exfat_volume_t *vol, const char *path);

/* Same-directory rename (the entry set is rebuilt with the new name,
 * data untouched).  0 = ok, -2 = bad name, -3 = name already exists. */
int  exfat_rename(exfat_volume_t *vol, const char *path,
                  const char *new_name);

/* Chunked read: up to `len` bytes from byte `offset`.  Returns bytes
 * copied (0 at EOF), -1 on error.  Sequential offsets reuse a cursor. */
int  exfat_read_at(exfat_volume_t *vol, const char *path, uint32_t offset,
                   void *buf, uint32_t len);

/* Streaming write: open (create/overwrite), append chunks, close.
 * close(commit=false) aborts + frees.  Entry set appears on commit. */
int  exfat_write_open  (exfat_volume_t *vol, const char *path);
int  exfat_write_append(exfat_volume_t *vol, const void *data, uint32_t len);
int  exfat_write_close (exfat_volume_t *vol, bool commit);

#endif /* NEXXON_EXFAT_H */

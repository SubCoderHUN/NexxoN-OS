/* ============================================================================
 * NexxoN OS - Virtual filesystem layer  (v1.0)
 * ----------------------------------------------------------------------------
 * A tiny mount-table abstraction that lets the kernel expose multiple
 * filesystems (NXFS, USB FAT, USB NTFS, ...) behind a single absolute-path
 * namespace.  The first mount at "/" is NXFS itself - everything below
 * "/usb0", "/usb1" or similar is owned by external drivers registered via
 * vfs_mount().
 *
 * The interface is read-only for non-NXFS mounts in this release.  Each
 * mount supplies four function pointers:
 *
 *   list(mount, relpath, out, max)   - directory listing
 *   read(mount, relpath, buf, cap)   - file read (returns bytes)
 *   info(mount, out_buf, out_cap)    - one-line human description
 *   unmount(mount)                   - flush + release any cached state
 *
 * The path resolver strips the mount prefix and forwards the relative
 * path verbatim, so each backend can pick its own canonicalisation.
 * ============================================================================ */
#ifndef NEXXON_VFS_H
#define NEXXON_VFS_H

#include "types.h"

#define VFS_MAX_MOUNTS    8
#define VFS_NAME_MAX      32

typedef struct vfs_mount vfs_mount_t;

typedef struct {
    char     name[64];
    uint32_t size;
    bool     is_dir;
} vfs_entry_t;

typedef int  (*vfs_list_t)   (vfs_mount_t *m, const char *path,
                              vfs_entry_t *out, int max);
typedef int  (*vfs_read_t)   (vfs_mount_t *m, const char *path,
                              void *buf, uint32_t cap);
typedef int  (*vfs_write_t)  (vfs_mount_t *m, const char *path,
                              const void *data, uint32_t len);
typedef int  (*vfs_mkdir_t)  (vfs_mount_t *m, const char *path);
typedef int  (*vfs_delete_t) (vfs_mount_t *m, const char *path);
/* Rename the LAST path component in place (same directory).  Returns 0 on
 * success; VFS_RENAME_EBADNAME when the target FS cannot represent the new
 * name (FAT 8.3); VFS_RENAME_EEXISTS when the name is already taken. */
typedef int  (*vfs_rename_t) (vfs_mount_t *m, const char *path,
                              const char *new_name);
typedef int  (*vfs_info_t)   (vfs_mount_t *m, char *out, size_t cap);
typedef void (*vfs_unmount_t)(vfs_mount_t *m);

/* Chunked read: copy up to `len` bytes from byte `offset` of the file.
 * Returns bytes copied (0 at EOF), negative on error.  Backends keep a
 * sequential cursor so chunked big-file copies stay linear. */
typedef int  (*vfs_read_at_t)(vfs_mount_t *m, const char *path,
                              uint32_t offset, void *buf, uint32_t len);

/* Streaming write: open (create/overwrite) -> append chunks -> close.
 * close(commit=false) aborts and frees; the directory entry appears
 * only on a committed close.  One open stream per mount. */
typedef int  (*vfs_wopen_t)  (vfs_mount_t *m, const char *path);
typedef int  (*vfs_wappend_t)(vfs_mount_t *m, const void *data, uint32_t len);
typedef int  (*vfs_wclose_t) (vfs_mount_t *m, bool commit);

#define VFS_RENAME_EBADNAME  (-2)
#define VFS_RENAME_EEXISTS   (-3)

struct vfs_mount {
    bool          in_use;
    char          mountpoint[VFS_NAME_MAX];   /* "usb0", "usb1", "cdrom"   */
    char          label[VFS_NAME_MAX];        /* "MY-PENDRIVE"             */
    char          fs_name[16];                /* "FAT32", "NTFS", ...      */
    void         *driver_state;               /* whatever the FS needs     */
    vfs_list_t    list;
    vfs_read_t    read;
    vfs_write_t   write;
    vfs_mkdir_t   mkdir;
    vfs_delete_t  del;
    vfs_rename_t  rename;
    vfs_read_at_t read_at;
    vfs_wopen_t   wopen;
    vfs_wappend_t wappend;
    vfs_wclose_t  wclose;
    vfs_info_t    info;
    vfs_unmount_t unmount;
};

void          vfs_init       (void);
vfs_mount_t  *vfs_mount      (const char *mountpoint,
                              const char *label,
                              const char *fs_name,
                              void *driver_state,
                              vfs_list_t l, vfs_read_t r,
                              vfs_info_t i, vfs_unmount_t u);
bool          vfs_unmount_by_name(const char *mountpoint);

int           vfs_mount_count(void);
vfs_mount_t  *vfs_get_mount  (int idx);
vfs_mount_t  *vfs_find_mount (const char *mountpoint);

/* Public operations - take an absolute path like "/usb0/photos" or a
 * relative path like "usb0/photos".  Returns the number of entries on
 * success, negative on failure. */
int  vfs_list      (const char *path, vfs_entry_t *out, int max);
int  vfs_read      (const char *path, void *buf, uint32_t cap);
int  vfs_write_file(const char *path, const void *data, uint32_t len);
int  vfs_mkdir_path(const char *path);
int  vfs_delete    (const char *path);
int  vfs_rename    (const char *path, const char *new_name);
bool vfs_writable  (const char *mountpoint);
bool vfs_renamable (const char *mountpoint);

/* Chunked / streaming surface (negative when the backend lacks it). */
int  vfs_read_at     (const char *path, uint32_t offset,
                      void *buf, uint32_t len);
int  vfs_write_open  (const char *path);
int  vfs_write_append(const char *path, const void *data, uint32_t len);
int  vfs_write_close (const char *path, bool commit);
/* Both ends support the streamed copy path? */
bool vfs_can_stream_copy(const char *src_path, const char *dst_path);
/* Streamed file copy src -> dst in `chunk`-sized pieces through `buf`.
 * `pump` (optional) runs between chunks so the UI stays alive during
 * multi-MiB copies.  Returns total bytes copied, negative on error. */
int  vfs_copy_streamed(const char *src_path, const char *dst_path,
                       void *buf, uint32_t chunk, void (*pump)(void));

#endif /* NEXXON_VFS_H */

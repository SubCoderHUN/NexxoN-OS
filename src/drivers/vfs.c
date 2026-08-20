/* ============================================================================
 * NexxoN OS - Virtual filesystem mount table
 * ----------------------------------------------------------------------------
 * Owns a small table of mount records.  Each record carries a function-
 * pointer triplet (list / read / info / unmount) that adapts the
 * underlying filesystem to a unified "vfs_entry_t" listing.  NXFS is the
 * implicit "/" mount; this layer only manages the *external* mounts
 * ("/usb0", "/usb1", "/cdrom", ...) that come and go as devices arrive.
 *
 * Path resolution is intentionally minimalist: strip the leading slash(es),
 * split off the first path component, look it up in the mount table, and
 * forward the residual path to the backend.  No symlinks, no canonical
 * casing, no cross-mount traversal - those belong in a future epoch.
 * ============================================================================ */
#include "vfs.h"
#include "string.h"
#include "debug.h"

static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static int         g_n_mounts = 0;

void vfs_init(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) g_mounts[i].in_use = false;
    g_n_mounts = 0;
}

static int strncpy_safe(char *dst, const char *src, int max) {
    int n = 0;
    while (src[n] && n < max - 1) { dst[n] = src[n]; n++; }
    dst[n] = 0;
    return n;
}

vfs_mount_t *vfs_mount(const char *mountpoint, const char *label,
                       const char *fs_name, void *driver_state,
                       vfs_list_t l, vfs_read_t r,
                       vfs_info_t i, vfs_unmount_t u) {
    if (!mountpoint) return NULL;
    /* Reject duplicates. */
    if (vfs_find_mount(mountpoint)) {
        debug_printf("[vfs] refusing duplicate mount '%s'\n", mountpoint);
        return NULL;
    }
    for (int k = 0; k < VFS_MAX_MOUNTS; k++) {
        vfs_mount_t *m = &g_mounts[k];
        if (m->in_use) continue;
        memset(m, 0, sizeof(*m));
        m->in_use = true;
        strncpy_safe(m->mountpoint, mountpoint, VFS_NAME_MAX);
        strncpy_safe(m->label, label ? label : "", VFS_NAME_MAX);
        strncpy_safe(m->fs_name, fs_name ? fs_name : "?", sizeof(m->fs_name));
        m->driver_state = driver_state;
        m->list    = l;
        m->read    = r;
        m->info    = i;
        m->unmount = u;
        g_n_mounts++;
        debug_printf("[vfs] mounted /%s (%s) label='%s'\n",
                     m->mountpoint, m->fs_name, m->label);
        return m;
    }
    debug_printf("[vfs] mount table full, can't mount '%s'\n", mountpoint);
    return NULL;
}

bool vfs_unmount_by_name(const char *mountpoint) {
    vfs_mount_t *m = vfs_find_mount(mountpoint);
    if (!m) return false;
    if (m->unmount) m->unmount(m);
    debug_printf("[vfs] unmounted /%s\n", m->mountpoint);
    m->in_use = false;
    g_n_mounts--;
    return true;
}

int           vfs_mount_count(void)      { return g_n_mounts; }
vfs_mount_t  *vfs_get_mount(int idx) {
    int seen = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) continue;
        if (seen == idx) return &g_mounts[i];
        seen++;
    }
    return NULL;
}

vfs_mount_t *vfs_find_mount(const char *mountpoint) {
    if (!mountpoint) return NULL;
    while (*mountpoint == '/' || *mountpoint == '\\') mountpoint++;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) continue;
        if (strcmp(g_mounts[i].mountpoint, mountpoint) == 0)
            return &g_mounts[i];
    }
    return NULL;
}

/* Strip the leading mount-name from `path`, return a pointer to the
 * remaining (mount-relative) path.  Updates `*out` with the matching
 * mount or NULL if none found.  Treats both "/usb0/foo/bar" and
 * "usb0/foo/bar" identically. */
static const char *resolve_mount(const char *path, vfs_mount_t **out) {
    if (!path) { *out = NULL; return NULL; }
    while (*path == '/' || *path == '\\') path++;
    char name[VFS_NAME_MAX];
    int n = 0;
    while (path[n] && path[n] != '/' && path[n] != '\\' && n < VFS_NAME_MAX - 1) {
        name[n] = path[n]; n++;
    }
    name[n] = 0;
    vfs_mount_t *m = vfs_find_mount(name);
    if (!m) { *out = NULL; return NULL; }
    *out = m;
    return path + n;
}

int vfs_list(const char *path, vfs_entry_t *out, int max) {
    vfs_mount_t *m = NULL;
    const char *rel = resolve_mount(path, &m);
    if (!m) return -1;
    if (!m->list) return -1;
    return m->list(m, rel, out, max);
}

int vfs_read(const char *path, void *buf, uint32_t cap) {
    vfs_mount_t *m = NULL;
    const char *rel = resolve_mount(path, &m);
    if (!m) return -1;
    if (!m->read) return -1;
    return m->read(m, rel, buf, cap);
}

int vfs_write_file(const char *path, const void *data, uint32_t len) {
    vfs_mount_t *m = NULL;
    const char *rel = resolve_mount(path, &m);
    if (!m) return -1;
    if (!m->write) return -1;
    return m->write(m, rel, data, len);
}

int vfs_mkdir_path(const char *path) {
    vfs_mount_t *m = NULL;
    const char *rel = resolve_mount(path, &m);
    if (!m) return -1;
    if (!m->mkdir) return -1;
    return m->mkdir(m, rel);
}

int vfs_delete(const char *path) {
    vfs_mount_t *m = NULL;
    const char *rel = resolve_mount(path, &m);
    if (!m) return -1;
    if (!m->del) return -1;
    return m->del(m, rel);
}

int vfs_rename(const char *path, const char *new_name) {
    vfs_mount_t *m = NULL;
    const char *rel = resolve_mount(path, &m);
    if (!m) return -1;
    if (!m->rename) return -1;
    return m->rename(m, rel, new_name);
}

int vfs_read_at(const char *path, uint32_t offset, void *buf, uint32_t len) {
    vfs_mount_t *m = NULL;
    const char *rel = resolve_mount(path, &m);
    if (!m || !m->read_at) return -1;
    return m->read_at(m, rel, offset, buf, len);
}

int vfs_write_open(const char *path) {
    vfs_mount_t *m = NULL;
    const char *rel = resolve_mount(path, &m);
    if (!m || !m->wopen) return -1;
    return m->wopen(m, rel);
}

int vfs_write_append(const char *path, const void *data, uint32_t len) {
    vfs_mount_t *m = NULL;
    resolve_mount(path, &m);
    if (!m || !m->wappend) return -1;
    return m->wappend(m, data, len);
}

int vfs_write_close(const char *path, bool commit) {
    vfs_mount_t *m = NULL;
    resolve_mount(path, &m);
    if (!m || !m->wclose) return -1;
    return m->wclose(m, commit);
}

bool vfs_can_stream_copy(const char *src_path, const char *dst_path) {
    vfs_mount_t *ms = NULL, *md = NULL;
    resolve_mount(src_path, &ms);
    resolve_mount(dst_path, &md);
    return ms && ms->read_at && md && md->wopen && md->wappend && md->wclose;
}

/* Streamed copy: read_at chunks from the source, append them into the
 * destination's write stream.  No size cap - this replaces the old
 * "whole file through one 2 MiB buffer" copy for FAT/exFAT mounts.
 * The pump callback keeps the compositor + input alive between chunks
 * (the stability rule: long operations must keep pumping). */
int vfs_copy_streamed(const char *src_path, const char *dst_path,
                      void *buf, uint32_t chunk, void (*pump)(void)) {
    if (!buf || chunk == 0) return -1;
    if (!vfs_can_stream_copy(src_path, dst_path)) return -1;
    if (vfs_write_open(dst_path) != 0) return -1;

    uint32_t off = 0;
    for (;;) {
        int n = vfs_read_at(src_path, off, buf, chunk);
        if (n < 0) {
            vfs_write_close(dst_path, false);
            return -1;
        }
        if (n == 0) break;                       /* EOF */
        if (vfs_write_append(dst_path, buf, (uint32_t)n) != 0) {
            /* append aborts the stream internally on failure */
            return -1;
        }
        off += (uint32_t)n;
        if (pump) pump();
        if ((uint32_t)n < chunk) break;          /* short read = EOF */
    }
    if (vfs_write_close(dst_path, true) != 0) return -1;
    debug_printf("[vfs] streamed copy '%s' -> '%s' (%u bytes)\n",
                 src_path, dst_path, off);
    return (int)off;
}

bool vfs_writable(const char *mountpoint) {
    vfs_mount_t *m = vfs_find_mount(mountpoint);
    if (!m) return false;
    return m->write != NULL;
}

bool vfs_renamable(const char *mountpoint) {
    vfs_mount_t *m = vfs_find_mount(mountpoint);
    if (!m) return false;
    return m->rename != NULL;
}

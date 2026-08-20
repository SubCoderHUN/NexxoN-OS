/* ============================================================================
 * NexxoN OS - FAT12/16/32 read-only parser (v2.0)
 * ----------------------------------------------------------------------------
 * Drives every FAT-flavoured volume in the system through a single block-
 * device callback, so the same parser can sit on top of:
 *
 *    - an MBR-partitioned USB mass-storage volume (UHCI/EHCI -> usbms),
 *    - an AHCI SATA partition (rare in practice but supported),
 *    - a raw FAT image mounted as a regression-test artefact.
 *
 * Public surface (fat.h):
 *
 *    fat_mount(vol, rd, user, partition_lba)
 *        - Read the BPB at the partition start, decide FAT12/16/32 by
 *          cluster count, cache the layout (FAT LBA, root cluster /
 *          root LBA, data LBA).
 *
 *    fat_list(vol, path, out, max)
 *        - Walk a directory and fill `out` with up to `max` entries.
 *        - Supports nested directories; "/" returns the root.
 *
 *    fat_read(vol, path, buf, cap)
 *        - Resolve `path` to a file's first cluster + size and copy up to
 *          `cap` bytes into `buf`.  Returns the number of bytes that would
 *          have been read had `cap` been infinite (i.e. file size on
 *          success, or -1 on failure).
 *
 * NTFS volumes are *not* handled here - ntfs.c owns those.  fat_mount()
 * returns false on an NTFS-stamped boot sector so the caller can try the
 * NTFS parser next.
 *
 * Long File Name (LFN) support: we decode 0x0F sequences and assemble the
 * full UTF-16-LE name into a 7-bit ASCII buffer (non-ASCII codepoints
 * render as '?').  Up to 13 chars per LFN entry, max 5 entries.
 * ============================================================================ */
#include "fat.h"
#include "string.h"
#include "debug.h"

/* ---------- BPB layout ---------------------------------------------------- */
typedef struct PACKED {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extension */
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fsinfo;
    uint16_t backup_boot;
    uint8_t  reserved2[12];
    uint8_t  drive_num;
    uint8_t  reserved3;
    uint8_t  signature;
    uint32_t volume_id;
    char     label[11];
    char     fs_type[8];
} fat_bpb_t;

/* ---------- Helpers ------------------------------------------------------- */
static void rtrim8(char *s, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (s[i] == ' ' || s[i] == 0) s[i] = 0;
        else break;
    }
}

bool fat_mount(fat_volume_t *vol, blockdev_read_t rd, void *user,
               uint32_t partition_lba) {
    if (!vol || !rd) return false;
    memset(vol, 0, sizeof(*vol));
    vol->read = rd;
    vol->user = user;
    vol->lba_start = partition_lba;

    uint8_t sec[512];
    if (rd(user, partition_lba, sec) != 0) return false;
    if (memcmp(sec + 3, "NTFS    ", 8) == 0) {
        /* Caller should retry with ntfs_mount(). */
        return false;
    }
    if (memcmp(sec + 3, "EXFAT   ", 8) == 0) {
        /* exFAT zeroes the legacy BPB; caller should retry exfat_mount(). */
        return false;
    }
    const fat_bpb_t *b = (const fat_bpb_t *)sec;
    if (b->bytes_per_sector != 512 || b->num_fats == 0
        || b->sectors_per_cluster == 0) {
        debug_printf("[fat] no recognisable FAT BPB at LBA %u\n", partition_lba);
        return false;
    }

    vol->bytes_per_sector    = b->bytes_per_sector;
    vol->sectors_per_cluster = b->sectors_per_cluster;
    vol->reserved_sectors    = b->reserved_sectors;
    vol->num_fats            = b->num_fats;
    vol->root_entries        = b->root_entries;
    uint32_t spf = b->sectors_per_fat_16
                   ? b->sectors_per_fat_16
                   : b->sectors_per_fat_32;
    vol->sectors_per_fat     = spf;
    vol->root_cluster        = b->root_cluster;
    vol->fat_lba             = partition_lba + b->reserved_sectors;
    uint32_t root_sectors    = (b->root_entries * 32u + 511u) / 512u;
    uint32_t data_start_rel  = b->reserved_sectors + b->num_fats * spf
                               + (b->root_entries ? root_sectors : 0);
    vol->data_lba            = partition_lba + data_start_rel;

    uint32_t total_sectors = b->total_sectors_16
                             ? b->total_sectors_16
                             : b->total_sectors_32;
    uint32_t data_sectors  = total_sectors - data_start_rel;
    uint32_t total_clusters = data_sectors / b->sectors_per_cluster;
    if (total_clusters < 4085)        vol->kind = FS_FAT12;
    else if (total_clusters < 65525)  vol->kind = FS_FAT16;
    else                               vol->kind = FS_FAT32;

    /* FAT32 FSInfo: remember where it lives so the first WRITE can blank
     * its free-cluster counters to "unknown" (we don't maintain them, and
     * a stale count makes fsck/Windows report the wrong free space). */
    vol->fsinfo_lba     = 0;
    vol->fsinfo_blanked = false;
    if (vol->kind == FS_FAT32 && b->fsinfo &&
        b->fsinfo != 0xFFFF && b->fsinfo < b->reserved_sectors) {
        vol->fsinfo_lba = partition_lba + b->fsinfo;
    }

    memcpy(vol->label, b->label, 11);
    vol->label[11] = 0;
    rtrim8(vol->label, 11);
    vol->mounted = true;
    debug_printf("[fat] FAT%d mounted at LBA %u, label='%s', clusters=%u\n",
                 (int)vol->kind, partition_lba, vol->label, total_clusters);
    return true;
}

/* ---------- FAT entry decoding -------------------------------------------- */
static uint32_t fat_next_cluster(fat_volume_t *vol, uint32_t cluster) {
    if (vol->kind == FS_FAT12) {
        uint32_t off = cluster + (cluster / 2);
        uint32_t sec_n  = off / 512;
        uint32_t sec_off = off % 512;
        uint8_t  s0[512], s1[512];
        if (vol->read(vol->user, vol->fat_lba + sec_n, s0) != 0) return 0;
        uint16_t lo, hi;
        if (sec_off == 511) {
            if (vol->read(vol->user, vol->fat_lba + sec_n + 1, s1) != 0) return 0;
            lo = s0[511]; hi = s1[0];
        } else {
            lo = s0[sec_off]; hi = s0[sec_off + 1];
        }
        uint16_t raw = lo | (hi << 8);
        uint16_t e = (cluster & 1) ? (raw >> 4) : (raw & 0x0FFF);
        return e >= 0xFF8 ? 0 : e;
    }
    if (vol->kind == FS_FAT16) {
        uint32_t off = cluster * 2;
        uint8_t sec[512];
        if (vol->read(vol->user, vol->fat_lba + off / 512, sec) != 0) return 0;
        uint32_t e = sec[off % 512] | (sec[(off % 512) + 1] << 8);
        return e >= 0xFFF8 ? 0 : e;
    }
    if (vol->kind == FS_FAT32) {
        uint32_t off = cluster * 4;
        uint8_t sec[512];
        if (vol->read(vol->user, vol->fat_lba + off / 512, sec) != 0) return 0;
        uint32_t e = sec[off % 512]
                   | (sec[(off % 512) + 1] << 8)
                   | (sec[(off % 512) + 2] << 16)
                   | (sec[(off % 512) + 3] << 24);
        e &= 0x0FFFFFFFu;
        return e >= 0x0FFFFFF8u ? 0 : e;
    }
    return 0;
}

/* ---------- Directory iteration ------------------------------------------- */
typedef int (*dir_visit_cb_t)(const uint8_t *dirent, const char *long_name,
                              void *user);

/* Read one cluster of a directory into `buf`.  Caller must have already
 * verified the cluster is in range.  Returns the number of bytes written. */
static uint32_t read_cluster(fat_volume_t *vol, uint32_t cluster, uint8_t *buf,
                             uint32_t buf_sz) {
    uint32_t lba = vol->data_lba + (cluster - 2) * vol->sectors_per_cluster;
    uint32_t bytes = (uint32_t)vol->sectors_per_cluster * 512u;
    if (bytes > buf_sz) bytes = buf_sz;
    for (uint32_t i = 0; i < bytes / 512u; i++) {
        if (vol->read(vol->user, lba + i, buf + i * 512u) != 0) return 0;
    }
    return bytes;
}

static void lfn_append(char *out, int *out_pos, const uint8_t *src, int n) {
    for (int i = 0; i < n; i += 2) {
        uint16_t c = src[i] | (src[i + 1] << 8);
        if (c == 0xFFFF || c == 0x0000) continue;
        if (*out_pos < 63)
            out[(*out_pos)++] = (c < 128) ? (char)c : '?';
    }
}

/* Build the long-file-name for a sequence of 0x0F entries.  `entries` is
 * an array of `n` directory entries, ordered as they appear in the
 * directory (which is reverse: high-sequence first).  We assemble back-
 * to-front. */
static void build_lfn(const uint8_t *entries, int n, char *out, int cap) {
    out[0] = 0;
    char parts[5][14];
    for (int i = 0; i < 5; i++) parts[i][0] = 0;
    int used = 0;
    for (int i = 0; i < n && used < 5; i++) {
        const uint8_t *d = entries + i * 32;
        if (d[11] != 0x0F) break;
        int seq = (d[0] & 0x1F);
        if (seq < 1 || seq > 5) continue;
        int pos = 0;
        char *dst = parts[seq - 1];
        lfn_append(dst, &pos, d + 1,  10);
        lfn_append(dst, &pos, d + 14, 12);
        lfn_append(dst, &pos, d + 28, 4);
        dst[pos] = 0;
        used = seq > used ? seq : used;
    }
    int outpos = 0;
    for (int i = 0; i < used && outpos < cap - 1; i++) {
        for (int j = 0; parts[i][j] && outpos < cap - 1; j++) {
            out[outpos++] = parts[i][j];
        }
    }
    out[outpos] = 0;
}

static void parse_83(const uint8_t *dirent, char out[13]) {
    int n = 0;
    for (int i = 0; i < 8 && dirent[i] != ' '; i++) out[n++] = (char)dirent[i];
    if (dirent[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && dirent[i] != ' '; i++) out[n++] = (char)dirent[i];
    }
    out[n] = 0;
}

/* Walk one directory's worth of entries via the appropriate iterator.
 * The callback returns 0 to continue, non-zero to stop early. */
static int walk_dir_entries(fat_volume_t *vol, uint32_t dir_cluster,
                            bool is_fat32_root, dir_visit_cb_t cb, void *user) {
    /* FAT12/16 root: fixed-size, fixed-position. */
    if (vol->kind != FS_FAT32 && dir_cluster == 0) {
        uint32_t root_lba = vol->fat_lba + vol->num_fats * vol->sectors_per_fat;
        uint32_t entries  = vol->root_entries;
        uint8_t  lfn_acc[5 * 32];
        int      lfn_n = 0;
        for (uint32_t e_i = 0; e_i < entries; e_i++) {
            uint8_t sec[512];
            uint32_t lba = root_lba + (e_i * 32) / 512u;
            if (vol->read(vol->user, lba, sec) != 0) break;
            uint8_t *d = sec + (e_i * 32) % 512;
            if (d[0] == 0) break;
            if (d[0] == 0xE5) { lfn_n = 0; continue; }
            if (d[11] == 0x0F) {
                if (lfn_n < 5) { memcpy(lfn_acc + lfn_n * 32, d, 32); lfn_n++; }
                continue;
            }
            char long_name[64] = {0};
            if (lfn_n > 0) build_lfn(lfn_acc, lfn_n, long_name, sizeof(long_name));
            lfn_n = 0;
            if (cb(d, long_name, user) != 0) return 1;
        }
        return 0;
    }
    /* FAT32 root or any cluster-chained subdirectory. */
    uint32_t cluster = dir_cluster ? dir_cluster :
                       (is_fat32_root ? vol->root_cluster : 0);
    uint8_t  cluster_buf[16 * 1024];
    uint8_t  lfn_acc[5 * 32];
    int      lfn_n = 0;
    while (cluster) {
        uint32_t bytes = read_cluster(vol, cluster, cluster_buf,
                                      sizeof(cluster_buf));
        if (bytes == 0) return -1;
        for (uint32_t i = 0; i + 32 <= bytes; i += 32) {
            uint8_t *d = cluster_buf + i;
            if (d[0] == 0) return 0;
            if (d[0] == 0xE5) { lfn_n = 0; continue; }
            if (d[11] == 0x0F) {
                if (lfn_n < 5) { memcpy(lfn_acc + lfn_n * 32, d, 32); lfn_n++; }
                continue;
            }
            char long_name[64] = {0};
            if (lfn_n > 0) build_lfn(lfn_acc, lfn_n, long_name, sizeof(long_name));
            lfn_n = 0;
            if (cb(d, long_name, user) != 0) return 1;
        }
        cluster = fat_next_cluster(vol, cluster);
    }
    return 0;
}

/* ---------- fat_list ------------------------------------------------------ */
typedef struct {
    fat_entry_t *out;
    int          max;
    int          n;
} list_ctx_t;

static int list_cb(const uint8_t *d, const char *long_name, void *user) {
    list_ctx_t *ctx = (list_ctx_t *)user;
    if (ctx->n >= ctx->max) return 1;
    if (d[11] & 0x08) return 0;            /* volume label */
    fat_entry_t *e = &ctx->out[ctx->n++];
    if (long_name[0]) {
        strncpy(e->name, long_name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = 0;
    } else {
        char shortname[13];
        parse_83(d, shortname);
        strncpy(e->name, shortname, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = 0;
    }
    e->is_dir = (d[11] & 0x10) != 0;
    e->size   = d[28] | (d[29] << 8) | (d[30] << 16) | (d[31] << 24);
    return 0;
}

/* Walk an absolute path and resolve to (cluster, is_dir, size). */
typedef struct {
    const char *target;
    bool        match_dir;
    uint32_t    out_cluster;
    uint32_t    out_size;
    bool        found;
} resolve_ctx_t;

static int eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int resolve_cb(const uint8_t *d, const char *long_name, void *user) {
    resolve_ctx_t *ctx = (resolve_ctx_t *)user;
    if (d[11] & 0x08) return 0;
    if (((d[11] & 0x10) != 0) != ctx->match_dir) return 0;
    char shortname[13];
    parse_83(d, shortname);
    if (eq_ci(shortname, ctx->target) ||
        (long_name[0] && eq_ci(long_name, ctx->target))) {
        ctx->out_cluster = ((uint32_t)d[20] << 16) | ((uint32_t)d[21] << 24)
                         |  (uint32_t)d[26]        | ((uint32_t)d[27] << 8);
        ctx->out_size    =  (uint32_t)d[28]        | ((uint32_t)d[29] << 8)
                         | ((uint32_t)d[30] << 16) | ((uint32_t)d[31] << 24);
        ctx->found       = true;
        return 1;
    }
    return 0;
}

/* Split-and-walk: starting at the root, descend into each path component.
 * Returns the cluster of the *parent* directory of `last_name`, with
 * `last_name` left pointing inside `path` to the final component. */
static int walk_path(fat_volume_t *vol, const char *path,
                     uint32_t *out_cluster, bool *out_is_dir,
                     uint32_t *out_size) {
    if (!path) return -1;
    /* Skip leading slashes. */
    while (*path == '/' || *path == '\\') path++;
    if (*path == 0) {
        /* Path is just "/" - return root directory. */
        if (out_cluster) *out_cluster = (vol->kind == FS_FAT32)
                                        ? vol->root_cluster : 0;
        if (out_is_dir) *out_is_dir = true;
        if (out_size)   *out_size   = 0;
        return 0;
    }
    uint32_t cur_cluster = (vol->kind == FS_FAT32) ? vol->root_cluster : 0;
    char comp[64];
    while (*path) {
        int n = 0;
        while (*path && *path != '/' && *path != '\\' && n < 63) {
            comp[n++] = *path++;
        }
        comp[n] = 0;
        while (*path == '/' || *path == '\\') path++;

        resolve_ctx_t ctx = { comp, *path != 0, 0, 0, false };
        walk_dir_entries(vol, cur_cluster, vol->kind == FS_FAT32,
                         resolve_cb, &ctx);
        if (!ctx.found) {
            /* Try the other type as a last-resort fallback. */
            ctx.match_dir = !ctx.match_dir;
            walk_dir_entries(vol, cur_cluster, vol->kind == FS_FAT32,
                             resolve_cb, &ctx);
        }
        if (!ctx.found) return -1;
        if (*path) {
            cur_cluster = ctx.out_cluster;
        } else {
            if (out_cluster) *out_cluster = ctx.out_cluster;
            if (out_is_dir)  *out_is_dir  = ctx.match_dir;
            if (out_size)    *out_size    = ctx.out_size;
            return 0;
        }
    }
    return -1;
}

int fat_list(fat_volume_t *vol, const char *path, fat_entry_t *out, int max) {
    if (!vol || !vol->mounted) return -1;
    if (vol->kind == FS_NTFS) return -1;
    uint32_t cluster = 0;
    bool is_dir = true;
    if (path && *path && !(path[0] == '/' && path[1] == 0)) {
        if (walk_path(vol, path, &cluster, &is_dir, NULL) != 0) return -1;
        if (!is_dir) return -1;
    }
    list_ctx_t ctx = { out, max, 0 };
    walk_dir_entries(vol, cluster, vol->kind == FS_FAT32, list_cb, &ctx);
    return ctx.n;
}

int fat_read(fat_volume_t *vol, const char *path, void *buf, uint32_t cap) {
    if (!vol || !vol->mounted || vol->kind == FS_NTFS) return -1;
    uint32_t cluster = 0, size = 0;
    bool is_dir = false;
    if (walk_path(vol, path, &cluster, &is_dir, &size) != 0) return -1;
    if (is_dir) return -1;
    uint32_t copied = 0;
    uint8_t  cluster_buf[16 * 1024];
    while (cluster && copied < size) {
        uint32_t got = read_cluster(vol, cluster, cluster_buf,
                                    sizeof(cluster_buf));
        if (got == 0) break;
        uint32_t want = size - copied;
        if (want > got) want = got;
        if (copied < cap) {
            uint32_t chunk = want;
            if (copied + chunk > cap) chunk = cap - copied;
            memcpy((uint8_t *)buf + copied, cluster_buf, chunk);
        }
        copied += want;
        cluster = fat_next_cluster(vol, cluster);
    }
    return (int)size;
}

/* ========================================================================= *
 * FAT write support
 * ========================================================================= */

/* Position of a directory entry on disk + its preceding LFN run.  Filled
 * by fat_locate_entry() (defined with the delete/rename machinery below)
 * and shared with fat_write_file so an overwrite UPDATES the existing
 * dirent in place instead of leaving a dangling duplicate behind. */
#define FAT_LFN_MAX 5
typedef struct {
    uint32_t short_lba;                   /* sector of the 8.3 entry        */
    uint32_t short_off;                   /* byte offset inside that sector */
    uint32_t lfn_lba[FAT_LFN_MAX];
    uint32_t lfn_off[FAT_LFN_MAX];
    int      lfn_count;
    uint32_t first_cluster;
    uint32_t size;
    bool     is_dir;
} fat_entpos_t;
static int fat_locate_entry(fat_volume_t *vol, uint32_t dir_cluster,
                            const char *name, fat_entpos_t *out);
/* LFN-aware entry creation (defined with the rename machinery below). */
static int fat_create_dirent(fat_volume_t *vol, uint32_t dir_cluster,
                             const char *name, uint8_t attr,
                             uint32_t first_cluster, uint32_t size);

static int write_sector(fat_volume_t *vol, uint32_t lba, const void *buf) {
    if (!vol->write) return -1;
    /* First write to a FAT32 volume: mark the FSInfo free-cluster counters
     * "unknown" (0xFFFFFFFF — spec-sanctioned).  We don't maintain them on
     * alloc/free, and a stale count shows wrong free space elsewhere. */
    if (vol->fsinfo_lba && !vol->fsinfo_blanked) {
        vol->fsinfo_blanked = true;            /* before recursing via us  */
        uint8_t fs[512];
        if (vol->read(vol->user, vol->fsinfo_lba, fs) == 0 &&
            fs[0] == 'R' && fs[1] == 'R' && fs[2] == 'a' && fs[3] == 'A') {
            memset(fs + 488, 0xFF, 8);         /* free count + next free   */
            vol->write(vol->user, vol->fsinfo_lba, fs);
        }
    }
    return vol->write(vol->user, lba, buf);
}

static int write_cluster(fat_volume_t *vol, uint32_t cluster,
                          const uint8_t *data, uint32_t len) {
    uint32_t lba = vol->data_lba + (cluster - 2) * vol->sectors_per_cluster;
    uint32_t bytes = (uint32_t)vol->sectors_per_cluster * 512u;
    uint8_t sec[512];
    for (uint32_t i = 0; i < vol->sectors_per_cluster; i++) {
        uint32_t off = i * 512;
        if (off < len) {
            uint32_t chunk = len - off;
            if (chunk >= 512) {
                if (write_sector(vol, lba + i, data + off) != 0) return -1;
            } else {
                memset(sec, 0, 512);
                memcpy(sec, data + off, chunk);
                if (write_sector(vol, lba + i, sec) != 0) return -1;
            }
        } else {
            memset(sec, 0, 512);
            if (write_sector(vol, lba + i, sec) != 0) return -1;
        }
    }
    (void)bytes;
    return 0;
}

static uint32_t fat_eoc(fat_volume_t *vol) {
    if (vol->kind == FS_FAT12) return 0xFF8;
    if (vol->kind == FS_FAT16) return 0xFFF8;
    return 0x0FFFFFF8u;
}

static int fat_write_fat_entry(fat_volume_t *vol, uint32_t cluster,
                               uint32_t value) {
    uint8_t sec[512];
    if (vol->kind == FS_FAT32) {
        uint32_t off = cluster * 4;
        uint32_t sec_n = off / 512;
        uint32_t sec_off = off % 512;
        if (vol->read(vol->user, vol->fat_lba + sec_n, sec) != 0) return -1;
        uint32_t old = sec[sec_off] | (sec[sec_off+1]<<8)
                     | (sec[sec_off+2]<<16) | (sec[sec_off+3]<<24);
        uint32_t nv = (old & 0xF0000000u) | (value & 0x0FFFFFFFu);
        sec[sec_off]   = nv & 0xFF;
        sec[sec_off+1] = (nv >> 8) & 0xFF;
        sec[sec_off+2] = (nv >> 16) & 0xFF;
        sec[sec_off+3] = (nv >> 24) & 0xFF;
        for (uint8_t f = 0; f < vol->num_fats; f++)
            if (write_sector(vol, vol->fat_lba + sec_n
                             + f * vol->sectors_per_fat, sec) != 0) return -1;
        return 0;
    }
    if (vol->kind == FS_FAT16) {
        uint32_t off = cluster * 2;
        uint32_t sec_n = off / 512;
        uint32_t sec_off = off % 512;
        if (vol->read(vol->user, vol->fat_lba + sec_n, sec) != 0) return -1;
        sec[sec_off]   = value & 0xFF;
        sec[sec_off+1] = (value >> 8) & 0xFF;
        for (uint8_t f = 0; f < vol->num_fats; f++)
            if (write_sector(vol, vol->fat_lba + sec_n
                             + f * vol->sectors_per_fat, sec) != 0) return -1;
        return 0;
    }
    return -1;
}

/* Allocate one free cluster.  Scans ONE FAT sector at a time starting
 * at the next-free hint (wrapping once), so back-to-back allocations
 * from a streaming write are O(1) instead of a full-FAT rescan each. */
static uint32_t fat_alloc_cluster(fat_volume_t *vol) {
    uint8_t sec[512];
    uint32_t per_sec = (vol->kind == FS_FAT32) ? 128 : 256;
    uint32_t total   = vol->sectors_per_fat * per_sec;
    if (vol->kind != FS_FAT32 && vol->kind != FS_FAT16) return 0;

    uint32_t start = (vol->alloc_hint >= 2 && vol->alloc_hint < total)
                   ? vol->alloc_hint : 2;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t c   = pass ? 2 : start;
        uint32_t end = pass ? start : total;
        uint32_t cur_sn = 0xFFFFFFFFu;
        for (; c < end; c++) {
            uint32_t off = c * ((vol->kind == FS_FAT32) ? 4 : 2);
            uint32_t sn  = off / 512;
            if (sn != cur_sn) {
                if (vol->read(vol->user, vol->fat_lba + sn, sec) != 0) {
                    c = (sn + 1) * per_sec - 1;   /* skip the bad sector */
                    continue;
                }
                cur_sn = sn;
            }
            uint32_t so = off % 512;
            uint32_t e;
            if (vol->kind == FS_FAT32)
                e = (sec[so] | (sec[so+1]<<8) | (sec[so+2]<<16)
                   | ((uint32_t)sec[so+3]<<24)) & 0x0FFFFFFFu;
            else
                e = sec[so] | (sec[so+1]<<8);
            if (e == 0) {
                if (fat_write_fat_entry(vol, c, fat_eoc(vol)) == 0) {
                    vol->alloc_hint = c + 1;
                    return c;
                }
                cur_sn = 0xFFFFFFFFu;     /* sec[] is stale after write  */
            }
        }
    }
    return 0;
}

static void fat_free_chain(fat_volume_t *vol, uint32_t cluster) {
    while (cluster && cluster < fat_eoc(vol)) {
        uint32_t next = fat_next_cluster(vol, cluster);
        fat_write_fat_entry(vol, cluster, 0);
        cluster = next;
    }
}

static void make_83_name(const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char *dot = NULL;
    for (const char *p = name; *p; p++) { if (*p == '.') dot = p; }
    int i = 0;
    for (const char *p = name; *p && p != dot && i < 8; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i++] = c;
    }
    if (dot) {
        int j = 0;
        for (const char *p = dot + 1; *p && j < 3; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c -= 32;
            out[8 + j++] = c;
        }
    }
}

static int find_or_create_dir_slot(fat_volume_t *vol, uint32_t dir_cluster,
                                   uint8_t *out_sec, uint32_t *out_lba,
                                   uint32_t *out_off) {
    if (vol->kind != FS_FAT32 && dir_cluster == 0) {
        uint32_t root_lba = vol->fat_lba + vol->num_fats * vol->sectors_per_fat;
        for (uint32_t e = 0; e < vol->root_entries; e++) {
            uint32_t lba = root_lba + (e * 32) / 512;
            if (vol->read(vol->user, lba, out_sec) != 0) continue;
            uint8_t *d = out_sec + (e * 32) % 512;
            if (d[0] == 0 || d[0] == 0xE5) {
                *out_lba = lba;
                *out_off = (e * 32) % 512;
                return 0;
            }
        }
        return -1;
    }
    uint32_t cluster = dir_cluster;
    uint8_t cbuf[16 * 1024];
    while (cluster) {
        uint32_t bytes = read_cluster(vol, cluster, cbuf, sizeof(cbuf));
        if (bytes == 0) break;
        for (uint32_t i = 0; i + 32 <= bytes; i += 32) {
            if (cbuf[i] == 0 || cbuf[i] == 0xE5) {
                uint32_t sec_in_cluster = i / 512;
                uint32_t lba = vol->data_lba
                             + (cluster - 2) * vol->sectors_per_cluster
                             + sec_in_cluster;
                if (vol->read(vol->user, lba, out_sec) != 0) return -1;
                *out_lba = lba;
                *out_off = i % 512;
                return 0;
            }
        }
        uint32_t next = fat_next_cluster(vol, cluster);
        if (!next) {
            next = fat_alloc_cluster(vol);
            if (!next) return -1;
            fat_write_fat_entry(vol, cluster, next);
            uint8_t zero[512];
            memset(zero, 0, 512);
            uint32_t nlba = vol->data_lba
                          + (next - 2) * vol->sectors_per_cluster;
            for (uint8_t s = 0; s < vol->sectors_per_cluster; s++)
                write_sector(vol, nlba + s, zero);
            if (vol->read(vol->user, nlba, out_sec) != 0) return -1;
            *out_lba = nlba;
            *out_off = 0;
            return 0;
        }
        cluster = next;
    }
    return -1;
}

static int split_parent_child(const char *path, char *parent, int psz,
                              char *child, int csz) {
    while (*path == '/' || *path == '\\') path++;
    const char *last_sep = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last_sep = p;
    if (!last_sep) {
        parent[0] = '/'; parent[1] = 0;
        strncpy(child, path, csz - 1); child[csz - 1] = 0;
    } else {
        int plen = (int)(last_sep - path);
        if (plen >= psz) plen = psz - 1;
        memcpy(parent, path, plen);
        parent[plen] = 0;
        strncpy(child, last_sep + 1, csz - 1); child[csz - 1] = 0;
    }
    return child[0] ? 0 : -1;
}

int fat_write_file(fat_volume_t *vol, const char *path,
                   const void *data, uint32_t len) {
    if (!vol || !vol->mounted || !vol->write) return -1;
    if (vol->kind == FS_FAT12 || vol->kind == FS_NTFS) return -1;

    char parent[256], child[64];
    if (split_parent_child(path, parent, sizeof(parent),
                           child, sizeof(child)) != 0) return -1;

    uint32_t dir_cluster = 0;
    bool is_dir = true;
    if (parent[0] != '/' || parent[1] != 0) {
        if (walk_path(vol, parent, &dir_cluster, &is_dir, NULL) != 0) return -1;
        if (!is_dir) return -1;
    } else {
        dir_cluster = (vol->kind == FS_FAT32) ? vol->root_cluster : 0;
    }

    /* Overwrite: free the old chain and REMEMBER the dirent position so we
     * can update it in place.  (The old code freed the chain but then wrote
     * a brand-new dirent next to the old one — duplicate names + a dangling
     * entry pointing at freed clusters after every save-over.) */
    fat_entpos_t ex_pos;
    bool overwrite = (fat_locate_entry(vol, dir_cluster, child, &ex_pos) == 0);
    if (overwrite) {
        if (ex_pos.is_dir) return -1;
        fat_free_chain(vol, ex_pos.first_cluster);
    }

    uint32_t first_cluster = 0, prev = 0;
    uint32_t cluster_bytes = (uint32_t)vol->sectors_per_cluster * 512u;
    uint32_t remaining = len;
    const uint8_t *src = (const uint8_t *)data;
    while (remaining > 0 || first_cluster == 0) {
        uint32_t c = fat_alloc_cluster(vol);
        if (!c) { if (first_cluster) fat_free_chain(vol, first_cluster); return -1; }
        if (!first_cluster) first_cluster = c;
        if (prev) fat_write_fat_entry(vol, prev, c);
        uint32_t chunk = remaining > cluster_bytes ? cluster_bytes : remaining;
        if (chunk > 0) write_cluster(vol, c, src, chunk);
        else {
            uint8_t z[512]; memset(z, 0, 512);
            uint32_t lba = vol->data_lba + (c - 2) * vol->sectors_per_cluster;
            for (uint8_t s = 0; s < vol->sectors_per_cluster; s++)
                write_sector(vol, lba + s, z);
        }
        src += chunk;
        remaining -= chunk;
        prev = c;
        if (remaining == 0 && first_cluster) break;
    }

    if (overwrite) {
        /* Update the EXISTING dirent in place: name, attributes and the
         * LFN run stay untouched, only the chain start + size change. */
        uint8_t sec[512];
        uint32_t lba = ex_pos.short_lba;
        uint32_t off = ex_pos.short_off;
        if (vol->read(vol->user, lba, sec) != 0) {
            fat_free_chain(vol, first_cluster);
            return -1;
        }
        uint8_t *d = sec + off;
        d[26] = first_cluster & 0xFF;
        d[27] = (first_cluster >> 8) & 0xFF;
        d[20] = (first_cluster >> 16) & 0xFF;
        d[21] = (first_cluster >> 24) & 0xFF;
        d[28] = len & 0xFF;
        d[29] = (len >> 8) & 0xFF;
        d[30] = (len >> 16) & 0xFF;
        d[31] = (len >> 24) & 0xFF;
        if (write_sector(vol, lba, sec) != 0) return -1;
    } else {
        /* New file: LFN-aware entry set (long names get a synthesised
         * LFN run + NAME~N alias instead of a truncated 8.3 stub). */
        if (fat_create_dirent(vol, dir_cluster, child, 0x20,
                              first_cluster, len) != 0) {
            fat_free_chain(vol, first_cluster);
            return -1;
        }
    }
    vol->rc_path[0] = 0;                  /* invalidate the read cursor   */

    debug_printf("[fat] wrote file '%s' (%u bytes, cluster %u%s)\n",
                 child, len, first_cluster, overwrite ? ", overwrote" : "");
    return 0;
}

int fat_mkdir(fat_volume_t *vol, const char *path) {
    if (!vol || !vol->mounted || !vol->write) return -1;
    if (vol->kind == FS_FAT12 || vol->kind == FS_NTFS) return -1;

    char parent[256], child[64];
    if (split_parent_child(path, parent, sizeof(parent),
                           child, sizeof(child)) != 0) return -1;

    uint32_t dir_cluster = 0;
    bool is_dir = true;
    if (parent[0] != '/' || parent[1] != 0) {
        if (walk_path(vol, parent, &dir_cluster, &is_dir, NULL) != 0) return -1;
        if (!is_dir) return -1;
    } else {
        dir_cluster = (vol->kind == FS_FAT32) ? vol->root_cluster : 0;
    }

    uint32_t c = fat_alloc_cluster(vol);
    if (!c) return -1;
    uint8_t zero[512];
    memset(zero, 0, 512);
    uint32_t lba = vol->data_lba + (c - 2) * vol->sectors_per_cluster;
    for (uint8_t s = 0; s < vol->sectors_per_cluster; s++)
        write_sector(vol, lba + s, zero);

    uint8_t dot[32];
    memset(dot, 0, 32);
    memset(dot, ' ', 11);
    dot[0] = '.';
    dot[11] = 0x10;
    dot[26] = c & 0xFF; dot[27] = (c >> 8) & 0xFF;
    dot[20] = (c >> 16) & 0xFF; dot[21] = (c >> 24) & 0xFF;

    /* Spec: when the parent is the ROOT directory the ".." entry stores
     * first-cluster 0, NOT the root's actual cluster number — fsck flags
     * the latter as "Invalid '..' entry". */
    uint32_t dotdot_cluster = dir_cluster;
    if ((vol->kind == FS_FAT32 && dir_cluster == vol->root_cluster) ||
        (vol->kind != FS_FAT32 && dir_cluster == 0)) {
        dotdot_cluster = 0;
    }
    uint8_t dotdot[32];
    memset(dotdot, 0, 32);
    memset(dotdot, ' ', 11);
    dotdot[0] = '.'; dotdot[1] = '.';
    dotdot[11] = 0x10;
    dotdot[26] = dotdot_cluster & 0xFF;
    dotdot[27] = (dotdot_cluster >> 8) & 0xFF;
    dotdot[20] = (dotdot_cluster >> 16) & 0xFF;
    dotdot[21] = (dotdot_cluster >> 24) & 0xFF;

    uint8_t first_sec[512];
    memset(first_sec, 0, 512);
    memcpy(first_sec, dot, 32);
    memcpy(first_sec + 32, dotdot, 32);
    write_sector(vol, lba, first_sec);

    if (fat_create_dirent(vol, dir_cluster, child, 0x10, c, 0) != 0) {
        fat_write_fat_entry(vol, c, 0);
        return -1;
    }
    vol->rc_path[0] = 0;                  /* invalidate the read cursor   */

    debug_printf("[fat] created dir '%s' (cluster %u)\n", child, c);
    return 0;
}

/* ---- Position-aware directory-entry lookup ----------------------------- *
 * The old delete freed the cluster chain and then re-matched the dirent by
 * its 8.3 SHORT name only.  Files written by Windows carry LFN entries and
 * an auto-generated short name ("HELLO~1TXT"): matching the DISPLAYED long
 * name found the file (chain freed) but the short-name re-match failed, so
 * the dirent stayed live pointing at freed clusters — silent corruption on
 * every Windows-named delete.  This finder records WHERE the short entry
 * and its preceding LFN run live so callers mutate exactly those bytes.
 * (fat_entpos_t itself is defined up with the write support, which shares
 * it for in-place overwrites.)
 *
 * Visit every 32-byte entry of a directory with its on-disk position.
 * Matches `name` case-insensitively against both the 8.3 and the LFN. */
static int fat_locate_entry(fat_volume_t *vol, uint32_t dir_cluster,
                            const char *name, fat_entpos_t *out) {
    uint8_t  lfn_acc[FAT_LFN_MAX * 32];
    uint32_t lfn_lba[FAT_LFN_MAX], lfn_off[FAT_LFN_MAX];
    int      lfn_n = 0;

    bool fixed_root = (vol->kind != FS_FAT32 && dir_cluster == 0);
    uint32_t root_lba = vol->fat_lba + vol->num_fats * vol->sectors_per_fat;
    uint32_t cluster  = dir_cluster ? dir_cluster
                       : (vol->kind == FS_FAT32 ? vol->root_cluster : 0);
    uint8_t  sec[512];
    uint32_t e_i = 0;

    while (1) {
        uint32_t lba, off;
        if (fixed_root) {
            if (e_i >= vol->root_entries) return -1;
            lba = root_lba + (e_i * 32) / 512u;
            off = (e_i * 32) % 512u;
            e_i++;
        } else {
            if (!cluster) return -1;
            uint32_t per_cluster = (uint32_t)vol->sectors_per_cluster * 16u;
            uint32_t in_cluster  = e_i % per_cluster;
            lba = vol->data_lba + (cluster - 2) * vol->sectors_per_cluster
                + in_cluster / 16u;
            off = (in_cluster % 16u) * 32u;
            e_i++;
            if (e_i % per_cluster == 0) {
                /* advance AFTER processing the last entry of this cluster */
            }
        }
        if (vol->read(vol->user, lba, sec) != 0) return -1;
        uint8_t *d = sec + off;
        if (d[0] == 0) return -1;                       /* end of directory */
        if (d[0] == 0xE5) { lfn_n = 0; goto next; }
        if (d[11] == 0x0F) {
            if (lfn_n < FAT_LFN_MAX) {
                memcpy(lfn_acc + lfn_n * 32, d, 32);
                lfn_lba[lfn_n] = lba;
                lfn_off[lfn_n] = off;
                lfn_n++;
            }
            goto next;
        }
        if (!(d[11] & 0x08)) {                          /* not a volume label */
            char shortname[13];
            parse_83(d, shortname);
            char long_name[64] = {0};
            if (lfn_n > 0) build_lfn(lfn_acc, lfn_n, long_name,
                                     sizeof(long_name));
            if (eq_ci(shortname, name) ||
                (long_name[0] && eq_ci(long_name, name))) {
                out->short_lba = lba;
                out->short_off = off;
                out->lfn_count = lfn_n;
                for (int i = 0; i < lfn_n; i++) {
                    out->lfn_lba[i] = lfn_lba[i];
                    out->lfn_off[i] = lfn_off[i];
                }
                out->first_cluster = ((uint32_t)d[20] << 16)
                                   | ((uint32_t)d[21] << 24)
                                   |  (uint32_t)d[26]
                                   | ((uint32_t)d[27] << 8);
                out->size   =  (uint32_t)d[28]        | ((uint32_t)d[29] << 8)
                            | ((uint32_t)d[30] << 16) | ((uint32_t)d[31] << 24);
                out->is_dir = (d[11] & 0x10) != 0;
                return 0;
            }
        }
        lfn_n = 0;
next:
        if (!fixed_root) {
            uint32_t per_cluster = (uint32_t)vol->sectors_per_cluster * 16u;
            if (e_i % per_cluster == 0) {
                cluster = fat_next_cluster(vol, cluster);
                e_i = 0;
            }
        }
    }
}

/* Mark one located entry (short + LFN run) deleted on disk. */
static int fat_mark_deleted(fat_volume_t *vol, const fat_entpos_t *p) {
    uint8_t sec[512];
    for (int i = 0; i <= p->lfn_count; i++) {
        uint32_t lba = (i < p->lfn_count) ? p->lfn_lba[i] : p->short_lba;
        uint32_t off = (i < p->lfn_count) ? p->lfn_off[i] : p->short_off;
        if (vol->read(vol->user, lba, sec) != 0) return -1;
        sec[off] = 0xE5;
        if (write_sector(vol, lba, sec) != 0) return -1;
    }
    return 0;
}

/* Resolve a path's PARENT directory cluster (root when path has a single
 * component).  Shared by delete + rename. */
static int fat_parent_cluster(fat_volume_t *vol, const char *parent,
                              uint32_t *out_cluster) {
    if (parent[0] == '/' && parent[1] == 0) {
        *out_cluster = (vol->kind == FS_FAT32) ? vol->root_cluster : 0;
        return 0;
    }
    bool is_dir = true;
    if (walk_path(vol, parent, out_cluster, &is_dir, NULL) != 0) return -1;
    return is_dir ? 0 : -1;
}

/* Is a directory empty (only "." / ".." inside)? */
static int empty_dir_cb(const uint8_t *d, const char *long_name, void *user) {
    (void)long_name;
    if (d[11] & 0x08) return 0;
    if (d[0] == '.')  return 0;            /* "." and ".." entries          */
    *(bool *)user = false;
    return 1;
}
static bool fat_dir_is_empty(fat_volume_t *vol, uint32_t dir_cluster) {
    bool empty = true;
    walk_dir_entries(vol, dir_cluster, vol->kind == FS_FAT32,
                     empty_dir_cb, &empty);
    return empty;
}

/* Delete one located entry: free its chain, then kill the dirent + LFN. */
static int fat_delete_located(fat_volume_t *vol, uint32_t parent_cluster,
                              const char *child) {
    fat_entpos_t pos;
    if (fat_locate_entry(vol, parent_cluster, child, &pos) != 0) return -1;
    if (pos.is_dir && !fat_dir_is_empty(vol, pos.first_cluster)) return -2;
    fat_free_chain(vol, pos.first_cluster);
    if (fat_mark_deleted(vol, &pos) != 0) return -1;
    return 0;
}

int fat_delete(fat_volume_t *vol, const char *path) {
    if (!vol || !vol->mounted || !vol->write) return -1;
    if (vol->kind == FS_FAT12 || vol->kind == FS_NTFS) return -1;
    vol->rc_path[0] = 0;                  /* invalidate the read cursor   */

    char parent[256], child[64];
    if (split_parent_child(path, parent, sizeof(parent),
                           child, sizeof(child)) != 0) return -1;
    uint32_t dir_cluster = 0;
    if (fat_parent_cluster(vol, parent, &dir_cluster) != 0) return -1;

    int r = fat_delete_located(vol, dir_cluster, child);
    if (r != -2) {
        if (r == 0) debug_printf("[fat] deleted '%s'\n", child);
        return r;
    }

    /* Non-empty directory: delete the tree ITERATIVELY (deepest leaf
     * first) — recursion would stack a 16 KiB cluster buffer per level on
     * the 64 KiB kernel stack.  Each pass descends from the target to one
     * leaf entry using a single reusable listing buffer and deletes it;
     * the pass that finds the target itself empty finishes the job. */
    static fat_entry_t tree_ents[64];      /* single-threaded kernel        */
    char cur[300];
    for (int guard = 0; guard < 4096; guard++) {
        strncpy(cur, path, sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = 0;
        bool descended = true;
        while (descended) {
            descended = false;
            int n = fat_list(vol, cur, tree_ents, 64);
            if (n < 0) return -1;
            for (int i = 0; i < n; i++) {
                if (tree_ents[i].name[0] == '.') continue;
                size_t cl = strlen(cur);
                if (cl + strlen(tree_ents[i].name) + 2 >= sizeof(cur))
                    return -1;
                if (cl == 0 || cur[cl - 1] != '/') strcat(cur, "/");
                strcat(cur, tree_ents[i].name);
                if (tree_ents[i].is_dir) descended = true;
                break;                     /* take the FIRST entry          */
            }
            if (!descended && strcmp(cur, path) != 0) {
                /* `cur` is a leaf (file, or dir listed empty): delete it. */
                char p2[256], c2[64];
                if (split_parent_child(cur, p2, sizeof(p2),
                                       c2, sizeof(c2)) != 0) return -1;
                uint32_t pc = 0;
                if (fat_parent_cluster(vol, p2, &pc) != 0) return -1;
                if (fat_delete_located(vol, pc, c2) != 0) return -1;
            }
        }
        /* Try the target again; once its subtree is gone this succeeds. */
        r = fat_delete_located(vol, dir_cluster, child);
        if (r == 0) {
            debug_printf("[fat] deleted tree '%s'\n", child);
            return 0;
        }
        if (r != -2) return r;
    }
    return -1;
}

/* ========================================================================= *
 * LFN-aware entry creation
 * ------------------------------------------------------------------------- *
 * fat_create_dirent() writes a complete entry set (synthesised LFN run +
 * unique NAME~N short entry when the name doesn't fit 8.3) into a run of
 * consecutive free directory slots, extending the directory cluster chain
 * when needed.  Shared by write/mkdir/rename so Windows sees real long
 * names instead of silently truncated 8.3 stubs.
 * ========================================================================= */

static uint8_t lfn_checksum(const uint8_t s[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + s[i]);
    return sum;
}

/* Is `name` directly 8.3-representable (case-folded)? */
static bool fat_name_is_83(const char *name) {
    if (!name || !name[0]) return false;
    const char *dot = NULL;
    int base = 0, ext = 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '.') {
            if (dot) return false;          /* second dot                  */
            dot = p;
            continue;
        }
        if (c <= ' ' || (unsigned char)c > 126 ||
            c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|' || c == '+' || c == ',' || c == ';' ||
            c == '=' || c == '[' || c == ']')
            return false;
        if (dot) ext++; else base++;
    }
    if (base < 1 || base > 8 || ext > 3) return false;
    if (dot && ext == 0) return false;
    return true;
}

/* Is `name` legal as a LONG name (anything the UI can type minus the FAT
 * reserved characters)?  Length capped at 64 so the LFN run fits the
 * 5-entry decoder limit used across this driver. */
static bool fat_name_is_legal_lfn(const char *name) {
    if (!name || !name[0]) return false;
    int n = 0;
    for (const char *p = name; *p; p++, n++) {
        char c = *p;
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
        if ((unsigned char)c < 0x20) return false;
    }
    if (n > 64) return false;
    if (name[n - 1] == ' ' || name[n - 1] == '.') return false;
    return true;
}

/* Generate a unique NAME~N 8.3 alias for a long name. */
static int fat_gen_short_alias(fat_volume_t *vol, uint32_t dir_cluster,
                               const char *name, uint8_t out11[11]) {
    /* Sanitised base (up to 6 chars) + extension (up to 3). */
    char base[7], ext[4];
    int bn = 0, en = 0;
    const char *dot = NULL;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    for (const char *p = name; *p && p != dot && bn < 6; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-') base[bn++] = c;
    }
    if (bn == 0) { base[0]='F'; base[1]='I'; base[2]='L'; base[3]='E'; bn=4; }
    base[bn] = 0;
    if (dot) {
        for (const char *p = dot + 1; *p && en < 3; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c -= 32;
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-') ext[en++] = c;
        }
    }
    ext[en] = 0;

    for (int n = 1; n <= 99; n++) {
        char cand[14];
        int  ci = 0;
        int  keep = (n < 10) ? bn : (bn > 5 ? 5 : bn);
        if (keep > (n < 10 ? 6 : 5)) keep = (n < 10 ? 6 : 5);
        for (int i = 0; i < keep; i++) cand[ci++] = base[i];
        cand[ci++] = '~';
        if (n >= 10) cand[ci++] = (char)('0' + n / 10);
        cand[ci++] = (char)('0' + n % 10);
        if (en) {
            cand[ci++] = '.';
            for (int i = 0; i < en; i++) cand[ci++] = ext[i];
        }
        cand[ci] = 0;
        fat_entpos_t tmp;
        if (fat_locate_entry(vol, dir_cluster, cand, &tmp) != 0) {
            make_83_name(cand, out11);
            return 0;
        }
    }
    return -1;
}

/* Find `need` CONSECUTIVE free directory slots, extending the cluster
 * chain when the directory runs out of space.  Fills lbas[]/offs[] in
 * directory order.  When the run consumed the 0x00 end-of-directory
 * marker, a fresh terminator is written after the run (unless the run
 * ends exactly at the end of the allocated directory space). */
#define FAT_SET_MAX 6                     /* 5 LFN entries + short entry  */
static int fat_find_free_run(fat_volume_t *vol, uint32_t dir_cluster,
                             int need, uint32_t *lbas, uint32_t *offs) {
    if (need > FAT_SET_MAX) return -1;
    bool fixed_root = (vol->kind != FS_FAT32 && dir_cluster == 0);
    uint32_t root_lba = vol->fat_lba + vol->num_fats * vol->sectors_per_fat;
    uint32_t cluster  = dir_cluster ? dir_cluster
                       : (vol->kind == FS_FAT32 ? vol->root_cluster : 0);
    uint32_t per_cluster = (uint32_t)vol->sectors_per_cluster * 16u;
    uint8_t  sec[512];
    uint32_t e_i = 0;
    int      run = 0;
    bool     term_seen = false;

    for (;;) {
        uint32_t lba, off;
        if (fixed_root) {
            if (e_i >= vol->root_entries) {
                /* Root full mid-run: fail (fixed root can't grow). */
                return (run >= need) ? 0 : -1;
            }
            lba = root_lba + (e_i * 32) / 512u;
            off = (e_i * 32) % 512u;
        } else {
            if (!cluster) return -1;
            uint32_t in_cluster = e_i % per_cluster;
            lba = vol->data_lba + (cluster - 2) * vol->sectors_per_cluster
                + in_cluster / 16u;
            off = (in_cluster % 16u) * 32u;
        }
        if (vol->read(vol->user, lba, sec) != 0) return -1;
        uint8_t first = sec[off];
        if (first == 0x00) term_seen = true;
        if (first == 0x00 || first == 0xE5) {
            lbas[run] = lba;
            offs[run] = off;
            run++;
            if (run >= need) {
                /* Re-terminate after the run if we ate the 0x00 marker
                 * and the next slot is still inside allocated space. */
                if (term_seen) {
                    uint32_t n_i = e_i + 1;
                    bool have_next = false;
                    uint32_t nlba = 0, noff = 0;
                    if (fixed_root) {
                        if (n_i < vol->root_entries) {
                            nlba = root_lba + (n_i * 32) / 512u;
                            noff = (n_i * 32) % 512u;
                            have_next = true;
                        }
                    } else if (n_i % per_cluster != 0) {
                        uint32_t in_c = n_i % per_cluster;
                        nlba = vol->data_lba
                             + (cluster - 2) * vol->sectors_per_cluster
                             + in_c / 16u;
                        noff = (in_c % 16u) * 32u;
                        have_next = true;
                    } else {
                        uint32_t nc = fat_next_cluster(vol, cluster);
                        if (nc) {
                            nlba = vol->data_lba
                                 + (nc - 2) * vol->sectors_per_cluster;
                            noff = 0;
                            have_next = true;
                        }
                    }
                    if (have_next) {
                        if (vol->read(vol->user, nlba, sec) != 0) return -1;
                        if (sec[noff] != 0x00) {
                            memset(sec + noff, 0, 32);
                            if (write_sector(vol, nlba, sec) != 0) return -1;
                        }
                    }
                }
                return 0;
            }
        } else {
            run = 0;
        }
        e_i++;
        if (!fixed_root && e_i % per_cluster == 0) {
            uint32_t next = fat_next_cluster(vol, cluster);
            if (!next) {
                /* Extend the directory with a zeroed cluster. */
                next = fat_alloc_cluster(vol);
                if (!next) return -1;
                fat_write_fat_entry(vol, cluster, next);
                uint8_t zero[512];
                memset(zero, 0, 512);
                uint32_t nlba = vol->data_lba
                              + (next - 2) * vol->sectors_per_cluster;
                for (uint8_t s = 0; s < vol->sectors_per_cluster; s++)
                    write_sector(vol, nlba + s, zero);
            }
            cluster = next;
            e_i = 0;
            /* run continuity across the cluster boundary is preserved:
             * lbas/offs already collected stay valid. */
        }
    }
}

/* ASCII/UTF-8 -> UCS-2 (BMP, multi-byte folded to '_').  Mirrors the
 * decoder in build_lfn which renders non-ASCII as '?'. */
static int fat_name_to_u16(const char *s, uint16_t *d, int cap) {
    int n = 0;
    while (*s && n < cap) {
        unsigned char c = (unsigned char)*s;
        uint32_t cu;
        if      (c < 0x80) { cu = c; s++; }
        else if (c < 0xE0) { cu = ((c & 0x1F) << 6) | (s[1] & 0x3F); s += 2; }
        else               { cu = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6)
                                | (s[2] & 0x3F); s += 3; }
        d[n++] = (uint16_t)cu;
    }
    return n;
}

/* Create a directory entry (set) for `name` in `dir_cluster`:
 * LFN run + alias when needed, plain 8.3 short entry otherwise. */
static int fat_create_dirent(fat_volume_t *vol, uint32_t dir_cluster,
                             const char *name, uint8_t attr,
                             uint32_t first_cluster, uint32_t size) {
    if (!fat_name_is_legal_lfn(name)) return -2;
    uint8_t short11[11];
    int nlfn = 0;
    uint16_t u16[66];
    int ulen = 0;

    if (fat_name_is_83(name)) {
        make_83_name(name, short11);
    } else {
        if (fat_gen_short_alias(vol, dir_cluster, name, short11) != 0)
            return -1;
        ulen = fat_name_to_u16(name, u16, 65);
        nlfn = (ulen + 12) / 13;
        if (nlfn > 5) return -2;
    }

    uint32_t lbas[FAT_SET_MAX], offs[FAT_SET_MAX];
    if (fat_find_free_run(vol, dir_cluster, nlfn + 1, lbas, offs) != 0)
        return -1;

    uint8_t sec[512];
    uint8_t cks = lfn_checksum(short11);

    /* LFN entries: highest sequence first on disk. */
    for (int i = 0; i < nlfn; i++) {
        int seq = nlfn - i;                /* descending                  */
        uint8_t ent[32];
        memset(ent, 0xFF, 32);
        ent[0]  = (uint8_t)(seq | (i == 0 ? 0x40 : 0));
        ent[11] = 0x0F;
        ent[12] = 0;
        ent[13] = cks;
        ent[26] = ent[27] = 0;
        /* 13 UCS-2 slots: bytes 1-10, 14-25, 28-31.  Terminate with
         * 0x0000, pad the rest with 0xFFFF. */
        static const int sl_off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
        for (int k = 0; k < 13; k++) {
            int ci = (seq - 1) * 13 + k;
            uint16_t cu;
            if      (ci <  ulen) cu = u16[ci];
            else if (ci == ulen) cu = 0x0000;
            else                 cu = 0xFFFF;
            ent[sl_off[k]]     = (uint8_t)cu;
            ent[sl_off[k] + 1] = (uint8_t)(cu >> 8);
        }
        if (vol->read(vol->user, lbas[i], sec) != 0) return -1;
        memcpy(sec + offs[i], ent, 32);
        if (write_sector(vol, lbas[i], sec) != 0) return -1;
    }

    /* Short entry. */
    if (vol->read(vol->user, lbas[nlfn], sec) != 0) return -1;
    uint8_t *d = sec + offs[nlfn];
    memset(d, 0, 32);
    memcpy(d, short11, 11);
    d[11] = attr;
    d[26] = first_cluster & 0xFF;
    d[27] = (first_cluster >> 8) & 0xFF;
    d[20] = (first_cluster >> 16) & 0xFF;
    d[21] = (first_cluster >> 24) & 0xFF;
    d[28] = size & 0xFF;
    d[29] = (size >> 8) & 0xFF;
    d[30] = (size >> 16) & 0xFF;
    d[31] = (size >> 24) & 0xFF;
    if (write_sector(vol, lbas[nlfn], sec) != 0) return -1;
    return 0;
}

/* ---- Rename (same directory; LFN targets supported) --------------------- */
/* Returns 0 ok; -2 when new_name contains illegal characters / is too
 * long; -3 when the name is already taken.  8.3-representable targets
 * rewrite the short entry's name bytes in place (and retire the stale
 * LFN run); longer targets get a brand-new entry set with a synthesised
 * LFN run + unique NAME~N alias, after which the old set is retired. */
int fat_rename(fat_volume_t *vol, const char *path, const char *new_name) {
    if (!vol || !vol->mounted || !vol->write) return -1;
    if (vol->kind == FS_FAT12 || vol->kind == FS_NTFS) return -1;
    if (!new_name || !new_name[0]) return -2;
    if (!fat_name_is_legal_lfn(new_name)) return -2;
    vol->rc_path[0] = 0;                  /* invalidate the read cursor   */

    char parent[256], child[64];
    if (split_parent_child(path, parent, sizeof(parent),
                           child, sizeof(child)) != 0) return -1;
    uint32_t dir_cluster = 0;
    if (fat_parent_cluster(vol, parent, &dir_cluster) != 0) return -1;

    fat_entpos_t dup;
    if (fat_locate_entry(vol, dir_cluster, new_name, &dup) == 0) return -3;

    fat_entpos_t pos;
    if (fat_locate_entry(vol, dir_cluster, child, &pos) != 0) return -1;

    uint8_t sec[512];
    if (fat_name_is_83(new_name)) {
        if (vol->read(vol->user, pos.short_lba, sec) != 0) return -1;
        make_83_name(new_name, sec + pos.short_off);
        if (write_sector(vol, pos.short_lba, sec) != 0) return -1;
        /* The old LFN entries spell the OLD name: retire them. */
        for (int i = 0; i < pos.lfn_count; i++) {
            if (vol->read(vol->user, pos.lfn_lba[i], sec) != 0) return -1;
            sec[pos.lfn_off[i]] = 0xE5;
            if (write_sector(vol, pos.lfn_lba[i], sec) != 0) return -1;
        }
        debug_printf("[fat] renamed '%s' -> '%s' (8.3)\n", child, new_name);
        return 0;
    }

    /* Long target: read the old attr byte, write the new entry set first
     * (same chain + size), then retire the old set.  A crash in between
     * leaves a transient duplicate, never a lost file. */
    if (vol->read(vol->user, pos.short_lba, sec) != 0) return -1;
    uint8_t attr = sec[pos.short_off + 11];
    int r = fat_create_dirent(vol, dir_cluster, new_name, attr,
                              pos.first_cluster, pos.size);
    if (r != 0) return r;
    if (fat_mark_deleted(vol, &pos) != 0) return -1;
    debug_printf("[fat] renamed '%s' -> '%s' (LFN)\n", child, new_name);
    return 0;
}

/* ---- Chunked read with a sequential cursor ------------------------------ */
int fat_read_at(fat_volume_t *vol, const char *path, uint32_t offset,
                void *buf, uint32_t len) {
    if (!vol || !vol->mounted || vol->kind == FS_NTFS || !buf) return -1;

    uint32_t cluster_bytes = (uint32_t)vol->sectors_per_cluster * 512u;
    bool same = (vol->rc_path[0] && strcmp(vol->rc_path, path) == 0);
    if (!same || offset < vol->rc_pos) {
        uint32_t first = 0, size = 0;
        bool is_dir = false;
        if (walk_path(vol, path, &first, &is_dir, &size) != 0) return -1;
        if (is_dir) return -1;
        strncpy(vol->rc_path, path, sizeof(vol->rc_path) - 1);
        vol->rc_path[sizeof(vol->rc_path) - 1] = 0;
        vol->rc_first_cluster = first;
        vol->rc_size          = size;
        vol->rc_cluster       = first;
        vol->rc_pos           = 0;
    }
    if (offset >= vol->rc_size) return 0;
    if (len > vol->rc_size - offset) len = vol->rc_size - offset;
    if (len == 0) return 0;

    /* Advance the cursor to the cluster containing `offset`. */
    while (vol->rc_cluster && vol->rc_pos + cluster_bytes <= offset) {
        vol->rc_cluster = fat_next_cluster(vol, vol->rc_cluster);
        vol->rc_pos    += cluster_bytes;
    }
    if (!vol->rc_cluster) return -1;       /* chain shorter than size     */

    static uint8_t cbuf[16 * 1024];        /* single-threaded FS layer    */
    uint32_t copied = 0;
    while (copied < len && vol->rc_cluster) {
        if (read_cluster(vol, vol->rc_cluster, cbuf, sizeof(cbuf)) == 0)
            return (int)(copied ? copied : (uint32_t)-1);
        uint32_t in_off = offset + copied - vol->rc_pos;
        uint32_t chunk  = cluster_bytes - in_off;
        if (chunk > len - copied) chunk = len - copied;
        memcpy((uint8_t *)buf + copied, cbuf + in_off, chunk);
        copied += chunk;
        if (in_off + chunk >= cluster_bytes) {
            vol->rc_cluster = fat_next_cluster(vol, vol->rc_cluster);
            vol->rc_pos    += cluster_bytes;
        }
    }
    return (int)copied;
}

/* ---- Streaming write ----------------------------------------------------- */
/* One open stream per volume; the cluster-accumulation buffer is shared
 * (single-threaded FS layer).  The directory entry appears only at
 * commit, so an aborted copy never leaves a half-file behind. */
static uint8_t g_fat_ws_buf[32 * 1024];   /* FAT max cluster = 32 KiB     */

static int fat_ws_flush_cluster(fat_volume_t *vol) {
    uint32_t c = fat_alloc_cluster(vol);
    if (!c) return -1;
    if (vol->ws_prev) fat_write_fat_entry(vol, vol->ws_prev, c);
    if (!vol->ws_first) vol->ws_first = c;
    if (write_cluster(vol, c, g_fat_ws_buf, vol->ws_buffered) != 0) return -1;
    vol->ws_prev = c;
    vol->ws_buffered = 0;
    return 0;
}

int fat_write_open(fat_volume_t *vol, const char *path) {
    if (!vol || !vol->mounted || !vol->write) return -1;
    if (vol->kind == FS_FAT12 || vol->kind == FS_NTFS) return -1;
    if (vol->ws_active) return -1;
    if ((uint32_t)vol->sectors_per_cluster * 512u > sizeof(g_fat_ws_buf))
        return -1;

    char parent[256], child[64];
    if (split_parent_child(path, parent, sizeof(parent),
                           child, sizeof(child)) != 0) return -1;
    uint32_t dir_cluster = 0;
    if (fat_parent_cluster(vol, parent, &dir_cluster) != 0) return -1;

    /* Overwrite semantics: retire any existing entry up front. */
    fat_entpos_t ex;
    if (fat_locate_entry(vol, dir_cluster, child, &ex) == 0) {
        if (ex.is_dir) return -1;
        fat_free_chain(vol, ex.first_cluster);
        if (fat_mark_deleted(vol, &ex) != 0) return -1;
    }

    vol->ws_active      = true;
    vol->ws_dir_cluster = dir_cluster;
    strncpy(vol->ws_name, child, sizeof(vol->ws_name) - 1);
    vol->ws_name[sizeof(vol->ws_name) - 1] = 0;
    vol->ws_first = vol->ws_prev = 0;
    vol->ws_len = 0;
    vol->ws_buffered = 0;
    vol->rc_path[0] = 0;
    return 0;
}

int fat_write_append(fat_volume_t *vol, const void *data, uint32_t len) {
    if (!vol || !vol->ws_active) return -1;
    uint32_t cluster_bytes = (uint32_t)vol->sectors_per_cluster * 512u;
    const uint8_t *src = (const uint8_t *)data;
    while (len > 0) {
        uint32_t space = cluster_bytes - vol->ws_buffered;
        uint32_t take  = (len < space) ? len : space;
        memcpy(g_fat_ws_buf + vol->ws_buffered, src, take);
        vol->ws_buffered += take;
        vol->ws_len      += take;
        src += take;
        len -= take;
        if (vol->ws_buffered == cluster_bytes) {
            if (fat_ws_flush_cluster(vol) != 0) {
                fat_write_close(vol, false);
                return -1;
            }
        }
    }
    return 0;
}

int fat_write_close(fat_volume_t *vol, bool commit) {
    if (!vol || !vol->ws_active) return -1;
    vol->ws_active = false;
    vol->rc_path[0] = 0;
    if (!commit) {
        if (vol->ws_first) fat_free_chain(vol, vol->ws_first);
        return 0;
    }
    /* Flush the tail (or allocate one empty cluster for 0-byte files,
     * mirroring fat_write_file). */
    if (vol->ws_buffered > 0 || vol->ws_first == 0) {
        if (fat_ws_flush_cluster(vol) != 0) {
            if (vol->ws_first) fat_free_chain(vol, vol->ws_first);
            return -1;
        }
    }
    int r = fat_create_dirent(vol, vol->ws_dir_cluster, vol->ws_name,
                              0x20, vol->ws_first, vol->ws_len);
    if (r != 0) {
        fat_free_chain(vol, vol->ws_first);
        return -1;
    }
    debug_printf("[fat] streamed file '%s' (%u bytes, cluster %u)\n",
                 vol->ws_name, vol->ws_len, vol->ws_first);
    return 0;
}

/* ============================================================================
 * NexxoN OS - NTFS MFT walker (read-only)
 * ----------------------------------------------------------------------------
 * A minimum-viable NTFS reader that can list the root directory and read a
 * file by absolute path.  This sits on top of a generic block-device read
 * callback so the parser doesn't care whether the volume is on a USB stick
 * or a SATA partition.
 *
 * Key concepts (the bare minimum subset of the NTFS reference):
 *
 *   * Cluster = bytes_per_sector * sectors_per_cluster.  Boot-sector
 *     reports both.  $MFT lives at MFT_LCN clusters in from the
 *     partition start.
 *
 *   * FILE RECORD = a fixed-size on-disk structure (default 1024 bytes
 *     unless the boot sector says otherwise).  Each record has a tiny
 *     header followed by a sequence of NTFS_ATTRIBUTE structures.
 *
 *   * Attributes we care about:
 *       0x10  $STANDARD_INFORMATION  - timestamps (ignored)
 *       0x30  $FILE_NAME             - the canonical name of the entry
 *       0x80  $DATA                  - the file's content (resident OR
 *                                      non-resident with data runs)
 *       0x90  $INDEX_ROOT            - directory inline index
 *       0xA0  $INDEX_ALLOCATION      - directory's larger index
 *
 *   * Each FILE RECORD has a "fixup" array.  We don't bother validating
 *     the USN word at the end of every sector - corrupted records will
 *     surface as bogus attribute lengths and we'll bail out at the first
 *     out-of-range value.
 *
 *   * Non-resident attributes encode their cluster runs as a packed list:
 *         byte 0: header = (len_size << 4) | offset_size
 *         len_size bytes: cluster count (LE)
 *         offset_size bytes: signed delta from previous offset (LE)
 *
 * For files >8 MiB this is more than a kernel pendrive driver should be
 * doing, so we cap copies at the caller-supplied capacity and bail.
 * ============================================================================ */
#include "ntfs.h"
#include "string.h"
#include "debug.h"

typedef struct PACKED {
    uint8_t  jmp[3];
    char     oem[8];                /* "NTFS    "                            */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  zeroes1[3];
    uint16_t unused1;
    uint8_t  media;
    uint16_t zeroes2;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t zeroes3;
    uint32_t unused2;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mftmirr_lcn;
    int8_t   clusters_per_mft_record;     /* negative -> 2^-N bytes        */
    uint8_t  reserved3[3];
    int8_t   clusters_per_index_record;
    uint8_t  reserved4[3];
    uint64_t serial;
} ntfs_bpb_t;

typedef struct PACKED {
    char     magic[4];           /* "FILE" */
    uint16_t usa_offset;
    uint16_t usa_count;
    uint64_t lsn;
    uint16_t seq;
    uint16_t link_count;
    uint16_t attrs_offset;
    uint16_t flags;              /* 0x01 = in use, 0x02 = directory */
    uint32_t used_size;
    uint32_t allocated_size;
    uint64_t base_record_ref;
    uint16_t next_attr_id;
} ntfs_file_record_t;

typedef struct PACKED {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t id;
} ntfs_attr_hdr_t;

typedef struct PACKED {
    uint32_t value_length;       /* resident only */
    uint16_t value_offset;
    uint8_t  indexed_flag;
    uint8_t  reserved;
} ntfs_attr_resident_t;

typedef struct PACKED {
    uint64_t starting_vcn;
    uint64_t last_vcn;
    uint16_t mapping_pairs_offset;
    uint16_t compression_unit_size;
    uint32_t padding;
    uint64_t allocated_size;
    uint64_t real_size;
    uint64_t initialised_size;
} ntfs_attr_non_resident_t;

/* ---------- Sector / cluster helpers -------------------------------------- */
static int read_sector(ntfs_volume_t *vol, uint64_t lba, void *buf) {
    return vol->read(vol->user, (uint32_t)lba, buf);
}

static int read_cluster(ntfs_volume_t *vol, uint64_t lcn, void *buf) {
    uint64_t lba = vol->lba_start + lcn * vol->sectors_per_cluster;
    for (uint32_t i = 0; i < vol->sectors_per_cluster; i++) {
        if (read_sector(vol, lba + i, (uint8_t *)buf + i * 512u) != 0) return -1;
    }
    return 0;
}

/* Read a single FILE record (1024 B default) from $MFT.  We treat the
 * MFT as a contiguous run starting at mft_lcn - safe for the common case
 * where $MFT itself hasn't been fragmented by a defragger.
 *
 * We deliberately keep this in 32-bit arithmetic: rec * file_record_size
 * stays under 4 GiB for any realistic pendrive (rec_size==1024 means a
 * 4M-entry MFT, which would itself be 4 GiB).  Avoiding 64-bit division
 * keeps the freestanding kernel free of libgcc helper dependencies. */
static int read_file_record(ntfs_volume_t *vol, uint64_t rec, void *buf) {
    uint32_t byte_off = (uint32_t)rec * vol->file_record_size;
    uint32_t cluster_bytes = (uint32_t)vol->sectors_per_cluster * 512u;
    if (cluster_bytes == 0) return -1;
    uint64_t lcn      = vol->mft_lcn + (uint64_t)(byte_off / cluster_bytes);
    uint32_t lcn_off  = byte_off % cluster_bytes;
    static uint8_t cluster_buf[16 * 1024];
    if (read_cluster(vol, lcn, cluster_buf) != 0) return -1;
    if (lcn_off + vol->file_record_size > sizeof(cluster_buf)) return -1;
    memcpy(buf, cluster_buf + lcn_off, vol->file_record_size);
    return 0;
}

/* ---------- Mount --------------------------------------------------------- */
bool ntfs_mount(ntfs_volume_t *vol, ntfs_blockdev_read_t rd, void *user,
                uint32_t partition_lba) {
    if (!vol || !rd) return false;
    memset(vol, 0, sizeof(*vol));
    vol->read = rd;
    vol->user = user;
    vol->lba_start = partition_lba;

    uint8_t sec[512];
    if (rd(user, partition_lba, sec) != 0) return false;
    if (memcmp(sec + 3, "NTFS    ", 8) != 0) return false;
    const ntfs_bpb_t *b = (const ntfs_bpb_t *)sec;
    if (b->bytes_per_sector != 512 || b->sectors_per_cluster == 0)
        return false;
    vol->bytes_per_sector    = b->bytes_per_sector;
    vol->sectors_per_cluster = b->sectors_per_cluster;
    vol->total_sectors       = b->total_sectors;
    vol->mft_lcn             = b->mft_lcn;
    int8_t cpr = b->clusters_per_mft_record;
    if (cpr > 0) {
        vol->file_record_size = (uint16_t)((uint32_t)cpr *
                                           vol->sectors_per_cluster * 512u);
    } else {
        vol->file_record_size = (uint16_t)(1u << (uint8_t)(-cpr));
    }
    if (vol->file_record_size < 1024) vol->file_record_size = 1024;
    if (vol->file_record_size > 4096) vol->file_record_size = 4096;
    int8_t ipr = b->clusters_per_index_record;
    if (ipr > 0) {
        vol->index_record_size = (uint16_t)((uint32_t)ipr *
                                            vol->sectors_per_cluster * 512u);
    } else {
        vol->index_record_size = (uint16_t)(1u << (uint8_t)(-ipr));
    }
    if (vol->index_record_size < 1024) vol->index_record_size = 4096;
    memcpy(vol->label, "NTFS-VOL", 9);
    vol->mounted = true;
    debug_printf("[ntfs] mounted at LBA %u, MFT LCN=%llu, rec_sz=%u\n",
                 partition_lba, (unsigned long long)vol->mft_lcn,
                 vol->file_record_size);
    return true;
}

/* ---------- Attribute iteration ------------------------------------------- */
typedef struct {
    const ntfs_attr_hdr_t *hdr;
    uint16_t               total_length;
    const uint8_t         *value;        /* resident only */
    uint32_t               value_length;
} ntfs_attr_view_t;

static bool find_attr(uint8_t *record, uint32_t record_size, uint32_t type,
                      ntfs_attr_view_t *out) {
    const ntfs_file_record_t *fr = (const ntfs_file_record_t *)record;
    if (memcmp(fr->magic, "FILE", 4) != 0) return false;
    uint16_t off = fr->attrs_offset;
    while ((uint32_t)off + 4u < record_size) {
        const ntfs_attr_hdr_t *h = (const ntfs_attr_hdr_t *)(record + off);
        if (h->type == 0xFFFFFFFF) break;
        if (h->length == 0 || off + h->length > record_size) break;
        if (h->type == type) {
            out->hdr = h;
            out->total_length = h->length;
            if (!h->non_resident) {
                const ntfs_attr_resident_t *r =
                    (const ntfs_attr_resident_t *)(record + off + sizeof(*h));
                out->value        = record + off + r->value_offset;
                out->value_length = r->value_length;
            } else {
                out->value        = NULL;
                out->value_length = 0;
            }
            return true;
        }
        off += h->length;
    }
    return false;
}

/* ---------- Data-run decode + read --------------------------------------- */
typedef struct {
    int64_t  lcn;
    uint64_t length;
} ntfs_run_t;

static int decode_runs(const uint8_t *runs, int max_bytes,
                       ntfs_run_t *out, int max) {
    int64_t  cur_lcn = 0;
    int      n = 0;
    int      off = 0;
    while (off < max_bytes && n < max) {
        uint8_t hdr = runs[off++];
        if (hdr == 0) break;
        int len_sz  = hdr & 0x0F;
        int off_sz  = (hdr >> 4) & 0x0F;
        if (off + len_sz + off_sz > max_bytes) break;
        uint64_t length = 0;
        for (int i = 0; i < len_sz; i++) {
            length |= (uint64_t)runs[off + i] << (i * 8);
        }
        off += len_sz;
        int64_t delta = 0;
        for (int i = 0; i < off_sz; i++) {
            delta |= (int64_t)runs[off + i] << (i * 8);
        }
        if (off_sz > 0 && (runs[off + off_sz - 1] & 0x80)) {
            /* Sign-extend. */
            for (int i = off_sz; i < 8; i++) delta |= (int64_t)0xFFu << (i * 8);
        }
        off += off_sz;
        cur_lcn += delta;
        out[n].lcn    = (off_sz == 0) ? -1 : cur_lcn;   /* sparse */
        out[n].length = length;
        n++;
    }
    return n;
}

/* Read a non-resident $DATA attribute up to `cap` bytes into `out`.
 * Returns the real size of the file. */
static int read_non_resident_data(ntfs_volume_t *vol, uint8_t *record,
                                  const ntfs_attr_hdr_t *attr,
                                  void *out, uint32_t cap) {
    const ntfs_attr_non_resident_t *nr =
        (const ntfs_attr_non_resident_t *)((const uint8_t *)attr + sizeof(*attr));
    uint64_t real_size = nr->real_size;
    const uint8_t *runs = (const uint8_t *)attr + nr->mapping_pairs_offset;
    int runs_max = attr->length - nr->mapping_pairs_offset;
    if (runs_max <= 0) return -1;
    ntfs_run_t r[16];
    int n = decode_runs(runs, runs_max, r, 16);
    if (n <= 0) return -1;
    uint32_t copied = 0;
    static uint8_t cluster_buf[16 * 1024];
    for (int i = 0; i < n && copied < cap; i++) {
        if (r[i].lcn < 0) {
            /* Sparse run - bytes are implicitly zero. */
            uint64_t bytes = r[i].length *
                             (uint64_t)vol->sectors_per_cluster * 512u;
            uint32_t want = (uint32_t)bytes;
            if (copied + want > cap) want = cap - copied;
            memset((uint8_t *)out + copied, 0, want);
            copied += want;
            continue;
        }
        for (uint64_t k = 0; k < r[i].length && copied < cap; k++) {
            if (read_cluster(vol, (uint64_t)r[i].lcn + k, cluster_buf) != 0)
                return -1;
            uint32_t chunk = vol->sectors_per_cluster * 512u;
            if (copied + chunk > cap) chunk = cap - copied;
            memcpy((uint8_t *)out + copied, cluster_buf, chunk);
            copied += chunk;
        }
    }
    (void)record;
    return (int)real_size;
}

/* ---------- Directory listing -------------------------------------------- */
typedef struct PACKED {
    uint64_t parent_dir_ref;
    uint64_t creation_time;
    uint64_t modified_time;
    uint64_t mft_modified_time;
    uint64_t accessed_time;
    uint64_t allocated_size;
    uint64_t real_size;
    uint32_t flags;
    uint32_t reparse;
    uint8_t  name_length;
    uint8_t  name_type;
    uint16_t name[1];
} ntfs_filename_attr_t;

typedef struct PACKED {
    uint64_t file_ref;
    uint16_t length;
    uint16_t content_length;
    uint32_t flags;            /* bit0 = subnode, bit1 = last */
} ntfs_index_entry_t;

static void utf16_to_ascii(const uint16_t *src, int n, char *dst, int cap) {
    int o = 0;
    for (int i = 0; i < n && o < cap - 1; i++) {
        uint16_t c = src[i];
        dst[o++] = (c < 128) ? (char)c : '?';
    }
    dst[o] = 0;
}

int ntfs_list(ntfs_volume_t *vol, const char *path, ntfs_entry_t *out, int max) {
    if (!vol || !vol->mounted) return -1;
    (void)path;   /* only root listing in v1 */
    uint8_t record[4096];
    if (read_file_record(vol, 5, record) != 0) return -1;
    ntfs_attr_view_t iroot;
    if (!find_attr(record, vol->file_record_size, 0x90, &iroot)) {
        debug_printf("[ntfs] $INDEX_ROOT missing on root dir record\n");
        return -1;
    }
    /* $INDEX_ROOT layout:
     *   16-byte header (attribute type, etc.)
     *   16-byte index-header (entries_offset, total_size, allocated_size, flags)
     *   then index entries.
     * Each entry has a header + content (FILE_NAME attribute). */
    const uint8_t *p = iroot.value;
    if (!p) return -1;
    uint32_t entries_offset = *(const uint32_t *)(p + 16);
    const uint8_t *e = p + 16 + entries_offset;
    int n = 0;
    while (n < max) {
        const ntfs_index_entry_t *ie = (const ntfs_index_entry_t *)e;
        if (ie->length == 0) break;
        if (ie->flags & 0x02) break;   /* end marker */
        if (ie->content_length >= sizeof(ntfs_filename_attr_t) - 2) {
            const ntfs_filename_attr_t *fn =
                (const ntfs_filename_attr_t *)(e + sizeof(*ie));
            /* Skip DOS-only names; they duplicate WIN32 entries. */
            if (fn->name_type != 0x02) {
                ntfs_entry_t *o = &out[n++];
                utf16_to_ascii(fn->name, fn->name_length, o->name, sizeof(o->name));
                o->size       = fn->real_size;
                o->is_dir     = (fn->flags & 0x10000000) != 0;
                o->mft_record = ie->file_ref & 0x0000FFFFFFFFFFFFull;
            }
        }
        e += ie->length;
        if (e >= iroot.value + iroot.value_length) break;
    }
    return n;
}

int ntfs_read(ntfs_volume_t *vol, const char *path, void *buf, uint32_t cap) {
    if (!vol || !vol->mounted) return -1;
    /* Walk root looking for a name match - subdirectories not supported
     * in v1, which is consistent with the read-only "browse the stick"
     * use case. */
    ntfs_entry_t entries[64];
    int n = ntfs_list(vol, "/", entries, 64);
    while (*path == '/' || *path == '\\') path++;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, path) != 0 || entries[i].is_dir) continue;
        uint8_t record[4096];
        if (read_file_record(vol, entries[i].mft_record, record) != 0) return -1;
        ntfs_attr_view_t data;
        if (!find_attr(record, vol->file_record_size, 0x80, &data)) return -1;
        if (!data.hdr->non_resident) {
            uint32_t want = data.value_length;
            if (want > cap) want = cap;
            memcpy(buf, data.value, want);
            return (int)data.value_length;
        }
        return read_non_resident_data(vol, record, data.hdr, buf, cap);
    }
    return -1;
}

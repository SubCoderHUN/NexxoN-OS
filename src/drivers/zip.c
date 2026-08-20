/* ============================================================================
 * NexxoN OS - Zip archive reader
 * ----------------------------------------------------------------------------
 * Reads the End of Central Directory record, walks the CD entries,
 * caches each member's local header offset + compressed size, and
 * extracts on demand.  Inflation calls into the DEFLATE decoder from
 * png.c via the publicly-exposed `deflate_inflate` shim (declared
 * inline below to avoid a header round-trip).
 *
 * Limits: 128 entries, names up to 128 chars, no Zip64 (4 GiB max).
 * ============================================================================ */
#include "zip.h"
#include "nxfs.h"
#include "string.h"
#include "debug.h"

/* DEFLATE inflate function exposed by png.c for cross-module reuse. */
extern int deflate_inflate(const uint8_t *src, uint32_t src_len,
                           uint8_t *out, uint32_t out_cap);

#define LFH_SIG  0x04034B50u
#define CFH_SIG  0x02014B50u
#define EOCD_SIG 0x06054B50u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool zip_open(zip_t *z, const uint8_t *blob, uint32_t blob_len) {
    if (!z || !blob || blob_len < 22) return false;
    memset(z, 0, sizeof(*z));
    z->blob = blob;
    z->blob_len = blob_len;
    /* Find EOCD by scanning the last 1 KiB (no Zip64 comment support). */
    int eocd = -1;
    uint32_t max_scan = blob_len < 1024 ? blob_len : 1024;
    for (uint32_t i = 0; i + 22 <= max_scan; i++) {
        if (rd32(blob + blob_len - 22 - i) == EOCD_SIG) {
            eocd = (int)(blob_len - 22 - i);
            break;
        }
    }
    if (eocd < 0) return false;
    uint16_t entries = rd16(blob + eocd + 10);
    uint32_t cd_size = rd32(blob + eocd + 12);
    uint32_t cd_off  = rd32(blob + eocd + 16);
    if (cd_off + cd_size > blob_len) return false;
    if (entries > ZIP_MAX_ENTRIES) entries = ZIP_MAX_ENTRIES;
    uint32_t off = cd_off;
    int n = 0;
    while (n < entries && off + 46 <= blob_len) {
        if (rd32(blob + off) != CFH_SIG) break;
        uint16_t method   = rd16(blob + off + 10);
        uint32_t comp_sz  = rd32(blob + off + 20);
        uint32_t un_sz    = rd32(blob + off + 24);
        uint16_t name_len = rd16(blob + off + 28);
        uint16_t extra_len= rd16(blob + off + 30);
        uint16_t com_len  = rd16(blob + off + 32);
        uint32_t local_off= rd32(blob + off + 42);
        zip_entry_t *e = &z->entries[n];
        e->method = method;
        e->comp_size = comp_sz;
        e->uncomp_size = un_sz;
        e->local_header_offset = local_off;
        uint16_t cap = (name_len < ZIP_NAME_MAX - 1) ? name_len : ZIP_NAME_MAX - 1;
        memcpy(e->name, blob + off + 46, cap);
        e->name[cap] = 0;
        e->is_dir = (cap > 0 && e->name[cap - 1] == '/');
        off += 46u + name_len + extra_len + com_len;
        n++;
    }
    z->count = n;
    debug_printf("[zip] EOCD@0x%x entries=%d\n", eocd, n);
    return true;
}

int zip_extract(zip_t *z, int idx, uint8_t *dst, uint32_t dst_cap) {
    if (!z || idx < 0 || idx >= z->count || !dst) return -1;
    zip_entry_t *e = &z->entries[idx];
    if (e->is_dir) return 0;
    uint32_t lo = e->local_header_offset;
    if (lo + 30 > z->blob_len) return -1;
    if (rd32(z->blob + lo) != LFH_SIG) return -1;
    uint16_t name_len  = rd16(z->blob + lo + 26);
    uint16_t extra_len = rd16(z->blob + lo + 28);
    uint32_t data_off  = lo + 30u + name_len + extra_len;
    if (data_off + e->comp_size > z->blob_len) return -1;
    if (e->method == 0) {
        uint32_t want = e->uncomp_size < dst_cap ? e->uncomp_size : dst_cap;
        memcpy(dst, z->blob + data_off, want);
        return (int)want;
    }
    if (e->method == 8) {
        return deflate_inflate(z->blob + data_off, e->comp_size,
                               dst, dst_cap);
    }
    return -1;
}

int zip_extract_to_nxfs(zip_t *z, int idx, uint32_t parent_inode) {
    if (!z || idx < 0 || idx >= z->count) return -1;
    zip_entry_t *e = &z->entries[idx];
    if (e->is_dir) {
        uint32_t out_inode;
        return nxfs_create_dir(parent_inode, e->name, &out_inode);
    }
    static uint8_t scratch[256 * 1024];
    if (e->uncomp_size > sizeof(scratch)) return -1;
    int got = zip_extract(z, idx, scratch, sizeof(scratch));
    if (got < 0) return -1;
    /* Only the basename - skip path components for now. */
    const char *base = e->name;
    for (const char *p = e->name; *p; p++) if (*p == '/') base = p + 1;
    if (!*base) return 0;
    uint32_t ino;
    if (nxfs_create_file(parent_inode, base, &ino) != NXFS_OK) return -1;
    return nxfs_write_file(ino, scratch, (uint32_t)got);
}

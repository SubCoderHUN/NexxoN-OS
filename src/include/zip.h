/* ============================================================================
 * NexxoN OS - Zip archive reader (NexxoN Zip)  (v1.0)
 * ----------------------------------------------------------------------------
 * Parses standard ZIP archives (PKWARE local file header + central
 * directory) and exposes a per-entry iteration API.  Inflation reuses
 * the DEFLATE decoder that already ships with png.c via a tiny shim.
 *
 * Capabilities:
 *   - Local headers + central directory walk (no Zip64).
 *   - Store (method 0) and Deflate (method 8) member content.
 *   - File / directory bit decoded from the external attributes.
 *   - Extraction to NXFS as raw bytes; directories created on demand.
 * ============================================================================ */
#ifndef NEXXON_ZIP_H
#define NEXXON_ZIP_H

#include "types.h"

#define ZIP_MAX_ENTRIES   128
#define ZIP_NAME_MAX      128

typedef struct {
    char     name[ZIP_NAME_MAX];
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_header_offset;
    uint16_t method;           /* 0 = store, 8 = deflate */
    bool     is_dir;
} zip_entry_t;

typedef struct {
    const uint8_t *blob;
    uint32_t       blob_len;
    int            count;
    zip_entry_t    entries[ZIP_MAX_ENTRIES];
} zip_t;

bool zip_open       (zip_t *z, const uint8_t *blob, uint32_t blob_len);
int  zip_extract    (zip_t *z, int idx, uint8_t *dst, uint32_t dst_cap);
int  zip_extract_to_nxfs(zip_t *z, int idx, uint32_t parent_inode);

#endif /* NEXXON_ZIP_H */

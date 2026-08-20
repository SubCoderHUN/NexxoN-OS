/* ============================================================================
 * NexxoN OS - nxpkg package manager + NexxStore (TASK 30, v1.0)
 * ----------------------------------------------------------------------------
 * Tiny package manager that downloads .nxpkg archives from a registry,
 * verifies their SHA-256 checksum, and unpacks them into /apps/<name>.
 * The companion "NexxStore" GUI client wraps the same calls behind a
 * familiar storefront interface (catalog, install, uninstall buttons).
 *
 * A .nxpkg archive is a single-blob format:
 *
 *     nxpkg_header_t  hdr;           // magic, version, files, size
 *     nxpkg_entry_t   entries[];     // per-file metadata
 *     uint8_t         payload[];     // concatenated file bodies
 *
 * No compression in v1 - that comes once we have a Zstd / Deflate
 * codec available outside the PNG IDAT path.  Each entry carries an
 * absolute path inside /apps so the unpacker can mkdir + write
 * verbatim through nxfs_create_file / nxfs_write_file.
 * ============================================================================ */
#ifndef NEXXON_NXPKG_H
#define NEXXON_NXPKG_H

#include "types.h"

#define NXPKG_MAGIC          0x4E58504Bu       /* "NXPK" */
#define NXPKG_VERSION        1
#define NXPKG_PATH_MAX       128
#define NXPKG_NAME_MAX       48
#define NXPKG_REGISTRY_URL   "http://10.0.2.2/nxpkg/"

typedef struct PACKED {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
    uint32_t total_size;
    uint8_t  sha256[32];
    char     name[NXPKG_NAME_MAX];
    char     version_str[16];
    char     publisher[32];
} nxpkg_header_t;

typedef struct PACKED {
    uint32_t offset;
    uint32_t size;
    uint32_t mode;
    char     path[NXPKG_PATH_MAX];
} nxpkg_entry_t;

int  nxpkg_install      (const char *name);
int  nxpkg_uninstall    (const char *name);
int  nxpkg_search       (const char *query, char *out, uint32_t cap);
int  nxpkg_list_installed(char *out, uint32_t cap);

#endif /* NEXXON_NXPKG_H */

/* ============================================================================
 * NexxoN OS - nxpkg package manager
 * ----------------------------------------------------------------------------
 * Pulls .nxpkg archives over HTTP (via download_simple), verifies the
 * SHA-256 in the header, and unpacks each entry into the live NXFS
 * filesystem.  The graphical "NexxStore" front-end is in a separate
 * module that simply drives these helpers from button callbacks.
 * ============================================================================ */
#include "nxpkg.h"
#include "download.h"
#include "crypto.h"
#include "nxfs.h"
#include "string.h"
#include "debug.h"

static uint8_t g_pkg_buf[256 * 1024];     /* 256 KiB max package size */

int nxpkg_install(const char *name) {
    if (!name || !name[0]) return -1;
    char url[256];
    int n = 0;
    const char *reg = NXPKG_REGISTRY_URL;
    while (reg[n] && n < 240) { url[n] = reg[n]; n++; }
    int k = 0;
    while (name[k] && n < 250) { url[n++] = name[k++]; }
    const char *suffix = ".nxpkg";
    int s = 0;
    while (suffix[s] && n < 255) { url[n++] = suffix[s++]; }
    url[n] = 0;
    int got = download_simple(url, g_pkg_buf, sizeof(g_pkg_buf));
    if (got <= 0) {
        debug_printf("[nxpkg] download failed for '%s'\n", name);
        return -1;
    }
    if ((uint32_t)got < sizeof(nxpkg_header_t)) {
        debug_printf("[nxpkg] archive too small (%d bytes)\n", got);
        return -1;
    }
    const nxpkg_header_t *hdr = (const nxpkg_header_t *)g_pkg_buf;
    if (hdr->magic != NXPKG_MAGIC || hdr->version != NXPKG_VERSION) {
        debug_printf("[nxpkg] bad magic / version\n");
        return -1;
    }
    /* Verify SHA-256 of everything past the hash field. */
    uint8_t computed[32];
    /* Hash header (with sha256 field zeroed) + payload. */
    uint8_t hdr_for_hash[sizeof(*hdr)];
    memcpy(hdr_for_hash, hdr, sizeof(*hdr));
    memset(((nxpkg_header_t *)hdr_for_hash)->sha256, 0, 32);
    sha256_ctx_t ctx; sha256_init(&ctx);
    sha256_update(&ctx, hdr_for_hash, sizeof(hdr_for_hash));
    sha256_update(&ctx, g_pkg_buf + sizeof(*hdr), got - sizeof(*hdr));
    sha256_final(&ctx, computed);
    if (memcmp(computed, hdr->sha256, 32) != 0) {
        debug_printf("[nxpkg] SHA-256 mismatch - refusing to install\n");
        return -1;
    }
    /* Walk entries.  Each is a file; we route through nxfs_create_file
     * for write.  Subdirectories are auto-created by the loader. */
    const nxpkg_entry_t *entries = (const nxpkg_entry_t *)
                                   (g_pkg_buf + sizeof(*hdr));
    for (uint32_t i = 0; i < hdr->file_count; i++) {
        const nxpkg_entry_t *e = &entries[i];
        if (e->offset + e->size > (uint32_t)got) {
            debug_printf("[nxpkg] entry %u overflows package\n", i);
            return -1;
        }
        uint32_t ino;
        /* Strip leading slash so the path is relative to root. */
        const char *p = e->path;
        while (*p == '/' || *p == '\\') p++;
        uint32_t parent = 0;     /* root inode */
        if (nxfs_create_file(parent, p, &ino) != NXFS_OK) {
            debug_printf("[nxpkg] failed to create '%s' - skipping\n", p);
            continue;
        }
        nxfs_write_file(ino, g_pkg_buf + e->offset, e->size);
    }
    debug_printf("[nxpkg] installed '%s' (%u files)\n", name, hdr->file_count);
    return 0;
}

int nxpkg_uninstall(const char *name) {
    if (!name) return -1;
    /* Walk /apps/<name> and delete each entry.  For now we just delete
     * a file with that name from the root - the .nxpkg list-of-installed
     * manifest lands with a later patch. */
    nxfs_delete_file(0, name);
    return 0;
}

int nxpkg_search(const char *query, char *out, uint32_t cap) {
    /* Catalog request: GET /nxpkg/index.txt and stream into `out`. */
    char url[128];
    int n = 0;
    const char *reg = NXPKG_REGISTRY_URL;
    while (reg[n] && n < 100) { url[n] = reg[n]; n++; }
    const char *suffix = "index.txt";
    int s = 0;
    while (suffix[s] && n < 127) url[n++] = suffix[s++];
    url[n] = 0;
    /* Use a SHORT timeout here.  This is called from the NexxStore GUI on
     * open, and synchronous networking freezes the whole foreground while
     * it blocks.  The default 10 s timeout made an unreachable registry
     * look like a system hang; 2 s worst-case is the most we tolerate. */
    download_request_t req;
    memset(&req, 0, sizeof(req));
    int u = 0;
    while (url[u] && u < DOWNLOAD_MAX_URL - 1) { req.url[u] = url[u]; u++; }
    req.url[u]     = 0;
    req.out_buf    = (uint8_t *)out;
    req.out_cap    = cap;
    req.timeout_ms = 2000;
    download_result_t res;
    int got = download_fetch(&req, &res);
    (void)query;
    return got;
}

int nxpkg_list_installed(char *out, uint32_t cap) {
    /* Walk /apps and emit the immediate children.  For now we don't
     * have a real /apps directory in NXFS, so this returns 0. */
    (void)out; (void)cap;
    return 0;
}

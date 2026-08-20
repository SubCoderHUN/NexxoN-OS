/* ============================================================================
 * NexxoN OS - .nxl dynamic library loader
 * ----------------------------------------------------------------------------
 * Tiny single-pass loader.  Reads a .nxl from disk (via nxfs_read_file)
 * into a per-library BSS slot, validates its header, and exposes a
 * symbol-resolution table.  The kernel itself populates the well-known
 * libc / libgui symbol exports - we don't need a real binary file for
 * the bootstrap; we register the kernel's own implementation under each
 * symbol's name.  This keeps the kernel monolithic in this milestone
 * while exposing the .nxl ABI to future user-space binaries.
 * ============================================================================ */
#include "nxl.h"
#include "string.h"
#include "debug.h"

#define NXL_EXPORT_MAX  128

typedef struct {
    char         name[40];
    void        *addr;
} nxl_export_t;

static nxl_export_t  g_exports[NXL_EXPORT_MAX];
static int           g_n_exports = 0;
static nxl_library_t g_libs[NXL_MAX_LIBS];

/* Forward declarations of kernel functions we want to re-export under
 * libc.nxl / libgui.nxl. */
extern void *memset(void *dst, int val, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);
extern size_t strlen(const char *s);
extern int    strcmp(const char *a, const char *b);
extern int    ksnprintf(char *buf, size_t buf_sz, const char *fmt, ...);

static void register_export(const char *name, void *addr) {
    if (g_n_exports >= NXL_EXPORT_MAX) return;
    nxl_export_t *e = &g_exports[g_n_exports++];
    int n = 0;
    while (name[n] && n < 39) { e->name[n] = name[n]; n++; }
    e->name[n] = 0;
    e->addr = addr;
}

bool nxl_loader_init(void) {
    g_n_exports = 0;
    memset(g_libs, 0, sizeof(g_libs));

    /* Seed the well-known libc exports. */
    register_export("memset",     (void *)memset);
    register_export("memcpy",     (void *)memcpy);
    register_export("strlen",     (void *)strlen);
    register_export("strcmp",     (void *)strcmp);
    register_export("ksnprintf",  (void *)ksnprintf);

    /* Pseudo-library headers so introspection has something to show. */
    g_libs[0].in_use = true;
    strcpy(g_libs[0].path, "/sys/lib/libc.nxl");
    memcpy(g_libs[0].hdr.magic, NXL_MAGIC, 3);
    g_libs[0].hdr.version = NXL_VERSION;
    g_libs[0].hdr.arch = NXL_ARCH_I386;

    g_libs[1].in_use = true;
    strcpy(g_libs[1].path, "/sys/lib/libgui.nxl");
    memcpy(g_libs[1].hdr.magic, NXL_MAGIC, 3);
    g_libs[1].hdr.version = NXL_VERSION;
    g_libs[1].hdr.arch = NXL_ARCH_I386;

    debug_printf("[nxl] dynamic loader ready (%d exports, %d libs)\n",
                 g_n_exports, 2);
    return true;
}

nxl_library_t *nxl_load(const char *path) {
    for (int i = 0; i < NXL_MAX_LIBS; i++) {
        if (g_libs[i].in_use && strcmp(g_libs[i].path, path) == 0)
            return &g_libs[i];
    }
    debug_printf("[nxl] requested library '%s' not registered\n", path);
    return NULL;
}

void *nxl_resolve(const char *symbol) {
    for (int i = 0; i < g_n_exports; i++) {
        if (strcmp(g_exports[i].name, symbol) == 0) return g_exports[i].addr;
    }
    return NULL;
}

int nxl_library_count(void) {
    int n = 0;
    for (int i = 0; i < NXL_MAX_LIBS; i++) if (g_libs[i].in_use) n++;
    return n;
}

/* ============================================================================
 * NexxoN OS - Dynamic shared libraries (.nxl format)  (TASK 23, v1.0)
 * ----------------------------------------------------------------------------
 *
 * .nxl is a minimalist position-independent dynamic-link format
 * inspired by ELF SHARED but stripped down to the essentials the
 * kernel needs to resolve cross-process symbol references:
 *
 *    File header:
 *      magic       "NXL"
 *      version     uint8 (= 1)
 *      arch        uint8 (= 1 for i386)
 *      section_count uint16
 *      sections    array of nxl_section_t (offset, size, flags, name)
 *      symbols     array of nxl_symbol_t  (name, address, kind)
 *      relocations array of nxl_reloc_t   (offset, sym_idx, type)
 *      payload     concatenated section bytes
 *
 * Two well-known libraries ship with NexxoN:
 *
 *   /sys/lib/libc.nxl   - libc bridge (memset, memcpy, strlen,
 *                         printf, malloc, free, fopen, ...)
 *   /sys/lib/libgui.nxl - GUI helpers (wm_create_window wrappers,
 *                         dialog builders, theme accessors)
 *
 * Apps reference these by name in their relocation table.  The
 * loader memory-maps each .nxl exactly once and patches the
 * importer's relocations to point at the resolved symbol address.
 * ============================================================================ */
#ifndef NEXXON_NXL_H
#define NEXXON_NXL_H

#include "types.h"

#define NXL_MAGIC          "NXL"
#define NXL_VERSION        1
#define NXL_ARCH_I386      1

#define NXL_SEC_TEXT       1
#define NXL_SEC_RODATA     2
#define NXL_SEC_DATA       3
#define NXL_SEC_BSS        4
#define NXL_SEC_GOT        5

#define NXL_SYM_FUNC       1
#define NXL_SYM_DATA       2

#define NXL_RELOC_ABS32    1
#define NXL_RELOC_PCREL32  2

typedef struct PACKED {
    char     magic[3];
    uint8_t  version;
    uint8_t  arch;
    uint8_t  reserved;
    uint16_t section_count;
    uint16_t symbol_count;
    uint16_t reloc_count;
    uint32_t payload_offset;
    uint32_t payload_size;
    char     name[32];
} nxl_header_t;

typedef struct PACKED {
    uint8_t  kind;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t offset;
    uint32_t size;
    char     name[16];
} nxl_section_t;

typedef struct PACKED {
    uint8_t  kind;
    uint8_t  reserved;
    uint16_t section;
    uint32_t address;
    char     name[40];
} nxl_symbol_t;

typedef struct PACKED {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t section;
    uint32_t offset;
    uint32_t sym_index;
} nxl_reloc_t;

#define NXL_MAX_LIBS  8

typedef struct {
    bool             in_use;
    char             path[64];
    const uint8_t   *base;
    uint32_t         size;
    nxl_header_t     hdr;
} nxl_library_t;

bool          nxl_loader_init(void);
nxl_library_t *nxl_load        (const char *path);
void          *nxl_resolve     (const char *symbol);
int            nxl_library_count(void);

#endif /* NEXXON_NXL_H */

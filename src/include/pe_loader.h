/* ============================================================================
 * NexxoN OS - Minimal PE32/PE32+ loader (Wine path)
 * ----------------------------------------------------------------------------
 * Parses MZ/PE headers, maps section bytes into a caller-owned buffer or a
 * private user VA window.  Does not execute guest code yet; reports the
 * image entry point for Win32 stub milestones.
 * ============================================================================ */
#ifndef NEXXON_PE_LOADER_H
#define NEXXON_PE_LOADER_H

#include "types.h"
#include "vmm.h"

#define PE_IMAGE_MAX   (4u * 1024u * 1024u)
#define PE_DEFAULT_BASE 0x40000000ULL

typedef struct {
    bool     ok;
    uint16_t machine;       /* 0x014C=i386, 0x8664=amd64 */
    uint32_t entry_rva;
    uint64_t image_base;    /* preferred load address from optional hdr */
    uint32_t size_of_image;
    uint32_t section_count;
    uint32_t import_rva;
    uint32_t import_sz;
    char     err[64];
} pe_image_t;

/* Validate MZ + PE signature and fill metadata (no mapping). */
bool pe_parse(const uint8_t *data, uint32_t len, pe_image_t *out);

/* Copy section raw bytes into `dest` (flat image, size >= size_of_image). */
bool pe_load_flat(const uint8_t *data, uint32_t len,
                  uint8_t *dest, uint32_t dest_cap, pe_image_t *out);

/* Map PE sections into user VA starting at `base` inside `pd`. */
bool pe_map_user(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                 uint64_t base, uint64_t user_min, uint64_t user_max,
                 pe_image_t *out);

#endif /* NEXXON_PE_LOADER_H */

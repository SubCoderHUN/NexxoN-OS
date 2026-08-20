/* ============================================================================
 * NexxoN OS - Minimal x86_64 ELF64 loader (static + dynamic musl)
 * ----------------------------------------------------------------------------
 * Loads x86_64 Linux ELF images into a private user page table.  Static PIE,
 * fixed ET_EXEC and PT_INTERP / shared-object (musl ld.so) images are
 * supported.  Dynamic relocations are left to the guest runtime / ld.so.
 * ============================================================================ */
#ifndef NEXXON_ELF64_H
#define NEXXON_ELF64_H

#include "types.h"
#include "vmm.h"

#define ELF64_INTERP_MAX 128

typedef struct {
    bool     ok;
    uint64_t entry;        /* absolute entry point to jump to (CPL3)          */
    uint64_t base;         /* load base actually used                         */
    uint64_t load_lo;      /* first byte covered by a PT_LOAD                 */
    uint64_t load_hi;      /* byte after the final PT_LOAD                    */
    uint64_t brk;          /* first free byte after the highest LOAD segment  */
    uint64_t phdr;         /* in-memory program-header table, or zero          */
    uint16_t phent;        /* ELF program-header entry size                    */
    uint16_t phnum;        /* ELF program-header count                         */
    bool     is_pie;       /* ET_DYN (relocated at base) vs ET_EXEC           */
    char     err[64];      /* human-readable failure reason                   */
} elf64_image_t;

typedef struct {
    elf64_image_t exec;
    bool          has_interp;
    char          interp_path[ELF64_INTERP_MAX];
} elf64_mapped_t;

/* Parse + load `data` into a flat region (legacy helper). */
void elf64_load(const uint8_t *data, uint32_t len,
                uint8_t *region, uint32_t region_sz, elf64_image_t *out);

/* Map a static or dynamic PIE / ET_EXEC image into a private page table.
 * When PT_INTERP is present the segment mapping still completes and the
 * interpreter path is returned in `out->interp_path`. */
void elf64_map_exec(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                    uint64_t pie_base, uint64_t user_min, uint64_t user_max,
                    elf64_mapped_t *out);

/* Map an ET_DYN shared object or musl interpreter at `load_base`. */
void elf64_map_so(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                  uint64_t load_base, uint64_t user_min, uint64_t user_max,
                  elf64_image_t *out);

/* Back-compat wrapper used by older call sites. */
void elf64_map_load(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                    uint64_t pie_base, uint64_t user_min, uint64_t user_max,
                    elf64_image_t *out);

#endif /* NEXXON_ELF64_H */

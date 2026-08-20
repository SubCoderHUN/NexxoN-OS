/* ============================================================================
 * NexxoN OS - Win32 API stub layer for the Wine/PE path
 * ----------------------------------------------------------------------------
 * Maps minimal kernel32 stubs into user VA and binds PE import tables.
 * PE guest code exits via NexxoN-private syscall 460 from the stubs.
 * ============================================================================ */
#ifndef NEXXON_PE_WIN32_H
#define NEXXON_PE_WIN32_H

#include "types.h"
#include "vmm.h"
#include "pe_loader.h"

#define PE_WIN32_BASE       0x18000000ULL
#define PE_WIN32_LIMIT      0x18100000ULL
#define PE_WIN32_STUB_EXIT  0x80u
#define PE_STACK_TOP        0x3FF00000ULL
#define PE_STACK_BYTES      (256u * 1024u)

/* Map ExitProcess stub into an already-mapped PE page; return its VA. */
uint64_t pe_win32_map_stubs(vmm_pd_t *pd, uint64_t image_base,
                            uint32_t size_of_image);

/* Patch PE import directory entries to point at our stubs. */
bool pe_win32_bind_imports(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                           uint64_t image_base, uint64_t stub_base,
                           pe_image_t *img);

/* Write a minimal amd64 entry thunk that calls ExitProcess(0). */
bool pe_win32_inject_entry(vmm_pd_t *pd, uint64_t image_base,
                           uint32_t entry_rva, uint64_t exit_stub);

/* Mark PE image pages executable and prepare a user stack. */
bool pe_win32_prepare_run(vmm_pd_t *pd, uint64_t image_base,
                          uint32_t size_of_image, uint32_t entry_rva,
                          uint64_t *entry_out, uint64_t *rsp_out);

#endif /* NEXXON_PE_WIN32_H */

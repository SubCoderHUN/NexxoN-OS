/* ============================================================================
 * NexxoN OS - Modern exploit mitigations  (TASK 35, v1.0)
 * ----------------------------------------------------------------------------
 *
 *   * NX Bit (No-Execute)
 *     Reads CPUID(0x80000001).EDX bit 20 to check XD support.  If
 *     present, sets EFER.NXE (MSR 0xC0000080 bit 11) so PTE bit 63
 *     becomes the no-execute marker.  The kernel's identity-map will
 *     have PTE.NX = 1 for every data range (stack + heap + bss),
 *     leaving only the .text + .rodata sections executable.
 *
 *   * Address Space Layout Randomisation (ASLR)
 *     aslr_offset() returns a per-boot random offset that the ELF
 *     loader uses when relocating user-space images.  The offset is
 *     16-byte aligned and bounded so it never overflows the userland
 *     code segment.
 *
 *   * Stack canaries (StackGuard-style)
 *     aslr_random_canary() emits a 32-bit random value for stack
 *     cookies.  Not wired into the compiler-emitted stack-protector
 *     yet because the kernel currently builds with
 *     -fno-stack-protector to avoid the libc dependency.
 * ============================================================================ */
#ifndef NEXXON_ASLR_H
#define NEXXON_ASLR_H

#include "types.h"

bool      aslr_init       (void);
bool      aslr_nx_enabled (void);
uint32_t  aslr_offset     (void);
uint32_t  aslr_random     (void);
uint32_t  aslr_random_canary(void);

#endif /* NEXXON_ASLR_H */

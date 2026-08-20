/* ============================================================================
 * NexxoN OS - Global Descriptor Table  (v2.0 - Ring 3 ready)
 * ----------------------------------------------------------------------------
 * Replaces GRUB's GDT with the table used by NexxoN.  Entries:
 *
 *   0x00  Null                  (mandatory)
 *   0x08  Kernel Code  DPL=0    flat 4 GiB, exec/read
 *   0x10  Kernel Data  DPL=0    flat 4 GiB, read/write
 *   0x18  User Code    DPL=3    flat 4 GiB, exec/read
 *   0x20  User Data    DPL=3    flat 4 GiB, read/write
 *   0x28  TSS                   one 32-bit TSS used during ring transitions
 *
 * The user-mode selectors carry DPL=3 in their access byte and the RPL
 * bits in the user segment registers must match (`0x18 | 3`, `0x20 | 3`).
 *
 * Defines exposed to the rest of the kernel so far/jmp targets in the
 * privilege-drop path and the int 0x80 stack-switch path can use them. */
#ifndef NEXXON_GDT_H
#define NEXXON_GDT_H

#include "types.h"

#define GDT_NULL_SEL        0x00
#define GDT_KCODE_SEL       0x08
#define GDT_KDATA_SEL       0x10
#define GDT_UCODE_SEL       0x18
#define GDT_UDATA_SEL       0x20
#define GDT_TSS_SEL         0x28
#define GDT_UCODE32_SEL     0x38

/* User selectors with RPL=3 baked in (what the user-mode code sees in CS/DS). */
#define GDT_UCODE_USER      (GDT_UCODE_SEL | 0x03)
#define GDT_UDATA_USER      (GDT_UDATA_SEL | 0x03)
#define GDT_UCODE32_USER    (GDT_UCODE32_SEL | 0x03)

void gdt_init(void);

/* Update the ring-0 stack pointer stored in the TSS.  Called any time we
 * are about to enter user mode AND whenever the kernel stack base moves.
 * uintptr_t: 32-bit ESP0 under -m32, 64-bit RSP0 under -m64 (ABI-identical
 * in 32-bit since uintptr_t == uint32_t there). */
void gdt_set_kernel_stack(uintptr_t esp0);

/* Install/update the IA-32 compatibility TLS data descriptor (GDT slot 8). */
void gdt_set_compat_tls(uint32_t base, uint32_t limit, bool page_granular);

#endif /* NEXXON_GDT_H */

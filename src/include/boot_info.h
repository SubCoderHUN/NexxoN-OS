/* ============================================================================
 * NexxoN OS - Boot-time information cache
 * ----------------------------------------------------------------------------
 * Snap-reads the Multiboot v1 info struct GRUB hands us at kernel_main()
 * entry and stashes the fields installsys needs later (BIOS boot drive,
 * the kernel-payload module bytes).  Decouples installsys from the
 * multiboot.h struct so the installer logic stays readable.
 *
 * The kernel payload itself is the same ELF that GRUB just loaded as the
 * kernel - we ship a second copy in the ISO as a multiboot module so
 * installsys can write it to LBA 64+ of the SATA disk without having to
 * read its own memory image (which would require parsing the kernel's
 * own ELF program headers at runtime).
 * ============================================================================ */
#ifndef NEXXON_BOOT_INFO_H
#define NEXXON_BOOT_INFO_H

#include "types.h"
#include "multiboot.h"

/* Capture the relevant fields from mbi.  Safe to call once at boot. */
void           boot_info_init(uint32_t magic, multiboot_info_t *mbi);

/* Original BIOS drive number (top byte of mbi->boot_device when valid).
 * 0xE0..0xEF = CD, 0x80..0xFF = HDD, 0x00..0x7F = floppy/removable.
 * Returns 0 if MB_FLAG_BOOTDEV wasn't set. */
uint8_t        boot_info_bios_drive(void);

/* Pointer + size of the kernel-payload module GRUB loaded for us.
 * Returns NULL if the module is missing - installsys then refuses
 * to write a bootable HDD because it has no kernel to write. */
const uint8_t *boot_info_kernel_payload(uint32_t *size_out);

/* The two raw 16-bit boot binaries embedded into the kernel via
 * objcopy.  These are the bytes that get written to LBA 0 and LBA 1..N
 * during installsys. */
const uint8_t *boot_info_stage1(uint32_t *size_out);
const uint8_t *boot_info_stage2(uint32_t *size_out);

/* System physical-memory snapshot captured from the multiboot info at
 * boot time.  Returns total RAM in KiB (mem_lower + mem_upper), or 0
 * if the loader didn't populate MB_FLAG_MEM.  Stable for the lifetime
 * of the boot. */
uint32_t       boot_info_mem_total_kib(void);

/* Size of the linked kernel image (text + rodata + data + bss) in
 * KiB, derived from the linker-script symbols.  Doesn't change at
 * runtime; safe to call from any context. */
uint32_t       boot_info_kernel_image_kib(void);

/* The kernel command line GRUB passed us (empty string if none). */
const char    *boot_info_cmdline(void);

/* True if `token` appears as a whitespace-delimited word on the kernel command
 * line.  Used for boot toggles like `nousbnative` and `bootpause`. */
bool           boot_info_cmdline_has(const char *token);

#endif /* NEXXON_BOOT_INFO_H */

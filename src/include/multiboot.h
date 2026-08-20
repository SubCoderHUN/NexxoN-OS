/* ============================================================================
 * NexxoN OS - Multiboot v1 info structure
 * ----------------------------------------------------------------------------
 * Only the fields we actually consume are declared.  The full struct is
 * 116 bytes long; we declare the prefix that matters and let everything past
 * `color_info` fall off the end.
 *
 * Bit positions in `flags` (see Multiboot v1 spec section 3.3):
 *   bit 0  - mem_lower / mem_upper valid
 *   bit 1  - boot_device valid
 *   bit 2  - cmdline valid
 *   bit 3  - mods_* valid
 *   bit 4  - aout symtab valid
 *   bit 5  - elf section header table valid
 *   bit 6  - mmap_* valid
 *   bit 12 - framebuffer_* valid
 * ============================================================================ */
#ifndef NEXXON_MULTIBOOT_H
#define NEXXON_MULTIBOOT_H

#include "types.h"

#define MULTIBOOT_BOOTLOADER_MAGIC      0x2BADB002

#define MB_FLAG_MEM         (1u << 0)
#define MB_FLAG_BOOTDEV     (1u << 1)
#define MB_FLAG_CMDLINE     (1u << 2)
#define MB_FLAG_MODS        (1u << 3)
#define MB_FLAG_AOUT        (1u << 4)
#define MB_FLAG_ELF         (1u << 5)
#define MB_FLAG_MMAP        (1u << 6)
#define MB_FLAG_FRAMEBUFFER (1u << 12)

typedef struct PACKED multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} multiboot_info_t;

/* Single mmap entry produced by GRUB when MB_FLAG_MMAP is asserted. */
typedef struct PACKED multiboot_mmap_entry {
    uint32_t size;                  /* size of this entry NOT counting 'size' */
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;                  /* 1 = usable RAM, others = reserved */
} multiboot_mmap_entry_t;

#endif /* NEXXON_MULTIBOOT_H */

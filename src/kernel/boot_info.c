/* ============================================================================
 * NexxoN OS - Boot-time information cache
 * ============================================================================ */
#include "boot_info.h"
#include "debug.h"

/* The objcopy-generated symbols for the two embedded boot binaries.  objcopy
 * names them after the input file PATH with '/' and '.' rewritten to '_'.
 * The Makefile invokes objcopy with the full "build/boot/stageN.bin" path,
 * so the symbols come out as "_binary_build_boot_stageN_bin_{start,end}". */
extern const uint8_t _binary_build_boot_stage1_bin_start[];
extern const uint8_t _binary_build_boot_stage1_bin_end[];
extern const uint8_t _binary_build_boot_stage2_bin_start[];
extern const uint8_t _binary_build_boot_stage2_bin_end[];

/* Linker-script anchors that bracket the loaded kernel image.  Used
 * to compute the kernel's fixed RAM footprint for taskmgr. */
extern const uint8_t _kernel_start[];
extern const uint8_t _kernel_end[];

/* Multiboot v1 module list entry (spec 3.3, see also include/multiboot.h). */
typedef struct PACKED multiboot_module {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} multiboot_module_t;

static uint8_t       g_bios_drive    = 0;
static const uint8_t *g_payload_ptr  = NULL;
static uint32_t      g_payload_size  = 0;
static uint32_t      g_mem_total_kib = 0;
static bool          g_initialised   = false;
static char          g_cmdline[256]  = { 0 };   /* kernel command line (GRUB) */

void boot_info_init(uint32_t magic, multiboot_info_t *mbi) {
    g_initialised = true;
    g_bios_drive  = 0;
    g_payload_ptr = NULL;
    g_payload_size = 0;

    /* Tolerate the case where the bootloader (a future custom stage-2)
     * passes us a custom magic.  We only honour 0x2BADB002 - GRUB / our
     * stage-2 both promise this value. */
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC || !mbi) {
        debug_printf("[boot_info] no usable mbi (magic=0x%x)\n", magic);
        return;
    }

    if (mbi->flags & MB_FLAG_BOOTDEV) {
        g_bios_drive = (uint8_t)((mbi->boot_device >> 24) & 0xFF);
    }

    /* Kernel command line (MB_FLAG_CMDLINE).  GRUB hands us the full
     * "multiboot <path> <args...>" string here; we snapshot it into a private
     * buffer so boot-time toggles (e.g. `nousbnative`, `bootpause`) survive
     * once paging relocates the low-memory mbi.  Paging is still OFF at this
     * point, so the physical cmdline pointer is directly dereferenceable. */
    g_cmdline[0] = 0;
    if ((mbi->flags & MB_FLAG_CMDLINE) && mbi->cmdline) {
        const char *src = (const char *)(uintptr_t)mbi->cmdline;
        uint32_t i = 0;
        while (src[i] && i < sizeof(g_cmdline) - 1) { g_cmdline[i] = src[i]; i++; }
        g_cmdline[i] = 0;
        debug_printf("[boot_info] cmdline = \"%s\"\n", g_cmdline);
    }

    /* MB_FLAG_MEM (bit 0): mem_lower + mem_upper are valid, both in
     * KiB.  mem_lower is the conventional <640 K region; mem_upper is
     * the contiguous extended-memory block above 1 MiB.  Their sum
     * understates real RAM by the 384 KiB BIOS hole but it's the
     * cheapest portable measurement we can take without walking the
     * full mmap. */
    if (mbi->flags & 0x1u) {
        g_mem_total_kib = mbi->mem_lower + mbi->mem_upper;
        debug_printf("[boot_info] mem_lower=%u KiB  mem_upper=%u KiB  "
                     "total=%u KiB (%u MiB)\n",
                     mbi->mem_lower, mbi->mem_upper,
                     g_mem_total_kib, g_mem_total_kib / 1024);
    }

    if (mbi->flags & MB_FLAG_MODS) {
        uint32_t n = mbi->mods_count;
        const multiboot_module_t *mods =
            (const multiboot_module_t *)(uintptr_t)mbi->mods_addr;
        debug_printf("[boot_info] %u multiboot module(s) at %p\n",
                     n, (void *)mods);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t start = mods[i].mod_start;
            uint32_t end   = mods[i].mod_end;
            debug_printf("[boot_info]   mod[%u]: %p .. %p (%u bytes)\n",
                         i, (void *)(uintptr_t)start,
                         (void *)(uintptr_t)end,
                         end - start);
            /* The first module is the kernel-payload module - grub.cfg
             * passes it that way.  Future modules might be NXFS images
             * or scripts; installsys ignores those. */
            if (i == 0 && end > start) {
                g_payload_ptr  = (const uint8_t *)(uintptr_t)start;
                g_payload_size = end - start;
            }
        }
    }
    debug_printf("[boot_info] bios_drive=0x%02x payload=%p size=%u\n",
                 g_bios_drive, (void *)g_payload_ptr, g_payload_size);
}

uint8_t boot_info_bios_drive(void) { return g_bios_drive; }

const uint8_t *boot_info_kernel_payload(uint32_t *size_out) {
    if (size_out) *size_out = g_payload_size;
    return g_payload_ptr;
}

const uint8_t *boot_info_stage1(uint32_t *size_out) {
    if (size_out)
        *size_out = (uint32_t)(_binary_build_boot_stage1_bin_end
                             - _binary_build_boot_stage1_bin_start);
    return _binary_build_boot_stage1_bin_start;
}

const uint8_t *boot_info_stage2(uint32_t *size_out) {
    if (size_out)
        *size_out = (uint32_t)(_binary_build_boot_stage2_bin_end
                             - _binary_build_boot_stage2_bin_start);
    return _binary_build_boot_stage2_bin_start;
}

uint32_t boot_info_mem_total_kib(void) {
    return g_mem_total_kib;
}

uint32_t boot_info_kernel_image_kib(void) {
    uintptr_t s = (uintptr_t)_kernel_start;
    uintptr_t e = (uintptr_t)_kernel_end;
    return (e > s) ? (uint32_t)((e - s) / 1024u) : 0u;
}

const char *boot_info_cmdline(void) { return g_cmdline; }

/* True if `token` appears as a whitespace-delimited word in the kernel command
 * line.  Word-boundary matching (not a raw substring) so "nousb" can't match
 * "nousbnative" and vice-versa. */
bool boot_info_cmdline_has(const char *token) {
    if (!token || !token[0]) return false;
    uint32_t tlen = 0;
    while (token[tlen]) tlen++;
    const char *s = g_cmdline;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;        /* skip leading space */
        const char *w = s;
        while (*s && *s != ' ' && *s != '\t') s++;  /* span one word      */
        uint32_t wlen = (uint32_t)(s - w);
        if (wlen == tlen) {
            uint32_t k = 0;
            while (k < tlen && w[k] == token[k]) k++;
            if (k == tlen) return true;
        }
    }
    return false;
}

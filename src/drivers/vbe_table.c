/* ============================================================================
 * NexxoN OS - VBE mode table reader
 * ============================================================================ */
#include "vbe_table.h"
#include "vga.h"
#include "debug.h"
#include "string.h"
#include "pit.h"

static vbe_mode_table_t g_merged;
static bool g_merged_built = false;

/* The standard 32-bpp resolutions we always want to offer in the Settings
 * Display list.  On QEMU/VirtualBox the BGA path switches to any of these
 * directly; on bare metal the VBE trampoline tries the matching mode number
 * and the rollback timer reverts if the BIOS rejects it. */
static const struct { uint16_t w, h, mode; } g_common[] = {
    {  800,  600, 0x0143 },
    { 1024,  768, 0x0118 },
    { 1152,  864, 0x014D },
    { 1280,  720, 0x0155 },
    { 1280,  800, 0x0157 },
    { 1280, 1024, 0x011A },
    { 1366,  768, 0x015C },
    { 1440,  900, 0x0161 },
    { 1600,  900, 0x0166 },
    { 1680, 1050, 0x016B },
    { 1920, 1080, 0x016E },
};

static void add_mode(uint16_t w, uint16_t h, uint16_t mode, uint32_t phys) {
    if (g_merged.count >= VBE_TABLE_MAX_MODES) return;
    for (int j = 0; j < g_merged.count; j++)
        if (g_merged.modes[j].width == w && g_merged.modes[j].height == h) return;
    vbe_mode_entry_t *e = &g_merged.modes[g_merged.count++];
    e->mode_number = mode;
    e->width = w; e->height = h;
    e->pitch = (uint32_t)w * 4;
    e->phys_base = phys;
    e->bpp = 32;
}

/* Build (once) a UNION of the firmware-provided modes and the standard
 * common modes.  Previously, if stage2 populated the hardware table with
 * only a couple of modes (the installed-disk case), the Settings list
 * showed just those two, while the Live CD fell through to the full
 * synthetic list -- so the two environments disagreed (problem #1).
 * Merging gives both the same full menu, up to 1920x1080. */
/* OP=2 trampoline: fills a low-memory buffer with every BIOS-supported
 * WxH x 32bpp mode.  These addresses MUST match vbe_tramp.asm (COMM_COUNT,
 * ENUM_BUF). */
extern int vbe_tramp_enum(void);
#define VBE_ENUM_COUNT_ADDR  0x3016u
#define VBE_ENUM_BUF_ADDR    0x4400u

/* Bare metal: ask the firmware which 32bpp modes it actually has, so the
 * Settings list offers ONLY resolutions that will switch (no more entries that
 * fail or shear).  Returns the number added. */
static int add_firmware_enum(void) {
    (void)vbe_tramp_enum();
    /* The enum trampoline is a BIOS round-trip too (256 INT 10h calls) —
     * re-assert the PIT rate the same way vga_switch_mode does, or a
     * firmware that touches channel 0 skews every timeout from here on. */
    pit_reprogram();
    int cnt = *(volatile uint16_t *)(uintptr_t)VBE_ENUM_COUNT_ADDR;
    if (cnt < 0 || cnt > 64) return 0;
    const volatile uint16_t *buf = (const volatile uint16_t *)(uintptr_t)VBE_ENUM_BUF_ADDR;
    int added = 0;
    for (int i = 0; i < cnt; i++) {
        uint16_t w = buf[2 * i], h = buf[2 * i + 1];
        if (w >= 640 && h >= 480) { add_mode(w, h, 0, 0); added++; }
    }
    return added;
}

static void add_common(void) {
    int n = (int)(sizeof(g_common) / sizeof(g_common[0]));
    for (int i = 0; i < n; i++)
        add_mode(g_common[i].w, g_common[i].h, g_common[i].mode, 0);
}

static const vbe_mode_table_t *table(void) {
    if (!g_merged_built) {
        g_merged_built = true;
        memset(&g_merged, 0, sizeof(g_merged));

        /* Firmware modes captured by stage2 (always BIOS-supported). */
        const vbe_mode_table_t *hw =
            (const vbe_mode_table_t *)(uintptr_t)VBE_TABLE_PHYS;
        if (hw->count > 0 && hw->count <= VBE_TABLE_MAX_MODES) {
            for (int i = 0; i < hw->count; i++)
                add_mode(hw->modes[i].width, hw->modes[i].height,
                         hw->modes[i].mode_number, hw->modes[i].phys_base);
        }

        if (vga_bga_available()) {
            /* QEMU/Bochs/VBox: the BGA sets any resolution, so offer the full
             * standard list. */
            add_common();
            debug_printf("[vbe_table] %d modes (BGA: firmware + common)\n",
                         g_merged.count);
        } else {
            /* Bare metal: only offer what the BIOS reports as supported. */
            int e = add_firmware_enum();
            if (g_merged.count == 0) add_common();   /* enum empty -> last resort */
            debug_printf("[vbe_table] %d modes (firmware enum=%d)\n",
                         g_merged.count, e);
        }
    }
    return &g_merged;
}

int vbe_table_count(void) {
    int n = table()->count;
    if (n < 0 || n > VBE_TABLE_MAX_MODES) return 0;
    return n;
}

const vbe_mode_entry_t *vbe_table_get(int idx) {
    if (idx < 0 || idx >= vbe_table_count()) return NULL;
    return &table()->modes[idx];
}

const vbe_mode_entry_t *vbe_table_find(uint16_t w, uint16_t h) {
    int n = vbe_table_count();
    for (int i = 0; i < n; i++) {
        const vbe_mode_entry_t *e = &table()->modes[i];
        if (e->width == w && e->height == h && e->bpp == 32)
            return e;
    }
    return NULL;
}

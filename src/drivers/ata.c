/* ============================================================================
 * NexxoN OS - ATA / IDE PIO disk driver (LBA28)
 * ----------------------------------------------------------------------------
 * Bare-metal hardening: do not assume the writable system disk is wired as a
 * single fixed IDE target.  Compatibility/CSM firmware and many SATA chipsets
 * present internal disks through the legacy primary/secondary IDE channels, and
 * either the master or the slave position can contain the real HDD/SSD while
 * the bootable USB/CD emulation occupies another slot.  We therefore probe all
 * four classic targets and bind to the first ATA fixed disk that IDENTIFYs.
 *
 * Register cheat-sheet (offset from channel base):
 *   +0  data        (16-bit transfer port)
 *   +1  error / features
 *   +2  sector count
 *   +3  LBA low  (bits  0.. 7)
 *   +4  LBA mid  (bits  8..15)
 *   +5  LBA high (bits 16..23)
 *   +6  drive/head select        (bits 24..27 + drive bit)
 *   +7  status / command
 *  control = base + 0x206 (alt status, software reset)
 * ============================================================================ */
#include "ata.h"
#include "io.h"
#include "string.h"
#include "debug.h"

#define ATA_PRIMARY_BASE       0x1F0
#define ATA_PRIMARY_CTRL       0x3F6
#define ATA_SECONDARY_BASE     0x170
#define ATA_SECONDARY_CTRL     0x376

#define REG_DATA(c)    ((c)->base + 0)
#define REG_ERROR(c)   ((c)->base + 1)
#define REG_FEAT(c)    ((c)->base + 1)
#define REG_SECCNT(c)  ((c)->base + 2)
#define REG_LBA_LO(c)  ((c)->base + 3)
#define REG_LBA_MID(c) ((c)->base + 4)
#define REG_LBA_HI(c)  ((c)->base + 5)
#define REG_DRVHD(c)   ((c)->base + 6)
#define REG_STATUS(c)  ((c)->base + 7)
#define REG_CMD(c)     ((c)->base + 7)
#define REG_ALTSTAT(c) ((c)->ctrl)

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY    0xEC

#define ATA_POLL_TIMEOUT    1000000

typedef struct {
    uint16_t    base;
    uint16_t    ctrl;
    uint8_t     drive;      /* 0 = master, 1 = slave */
    const char *name;
} ata_target_t;

static const ata_target_t g_probe_targets[] = {
    { ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL,   0, "primary master"   },
    { ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL,   1, "primary slave"    },
    { ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, 0, "secondary master" },
    { ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, 1, "secondary slave"  },
};

static bool         g_present = false;
static uint32_t     g_sectors = 0;
static ata_target_t g_target  = { ATA_PRIMARY_BASE, ATA_PRIMARY_CTRL, 0, "primary master" };

/* ---------- Polling helpers ----------------------------------------------- */
static inline void ata_io_pause_on(const ata_target_t *t) {
    /* Reading the alt-status register four times produces the canonical
     * 400 ns delay required between certain register writes per ATA-7. */
    (void)inb(REG_ALTSTAT(t));
    (void)inb(REG_ALTSTAT(t));
    (void)inb(REG_ALTSTAT(t));
    (void)inb(REG_ALTSTAT(t));
}

static uint8_t ata_select_pattern(const ata_target_t *t, bool lba, uint32_t lba28) {
    uint8_t p = (uint8_t)(t->drive ? 0xB0 : 0xA0);
    if (lba) p = (uint8_t)((t->drive ? 0xF0 : 0xE0) | ((lba28 >> 24) & 0x0F));
    return p;
}

static int ata_wait_not_busy_on(const ata_target_t *t) {
    for (uint32_t i = 0; i < ATA_POLL_TIMEOUT; i++) {
        uint8_t s = inb(REG_STATUS(t));
        if (!(s & ATA_SR_BSY)) return ATA_OK;
    }
    return ATA_ERR_TIMEOUT;
}

static int ata_wait_drq_on(const ata_target_t *t) {
    for (uint32_t i = 0; i < ATA_POLL_TIMEOUT; i++) {
        uint8_t s = inb(REG_STATUS(t));
        if (s & ATA_SR_ERR) return ATA_ERR_DRIVE;
        if (s & ATA_SR_DF)  return ATA_ERR_DRIVE;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return ATA_OK;
    }
    return ATA_ERR_TIMEOUT;
}

/* ---------- Init / identify ----------------------------------------------- */
static bool ata_probe_target(const ata_target_t *t) {
    debug_printf("[ata] probing %s (base=0x%03x ctrl=0x%03x drive=%u)\n",
                 t->name, t->base, t->ctrl, t->drive);

    /* Disable IRQs from the device while we are polling.  Do not assert SRST:
     * on some BIOS compatibility-mode SATA controllers that can also reset the
     * boot USB/CD emulation and leave the bus floating until power-cycle. */
    outb(REG_ALTSTAT(t), 0x02);

    outb(REG_DRVHD(t), ata_select_pattern(t, false, 0));
    ata_io_pause_on(t);

    /* Empty legacy channels normally float as 0xFF.  Some southbridges return
     * 0x00 until a real target is selected; both are safe no-device results. */
    uint8_t idle = inb(REG_STATUS(t));
    if (idle == 0xFF || idle == 0x00) {
        debug_printf("[ata]   %s: no status response (0x%02x)\n", t->name, idle);
        return false;
    }
    if (ata_wait_not_busy_on(t) != ATA_OK) {
        debug_printf("[ata]   %s: BSY did not clear before IDENTIFY\n", t->name);
        return false;
    }

    /* IDENTIFY DEVICE: zero count + zero LBA, then command 0xEC. */
    outb(REG_SECCNT(t),  0);
    outb(REG_LBA_LO(t),  0);
    outb(REG_LBA_MID(t), 0);
    outb(REG_LBA_HI(t),  0);
    outb(REG_CMD(t), ATA_CMD_IDENTIFY);

    ata_io_pause_on(t);
    uint8_t st = inb(REG_STATUS(t));
    debug_printf("[ata]   %s: post-IDENTIFY status=0x%02x\n", t->name, st);
    if (st == 0x00 || st == 0xFF) return false;

    /* Spin until BSY clears or ERR/DRQ asserts. */
    for (uint32_t i = 0; i < ATA_POLL_TIMEOUT; i++) {
        st = inb(REG_STATUS(t));
        if (!(st & ATA_SR_BSY)) break;
    }
    if (st & ATA_SR_BSY) {
        debug_printf("[ata]   %s: IDENTIFY BSY-clear timeout\n", t->name);
        return false;
    }

    uint8_t lba_mid = inb(REG_LBA_MID(t));
    uint8_t lba_hi  = inb(REG_LBA_HI(t));
    debug_printf("[ata]   %s: sig status=0x%02x lba_mid=0x%02x lba_hi=0x%02x\n",
                 t->name, st, lba_mid, lba_hi);
    if (lba_mid != 0 || lba_hi != 0) {
        /* ATAPI/SATA packet signatures commonly show up here when the boot
         * USB/CD-ROM is behind BIOS IDE emulation.  Skip them; never bind the
         * writable system disk abstraction to removable optical media. */
        debug_printf("[ata]   %s: non-ATA signature, skipping\n", t->name);
        return false;
    }

    for (uint32_t i = 0; i < ATA_POLL_TIMEOUT; i++) {
        st = inb(REG_STATUS(t));
        if (st & (ATA_SR_ERR | ATA_SR_DRQ)) break;
    }
    if (st & ATA_SR_ERR) {
        debug_printf("[ata]   %s: IDENTIFY raised ERR\n", t->name);
        return false;
    }
    if (!(st & ATA_SR_DRQ)) {
        debug_printf("[ata]   %s: IDENTIFY DRQ timeout\n", t->name);
        return false;
    }

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(REG_DATA(t));

    g_sectors = ((uint32_t)id[61] << 16) | id[60];
    if (g_sectors == 0) g_sectors = 262144;
    g_target  = *t;
    g_present = true;

    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)((id[27 + i] >> 8) & 0xFF);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;

    debug_printf("[ OK ] ata: %s ready, %u sectors (%u MiB), model='%s'\n",
                 g_target.name, g_sectors, g_sectors / 2048, model);
    return true;
}

bool ata_init(void) {
    g_present = false;
    g_sectors = 0;
    debug_step("ata: scanning legacy IDE targets (primary/secondary, master/slave)");

    for (uint32_t i = 0; i < sizeof(g_probe_targets) / sizeof(g_probe_targets[0]); i++) {
        if (ata_probe_target(&g_probe_targets[i])) return true;
    }

    debug_fail("ata", "no usable legacy IDE/ATA disk on any target");
    return false;
}

bool     ata_present     (void) { return g_present; }
uint32_t ata_sector_count(void) { return g_sectors; }

/* ---------- Single-sector PIO transfers ----------------------------------- */
int ata_read_sector(uint32_t lba, void *buf) {
    const ata_target_t *t = &g_target;
    if (!g_present)              return ATA_ERR_DRIVE;
    if (lba >= g_sectors)        return ATA_ERR_BOUNDS;
    if (lba & 0xF0000000u)       return ATA_ERR_BOUNDS;   /* > LBA28 */

    outb(REG_DRVHD(t), ata_select_pattern(t, true, lba));
    ata_io_pause_on(t);
    outb(REG_FEAT(t),    0);
    outb(REG_SECCNT(t),  1);
    outb(REG_LBA_LO(t),  (uint8_t)(lba & 0xFF));
    outb(REG_LBA_MID(t), (uint8_t)((lba >> 8) & 0xFF));
    outb(REG_LBA_HI(t),  (uint8_t)((lba >> 16) & 0xFF));
    outb(REG_CMD(t), ATA_CMD_READ_PIO);

    if (ata_wait_drq_on(t) != ATA_OK) return ATA_ERR_TIMEOUT;

    uint16_t *p = (uint16_t *)buf;
    for (int i = 0; i < 256; i++) p[i] = inw(REG_DATA(t));
    return ATA_OK;
}

int ata_write_sector(uint32_t lba, const void *buf) {
    const ata_target_t *t = &g_target;
    if (!g_present)              return ATA_ERR_DRIVE;
    if (lba >= g_sectors)        return ATA_ERR_BOUNDS;
    if (lba & 0xF0000000u)       return ATA_ERR_BOUNDS;

    outb(REG_DRVHD(t), ata_select_pattern(t, true, lba));
    ata_io_pause_on(t);
    outb(REG_FEAT(t),    0);
    outb(REG_SECCNT(t),  1);
    outb(REG_LBA_LO(t),  (uint8_t)(lba & 0xFF));
    outb(REG_LBA_MID(t), (uint8_t)((lba >> 8) & 0xFF));
    outb(REG_LBA_HI(t),  (uint8_t)((lba >> 16) & 0xFF));
    outb(REG_CMD(t), ATA_CMD_WRITE_PIO);

    if (ata_wait_drq_on(t) != ATA_OK) return ATA_ERR_TIMEOUT;

    const uint16_t *p = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        outw(REG_DATA(t), p[i]);
        __asm__ volatile ("nop");
    }

    outb(REG_CMD(t), ATA_CMD_CACHE_FLUSH);
    if (ata_wait_not_busy_on(t) != ATA_OK) return ATA_ERR_TIMEOUT;
    return ATA_OK;
}

/* ---------- Multi-sector convenience wrappers ----------------------------- */
int ata_read_sectors(uint32_t lba, uint32_t n, void *buf) {
    uint8_t *b = (uint8_t *)buf;
    for (uint32_t i = 0; i < n; i++) {
        int r = ata_read_sector(lba + i, b + i * ATA_SECTOR_SIZE);
        if (r != ATA_OK) return r;
    }
    return ATA_OK;
}

int ata_write_sectors(uint32_t lba, uint32_t n, const void *buf) {
    const uint8_t *b = (const uint8_t *)buf;
    for (uint32_t i = 0; i < n; i++) {
        int r = ata_write_sector(lba + i, b + i * ATA_SECTOR_SIZE);
        if (r != ATA_OK) return r;
    }
    return ATA_OK;
}

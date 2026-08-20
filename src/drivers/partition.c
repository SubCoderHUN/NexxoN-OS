/* ============================================================================
 * NexxoN OS - MBR + GPT partition parser
 * ----------------------------------------------------------------------------
 * Walks LBA 0 (and optionally LBA 1..n on a GPT-formatted disk) to build
 * an in-memory table of partition_t entries.  The parser is completely
 * filesystem-agnostic - it doesn't peek at the partition contents.  That
 * job belongs to the FAT / NTFS drivers further up the stack.
 *
 * MBR layout (446 bytes of bootstrap, 4*16-byte partition entries,
 *             then 0x55 0xAA):
 *
 *   0x1BE  Partition entry 0 (16 bytes)
 *   0x1CE  Partition entry 1
 *   0x1DE  Partition entry 2
 *   0x1EE  Partition entry 3
 *
 * Each entry is:
 *   off 0  : boot flag (0x80 = bootable, 0x00 = not)
 *   off 1-3: CHS first sector (ignored by NexxoN)
 *   off 4  : type byte (0x06 = FAT16, 0x07 = NTFS, 0x0B/0x0C = FAT32, ...)
 *   off 5-7: CHS last sector (ignored)
 *   off 8-11: starting LBA (little-endian)
 *   off 12-15: sector count (little-endian)
 *
 * GPT layout:
 *
 *   LBA 0     - protective MBR (single 0xEE entry covering the whole disk)
 *   LBA 1     - GPT header (signature "EFI PART", partition entry LBA = 2)
 *   LBA 2..33 - 128 partition entries of 128 bytes each (max 32 sectors)
 *
 * Each GPT partition entry is 128 bytes; we read up to PART_MAX of them.
 * ============================================================================ */
#include "partition.h"
#include "string.h"
#include "debug.h"

typedef struct PACKED {
    uint8_t  boot;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_first;
    uint32_t sector_count;
} mbr_entry_t;

typedef struct PACKED {
    char     signature[8];      /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32_header;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t crc32_partition_array;
} gpt_header_t;

typedef struct PACKED {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];           /* UTF-16LE */
} gpt_entry_t;

const char *partition_kind_name(uint8_t type) {
    switch (type) {
        case PART_KIND_FAT12:        return "FAT12";
        case PART_KIND_FAT16:        return "FAT16";
        case PART_KIND_NTFS:         return "NTFS";
        case PART_KIND_FAT32:        return "FAT32";
        case PART_KIND_FAT32_LBA:    return "FAT32";
        case PART_KIND_LINUX:        return "ext";
        case PART_KIND_GPT_BASIC:    return "GPT";
        case PART_KIND_GPT_PARTITION:return "GPT-Basic";
        case PART_KIND_NONE:         return "(empty)";
        default:                     return "?";
    }
}

/* Convert a UTF-16LE GPT name into a 7-bit ASCII label, trimming trailing
 * nulls.  Non-ASCII codepoints render as '?'. */
static void gpt_utf16_to_ascii(const uint16_t *src, char *dst, int max) {
    int n = 0;
    for (int i = 0; i < 36 && n < max - 1; i++) {
        uint16_t c = src[i];
        if (c == 0) break;
        dst[n++] = (c < 128) ? (char)c : '?';
    }
    dst[n] = 0;
}

int partition_parse(part_blockdev_read_t rd, void *user,
                    partition_table_t *out) {
    if (!rd || !out) return 0;
    memset(out, 0, sizeof(*out));

    uint8_t mbr[512];
    if (rd(user, 0, mbr) != 0) return 0;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        debug_printf("[part] LBA 0 lacks MBR signature (0x%02x 0x%02x)\n",
                     mbr[510], mbr[511]);
        /* Treat as raw - single partition covering everything from LBA 0. */
        out->is_gpt    = false;
        out->count     = 1;
        out->entries[0].present      = true;
        out->entries[0].start_lba    = 0;
        out->entries[0].sector_count = 0;
        out->entries[0].type_byte    = PART_KIND_UNKNOWN;
        memcpy(out->entries[0].name, "RAW", 4);
        return 1;
    }

    const mbr_entry_t *e = (const mbr_entry_t *)(mbr + 0x1BE);

    /* GPT detection: any entry with type 0xEE means GPT. */
    bool gpt = false;
    for (int i = 0; i < 4; i++) {
        if (e[i].type == 0xEE) { gpt = true; break; }
    }
    if (gpt) {
        out->is_gpt = true;
        uint8_t hdr_sec[512];
        if (rd(user, 1, hdr_sec) != 0) return 0;
        const gpt_header_t *h = (const gpt_header_t *)hdr_sec;
        if (memcmp(h->signature, "EFI PART", 8) != 0) {
            debug_printf("[part] protective MBR but no GPT header at LBA 1\n");
            return 0;
        }
        uint32_t nent = h->num_partition_entries;
        if (nent > 128) nent = 128;
        uint32_t esize = h->partition_entry_size;
        if (esize < sizeof(gpt_entry_t)) esize = sizeof(gpt_entry_t);

        uint8_t parr_sec[512];
        uint32_t lba = (uint32_t)h->partition_entries_lba;
        int filled = 0;
        for (uint32_t i = 0; i < nent && filled < PART_MAX; i++) {
            uint32_t byte_off = i * esize;
            uint32_t s_lba = lba + byte_off / 512;
            if (byte_off % 512 == 0) {
                if (rd(user, s_lba, parr_sec) != 0) break;
            }
            const gpt_entry_t *ge =
                (const gpt_entry_t *)(parr_sec + (byte_off % 512));
            /* type_guid == all zero -> empty slot. */
            bool empty = true;
            for (int j = 0; j < 16; j++) {
                if (ge->type_guid[j] != 0) { empty = false; break; }
            }
            if (empty) continue;
            partition_t *p = &out->entries[filled++];
            p->present      = true;
            p->start_lba    = (uint32_t)ge->first_lba;
            p->sector_count = (uint32_t)(ge->last_lba - ge->first_lba + 1);
            p->bootable     = 0;
            p->type_byte    = PART_KIND_GPT_PARTITION;
            gpt_utf16_to_ascii(ge->name, p->name, sizeof(p->name));
        }
        out->count = filled;
        debug_printf("[part] GPT: %d valid partition entries\n", filled);
        return filled;
    }

    /* Classic MBR. */
    out->is_gpt = false;
    int filled = 0;
    for (int i = 0; i < 4 && filled < PART_MAX; i++) {
        if (e[i].type == 0 || e[i].sector_count == 0) continue;
        partition_t *p = &out->entries[filled++];
        p->present      = true;
        p->start_lba    = e[i].lba_first;
        p->sector_count = e[i].sector_count;
        p->bootable     = e[i].boot;
        p->type_byte    = e[i].type;
        const char *n = partition_kind_name(e[i].type);
        memcpy(p->name, n, strlen(n) + 1);
    }
    out->count = filled;
    debug_printf("[part] MBR: %d primary partitions\n", filled);
    return filled;
}

/* ============================================================================
 * NexxoN OS - MBR + GPT partition table parser
 * ----------------------------------------------------------------------------
 * Decouples the question "where on this block device does each filesystem
 * start?" from the question "how do I read that filesystem?".  Walks LBA 0
 * looking for an MBR signature (0x55AA at offset 510) and either reads the
 * four primary partition entries directly OR follows the protective MBR
 * out to LBA 1 to decode a GPT header + partition-entry array.
 *
 * The parser is filesystem-agnostic - it does not look at the partition
 * contents themselves.  The higher-level VFS code does that by trying each
 * known FS parser against the start LBA in turn.
 * ============================================================================ */
#ifndef NEXXON_PARTITION_H
#define NEXXON_PARTITION_H

#include "types.h"

#define PART_MAX  8

typedef int (*part_blockdev_read_t)(void *user, uint32_t lba, void *buf);

typedef enum {
    PART_KIND_NONE  = 0,
    PART_KIND_FAT12 = 0x01,
    PART_KIND_FAT16 = 0x06,
    PART_KIND_NTFS  = 0x07,
    PART_KIND_FAT32 = 0x0B,
    PART_KIND_FAT32_LBA = 0x0C,
    PART_KIND_LINUX = 0x83,
    PART_KIND_GPT_BASIC = 0xEE,   /* (GPT protective MBR marker) */
    PART_KIND_GPT_PARTITION = 0xFE,
    PART_KIND_UNKNOWN = 0xFF,
} part_kind_t;

typedef struct {
    bool     present;
    uint32_t start_lba;
    uint32_t sector_count;
    uint8_t  type_byte;          /* MBR  type byte (or 0xFE for GPT entry) */
    uint8_t  bootable;
    char     name[36];           /* GPT label / "FAT" / "NTFS" / ...       */
} partition_t;

typedef struct {
    bool         is_gpt;
    int          count;
    partition_t  entries[PART_MAX];
} partition_table_t;

/* Read LBA 0 (and LBA 1..end of GPT array if GPT) using the supplied block
 * device callback and fill `out`.  Returns the number of valid partitions
 * found (0 on a totally raw / unpartitioned medium - the volume is then
 * treated as a single partition starting at LBA 0). */
int  partition_parse(part_blockdev_read_t rd, void *user,
                     partition_table_t *out);

/* Translate a type byte to a human-readable name ("FAT32", "NTFS", ...). */
const char *partition_kind_name(uint8_t type);

#endif /* NEXXON_PARTITION_H */

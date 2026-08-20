/* ============================================================================
 * NexxoN OS - NXFS v2 transactional journal  (TASK 28, v1.0)
 * ----------------------------------------------------------------------------
 * A small write-ahead log appended to the NXFS partition at a fixed
 * offset.  Every mutating filesystem operation (create / delete /
 * rename / write) records its INTENT to the journal *before* touching
 * the live inode table.  On boot, journal_replay() walks the log and
 * either commits every outstanding entry (if its commit-marker is
 * present) or rolls it back (if not).
 *
 * Journal layout (sector-aligned, after the existing NXFS data area):
 *
 *     header (1 sector)  - magic, head_lba, tail_lba, version
 *     records  (LRU ring) - 32 entries x 256 bytes each
 *
 * Records:
 *     uint32_t  seq
 *     uint32_t  op      (CREATE / DELETE / RENAME / WRITE)
 *     uint32_t  inode
 *     uint32_t  parent
 *     uint32_t  data_lba
 *     char      name[NXFS_NAME_MAX]
 *     uint32_t  commit_marker  (== 0xC0DECAFE when finalised)
 * ============================================================================ */
#ifndef NEXXON_JOURNAL_H
#define NEXXON_JOURNAL_H

#include "types.h"

#define JOURNAL_MAGIC           0x4E58524Cu     /* "NXRL"             */
#define JOURNAL_COMMIT_MARKER   0xC0DECAFEu
#define JOURNAL_MAX_RECORDS     32

#define JOURNAL_OP_NOOP    0
#define JOURNAL_OP_CREATE  1
#define JOURNAL_OP_DELETE  2
#define JOURNAL_OP_RENAME  3
#define JOURNAL_OP_WRITE   4
#define JOURNAL_OP_MKDIR   5
#define JOURNAL_OP_RMDIR   6

bool journal_init    (void);
int  journal_begin   (uint32_t op, uint32_t inode, uint32_t parent);
int  journal_commit  (int handle);
int  journal_replay  (void);
int  journal_pending_count(void);

#endif /* NEXXON_JOURNAL_H */

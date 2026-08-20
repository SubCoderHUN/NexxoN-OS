/* ============================================================================
 * NexxoN OS - NXFS v3: journaled filesystem with unbounded file sizes
 * ----------------------------------------------------------------------------
 * See nxfs.h for the on-disk format.  Implementation notes:
 *
 *   * Every structure is instance-based (nxfs_t): the live RAMFS and the
 *     SATA disk are separate instances, which is what lets installsys
 *     FORMAT the disk at its real size and tree-copy the live content
 *     instead of raw-imaging the tiny RAMFS geometry onto a big disk
 *     (the v2 flaw that froze the installed FS at RAMFS dimensions).
 *
 *   * Metadata I/O goes through a write-ahead transaction (g_tx):
 *     meta_write() collects dirty sectors; tx_commit() writes them to
 *     the journal region (header + payloads + commit record, CRC32),
 *     then to their home locations, then retires the journal header.
 *     Mount replays a committed-but-unretired transaction (idempotent)
 *     and discards a torn one.  meta_read() serves from the open
 *     transaction first, then a small LRU sector cache, then disk.
 *
 *   * File DATA bypasses the journal and is written BEFORE the
 *     transaction that references it ("ordered" mode).  A crash can
 *     therefore leak allocated-but-unreferenced blocks - never corrupt
 *     structure.  nxfs_check(repair=true) rebuilds the bitmaps from the
 *     reachable tree and reclaims leaks.
 *
 *   * No 64-bit division anywhere (no libgcc): offsets use shifts
 *     (block size 4 KiB = << 12) and 32-bit arithmetic.
 * ============================================================================ */
#include "nxfs.h"
#include "ahci.h"
#include "sysdisk.h"
#include "string.h"
#include "debug.h"
#include "window.h"
#include "terminal.h"
#include "vga.h"
#include "pit.h"
#include "keyboard.h"
#include "boot_info.h"

/* ---------------------------------------------------------------------------
 *                          Backend abstraction
 * --------------------------------------------------------------------------- */
typedef struct {
    const char *name;
    int       (*read) (uint32_t lba, void *buf);
    int       (*write)(uint32_t lba, const void *buf);
    uint32_t    lba_offset;
    uint32_t    sector_count;
    bool        is_ramfs;
} nxfs_backend_t;

/* RAMFS live store: 32 MiB in BSS (multi-arch Linux/Steam runtime staging). */
#define NXFS_RAMFS_SECTORS  65536u
#define NXFS_RAMFS_BYTES    (NXFS_RAMFS_SECTORS * NXFS_SECTOR_SIZE)
ALIGNED(512) static uint8_t g_ramfs[NXFS_RAMFS_BYTES];

static int ramfs_read(uint32_t lba, void *buf) {
    if (lba >= NXFS_RAMFS_SECTORS) return NXFS_ERR_IO;
    memcpy(buf, g_ramfs + lba * NXFS_SECTOR_SIZE, NXFS_SECTOR_SIZE);
    return NXFS_OK;
}
static int ramfs_write(uint32_t lba, const void *buf) {
    if (lba >= NXFS_RAMFS_SECTORS) return NXFS_ERR_IO;
    memcpy(g_ramfs + lba * NXFS_SECTOR_SIZE, buf, NXFS_SECTOR_SIZE);
    return NXFS_OK;
}
static int sata_read (uint32_t lba, void *buf) {
    return (sysdisk_read_sector (lba, buf) == AHCI_OK) ? NXFS_OK : NXFS_ERR_IO;
}
static int sata_write(uint32_t lba, const void *buf) {
    return (sysdisk_write_sector(lba, buf) == AHCI_OK) ? NXFS_OK : NXFS_ERR_IO;
}

static nxfs_backend_t g_backend_sata = {
    .name = "SATA(AHCI)", .read = sata_read, .write = sata_write,
    .lba_offset = NXFS_PARTITION_OFFSET, .sector_count = 0, .is_ramfs = false,
};
static nxfs_backend_t g_backend_ramfs = {
    .name = "RAMFS(live)", .read = ramfs_read, .write = ramfs_write,
    .lba_offset = 0, .sector_count = NXFS_RAMFS_SECTORS, .is_ramfs = true,
};

/* ---------------------------------------------------------------------------
 *                          Filesystem instance
 * --------------------------------------------------------------------------- */
typedef struct {
    nxfs_backend_t    *be;
    nxfs_superblock_t  sb;
    bool               mounted;
    bool               sb_dirty;       /* free counters changed in this tx */
    uint32_t           balloc_hint;    /* next-free search start (block #) */
    uint32_t           ialloc_hint;
} nxfs_t;

static nxfs_t g_live;                  /* RAMFS instance                   */
static nxfs_t g_disk;                  /* SATA instance                    */
static nxfs_t *g_fs = NULL;            /* the ACTIVE (public API) instance */

static nxfs_mode_t g_mode = NXFS_MODE_NONE;
static uint32_t    g_cwd  = 0;

static int raw_read(nxfs_t *fs, uint32_t lba, void *buf) {
    if (!fs || !fs->be) return NXFS_ERR_IO;
    return fs->be->read(fs->be->lba_offset + lba, buf);
}
static int raw_write(nxfs_t *fs, uint32_t lba, const void *buf) {
    if (!fs || !fs->be) return NXFS_ERR_IO;
    return fs->be->write(fs->be->lba_offset + lba, buf);
}

/* ---------------------------------------------------------------------------
 *                       Metadata sector cache (LRU-ish)
 * --------------------------------------------------------------------------- */
#define CACHE_ENTRIES 128
typedef struct {
    nxfs_t  *fs;
    uint32_t lba;
    uint32_t stamp;
    bool     valid;
    uint8_t  data[NXFS_SECTOR_SIZE];
} cache_ent_t;
static cache_ent_t g_cache[CACHE_ENTRIES];
static uint32_t    g_cache_clock = 0;

static void cache_invalidate_fs(nxfs_t *fs) {
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (!fs || g_cache[i].fs == fs) g_cache[i].valid = false;
}
static cache_ent_t *cache_find(nxfs_t *fs, uint32_t lba) {
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (g_cache[i].valid && g_cache[i].fs == fs && g_cache[i].lba == lba)
            return &g_cache[i];
    return NULL;
}
static void cache_put(nxfs_t *fs, uint32_t lba, const void *data) {
    cache_ent_t *e = cache_find(fs, lba);
    if (!e) {
        e = &g_cache[0];
        for (int i = 0; i < CACHE_ENTRIES; i++) {
            if (!g_cache[i].valid) { e = &g_cache[i]; break; }
            if (g_cache[i].stamp < e->stamp) e = &g_cache[i];
        }
        e->fs = fs; e->lba = lba; e->valid = true;
    }
    e->stamp = ++g_cache_clock;
    memcpy(e->data, data, NXFS_SECTOR_SIZE);
}

/* ---------------------------------------------------------------------------
 *                  Write-ahead metadata transaction (journal)
 * --------------------------------------------------------------------------- */
#define TX_MAX        56               /* payload sectors per transaction  */
#define TX_SOFT       44               /* batch threshold for long ops     */
#define JRNL_HDR_MAGIC 0x4C4E524Au     /* 'JRNL'                           */
#define JRNL_CMT_MAGIC 0x544D4D43u     /* 'CMMT'                           */

typedef struct PACKED {
    uint32_t magic;
    uint32_t seq;
    uint32_t count;
    uint32_t crc;                      /* CRC32 over payload sectors       */
    uint32_t lbas[TX_MAX];
    uint8_t  pad[512 - 16 - 4*TX_MAX];
} jrnl_hdr_t;

typedef struct PACKED {
    uint32_t magic;
    uint32_t seq;
    uint32_t crc;
    uint8_t  pad[512 - 12];
} jrnl_cmt_t;

static struct {
    nxfs_t  *fs;
    bool     active;
    int      count;
    uint32_t lbas[TX_MAX];
    uint8_t  data[TX_MAX][NXFS_SECTOR_SIZE];
} g_tx;
static uint32_t g_tx_seq = 1;

static uint32_t crc32_buf(uint32_t crc, const uint8_t *p, uint32_t n) {
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int tx_begin(nxfs_t *fs) {
    if (g_tx.active) return NXFS_ERR_BUSY;
    g_tx.fs = fs;
    g_tx.active = true;
    g_tx.count = 0;
    return NXFS_OK;
}

static void tx_abort(void) {
    /* Discard pending sectors.  The cache was NOT polluted (we only
     * insert into it at commit time), so dropping the buffer suffices. */
    g_tx.active = false;
    g_tx.count = 0;
}

/* Read with transaction + cache overlay. */
static int meta_read(nxfs_t *fs, uint32_t lba, void *buf) {
    if (g_tx.active && g_tx.fs == fs) {
        for (int i = g_tx.count - 1; i >= 0; i--) {
            if (g_tx.lbas[i] == lba) {
                memcpy(buf, g_tx.data[i], NXFS_SECTOR_SIZE);
                return NXFS_OK;
            }
        }
    }
    cache_ent_t *e = cache_find(fs, lba);
    if (e) {
        e->stamp = ++g_cache_clock;
        memcpy(buf, e->data, NXFS_SECTOR_SIZE);
        return NXFS_OK;
    }
    int r = raw_read(fs, lba, buf);
    if (r == NXFS_OK) cache_put(fs, lba, buf);
    return r;
}

static int meta_write(nxfs_t *fs, uint32_t lba, const void *buf) {
    if (!g_tx.active || g_tx.fs != fs) return NXFS_ERR_IO;
    for (int i = 0; i < g_tx.count; i++) {
        if (g_tx.lbas[i] == lba) {
            memcpy(g_tx.data[i], buf, NXFS_SECTOR_SIZE);
            return NXFS_OK;
        }
    }
    if (g_tx.count >= TX_MAX) return NXFS_ERR_FULL;
    g_tx.lbas[g_tx.count] = lba;
    memcpy(g_tx.data[g_tx.count], buf, NXFS_SECTOR_SIZE);
    g_tx.count++;
    return NXFS_OK;
}

static int tx_commit(void) {
    if (!g_tx.active) return NXFS_OK;
    nxfs_t *fs = g_tx.fs;

    /* Fold the superblock (free counters) into the transaction. */
    if (fs->sb_dirty) {
        fs->sb_dirty = false;
        if (g_tx.count >= TX_MAX) { tx_abort(); return NXFS_ERR_FULL; }
        int r = meta_write(fs, NXFS_LBA_SUPERBLOCK, &fs->sb);
        if (r != NXFS_OK) { tx_abort(); return r; }
    }
    if (g_tx.count == 0) { g_tx.active = false; return NXFS_OK; }

    /* 1. Journal: header + payloads + commit record. */
    static jrnl_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = JRNL_HDR_MAGIC;
    hdr.seq   = g_tx_seq;
    hdr.count = (uint32_t)g_tx.count;
    uint32_t crc = 0;
    for (int i = 0; i < g_tx.count; i++) {
        hdr.lbas[i] = g_tx.lbas[i];
        crc = crc32_buf(crc, g_tx.data[i], NXFS_SECTOR_SIZE);
    }
    hdr.crc = crc;
    uint32_t j = fs->sb.jrnl_start;
    if (raw_write(fs, j, &hdr) != NXFS_OK) { tx_abort(); return NXFS_ERR_IO; }
    for (int i = 0; i < g_tx.count; i++) {
        if (raw_write(fs, j + 1 + (uint32_t)i, g_tx.data[i]) != NXFS_OK) {
            tx_abort(); return NXFS_ERR_IO;
        }
    }
    static jrnl_cmt_t cmt;
    memset(&cmt, 0, sizeof(cmt));
    cmt.magic = JRNL_CMT_MAGIC;
    cmt.seq   = g_tx_seq;
    cmt.crc   = crc;
    if (raw_write(fs, j + 1 + (uint32_t)g_tx.count, &cmt) != NXFS_OK) {
        tx_abort(); return NXFS_ERR_IO;
    }
    g_tx_seq++;

    /* 2. Checkpoint: write the sectors home + refresh the cache. */
    for (int i = 0; i < g_tx.count; i++) {
        if (raw_write(fs, g_tx.lbas[i], g_tx.data[i]) != NXFS_OK) {
            /* The journal holds the committed copy: replay-on-mount will
             * finish the checkpoint.  Report the error regardless. */
            g_tx.active = false;
            return NXFS_ERR_IO;
        }
        cache_put(fs, g_tx.lbas[i], g_tx.data[i]);
    }

    /* 3. Retire the journal entry. */
    memset(&hdr, 0, sizeof(hdr));
    raw_write(fs, j, &hdr);

    g_tx.active = false;
    g_tx.count = 0;
    return NXFS_OK;
}

/* Commit the running batch and immediately reopen - used by long
 * operations (streaming appends, tree frees) so each intermediate
 * state is a CONSISTENT shorter/smaller version of the final one. */
static int tx_checkpoint(nxfs_t *fs) {
    int r = tx_commit();
    if (r != NXFS_OK) return r;
    return tx_begin(fs);
}

/* Mount-time replay of a committed-but-unretired transaction. */
static int jrnl_replay(nxfs_t *fs) {
    static jrnl_hdr_t hdr;
    if (raw_read(fs, fs->sb.jrnl_start, &hdr) != NXFS_OK) return NXFS_ERR_IO;
    if (hdr.magic != JRNL_HDR_MAGIC) return NXFS_OK;       /* clean        */
    bool ok = (hdr.count >= 1 && hdr.count <= TX_MAX);
    static jrnl_cmt_t cmt;
    static uint8_t sec[NXFS_SECTOR_SIZE];
    if (ok) {
        if (raw_read(fs, fs->sb.jrnl_start + 1 + hdr.count, &cmt) != NXFS_OK)
            return NXFS_ERR_IO;
        ok = (cmt.magic == JRNL_CMT_MAGIC && cmt.seq == hdr.seq &&
              cmt.crc == hdr.crc);
    }
    if (ok) {
        uint32_t crc = 0;
        for (uint32_t i = 0; i < hdr.count && ok; i++) {
            if (raw_read(fs, fs->sb.jrnl_start + 1 + i, sec) != NXFS_OK)
                return NXFS_ERR_IO;
            crc = crc32_buf(crc, sec, NXFS_SECTOR_SIZE);
        }
        ok = (crc == hdr.crc);
    }
    if (ok) {
        debug_printf("[nxfs] journal: replaying committed tx seq=%u "
                     "(%u sectors)\n", hdr.seq, hdr.count);
        for (uint32_t i = 0; i < hdr.count; i++) {
            if (raw_read(fs, fs->sb.jrnl_start + 1 + i, sec) != NXFS_OK)
                return NXFS_ERR_IO;
            if (hdr.lbas[i] < fs->sb.total_sectors)
                raw_write(fs, hdr.lbas[i], sec);
        }
    } else {
        debug_printf("[nxfs] journal: discarding torn tx (seq=%u)\n", hdr.seq);
    }
    memset(sec, 0, sizeof(sec));
    raw_write(fs, fs->sb.jrnl_start, sec);
    return NXFS_OK;
}

/* ---------------------------------------------------------------------------
 *                     Bitmap allocators (sector-cached)
 * --------------------------------------------------------------------------- */
/* Find + claim a clear bit in the bitmap that starts at sector `bm_start`
 * and covers `total` items, beginning the search at *hint. */
static int bm_alloc(nxfs_t *fs, uint32_t bm_start, uint32_t total,
                    uint32_t *hint, uint32_t *out_idx) {
    uint8_t sec[NXFS_SECTOR_SIZE];
    uint32_t start = (*hint < total) ? *hint : 0;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t idx = pass ? 0 : start;
        uint32_t end = pass ? start : total;
        uint32_t cur_s = 0xFFFFFFFFu;
        while (idx < end) {
            uint32_t s = idx >> 12;            /* 4096 bits per sector     */
            if (s != cur_s) {
                if (meta_read(fs, bm_start + s, sec) != NXFS_OK)
                    return NXFS_ERR_IO;
                cur_s = s;
            }
            uint32_t bit  = idx & 4095u;
            uint32_t byte = bit >> 3;
            if (sec[byte] == 0xFF) {           /* skip full bytes          */
                idx = (idx & ~7u) + 8;
                continue;
            }
            if (!((sec[byte] >> (bit & 7)) & 1u)) {
                sec[byte] |= (uint8_t)(1u << (bit & 7));
                if (meta_write(fs, bm_start + s, sec) != NXFS_OK)
                    return NXFS_ERR_IO;
                *hint = idx + 1;
                *out_idx = idx;
                return NXFS_OK;
            }
            idx++;
        }
    }
    return NXFS_ERR_NOSPACE;
}

static int bm_clear(nxfs_t *fs, uint32_t bm_start, uint32_t idx) {
    uint8_t sec[NXFS_SECTOR_SIZE];
    uint32_t s = idx >> 12;
    if (meta_read(fs, bm_start + s, sec) != NXFS_OK) return NXFS_ERR_IO;
    sec[(idx & 4095u) >> 3] &= (uint8_t)~(1u << (idx & 7u));
    return meta_write(fs, bm_start + s, sec);
}

static int balloc(nxfs_t *fs, uint32_t *out_blk) {
    int r = bm_alloc(fs, fs->sb.bbm_start, fs->sb.nblocks,
                     &fs->balloc_hint, out_blk);
    if (r != NXFS_OK) return r;
    fs->sb.free_blocks--;
    fs->sb_dirty = true;
    return NXFS_OK;
}
static int bfree(nxfs_t *fs, uint32_t blk) {
    if (blk == 0 || blk >= fs->sb.nblocks) return NXFS_OK;  /* tolerate    */
    int r = bm_clear(fs, fs->sb.bbm_start, blk);
    if (r != NXFS_OK) return r;
    fs->sb.free_blocks++;
    fs->sb_dirty = true;
    return NXFS_OK;
}
static int ialloc(nxfs_t *fs, uint32_t *out_ino) {
    int r = bm_alloc(fs, fs->sb.ibm_start, fs->sb.inode_count,
                     &fs->ialloc_hint, out_ino);
    if (r != NXFS_OK) return r;
    fs->sb.free_inodes--;
    fs->sb_dirty = true;
    return NXFS_OK;
}
static int ifree(nxfs_t *fs, uint32_t ino) {
    int r = bm_clear(fs, fs->sb.ibm_start, ino);
    if (r != NXFS_OK) return r;
    fs->sb.free_inodes++;
    fs->sb_dirty = true;
    return NXFS_OK;
}

/* ---------------------------------------------------------------------------
 *                          Inode + data sector I/O
 * --------------------------------------------------------------------------- */
static uint32_t inode_lba(nxfs_t *fs, uint32_t ino) {
    return fs->sb.itab_start + ino;
}
static uint32_t blk_sector(nxfs_t *fs, uint32_t blk) {
    return fs->sb.data_start + blk * NXFS_BLOCK_SECTORS;
}

static int inode_read(nxfs_t *fs, uint32_t ino, nxfs_inode_t *out) {
    if (!fs->mounted || ino >= fs->sb.inode_count || !out)
        return NXFS_ERR_NOTFOUND;
    return meta_read(fs, inode_lba(fs, ino), out);
}
static int inode_write(nxfs_t *fs, uint32_t ino, const nxfs_inode_t *in) {
    if (!fs->mounted || ino >= fs->sb.inode_count || !in)
        return NXFS_ERR_NOTFOUND;
    return meta_write(fs, inode_lba(fs, ino), in);
}

/* ---------------------------------------------------------------------------
 *                      Block mapping (direct + 3 indirects)
 * --------------------------------------------------------------------------- */
#define FAN       NXFS_PTRS_PER_BLOCK            /* 1024                   */
#define L1_FIRST  ((uint32_t)NXFS_DIRECT)        /* 12                     */
#define L2_FIRST  (L1_FIRST + FAN)               /* 12 + 1K                */
#define L3_FIRST  (L2_FIRST + FAN*FAN)           /* 12 + 1K + 1M           */

/* Read/patch one 4-byte entry of an indirect block. */
static int ind_get(nxfs_t *fs, uint32_t blk, uint32_t idx, uint32_t *out) {
    uint8_t sec[NXFS_SECTOR_SIZE];
    uint32_t s = (idx * 4) >> 9, o = (idx * 4) & 511u;
    if (meta_read(fs, blk_sector(fs, blk) + s, sec) != NXFS_OK)
        return NXFS_ERR_IO;
    memcpy(out, sec + o, 4);
    return NXFS_OK;
}
static int ind_set(nxfs_t *fs, uint32_t blk, uint32_t idx, uint32_t val) {
    uint8_t sec[NXFS_SECTOR_SIZE];
    uint32_t s = (idx * 4) >> 9, o = (idx * 4) & 511u;
    if (meta_read(fs, blk_sector(fs, blk) + s, sec) != NXFS_OK)
        return NXFS_ERR_IO;
    memcpy(sec + o, &val, 4);
    return meta_write(fs, blk_sector(fs, blk) + s, sec);
}

/* Allocate a ZEROED indirect block (journaled zeroing - readers must
 * never see stale bytes as pointers). */
static int alloc_indirect(nxfs_t *fs, uint32_t *out_blk) {
    int r = balloc(fs, out_blk);
    if (r != NXFS_OK) return r;
    uint8_t zero[NXFS_SECTOR_SIZE];
    memset(zero, 0, sizeof(zero));
    uint32_t base = blk_sector(fs, *out_blk);
    for (uint32_t s = 0; s < NXFS_BLOCK_SECTORS; s++) {
        r = meta_write(fs, base + s, zero);
        if (r != NXFS_OK) return r;
    }
    return NXFS_OK;
}

/* Map file-block index -> disk block (0 = hole).  With `alloc`, missing
 * levels are created and `n` (the in-memory inode) is updated; the
 * caller is responsible for eventually meta_write-ing the inode. */
static int map_block(nxfs_t *fs, nxfs_inode_t *n, uint32_t idx,
                     bool alloc, uint32_t *out_blk) {
    int r;
    *out_blk = 0;
    if (idx < L1_FIRST) {
        if (n->direct[idx] == 0 && alloc) {
            r = balloc(fs, &n->direct[idx]);
            if (r != NXFS_OK) return r;
            n->block_count++;
        }
        *out_blk = n->direct[idx];
        return NXFS_OK;
    }
    uint32_t root, i1, i2, i3;
    if (idx < L2_FIRST) {
        if (n->ind1 == 0) {
            if (!alloc) return NXFS_OK;
            r = alloc_indirect(fs, &n->ind1);
            if (r != NXFS_OK) return r;
            n->block_count++;
        }
        i1 = idx - L1_FIRST;
        uint32_t v;
        r = ind_get(fs, n->ind1, i1, &v);
        if (r != NXFS_OK) return r;
        if (v == 0 && alloc) {
            r = balloc(fs, &v);
            if (r != NXFS_OK) return r;
            r = ind_set(fs, n->ind1, i1, v);
            if (r != NXFS_OK) return r;
            n->block_count++;
        }
        *out_blk = v;
        return NXFS_OK;
    }
    if (idx < L3_FIRST) {
        if (n->ind2 == 0) {
            if (!alloc) return NXFS_OK;
            r = alloc_indirect(fs, &n->ind2);
            if (r != NXFS_OK) return r;
            n->block_count++;
        }
        uint32_t rel = idx - L2_FIRST;
        i1 = rel >> 10;                        /* / 1024 */
        i2 = rel & (FAN - 1);
        r = ind_get(fs, n->ind2, i1, &root);
        if (r != NXFS_OK) return r;
        if (root == 0) {
            if (!alloc) return NXFS_OK;
            r = alloc_indirect(fs, &root);
            if (r != NXFS_OK) return r;
            r = ind_set(fs, n->ind2, i1, root);
            if (r != NXFS_OK) return r;
            n->block_count++;
        }
        uint32_t v;
        r = ind_get(fs, root, i2, &v);
        if (r != NXFS_OK) return r;
        if (v == 0 && alloc) {
            r = balloc(fs, &v);
            if (r != NXFS_OK) return r;
            r = ind_set(fs, root, i2, v);
            if (r != NXFS_OK) return r;
            n->block_count++;
        }
        *out_blk = v;
        return NXFS_OK;
    }
    /* Triple indirect. */
    if (n->ind3 == 0) {
        if (!alloc) return NXFS_OK;
        r = alloc_indirect(fs, &n->ind3);
        if (r != NXFS_OK) return r;
        n->block_count++;
    }
    uint32_t rel = idx - L3_FIRST;
    i1 = rel >> 20;                            /* / 1024^2 */
    i2 = (rel >> 10) & (FAN - 1);
    i3 = rel & (FAN - 1);
    r = ind_get(fs, n->ind3, i1, &root);
    if (r != NXFS_OK) return r;
    if (root == 0) {
        if (!alloc) return NXFS_OK;
        r = alloc_indirect(fs, &root);
        if (r != NXFS_OK) return r;
        r = ind_set(fs, n->ind3, i1, root);
        if (r != NXFS_OK) return r;
        n->block_count++;
    }
    uint32_t mid;
    r = ind_get(fs, root, i2, &mid);
    if (r != NXFS_OK) return r;
    if (mid == 0) {
        if (!alloc) return NXFS_OK;
        r = alloc_indirect(fs, &mid);
        if (r != NXFS_OK) return r;
        r = ind_set(fs, root, i2, mid);
        if (r != NXFS_OK) return r;
        n->block_count++;
    }
    uint32_t v;
    r = ind_get(fs, mid, i3, &v);
    if (r != NXFS_OK) return r;
    if (v == 0 && alloc) {
        r = balloc(fs, &v);
        if (r != NXFS_OK) return r;
        r = ind_set(fs, mid, i3, v);
        if (r != NXFS_OK) return r;
        n->block_count++;
    }
    *out_blk = v;
    return NXFS_OK;
}

/* ---------------------------------------------------------------------------
 *                     Tree free (truncate support)
 * --------------------------------------------------------------------------- */
/* Free an indirect tree of the given depth (1 = entries are data blocks).
 * Reads the indirect block sector-by-sector; commits the running
 * transaction in batches.  The owning inode was already cleared and
 * committed, so every batch boundary is consistent (worst case: leaked
 * blocks reclaimable by nxfs_check). */
static int free_tree(nxfs_t *fs, uint32_t blk, int depth) {
    if (blk == 0) return NXFS_OK;
    uint8_t sec[NXFS_SECTOR_SIZE];
    for (uint32_t s = 0; s < NXFS_BLOCK_SECTORS; s++) {
        if (meta_read(fs, blk_sector(fs, blk) + s, sec) != NXFS_OK)
            return NXFS_ERR_IO;
        uint32_t ents[128];
        memcpy(ents, sec, sizeof(ents));
        for (int i = 0; i < 128; i++) {
            if (ents[i] == 0) continue;
            if (depth > 1) {
                int r = free_tree(fs, ents[i], depth - 1);
                if (r != NXFS_OK) return r;
            } else {
                int r = bfree(fs, ents[i]);
                if (r != NXFS_OK) return r;
            }
            if (g_tx.count >= TX_SOFT) {
                int r = tx_checkpoint(fs);
                if (r != NXFS_OK) return r;
            }
        }
    }
    int r = bfree(fs, blk);
    if (r != NXFS_OK) return r;
    if (g_tx.count >= TX_SOFT) return tx_checkpoint(fs);
    return NXFS_OK;
}

/* Truncate a FILE inode to zero length.  Two phases:
 *   1. one small tx clears the inode's mapping (size 0, no pointers) -
 *      after this commit the file is consistently empty;
 *   2. the saved trees are freed in batched transactions (a crash mid-
 *      phase-2 leaks blocks; nxfs_check reclaims).
 * Caller must NOT hold an open transaction. */
static int file_truncate(nxfs_t *fs, uint32_t ino, nxfs_inode_t *n) {
    uint32_t d[NXFS_DIRECT], i1, i2, i3;
    memcpy(d, n->direct, sizeof(d));
    i1 = n->ind1; i2 = n->ind2; i3 = n->ind3;

    memset(n->direct, 0, sizeof(n->direct));
    n->ind1 = n->ind2 = n->ind3 = 0;
    n->size = 0; n->size_hi = 0;
    n->block_count = 0;

    int r = tx_begin(fs);
    if (r != NXFS_OK) return r;
    r = inode_write(fs, ino, n);
    if (r != NXFS_OK) { tx_abort(); return r; }
    r = tx_commit();
    if (r != NXFS_OK) return r;

    r = tx_begin(fs);
    if (r != NXFS_OK) return r;
    for (int i = 0; i < NXFS_DIRECT; i++) {
        if (!d[i]) continue;
        r = bfree(fs, d[i]);
        if (r != NXFS_OK) { tx_abort(); return r; }
        if (g_tx.count >= TX_SOFT) {
            r = tx_checkpoint(fs);
            if (r != NXFS_OK) return r;
        }
    }
    if ((r = free_tree(fs, i1, 1)) != NXFS_OK) { tx_abort(); return r; }
    if ((r = free_tree(fs, i2, 2)) != NXFS_OK) { tx_abort(); return r; }
    if ((r = free_tree(fs, i3, 3)) != NXFS_OK) { tx_abort(); return r; }
    return tx_commit();
}

/* ---------------------------------------------------------------------------
 *                          Read at offset
 * --------------------------------------------------------------------------- */
static int read_at(nxfs_t *fs, uint32_t ino, uint64_t off,
                   void *out, uint32_t len) {
    nxfs_inode_t n;
    int r = inode_read(fs, ino, &n);
    if (r != NXFS_OK) return r;
    if (n.type != NXFS_TYPE_FILE) return NXFS_ERR_NOTFILE;

    uint64_t fsize = ((uint64_t)n.size_hi << 32) | n.size;
    if (off >= fsize) return 0;
    uint64_t left64 = fsize - off;
    if ((uint64_t)len > left64) len = (uint32_t)left64;
    if (len == 0) return 0;

    uint8_t *dst = (uint8_t *)out;
    uint32_t copied = 0;
    uint8_t sec[NXFS_SECTOR_SIZE];
    while (copied < len) {
        uint64_t pos = off + copied;
        uint32_t bidx  = (uint32_t)(pos >> 12);          /* / 4096        */
        uint32_t inblk = (uint32_t)(pos & 4095u);
        uint32_t blk;
        r = map_block(fs, &n, bidx, false, &blk);
        if (r != NXFS_OK) return r;
        uint32_t chunk = NXFS_BLOCK_BYTES - inblk;
        if (chunk > len - copied) chunk = len - copied;
        if (blk == 0) {
            memset(dst + copied, 0, chunk);              /* hole          */
            copied += chunk;
            continue;
        }
        uint32_t s     = inblk >> 9;
        uint32_t soff  = inblk & 511u;
        uint32_t done  = 0;
        while (done < chunk) {
            if (raw_read(fs, blk_sector(fs, blk) + s, sec) != NXFS_OK)
                return NXFS_ERR_IO;
            uint32_t c = NXFS_SECTOR_SIZE - soff;
            if (c > chunk - done) c = chunk - done;
            memcpy(dst + copied + done, sec + soff, c);
            done += c;
            s++;
            soff = 0;
        }
        copied += chunk;
    }
    return (int)copied;
}

/* ---------------------------------------------------------------------------
 *                          Streaming write
 * --------------------------------------------------------------------------- */
static struct {
    bool         active;
    bool         suspended;   /* tx released so a metadata op can run   */
    nxfs_t      *fs;
    uint32_t     ino;
    nxfs_inode_t n;
    uint64_t     size;
} g_ws;

/* Release the streaming writer's journal transaction so a concurrent
 * metadata op (mkdir / create / rename) can obtain it.  Persists a
 * consistent intermediate length first; the next stream_append reopens a
 * fresh tx.  This is what lets an app hold a file open for writing (e.g. a
 * log) while still creating directories — the single-transaction journal
 * would otherwise return BUSY for every other write. */
static int stream_suspend(void) {
    if (!g_ws.active || g_ws.suspended) return NXFS_OK;
    if (!g_tx.active) { g_ws.suspended = true; return NXFS_OK; }
    g_ws.n.size    = (uint32_t)g_ws.size;
    g_ws.n.size_hi = (uint32_t)(g_ws.size >> 32);
    int r = inode_write(g_ws.fs, g_ws.ino, &g_ws.n);
    if (r != NXFS_OK) { tx_abort(); g_ws.active = false; return r; }
    r = tx_commit();
    if (r != NXFS_OK) { g_ws.active = false; return r; }
    g_ws.suspended = true;
    return NXFS_OK;
}

static int stream_resume(void) {
    if (!g_ws.active || !g_ws.suspended) return NXFS_OK;
    int r = tx_begin(g_ws.fs);
    if (r != NXFS_OK) return r;
    g_ws.suspended = false;
    return NXFS_OK;
}

static int stream_begin(nxfs_t *fs, uint32_t ino) {
    if (g_ws.active) return NXFS_ERR_BUSY;
    nxfs_inode_t n;
    int r = inode_read(fs, ino, &n);
    if (r != NXFS_OK) return r;
    if (n.type != NXFS_TYPE_FILE) return NXFS_ERR_NOTFILE;
    r = file_truncate(fs, ino, &n);
    if (r != NXFS_OK) return r;
    g_ws.active = true;
    g_ws.fs   = fs;
    g_ws.ino  = ino;
    g_ws.n    = n;
    g_ws.size = 0;
    return tx_begin(fs);
}

static int stream_fail(void) {
    tx_abort();
    g_ws.active = false;
    return NXFS_ERR_IO;
}

static int stream_append(const void *data, uint32_t len) {
    if (!g_ws.active) return NXFS_ERR_IO;
    if (g_ws.suspended) {
        int rr = stream_resume();
        if (rr != NXFS_OK) return rr;
    }
    nxfs_t *fs = g_ws.fs;
    const uint8_t *src = (const uint8_t *)data;
    uint8_t sec[NXFS_SECTOR_SIZE];
    uint32_t done = 0;
    int r;

    while (done < len) {
        uint64_t pos = g_ws.size;
        uint32_t bidx  = (uint32_t)(pos >> 12);
        uint32_t inblk = (uint32_t)(pos & 4095u);
        uint32_t blk;
        r = map_block(fs, &g_ws.n, bidx, true, &blk);
        if (r != NXFS_OK) { tx_abort(); g_ws.active = false; return r; }
        if (blk == 0)     { tx_abort(); g_ws.active = false; return NXFS_ERR_NOSPACE; }

        uint32_t chunk = NXFS_BLOCK_BYTES - inblk;
        if (chunk > len - done) chunk = len - done;

        /* Data goes straight to disk (ordered mode): full sectors as-is,
         * edge sectors read-modify-write. */
        uint32_t s    = inblk >> 9;
        uint32_t soff = inblk & 511u;
        uint32_t cdone = 0;
        while (cdone < chunk) {
            uint32_t lba = blk_sector(fs, blk) + s;
            uint32_t c = NXFS_SECTOR_SIZE - soff;
            if (c > chunk - cdone) c = chunk - cdone;
            if (c == NXFS_SECTOR_SIZE) {
                if (raw_write(fs, lba, src + done + cdone) != NXFS_OK)
                    return stream_fail();
            } else {
                if (soff == 0) memset(sec, 0, sizeof(sec));
                else if (raw_read(fs, lba, sec) != NXFS_OK)
                    return stream_fail();
                memcpy(sec + soff, src + done + cdone, c);
                if (raw_write(fs, lba, sec) != NXFS_OK)
                    return stream_fail();
            }
            cdone += c;
            s++;
            soff = 0;
        }

        g_ws.size += chunk;
        done      += chunk;

        /* Batch boundary: persist a consistent intermediate length. */
        if (g_tx.count >= TX_SOFT) {
            g_ws.n.size    = (uint32_t)g_ws.size;
            g_ws.n.size_hi = (uint32_t)(g_ws.size >> 32);
            r = inode_write(fs, g_ws.ino, &g_ws.n);
            if (r != NXFS_OK) { tx_abort(); g_ws.active = false; return r; }
            r = tx_checkpoint(fs);
            if (r != NXFS_OK) { g_ws.active = false; return r; }
        }
    }
    return NXFS_OK;
}

static int stream_end(bool commit) {
    if (!g_ws.active) return NXFS_ERR_IO;
    nxfs_t *fs = g_ws.fs;
    /* Reopen a tx if a concurrent metadata op left the stream suspended. */
    if (g_ws.suspended) {
        int rr = stream_resume();
        if (rr != NXFS_OK) { g_ws.active = false; return rr; }
    }
    g_ws.active = false;
    if (!commit) {
        /* Abort: drop the pending tx, then truncate whatever the last
         * checkpoint made visible. */
        tx_abort();
        nxfs_inode_t n;
        if (inode_read(fs, g_ws.ino, &n) != NXFS_OK) return NXFS_ERR_IO;
        return file_truncate(fs, g_ws.ino, &n);
    }
    g_ws.n.size    = (uint32_t)g_ws.size;
    g_ws.n.size_hi = (uint32_t)(g_ws.size >> 32);
    int r = inode_write(fs, g_ws.ino, &g_ws.n);
    if (r != NXFS_OK) { tx_abort(); return r; }
    return tx_commit();
}

/* ---------------------------------------------------------------------------
 *                          Format
 * --------------------------------------------------------------------------- */
static int fs_format(nxfs_t *fs, const char *label, uint32_t total_sectors) {
    if (!fs || !fs->be) return NXFS_ERR_IO;
    debug_printf("[nxfs] formatting %s: %u sectors\n",
                 fs->be->name, total_sectors);

    nxfs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic         = NXFS_MAGIC;
    sb.version       = NXFS_VERSION;
    sb.total_sectors = total_sectors;
    sb.block_sectors = NXFS_BLOCK_SECTORS;

    uint32_t ic = total_sectors / 128;
    if (ic < 1024)  ic = 1024;
    if (ic > 32768) ic = 32768;
    sb.inode_count  = ic;
    sb.jrnl_start   = 16;
    sb.jrnl_sectors = fs->be->is_ramfs ? 128 : 1024;
    sb.ibm_start    = sb.jrnl_start + sb.jrnl_sectors;
    sb.ibm_sectors  = (ic + 4095) / 4096;
    sb.bbm_start    = sb.ibm_start + sb.ibm_sectors;

    /* Solve bitmap size + data start (converges in 2-3 rounds). */
    uint32_t nb = 0, bbm = 1, data_start = 0;
    for (int it = 0; it < 4; it++) {
        data_start = sb.bbm_start + bbm + ic;
        data_start = (data_start + 7) & ~7u;             /* 8-align       */
        if (data_start >= total_sectors) return NXFS_ERR_NOSPACE;
        nb  = (total_sectors - data_start) / NXFS_BLOCK_SECTORS;
        bbm = (nb + 4095) / 4096;
    }
    sb.bbm_sectors = bbm;
    sb.itab_start  = sb.bbm_start + bbm;
    sb.data_start  = data_start;
    sb.nblocks     = nb;
    sb.free_blocks = nb - 1;             /* block 0 reserved (NULL)       */
    sb.free_inodes = ic - 1;             /* inode 0 = root                */
    sb.root_inode  = 0;
    strncpy((char *)sb.label, label ? label : "NEXXON-NXFS",
            sizeof(sb.label) - 1);

    uint8_t sec[NXFS_SECTOR_SIZE];
    memset(sec, 0, sizeof(sec));
    /* Journal header = clean. */
    if (raw_write(fs, sb.jrnl_start, sec) != NXFS_OK) return NXFS_ERR_IO;
    /* Inode bitmap: zero, then mark inode 0. */
    for (uint32_t s = 0; s < sb.ibm_sectors; s++)
        if (raw_write(fs, sb.ibm_start + s, sec) != NXFS_OK) return NXFS_ERR_IO;
    /* Block bitmap: zero, then mark block 0. */
    for (uint32_t s = 0; s < sb.bbm_sectors; s++) {
        if (raw_write(fs, sb.bbm_start + s, sec) != NXFS_OK) return NXFS_ERR_IO;
        if ((s & 0x3F) == 0) wm_tick();      /* big disks: stay alive     */
    }
    sec[0] = 0x01;
    if (raw_write(fs, sb.ibm_start, sec) != NXFS_OK) return NXFS_ERR_IO;
    if (raw_write(fs, sb.bbm_start, sec) != NXFS_OK) return NXFS_ERR_IO;

    /* Root inode. */
    nxfs_inode_t root;
    memset(&root, 0, sizeof(root));
    root.magic = NXFS_MAGIC;
    root.type  = NXFS_TYPE_DIR;
    strncpy(root.name, "/", NXFS_NAME_MAX - 1);
    if (raw_write(fs, sb.itab_start + 0, &root) != NXFS_OK) return NXFS_ERR_IO;

    if (raw_write(fs, NXFS_LBA_SUPERBLOCK, &sb) != NXFS_OK) return NXFS_ERR_IO;

    fs->sb = sb;
    fs->sb_dirty = false;
    fs->balloc_hint = 1;
    fs->ialloc_hint = 1;
    cache_invalidate_fs(fs);
    debug_printf("[nxfs] format OK: %u blocks (%u KiB), %u inodes, "
                 "journal %u sectors, data@%u\n",
                 nb, nb * 4, ic, sb.jrnl_sectors, data_start);
    return NXFS_OK;
}

/* ---------------------------------------------------------------------------
 *                          Mount helpers
 * --------------------------------------------------------------------------- */
static bool fs_mount(nxfs_t *fs, nxfs_backend_t *be) {
    fs->be = be;
    fs->mounted = false;
    nxfs_superblock_t sb;
    if (raw_read(fs, NXFS_LBA_SUPERBLOCK, &sb) != NXFS_OK) {
        fs->be = NULL;
        return false;
    }
    if (sb.magic != NXFS_MAGIC || sb.version != NXFS_VERSION ||
        sb.block_sectors != NXFS_BLOCK_SECTORS ||
        sb.inode_count == 0 || sb.nblocks == 0) {
        debug_printf("[nxfs] %s: no v3 superblock (magic=0x%x ver=%u)\n",
                     be->name, sb.magic, sb.version);
        fs->be = NULL;
        return false;
    }
    fs->sb = sb;
    fs->sb_dirty = false;
    fs->balloc_hint = 1;
    fs->ialloc_hint = 1;
    cache_invalidate_fs(fs);
    if (jrnl_replay(fs) != NXFS_OK) {
        debug_fail("nxfs", "journal replay I/O failure");
        fs->be = NULL;
        return false;
    }
    /* Re-read the superblock - the replay may have updated counters. */
    raw_read(fs, NXFS_LBA_SUPERBLOCK, &fs->sb);
    fs->mounted = true;
    debug_printf("[nxfs] %s: v3 mounted  label=\"%s\"  blocks=%u free=%u  "
                 "inodes=%u free=%u  install_sig=0x%x\n",
                 be->name, (char *)fs->sb.label, fs->sb.nblocks,
                 fs->sb.free_blocks, fs->sb.inode_count,
                 fs->sb.free_inodes, fs->sb.install_sig);
    return true;
}

int nxfs_mount(void) {
    debug_step("nxfs: choosing storage backend (v3)");
    g_mode = NXFS_MODE_NONE;
    g_fs   = NULL;
    g_cwd  = 0;

    uint32_t payload_size = 0;
    const uint8_t *payload = boot_info_kernel_payload(&payload_size);
    bool live_boot = (payload != NULL && payload_size > 0);
    if (live_boot) {
        debug_printf("[nxfs] LIVE boot: kernel-payload module present "
                     "(%u bytes) - skipping SATA, going to RAMFS\n",
                     payload_size);
    }

    if (!live_boot && sysdisk_present()) {
        g_backend_sata.sector_count = sysdisk_sector_count();
        if (fs_mount(&g_disk, &g_backend_sata)) {
            g_fs   = &g_disk;
            g_mode = NXFS_MODE_SATA;
            debug_ok("nxfs: mounted v3 from SATA disk");
            return NXFS_OK;
        }
        debug_printf("[nxfs] SATA present but no valid NXFS v3 - leaving "
                     "disk untouched, falling back to RAMFS (reinstall to "
                     "upgrade v1/v2 disks)\n");
    } else if (!live_boot) {
        debug_printf("[nxfs] no SATA disk available - going to RAMFS\n");
    }

    g_live.be = &g_backend_ramfs;
    int r = fs_format(&g_live, "NEXXON-LIVE", NXFS_RAMFS_SECTORS);
    if (r != NXFS_OK) return r;
    if (!fs_mount(&g_live, &g_backend_ramfs)) {
        debug_fail("nxfs", "RAMFS post-format mount failed");
        return NXFS_ERR_IO;
    }
    g_fs   = &g_live;
    g_mode = NXFS_MODE_LIVE;
    debug_ok("nxfs: mounted in LIVE MODE (RAMFS, changes lost on reboot)");
    return NXFS_OK;
}

/* Diagnostic / rescue: switch the ACTIVE filesystem to the SATA NXFS
 * partition from a LIVE boot (same philosophy as `forcetramp`: QEMU
 * cannot exercise the stage2 HDD-boot path, so the SATA mount logic
 * gets a forced entry point).  Also doubles as a rescue mount. */
int nxfs_remount_sata(void) {
    if (!sysdisk_present()) return NXFS_ERR_IO;
    if (g_ws.active || g_tx.active) return NXFS_ERR_BUSY;
    g_backend_sata.sector_count = sysdisk_sector_count();
    if (!fs_mount(&g_disk, &g_backend_sata)) return NXFS_ERR_NOTFOUND;
    g_fs   = &g_disk;
    g_mode = NXFS_MODE_SATA;
    g_cwd  = 0;
    debug_ok("nxfs: remounted active FS from SATA (forced)");
    return NXFS_OK;
}

int nxfs_format(const char *label) {
    if (!g_fs) return NXFS_ERR_IO;
    uint32_t total = g_fs->be->sector_count
                   ? (g_fs->be->is_ramfs ? g_fs->be->sector_count
                                         : g_fs->be->sector_count
                                           - g_fs->be->lba_offset)
                   : NXFS_RAMFS_SECTORS;
    int r = fs_format(g_fs, label, total);
    if (r != NXFS_OK) return r;
    return fs_mount(g_fs, g_fs->be) ? NXFS_OK : NXFS_ERR_IO;
}

bool         nxfs_is_mounted   (void) { return g_fs && g_fs->mounted; }
nxfs_mode_t  nxfs_mode         (void) { return g_mode; }
const char  *nxfs_backend_name (void) { return g_fs && g_fs->be ? g_fs->be->name : "(none)"; }
bool nxfs_is_installed(void) {
    if (g_mode != NXFS_MODE_SATA || !g_fs) return false;
    return g_fs->sb.install_sig == NXFS_INSTALL_SIGNATURE;
}

uint32_t nxfs_total_sectors(void) { return g_fs ? g_fs->sb.total_sectors : 0; }
uint32_t nxfs_total_blocks (void) { return g_fs ? g_fs->sb.nblocks : 0; }
uint32_t nxfs_free_blocks  (void) { return g_fs ? g_fs->sb.free_blocks : 0; }
uint32_t nxfs_data_start   (void) { return g_fs ? g_fs->sb.data_start : 0; }

void nxfs_stats(uint32_t *inodes_used, uint32_t *blocks_used) {
    if (!g_fs || !g_fs->mounted) {
        if (inodes_used) *inodes_used = 0;
        if (blocks_used) *blocks_used = 0;
        return;
    }
    if (inodes_used) *inodes_used = g_fs->sb.inode_count - g_fs->sb.free_inodes;
    if (blocks_used) *blocks_used = g_fs->sb.nblocks - g_fs->sb.free_blocks;
}

/* ---------------------------------------------------------------------------
 *                       Names, navigation, listing
 * --------------------------------------------------------------------------- */
static bool valid_name(const char *n) {
    if (!n || !n[0]) return false;
    if (n[0] == '/' || n[0] == ' ') return false;
    size_t len = strlen(n);
    if (len + 1 > NXFS_NAME_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (c == '/' || (uint8_t)c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

int nxfs_read_inode(uint32_t inode, nxfs_inode_t *out) {
    if (!g_fs) return NXFS_ERR_IO;
    return inode_read(g_fs, inode, out);
}

int nxfs_write_inode(uint32_t inode, const nxfs_inode_t *in) {
    if (!g_fs) return NXFS_ERR_IO;
    /* Public callers are outside any transaction: auto-wrap. */
    int r = tx_begin(g_fs);
    if (r != NXFS_OK) return r;
    r = inode_write(g_fs, inode, in);
    if (r != NXFS_OK) { tx_abort(); return r; }
    return tx_commit();
}

uint32_t nxfs_cwd(void) { return g_cwd; }

int nxfs_set_cwd(uint32_t inode) {
    nxfs_inode_t n;
    if (nxfs_read_inode(inode, &n) != NXFS_OK) return NXFS_ERR_NOTFOUND;
    if (n.type != NXFS_TYPE_DIR) return NXFS_ERR_NOTDIR;
    g_cwd = inode;
    return NXFS_OK;
}

int nxfs_pwd_path(char *out, size_t out_sz) {
    if (out_sz == 0) return NXFS_ERR_FULL;
    if (g_cwd == 0) {
        if (out_sz < 2) return NXFS_ERR_FULL;
        out[0] = '/'; out[1] = 0;
        return NXFS_OK;
    }
    char tmp[256];
    size_t tp = 0;
    uint32_t cur = g_cwd;
    while (cur != 0) {
        nxfs_inode_t n;
        if (nxfs_read_inode(cur, &n) != NXFS_OK) return NXFS_ERR_IO;
        size_t nl = strlen(n.name);
        if (tp + nl + 1 >= sizeof(tmp)) return NXFS_ERR_FULL;
        tmp[tp++] = '/';
        memcpy(tmp + tp, n.name, nl);
        tp += nl;
        if (n.parent_inode == cur) break;
        cur = n.parent_inode;
    }
    int starts[32]; int nseg = 0;
    for (size_t i = 0; i < tp; i++)
        if (tmp[i] == '/' && nseg < 32) starts[nseg++] = (int)i;
    size_t op = 0;
    for (int s = nseg - 1; s >= 0; s--) {
        int start = starts[s];
        int end = (s + 1 < nseg) ? starts[s + 1] : (int)tp;
        for (int i = start; i < end; i++) {
            if (op + 1 >= out_sz) return NXFS_ERR_FULL;
            out[op++] = tmp[i];
        }
    }
    if (op == 0) out[op++] = '/';
    out[op] = 0;
    return NXFS_OK;
}

int nxfs_resolve(uint32_t cwd, const char *name, uint32_t *out_inode) {
    if (!name || !out_inode) return NXFS_ERR_NOTFOUND;
    if (strcmp(name, ".") == 0)  { *out_inode = cwd; return NXFS_OK; }
    if (strcmp(name, "..") == 0) {
        nxfs_inode_t n;
        if (nxfs_read_inode(cwd, &n) != NXFS_OK) return NXFS_ERR_IO;
        *out_inode = n.parent_inode;
        return NXFS_OK;
    }
    if (strcmp(name, "/") == 0) { *out_inode = 0; return NXFS_OK; }

    nxfs_inode_t d;
    if (nxfs_read_inode(cwd, &d) != NXFS_OK)  return NXFS_ERR_IO;
    if (d.type != NXFS_TYPE_DIR)              return NXFS_ERR_NOTDIR;
    for (uint32_t i = 0; i < d.child_count; i++) {
        nxfs_inode_t c;
        if (nxfs_read_inode(d.children[i], &c) != NXFS_OK) return NXFS_ERR_IO;
        if (strcmp(c.name, name) == 0) {
            *out_inode = d.children[i];
            return NXFS_OK;
        }
    }
    return NXFS_ERR_NOTFOUND;
}

int nxfs_resolve_path(const char *path, uint32_t *out_inode) {
    if (!path || !out_inode) return NXFS_ERR_NOTFOUND;
    uint32_t cur = 0;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) { *out_inode = 0; return NXFS_OK; }
    char comp[NXFS_NAME_MAX];
    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < NXFS_NAME_MAX - 1) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;
        uint32_t next;
        int r = nxfs_resolve(cur, comp, &next);
        if (r != NXFS_OK) return r;
        cur = next;
    }
    *out_inode = cur;
    return NXFS_OK;
}

int nxfs_list(uint32_t dir_inode, nxfs_list_cb_t cb, void *user) {
    nxfs_inode_t d;
    if (nxfs_read_inode(dir_inode, &d) != NXFS_OK) return NXFS_ERR_IO;
    if (d.type != NXFS_TYPE_DIR)                   return NXFS_ERR_NOTDIR;
    for (uint32_t i = 0; i < d.child_count; i++) {
        nxfs_inode_t c;
        if (nxfs_read_inode(d.children[i], &c) != NXFS_OK) return NXFS_ERR_IO;
        if (cb) cb(&c, user);
    }
    return NXFS_OK;
}

/* ---------------------------------------------------------------------------
 *                       Create / delete / rename
 * --------------------------------------------------------------------------- */
static int dir_add_child(nxfs_inode_t *dir, uint32_t child) {
    if (dir->child_count >= NXFS_MAX_CHILDREN) return NXFS_ERR_FULL;
    dir->children[dir->child_count++] = child;
    dir->size = dir->child_count;
    return NXFS_OK;
}
static int dir_remove_child(nxfs_inode_t *dir, uint32_t child) {
    for (uint32_t i = 0; i < dir->child_count; i++) {
        if (dir->children[i] == child) {
            for (uint32_t j = i + 1; j < dir->child_count; j++)
                dir->children[j - 1] = dir->children[j];
            dir->child_count--;
            dir->size = dir->child_count;
            return NXFS_OK;
        }
    }
    return NXFS_ERR_NOTFOUND;
}

/* fs-parameterised child lookup (create_node runs against the INSTALL
 * TARGET too - the public nxfs_resolve always reads the active fs). */
static int resolve_fs(nxfs_t *fs, uint32_t cwd, const char *name,
                      uint32_t *out_inode) {
    nxfs_inode_t d;
    int r = inode_read(fs, cwd, &d);
    if (r != NXFS_OK) return r;
    if (d.type != NXFS_TYPE_DIR) return NXFS_ERR_NOTDIR;
    for (uint32_t i = 0; i < d.child_count; i++) {
        nxfs_inode_t c;
        if (inode_read(fs, d.children[i], &c) != NXFS_OK) return NXFS_ERR_IO;
        if (strcmp(c.name, name) == 0) {
            *out_inode = d.children[i];
            return NXFS_OK;
        }
    }
    return NXFS_ERR_NOTFOUND;
}

static int create_node(nxfs_t *fs, uint32_t parent, const char *name,
                       uint32_t type, uint32_t *out_inode) {
    if (!valid_name(name)) return NXFS_ERR_NAME;
    uint32_t dummy;
    if (resolve_fs(fs, parent, name, &dummy) == NXFS_OK) return NXFS_ERR_EXISTS;

    nxfs_inode_t p;
    int r = inode_read(fs, parent, &p);
    if (r != NXFS_OK) return r;
    if (p.type != NXFS_TYPE_DIR)            return NXFS_ERR_NOTDIR;
    if (p.child_count >= NXFS_MAX_CHILDREN) return NXFS_ERR_FULL;

    r = tx_begin(fs);
    if (r != NXFS_OK) return r;

    uint32_t new_idx;
    r = ialloc(fs, &new_idx);
    if (r != NXFS_OK) { tx_abort(); return r; }

    nxfs_inode_t n;
    memset(&n, 0, sizeof(n));
    n.magic        = NXFS_MAGIC;
    n.type         = type;
    n.parent_inode = parent;
    strncpy(n.name, name, NXFS_NAME_MAX - 1);
    if ((r = inode_write(fs, new_idx, &n)) != NXFS_OK) { tx_abort(); return r; }
    if ((r = dir_add_child(&p, new_idx))   != NXFS_OK) { tx_abort(); return r; }
    if ((r = inode_write(fs, parent, &p))  != NXFS_OK) { tx_abort(); return r; }
    r = tx_commit();
    if (r != NXFS_OK) return r;
    if (out_inode) *out_inode = new_idx;
    return NXFS_OK;
}

/* Run a metadata op that needs the journal while a streaming write may be
 * open: borrow the tx (suspend), run, then hand it back (resume). */
static int create_node_concurrent(nxfs_t *fs, uint32_t parent,
                                  const char *name, uint32_t type,
                                  uint32_t *out_inode) {
    int sr = stream_suspend();
    if (sr != NXFS_OK) return sr;
    int r = create_node(fs, parent, name, type, out_inode);
    int rr = stream_resume();
    return r != NXFS_OK ? r : rr;
}

int nxfs_create_file(uint32_t parent, const char *name, uint32_t *out_inode) {
    if (!g_fs) return NXFS_ERR_IO;
    return create_node_concurrent(g_fs, parent, name, NXFS_TYPE_FILE, out_inode);
}
int nxfs_create_dir(uint32_t parent, const char *name, uint32_t *out_inode) {
    if (!g_fs) return NXFS_ERR_IO;
    return create_node_concurrent(g_fs, parent, name, NXFS_TYPE_DIR, out_inode);
}

static int delete_file_fs(nxfs_t *fs, uint32_t parent, const char *name) {
    uint32_t inode;
    int r = nxfs_resolve(parent, name, &inode);
    if (r != NXFS_OK) return r;

    nxfs_inode_t n;
    if ((r = inode_read(fs, inode, &n)) != NXFS_OK) return r;
    if (n.type != NXFS_TYPE_FILE) return NXFS_ERR_NOTFILE;

    /* Phase 1: truncate (consistent empty file). */
    r = file_truncate(fs, inode, &n);
    if (r != NXFS_OK) return r;

    /* Phase 2: unlink + release the inode in one tx. */
    nxfs_inode_t p;
    if ((r = inode_read(fs, parent, &p)) != NXFS_OK) return r;
    r = tx_begin(fs);
    if (r != NXFS_OK) return r;
    nxfs_inode_t zero;
    memset(&zero, 0, sizeof(zero));
    if ((r = inode_write(fs, inode, &zero)) != NXFS_OK) { tx_abort(); return r; }
    if ((r = ifree(fs, inode))              != NXFS_OK) { tx_abort(); return r; }
    dir_remove_child(&p, inode);
    if ((r = inode_write(fs, parent, &p))   != NXFS_OK) { tx_abort(); return r; }
    return tx_commit();
}

int nxfs_delete_file(uint32_t parent, const char *name) {
    if (!g_fs) return NXFS_ERR_IO;
    return delete_file_fs(g_fs, parent, name);
}

int nxfs_delete_dir(uint32_t parent, const char *name) {
    if (!g_fs) return NXFS_ERR_IO;
    uint32_t inode;
    int r = nxfs_resolve(parent, name, &inode);
    if (r != NXFS_OK) return r;
    if (inode == 0) return NXFS_ERR_NAME;

    nxfs_inode_t n;
    if (nxfs_read_inode(inode, &n) != NXFS_OK) return NXFS_ERR_IO;
    if (n.type != NXFS_TYPE_DIR)               return NXFS_ERR_NOTDIR;

    uint32_t snap[NXFS_MAX_CHILDREN];
    uint32_t snap_n = n.child_count;
    for (uint32_t i = 0; i < snap_n; i++) snap[i] = n.children[i];
    for (uint32_t i = 0; i < snap_n; i++) {
        nxfs_inode_t c;
        if (nxfs_read_inode(snap[i], &c) != NXFS_OK) return NXFS_ERR_IO;
        if (c.type == NXFS_TYPE_FILE)      nxfs_delete_file(inode, c.name);
        else if (c.type == NXFS_TYPE_DIR)  nxfs_delete_dir(inode, c.name);
    }

    nxfs_inode_t p;
    if (nxfs_read_inode(parent, &p) != NXFS_OK) return NXFS_ERR_IO;
    r = tx_begin(g_fs);
    if (r != NXFS_OK) return r;
    nxfs_inode_t zero;
    memset(&zero, 0, sizeof(zero));
    if ((r = inode_write(g_fs, inode, &zero)) != NXFS_OK) { tx_abort(); return r; }
    if ((r = ifree(g_fs, inode))              != NXFS_OK) { tx_abort(); return r; }
    dir_remove_child(&p, inode);
    if ((r = inode_write(g_fs, parent, &p))   != NXFS_OK) { tx_abort(); return r; }
    return tx_commit();
}

int nxfs_rename(uint32_t parent, const char *old_name, const char *new_name) {
    if (!g_fs) return NXFS_ERR_IO;
    if (!valid_name(new_name)) return NXFS_ERR_NAME;
    if (!old_name || strcmp(old_name, new_name) == 0) return NXFS_OK;

    uint32_t conflict;
    if (nxfs_resolve(parent, new_name, &conflict) == NXFS_OK)
        return NXFS_ERR_EXISTS;
    uint32_t inode;
    int r = nxfs_resolve(parent, old_name, &inode);
    if (r != NXFS_OK) return r;

    nxfs_inode_t n;
    if (nxfs_read_inode(inode, &n) != NXFS_OK) return NXFS_ERR_IO;
    memset(n.name, 0, sizeof(n.name));
    strncpy(n.name, new_name, NXFS_NAME_MAX - 1);
    return nxfs_write_inode(inode, &n);
}

int nxfs_rename_dir(uint32_t parent, const char *old_name, const char *new_name) {
    uint32_t inode;
    int r = nxfs_resolve(parent, old_name, &inode);
    if (r != NXFS_OK) return r;
    nxfs_inode_t n;
    if (nxfs_read_inode(inode, &n) != NXFS_OK) return NXFS_ERR_IO;
    if (n.type != NXFS_TYPE_DIR)               return NXFS_ERR_NOTDIR;
    return nxfs_rename(parent, old_name, new_name);
}

/* ---------------------------------------------------------------------------
 *                       Public file data API
 * --------------------------------------------------------------------------- */
int nxfs_write_begin(uint32_t inode) {
    if (!g_fs) return NXFS_ERR_IO;
    return stream_begin(g_fs, inode);
}
int nxfs_write_append(const void *data, uint32_t len) {
    return stream_append(data, len);
}
int nxfs_write_end(bool commit) {
    return stream_end(commit);
}

int nxfs_write_file(uint32_t inode, const void *data, uint32_t len) {
    if (!g_fs) return NXFS_ERR_IO;
    int r = stream_begin(g_fs, inode);
    if (r != NXFS_OK) return r;
    if (len > 0) {
        r = stream_append(data, len);
        if (r != NXFS_OK) {
            if (g_ws.active) stream_end(false);
            return r;
        }
    }
    return stream_end(true);
}

int nxfs_read_file(uint32_t inode, void *out, uint32_t out_sz,
                   uint32_t *bytes_read) {
    if (!g_fs) return NXFS_ERR_IO;
    int r = read_at(g_fs, inode, 0, out, out_sz);
    if (r < 0) {
        if (bytes_read) *bytes_read = 0;
        return r;
    }
    if (bytes_read) *bytes_read = (uint32_t)r;
    return NXFS_OK;
}

int nxfs_read_at(uint32_t inode, uint64_t offset, void *out, uint32_t len) {
    if (!g_fs) return NXFS_ERR_IO;
    return read_at(g_fs, inode, offset, out, len);
}

uint64_t nxfs_file_size(uint32_t inode) {
    nxfs_inode_t n;
    if (nxfs_read_inode(inode, &n) != NXFS_OK) return 0;
    if (n.type != NXFS_TYPE_FILE) return 0;
    return ((uint64_t)n.size_hi << 32) | n.size;
}

/* ---------------------------------------------------------------------------
 *                       Consistency check (fsck-lite)
 * --------------------------------------------------------------------------- */
/* Shadow bitmaps: 1 MiB covers volumes up to 8M blocks = 32 GiB; bigger
 * volumes get a structure-only walk (no bitmap rebuild). */
#define CHECK_SHADOW_BYTES (1024u * 1024u)
static uint8_t g_shadow_b[CHECK_SHADOW_BYTES];
static uint8_t g_shadow_i[32768 / 8];

static int g_chk_problems;
static bool g_chk_shadow_ok;

static void shadow_set(uint8_t *bm, uint32_t idx) {
    bm[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}
static bool shadow_get(const uint8_t *bm, uint32_t idx) {
    return (bm[idx >> 3] >> (idx & 7)) & 1u;
}

static void chk_mark_block(nxfs_t *fs, uint32_t blk) {
    if (blk == 0) return;
    if (blk >= fs->sb.nblocks) { g_chk_problems++; return; }
    if (!g_chk_shadow_ok) return;
    if (shadow_get(g_shadow_b, blk)) {
        g_chk_problems++;                 /* double-referenced block      */
        return;
    }
    shadow_set(g_shadow_b, blk);
}

static void chk_walk_tree(nxfs_t *fs, uint32_t blk, int depth) {
    if (blk == 0 || blk >= fs->sb.nblocks) return;
    chk_mark_block(fs, blk);
    if (depth == 0) return;
    uint8_t sec[NXFS_SECTOR_SIZE];
    for (uint32_t s = 0; s < NXFS_BLOCK_SECTORS; s++) {
        if (meta_read(fs, blk_sector(fs, blk) + s, sec) != NXFS_OK) {
            g_chk_problems++;
            return;
        }
        uint32_t ents[128];
        memcpy(ents, sec, sizeof(ents));
        for (int i = 0; i < 128; i++) {
            if (!ents[i]) continue;
            if (depth == 1) chk_mark_block(fs, ents[i]);
            else            chk_walk_tree(fs, ents[i], depth - 1);
        }
    }
}

static void chk_walk_inode(nxfs_t *fs, uint32_t ino, int depth_guard) {
    if (depth_guard > 24) { g_chk_problems++; return; }
    if (ino >= fs->sb.inode_count) { g_chk_problems++; return; }
    if (shadow_get(g_shadow_i, ino)) { g_chk_problems++; return; }
    shadow_set(g_shadow_i, ino);

    nxfs_inode_t n;
    if (inode_read(fs, ino, &n) != NXFS_OK) { g_chk_problems++; return; }
    if (n.magic != NXFS_MAGIC) { g_chk_problems++; return; }

    if (n.type == NXFS_TYPE_FILE) {
        for (int i = 0; i < NXFS_DIRECT; i++) chk_mark_block(fs, n.direct[i]);
        chk_walk_tree(fs, n.ind1, 1);
        chk_walk_tree(fs, n.ind2, 2);
        chk_walk_tree(fs, n.ind3, 3);
        wm_tick();                        /* big files: stay responsive   */
    } else if (n.type == NXFS_TYPE_DIR) {
        if (n.child_count > NXFS_MAX_CHILDREN) { g_chk_problems++; return; }
        for (uint32_t i = 0; i < n.child_count; i++)
            chk_walk_inode(fs, n.children[i], depth_guard + 1);
    } else {
        g_chk_problems++;
    }
}

int nxfs_check(bool repair, char *out, uint32_t out_cap) {
    if (!g_fs || !g_fs->mounted) return NXFS_ERR_IO;
    nxfs_t *fs = g_fs;

    g_chk_problems = 0;
    g_chk_shadow_ok = (fs->sb.nblocks <= CHECK_SHADOW_BYTES * 8) &&
                      (fs->sb.inode_count <= sizeof(g_shadow_i) * 8);
    if (g_chk_shadow_ok) {
        memset(g_shadow_b, 0, (fs->sb.nblocks + 7) / 8);
        memset(g_shadow_i, 0, (fs->sb.inode_count + 7) / 8);
        shadow_set(g_shadow_b, 0);        /* reserved NULL block          */
    }

    chk_walk_inode(fs, 0, 0);

    uint32_t leaked = 0, lost = 0, used_b = 0, used_i = 0;
    if (g_chk_shadow_ok) {
        /* Compare shadow vs on-disk bitmaps. */
        uint8_t sec[NXFS_SECTOR_SIZE];
        for (uint32_t s = 0; s < fs->sb.bbm_sectors; s++) {
            if (meta_read(fs, fs->sb.bbm_start + s, sec) != NXFS_OK)
                return NXFS_ERR_IO;
            for (uint32_t b = 0; b < 4096; b++) {
                uint32_t idx = s * 4096 + b;
                if (idx >= fs->sb.nblocks) break;
                bool disk = (sec[b >> 3] >> (b & 7)) & 1u;
                bool live = shadow_get(g_shadow_b, idx);
                if (live) used_b++;
                if (disk && !live) leaked++;
                if (!disk && live) lost++;
            }
        }
        for (uint32_t i = 0; i < fs->sb.inode_count; i++)
            if (shadow_get(g_shadow_i, i)) used_i++;
        if (lost) g_chk_problems += (int)lost;

        if (repair && (leaked || lost ||
                       fs->sb.free_blocks != fs->sb.nblocks - used_b ||
                       fs->sb.free_inodes != fs->sb.inode_count - used_i)) {
            int r = tx_begin(fs);
            if (r != NXFS_OK) return r;
            for (uint32_t s = 0; s < fs->sb.bbm_sectors; s++) {
                uint32_t base = s * 4096 / 8;
                memset(sec, 0, sizeof(sec));
                uint32_t bytes = (fs->sb.nblocks + 7) / 8;
                uint32_t copy = (base < bytes) ? bytes - base : 0;
                if (copy > NXFS_SECTOR_SIZE) copy = NXFS_SECTOR_SIZE;
                if (copy) memcpy(sec, g_shadow_b + base, copy);
                if ((r = meta_write(fs, fs->sb.bbm_start + s, sec)) != NXFS_OK) {
                    tx_abort(); return r;
                }
                if (g_tx.count >= TX_SOFT) {
                    if ((r = tx_checkpoint(fs)) != NXFS_OK) return r;
                }
            }
            for (uint32_t s = 0; s < fs->sb.ibm_sectors; s++) {
                uint32_t base = s * 4096 / 8;
                memset(sec, 0, sizeof(sec));
                uint32_t bytes = (fs->sb.inode_count + 7) / 8;
                uint32_t copy = (base < bytes) ? bytes - base : 0;
                if (copy > NXFS_SECTOR_SIZE) copy = NXFS_SECTOR_SIZE;
                if (copy) memcpy(sec, g_shadow_i + base, copy);
                if ((r = meta_write(fs, fs->sb.ibm_start + s, sec)) != NXFS_OK) {
                    tx_abort(); return r;
                }
            }
            fs->sb.free_blocks = fs->sb.nblocks - used_b;
            fs->sb.free_inodes = fs->sb.inode_count - used_i;
            fs->sb_dirty = true;
            if ((r = tx_commit()) != NXFS_OK) return r;
        }
    }

    if (out && out_cap) {
        ksnprintf(out, out_cap,
                  "v3 %s: %d problem(s), %u leaked, %u lost, "
                  "used %u/%u blk %u/%u ino%s%s",
                  (char *)fs->sb.label, g_chk_problems, leaked, lost,
                  used_b, fs->sb.nblocks, used_i, fs->sb.inode_count,
                  g_chk_shadow_ok ? "" : " (structure-only)",
                  (repair && (leaked || lost)) ? " [repaired]" : "");
    }
    return g_chk_problems + (int)leaked;
}

/* ===========================================================================
 *                       installsys: live tree -> SATA
 * --------------------------------------------------------------------------- */
static int build_bootable_mbr(uint8_t *mbr, uint32_t disk_sectors) {
    uint32_t stage1_size = 0;
    const uint8_t *stage1 = boot_info_stage1(&stage1_size);
    if (!stage1 || stage1_size != 512) {
        debug_fail("installsys", "embedded stage1 is missing or wrong size");
        return NXFS_ERR_IO;
    }
    memcpy(mbr, stage1, 512);
    uint8_t *p = mbr + 0x1BE;
    p[0x00] = 0x80;
    p[0x01] = 0x00; p[0x02] = 0x02; p[0x03] = 0x00;
    p[0x04] = 0x7F;
    p[0x05] = 0xFE; p[0x06] = 0xFF; p[0x07] = 0xFF;
    uint32_t lba_start = NXFS_PARTITION_OFFSET;
    uint32_t lba_len   = (disk_sectors > lba_start) ? disk_sectors - lba_start : 0;
    p[0x08] = (uint8_t)(lba_start);
    p[0x09] = (uint8_t)(lba_start >> 8);
    p[0x0A] = (uint8_t)(lba_start >> 16);
    p[0x0B] = (uint8_t)(lba_start >> 24);
    p[0x0C] = (uint8_t)(lba_len);
    p[0x0D] = (uint8_t)(lba_len >> 8);
    p[0x0E] = (uint8_t)(lba_len >> 16);
    p[0x0F] = (uint8_t)(lba_len >> 24);
    mbr[0x1FE] = 0x55;
    mbr[0x1FF] = 0xAA;
    return NXFS_OK;
}

/* Recursive live -> target tree copy.  Files stream through a 32 KiB
 * chunk buffer; the WM is pumped so the UI stays alive. */
static uint8_t g_inst_chunk[32 * 1024];

static int inst_copy_tree(nxfs_t *src, nxfs_t *dst,
                          uint32_t src_ino, uint32_t dst_ino, int depth) {
    if (depth > 24) return NXFS_ERR_FULL;
    nxfs_inode_t d;
    int r = inode_read(src, src_ino, &d);
    if (r != NXFS_OK) return r;
    if (d.type != NXFS_TYPE_DIR) return NXFS_ERR_NOTDIR;

    for (uint32_t i = 0; i < d.child_count; i++) {
        nxfs_inode_t c;
        if (inode_read(src, d.children[i], &c) != NXFS_OK) {
            debug_printf("[installsys] copy: source inode %u unreadable depth=%d\n",
                         d.children[i], depth);
            return NXFS_ERR_IO;
        }
        debug_printf("[installsys] copy: depth=%d '%s' type=%u size=%u:%u\n",
                     depth, c.name, c.type, c.size_hi, c.size);
        if (c.type == NXFS_TYPE_DIR) {
            uint32_t nd;
            r = create_node(dst, dst_ino, c.name, NXFS_TYPE_DIR, &nd);
            if (r != NXFS_OK) {
                debug_printf("[installsys] copy: mkdir '%s' failed %d\n",
                             c.name, r);
                return r;
            }
            r = inst_copy_tree(src, dst, d.children[i], nd, depth + 1);
            if (r != NXFS_OK) return r;
        } else if (c.type == NXFS_TYPE_FILE) {
            uint32_t nf;
            r = create_node(dst, dst_ino, c.name, NXFS_TYPE_FILE, &nf);
            if (r != NXFS_OK) {
                debug_printf("[installsys] copy: create '%s' failed %d\n",
                             c.name, r);
                return r;
            }
            r = stream_begin(dst, nf);
            if (r != NXFS_OK) {
                debug_printf("[installsys] copy: begin '%s' failed %d\n",
                             c.name, r);
                return r;
            }
            uint64_t size = ((uint64_t)c.size_hi << 32) | c.size;
            uint64_t off = 0;
            while (off < size) {
                uint32_t want = sizeof(g_inst_chunk);
                if ((uint64_t)want > size - off) want = (uint32_t)(size - off);
                int got = read_at(src, d.children[i], off, g_inst_chunk, want);
                if (got <= 0) {
                    debug_printf("[installsys] copy: read '%s' off=%lu "
                                 "want=%u failed %d\n",
                                 c.name, (unsigned long)off, want, got);
                    stream_end(false);
                    return NXFS_ERR_IO;
                }
                r = stream_append(g_inst_chunk, (uint32_t)got);
                if (r != NXFS_OK) {
                    debug_printf("[installsys] copy: append '%s' off=%lu "
                                 "got=%d failed %d\n",
                                 c.name, (unsigned long)off, got, r);
                    if (g_ws.active) stream_end(false);
                    return r;
                }
                off += (uint32_t)got;
                wm_tick();
            }
            r = stream_end(true);
            if (r != NXFS_OK) {
                debug_printf("[installsys] copy: close '%s' failed %d\n",
                             c.name, r);
                return r;
            }
        }
        term_printf(".");
    }
    return NXFS_OK;
}

int nxfs_install_to_sata(void) {
    if (g_mode != NXFS_MODE_LIVE) {
        debug_fail("installsys", "not running in Live Mode");
        return NXFS_ERR_IO;
    }
    if (!sysdisk_present()) {
        debug_fail("installsys", "no SATA disk available");
        return NXFS_ERR_IO;
    }
    uint32_t disk_sectors = sysdisk_sector_count();
    if (disk_sectors < NXFS_PARTITION_OFFSET + 8192) {
        debug_fail("installsys", "SATA disk is too small for NXFS v3");
        return NXFS_ERR_IO;
    }

    uint32_t payload_size = 0;
    const uint8_t *payload = boot_info_kernel_payload(&payload_size);
    if (!payload || payload_size == 0) {
        term_set_color(VGA_RED, VGA_BLACK);
        term_printf("[installsys] FATAL: no kernel-payload module found.\n");
        term_printf("             Re-image your CD/USB - the grub.cfg must\n");
        term_printf("             pass the kernel as a module.\n");
        term_set_color(VGA_LTGRAY, VGA_BLACK);
        return NXFS_ERR_IO;
    }
    uint32_t payload_sectors = (payload_size + NXFS_SECTOR_SIZE - 1) / NXFS_SECTOR_SIZE;
    debug_printf("[installsys] payload=%u bytes/%u sectors, partition LBA=%u\n",
                 payload_size, payload_sectors,
                 (uint32_t)NXFS_PARTITION_OFFSET);
    if (64 + payload_sectors >= NXFS_PARTITION_OFFSET) {
        term_set_color(VGA_RED, VGA_BLACK);
        term_printf("[installsys] kernel payload too large: %u sectors, max %u.\n",
                    payload_sectors, (uint32_t)(NXFS_PARTITION_OFFSET - 64));
        term_set_color(VGA_LTGRAY, VGA_BLACK);
        return NXFS_ERR_IO;
    }

    /* Step 1: MBR + stage2 + kernel ELF (outside the NXFS partition). */
    static uint8_t mbr_buf[NXFS_SECTOR_SIZE];
    int rb = build_bootable_mbr(mbr_buf, disk_sectors);
    if (rb != NXFS_OK) return rb;
    if (sysdisk_write_sector(0, mbr_buf) != AHCI_OK) {
        debug_fail("installsys", "MBR write failed");
        return NXFS_ERR_IO;
    }

    uint32_t stage2_size = 0;
    const uint8_t *stage2 = boot_info_stage2(&stage2_size);
    if (!stage2 || stage2_size == 0 || stage2_size > 63 * NXFS_SECTOR_SIZE) {
        debug_fail("installsys", "embedded stage2 missing or too large");
        return NXFS_ERR_IO;
    }
    uint32_t stage2_sectors = (stage2_size + NXFS_SECTOR_SIZE - 1) / NXFS_SECTOR_SIZE;
    term_set_color(VGA_CYAN, VGA_BLACK);
    term_printf("[installsys] writing stage-2 loader (%u bytes) ...\n", stage2_size);
    term_set_color(VGA_LTGRAY, VGA_BLACK);
    for (uint32_t s = 0; s < stage2_sectors; s++) {
        static uint8_t sect[NXFS_SECTOR_SIZE];
        memset(sect, 0, sizeof(sect));
        uint32_t copy = NXFS_SECTOR_SIZE;
        uint32_t off  = s * NXFS_SECTOR_SIZE;
        if (off + copy > stage2_size) copy = stage2_size - off;
        memcpy(sect, stage2 + off, copy);
        if (sysdisk_write_sector(1 + s, sect) != AHCI_OK) {
            debug_fail("installsys", "stage2 write failed");
            return NXFS_ERR_IO;
        }
    }

    term_set_color(VGA_CYAN, VGA_BLACK);
    term_printf("[installsys] writing kernel ELF to disk (%u bytes) ", payload_size);
    term_set_color(VGA_LTGRAY, VGA_BLACK);
    for (uint32_t s = 0; s < payload_sectors; s++) {
        if (keyboard_abort_requested()) {
            keyboard_clear_abort();
            term_printf("\n[installsys] ^C - aborted while writing kernel\n");
            return NXFS_ERR_IO;
        }
        static uint8_t sect[NXFS_SECTOR_SIZE];
        memset(sect, 0, sizeof(sect));
        uint32_t off = s * NXFS_SECTOR_SIZE;
        uint32_t copy = NXFS_SECTOR_SIZE;
        if (off + copy > payload_size) copy = payload_size - off;
        memcpy(sect, payload + off, copy);
        if (sysdisk_write_sector(64 + s, sect) != AHCI_OK) {
            debug_fail("installsys", "kernel-payload write failed");
            return NXFS_ERR_IO;
        }
        if ((s & 0x1F) == 0) { term_printf("."); wm_tick(); }
    }
    term_printf(" done\n");

    /* Step 2: format the partition at the disk's REAL size, then copy
     * the live tree into it.  (v2 raw-imaged the tiny RAMFS geometry
     * onto the disk, freezing the installed FS at RAMFS dimensions.) */
    g_backend_sata.sector_count = disk_sectors;
    g_disk.be = &g_backend_sata;
    term_set_color(VGA_CYAN, VGA_BLACK);
    term_printf("[installsys] formatting NXFS v3 partition (%u MiB) ...\n",
                (disk_sectors - NXFS_PARTITION_OFFSET) / 2048);
    term_set_color(VGA_LTGRAY, VGA_BLACK);
    int r = fs_format(&g_disk, "NEXXON-HDD",
                      disk_sectors - NXFS_PARTITION_OFFSET);
    if (r != NXFS_OK) return r;
    if (!fs_mount(&g_disk, &g_backend_sata)) return NXFS_ERR_IO;

    term_set_color(VGA_CYAN, VGA_BLACK);
    term_printf("[installsys] copying live tree ");
    term_set_color(VGA_LTGRAY, VGA_BLACK);
    r = inst_copy_tree(&g_live, &g_disk, 0, 0, 0);
    if (r != NXFS_OK) {
        term_printf("\n[installsys] tree copy failed (%d)\n", r);
        return r;
    }
    term_printf(" done\n");

    /* Step 3: stamp the install signature (journaled). */
    r = tx_begin(&g_disk);
    if (r != NXFS_OK) return r;
    g_disk.sb.install_sig  = NXFS_INSTALL_SIGNATURE;
    g_disk.sb.install_time = pit_ticks();
    g_disk.sb_dirty = true;
    r = tx_commit();
    if (r != NXFS_OK) return r;

    g_disk.mounted = false;          /* live session stays on RAMFS      */
    debug_ok("installsys: NXFS v3 written to SATA");
    return NXFS_OK;
}

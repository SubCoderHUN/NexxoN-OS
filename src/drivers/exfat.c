/* ============================================================================
 * NexxoN OS - exFAT reader (read-only)
 * ----------------------------------------------------------------------------
 * Implements just enough of the Microsoft exFAT specification to mount a
 * volume, list directories and read files:
 *   - boot sector parse (FAT/heap offsets, cluster shift, root cluster);
 *   - 32-bit FAT cluster-chain walking, with the NoFatChain "contiguous"
 *     fast path used by most files exFAT writes;
 *   - directory entry sets: 0x85 File + 0xC0 Stream-Extension + 0xC1 File-Name
 *     (UTF-16LE, 15 chars per entry), converted to UTF-8 for the rest of the
 *     OS so Hungarian filenames render correctly.
 * Assumes 512-byte logical sectors (every USB stick in practice).
 * ============================================================================ */
#include "exfat.h"
#include "string.h"
#include "debug.h"

#define EXSEC            512u
#define EX_EOC           0xFFFFFFF8u    /* >= this = end of chain (read)   */
#define EX_EOC_WRITE     0xFFFFFFFFu    /* the ONLY spec-valid EOC marker -
                                         * fsck flags 0xFFFFFFF8 as a
                                         * broken chain                    */
#define EX_ENT_FILE      0x85
#define EX_ENT_STREAM    0xC0
#define EX_ENT_NAME      0xC1
#define EX_ENT_LABEL     0x83
#define EX_ATTR_DIR      0x10
#define EX_FLAG_NOFATCHAIN 0x02         /* GeneralSecondaryFlags bit 1     */
#define EX_MAX_CLUSTERS  0x10000000u    /* sanity cap on chain length      */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ex_rd(exfat_volume_t *v, uint32_t lba, void *buf) {
    return v->read(v->user, lba, buf);
}

static uint32_t clus_lba(exfat_volume_t *v, uint32_t cluster) {
    return v->heap_lba + (cluster - 2) * v->sec_per_clus;
}

/* Next cluster in the FAT chain, or >= EX_EOC at the end. */
static uint32_t ex_next_cluster(exfat_volume_t *v, uint32_t cluster) {
    uint32_t byte = cluster * 4u;
    uint32_t lba  = v->fat_lba + byte / EXSEC;
    uint32_t off  = byte % EXSEC;
    uint8_t  sec[EXSEC];
    if (ex_rd(v, lba, sec) != 0) return EX_EOC;
    return rd32(sec + off);
}

/* Append one UTF-16 code unit to a UTF-8 buffer (BMP only; surrogates and
 * control chars become '_').  Returns the new length. */
static int utf8_put(char *out, int len, int cap, uint16_t cu) {
    if (cu == 0) return len;
    if (cu >= 0xD800 && cu <= 0xDFFF) cu = '_';
    if (cu < 0x80) {
        if (len + 1 < cap) out[len++] = (char)cu;
    } else if (cu < 0x800) {
        if (len + 2 < cap) {
            out[len++] = (char)(0xC0 | (cu >> 6));
            out[len++] = (char)(0x80 | (cu & 0x3F));
        }
    } else {
        if (len + 3 < cap) {
            out[len++] = (char)(0xE0 | (cu >> 12));
            out[len++] = (char)(0x80 | ((cu >> 6) & 0x3F));
            out[len++] = (char)(0x80 | (cu & 0x3F));
        }
    }
    return len;
}

/* lower-case an ASCII byte (exFAT compares via an up-case table; for path
 * matching we approximate with ASCII case-folding, which covers real-world
 * filenames). */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool name_eq(const char *a, const char *b) {
    while (*a && *b) { if (lc(*a) != lc(*b)) return false; a++; b++; }
    return *a == *b;
}

/* ---------- Mount -------------------------------------------------------- */
bool exfat_mount(exfat_volume_t *v, blockdev_read_t rd, void *user,
                 uint32_t partition_lba) {
    if (!v || !rd) return false;
    memset(v, 0, sizeof(*v));
    v->read = rd; v->user = user; v->part_lba = partition_lba;

    uint8_t bs[EXSEC];
    if (rd(user, partition_lba, bs) != 0) return false;
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0) return false;

    uint8_t bps_shift = bs[108];
    uint8_t spc_shift = bs[109];
    if (bps_shift != 9) {
        debug_printf("[exfat] unsupported sector size shift %u (need 512)\n",
                     bps_shift);
        return false;
    }
    if (spc_shift > 25) return false;

    uint32_t fat_off  = rd32(bs + 80);
    uint32_t heap_off = rd32(bs + 88);
    v->cluster_count  = rd32(bs + 92);
    v->root_cluster   = rd32(bs + 96);
    v->spc_shift      = spc_shift;
    v->sec_per_clus   = 1u << spc_shift;
    v->fat_lba        = partition_lba + fat_off;
    v->heap_lba       = partition_lba + heap_off;
    if (v->root_cluster < 2 || v->sec_per_clus == 0) return false;

    /* Scan the root directory for the allocation bitmap entry (type 0x81)
     * and the volume label (type 0x83) before reporting mount success. */
    {
        uint32_t rclus = v->root_cluster;
        uint32_t guard = 0;
        while (rclus >= 2 && rclus < EX_EOC && guard++ < 8) {
            uint32_t base = v->heap_lba + (rclus - 2) * v->sec_per_clus;
            bool stop = false;
            for (uint32_t s = 0; s < v->sec_per_clus && !stop; s++) {
                if (rd(user, base + s, bs) != 0) break;
                for (int e = 0; e < (int)(EXSEC / 32); e++) {
                    uint8_t *en = bs + e * 32;
                    if (en[0] == 0x00) { stop = true; break; }
                    if (en[0] == 0x83) {          /* volume label */
                        int nc = en[1] & 0x1F;
                        char *lp = v->label;
                        for (int k = 0; k < nc && k < 14; k++) {
                            uint16_t cu = (uint16_t)en[2 + k*2] | ((uint16_t)en[3 + k*2] << 8);
                            *lp++ = (cu < 0x80 && cu) ? (char)cu : '_';
                        }
                        *lp = 0;
                    }
                    if (en[0] == 0x81 && !(en[1] & 1)) { /* alloc bitmap (first) */
                        v->bitmap_cluster = rd32(en + 20);
                        v->bitmap_bytes   = (uint64_t)rd32(en + 24) |
                                            ((uint64_t)rd32(en + 28) << 32);
                    }
                }
            }
            if (stop) break;
            uint32_t by = rclus * 4u;
            uint8_t fsec[EXSEC];
            if (rd(user, v->fat_lba + by / EXSEC, fsec) != 0) break;
            rclus = rd32(fsec + by % EXSEC);
        }
    }

    if (!v->label[0]) strncpy(v->label, "EXFAT", sizeof(v->label) - 1);
    v->mounted = true;
    debug_printf("[exfat] mounted: root_clus=%u clusters=%u spc=%u fat_lba=%u heap_lba=%u\n",
                 v->root_cluster, v->cluster_count, v->sec_per_clus,
                 v->fat_lba, v->heap_lba);
    return true;
}

/* ---------- Directory traversal ----------------------------------------- *
 * Walk a directory's entry stream sequentially (across clusters), assembling
 * File(0x85)+Stream(0xC0)+Name(0xC1...) sets.  For each completed set, invoke
 * `emit` with the decoded name + metadata.  `emit` returns false to stop.
 * Returns 0 normally. */
typedef bool (*ex_dir_cb)(void *ctx, const char *name, bool is_dir,
                          uint32_t first_clus, uint64_t size, bool contig);

static int ex_walk_dir(exfat_volume_t *v, uint32_t dir_first_clus,
                       bool dir_contig, ex_dir_cb emit, void *ctx) {
    uint32_t cluster = dir_first_clus;
    uint32_t clusters_seen = 0;

    /* Pending entry-set assembly state. */
    bool     have_file = false;
    int      secondaries_left = 0;   /* entries still expected after 0x85   */
    bool     is_dir = false;
    bool     contig = false;
    uint32_t first_clus = 0;
    uint64_t data_len = 0;
    int      name_units = 0;         /* total UTF-16 units expected         */
    int      name_got   = 0;
    char     name[256];
    int      name_len = 0;

    while (cluster >= 2 && cluster < EX_EOC && clusters_seen < EX_MAX_CLUSTERS) {
        uint32_t base = clus_lba(v, cluster);
        for (uint32_t s = 0; s < v->sec_per_clus; s++) {
            uint8_t sec[EXSEC];
            if (ex_rd(v, base + s, sec) != 0) return -1;
            for (int e = 0; e < (int)(EXSEC / 32); e++) {
                const uint8_t *ent = sec + e * 32;
                uint8_t type = ent[0];

                if (type == 0x00) return 0;          /* end of directory */

                if (type == EX_ENT_FILE) {
                    have_file = true;
                    secondaries_left = ent[1];        /* SecondaryCount */
                    uint16_t attr = rd16(ent + 4);
                    is_dir = (attr & EX_ATTR_DIR) != 0;
                    first_clus = 0; data_len = 0; contig = false;
                    name_units = 0; name_got = 0; name_len = 0; name[0] = 0;
                    continue;
                }
                if (!have_file) continue;             /* skip stray entries */

                if (type == EX_ENT_STREAM) {
                    contig     = (ent[1] & EX_FLAG_NOFATCHAIN) != 0;
                    name_units = ent[3];              /* NameLength */
                    first_clus = rd32(ent + 20);
                    data_len   = (uint64_t)rd32(ent + 24) |
                                 ((uint64_t)rd32(ent + 28) << 32);
                    secondaries_left--;
                    continue;
                }
                if (type == EX_ENT_NAME) {
                    for (int k = 0; k < 15 && name_got < name_units; k++) {
                        uint16_t cu = rd16(ent + 2 + k * 2);
                        name_len = utf8_put(name, name_len, (int)sizeof(name), cu);
                        name_got++;
                    }
                    secondaries_left--;
                    if (secondaries_left <= 0 && name_got >= name_units) {
                        name[name_len] = 0;
                        have_file = false;
                        if (name[0] &&
                            !emit(ctx, name, is_dir, first_clus, data_len, contig))
                            return 0;
                    }
                    continue;
                }
                /* Any other 0x8x entry inside a set just counts down. */
                if (type & 0x80) { if (have_file) secondaries_left--; }
            }
        }
        clusters_seen++;
        if (dir_contig) cluster++;
        else            cluster = ex_next_cluster(v, cluster);
    }
    return 0;
}

/* Find one named child inside a directory.  Fills the out-params on hit. */
typedef struct {
    const char *want;
    bool        found;
    bool        is_dir;
    uint32_t    first_clus;
    uint64_t    size;
    bool        contig;
} ex_find_ctx_t;

static bool ex_find_cb(void *c, const char *name, bool is_dir,
                       uint32_t first_clus, uint64_t size, bool contig) {
    ex_find_ctx_t *f = (ex_find_ctx_t *)c;
    if (name_eq(name, f->want)) {
        f->found = true; f->is_dir = is_dir;
        f->first_clus = first_clus; f->size = size; f->contig = contig;
        return false;                                 /* stop */
    }
    return true;
}

/* Resolve a path to its directory's first cluster (for listing) or a file's
 * first cluster/size (for reading).  Returns true on success. */
static bool ex_resolve(exfat_volume_t *v, const char *path, bool want_dir,
                       uint32_t *out_clus, uint64_t *out_size, bool *out_contig) {
    uint32_t cur_clus = v->root_cluster;
    bool     cur_dir  = true;
    bool     cur_contig = false;       /* root walks the FAT chain */
    uint64_t cur_size = 0;

    const char *p = path;
    while (*p == '/') p++;
    char comp[256];

    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;
        if (!comp[0]) continue;
        if (!cur_dir) return false;     /* tried to descend into a file */

        ex_find_ctx_t f = { comp, false, false, 0, 0, false };
        ex_walk_dir(v, cur_clus, cur_contig, ex_find_cb, &f);
        if (!f.found) return false;
        cur_clus   = f.first_clus;
        cur_dir    = f.is_dir;
        cur_contig = f.contig;
        cur_size   = f.size;
    }

    if (want_dir && !cur_dir) return false;
    if (!want_dir && cur_dir) return false;
    if (out_clus)   *out_clus   = cur_clus;
    if (out_size)   *out_size   = cur_size;
    if (out_contig) *out_contig = cur_contig;
    return true;
}

/* ---------- Public: list ------------------------------------------------- */
typedef struct { fat_entry_t *out; int max; int n; } ex_list_ctx_t;

static bool ex_list_cb(void *c, const char *name, bool is_dir,
                       uint32_t first_clus, uint64_t size, bool contig) {
    (void)first_clus; (void)contig;
    ex_list_ctx_t *l = (ex_list_ctx_t *)c;
    if (l->n >= l->max) return false;
    fat_entry_t *e = &l->out[l->n++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = 0;
    e->size   = (size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)size;
    e->is_dir = is_dir;
    return true;
}

int exfat_list(exfat_volume_t *v, const char *path, fat_entry_t *out, int max) {
    if (!v || !v->mounted || !out) return -1;
    uint32_t dir_clus; bool dir_contig;
    if (!path || !path[0] || (path[0] == '/' && path[1] == 0)) {
        dir_clus = v->root_cluster; dir_contig = false;
    } else if (!ex_resolve(v, path, true, &dir_clus, NULL, &dir_contig)) {
        return -1;
    }
    ex_list_ctx_t ctx = { out, max, 0 };
    ex_walk_dir(v, dir_clus, dir_contig, ex_list_cb, &ctx);
    return ctx.n;
}

/* ---------- Public: read ------------------------------------------------- */
int exfat_read(exfat_volume_t *v, const char *path, void *buf, uint32_t cap) {
    if (!v || !v->mounted || !path) return -1;
    uint32_t first_clus; uint64_t size; bool contig;
    if (!ex_resolve(v, path, false, &first_clus, &size, &contig)) return -1;

    uint32_t total = (size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)size;
    uint32_t copied = 0;
    uint32_t cluster = first_clus;
    uint32_t clus_bytes = v->sec_per_clus * EXSEC;
    uint32_t guard = 0;

    while (cluster >= 2 && cluster < EX_EOC && copied < total &&
           guard++ < EX_MAX_CLUSTERS) {
        uint32_t base = clus_lba(v, cluster);
        for (uint32_t s = 0; s < v->sec_per_clus && copied < total; s++) {
            uint8_t sec[EXSEC];
            if (ex_rd(v, base + s, sec) != 0) return (int)copied;
            uint32_t want = total - copied;
            if (want > EXSEC) want = EXSEC;
            if (copied < cap) {
                uint32_t chunk = want;
                if (copied + chunk > cap) chunk = cap - copied;
                memcpy((uint8_t *)buf + copied, sec, chunk);
            }
            copied += want;
        }
        (void)clus_bytes;
        if (contig) cluster++;
        else        cluster = ex_next_cluster(v, cluster);
    }
    return (int)total;
}

/* ============================================================================
 * exFAT write support
 * ============================================================================ */

static int ex_wr(exfat_volume_t *v, uint32_t lba, const void *buf) {
    return v->write ? v->write(v->user, lba, buf) : -1;
}

/* Write a FAT entry. */
static int ex_fat_write(exfat_volume_t *v, uint32_t cluster, uint32_t val) {
    uint32_t byte = cluster * 4u, lba = v->fat_lba + byte / EXSEC, off = byte % EXSEC;
    uint8_t sec[EXSEC];
    if (ex_rd(v, lba, sec) != 0) return -1;
    sec[off]=(uint8_t)val; sec[off+1]=(uint8_t)(val>>8);
    sec[off+2]=(uint8_t)(val>>16); sec[off+3]=(uint8_t)(val>>24);
    return ex_wr(v, lba, sec);
}

/* Allocation bitmap: one bit per data cluster (cluster 2 = bit 0). */
static bool ex_bmap_get(exfat_volume_t *v, uint32_t c) {
    if (v->bitmap_cluster < 2 || c < 2) return true;
    uint32_t idx=c-2, byte=idx>>3, bit=idx&7;
    uint32_t lba=clus_lba(v,v->bitmap_cluster)+byte/EXSEC, off=byte%EXSEC;
    uint8_t sec[EXSEC];
    if (ex_rd(v,lba,sec) != 0) return true;
    return (sec[off]>>bit)&1;
}
static int ex_bmap_set(exfat_volume_t *v, uint32_t c, bool set) {
    if (v->bitmap_cluster < 2 || c < 2) return -1;
    uint32_t idx=c-2, byte=idx>>3, bit=idx&7;
    uint32_t lba=clus_lba(v,v->bitmap_cluster)+byte/EXSEC, off=byte%EXSEC;
    uint8_t sec[EXSEC];
    if (ex_rd(v,lba,sec) != 0) return -1;
    if (set) sec[off]|=(uint8_t)(1u<<bit); else sec[off]&=~(uint8_t)(1u<<bit);
    return ex_wr(v,lba,sec);
}

/* Allocate one free cluster: scan the bitmap ONE SECTOR (4096 bits) at
 * a time from the next-free hint, mark used, write EOC to the FAT.  The
 * old per-bit scan re-read the same bitmap sector for every candidate
 * cluster - O(n^2) on big sticks. */
static uint32_t ex_alloc_clus(exfat_volume_t *v) {
    uint32_t total = v->cluster_count + 2;
    uint32_t start = (v->alloc_hint >= 2 && v->alloc_hint < total)
                   ? v->alloc_hint : 2;
    uint8_t  sec[EXSEC];
    uint32_t cur_lba = 0xFFFFFFFFu;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t c   = pass ? 2 : start;
        uint32_t end = pass ? start : total;
        for (; c < end; c++) {
            uint32_t idx = c - 2, byte = idx >> 3, bit = idx & 7;
            uint32_t lba = clus_lba(v, v->bitmap_cluster) + byte / EXSEC;
            if (lba != cur_lba) {
                if (ex_rd(v, lba, sec) != 0) return 0;
                cur_lba = lba;
            }
            if (!((sec[byte % EXSEC] >> bit) & 1)) {
                if (ex_bmap_set(v, c, true) != 0) return 0;
                if (ex_fat_write(v, c, EX_EOC_WRITE) != 0) {
                    ex_bmap_set(v, c, false);
                    return 0;
                }
                v->alloc_hint = c + 1;
                return c;
            }
        }
        cur_lba = 0xFFFFFFFFu;
    }
    return 0;
}

/* Free a cluster chain (FAT chain or contiguous run). */
static void ex_free_chain(exfat_volume_t *v, uint32_t first,
                           bool contig, uint64_t len_bytes) {
    if (first<2) return;
    if (contig) {
        uint32_t clus_sz=v->sec_per_clus*EXSEC;
        uint32_t lb32=(uint32_t)len_bytes;
        uint32_t n=(lb32+clus_sz-1)/clus_sz;
        for (uint32_t i=0;i<n;i++) { ex_bmap_set(v,first+i,false); ex_fat_write(v,first+i,0); }
        return;
    }
    uint32_t c=first, g=0;
    while (c>=2 && c<EX_EOC && g++<EX_MAX_CLUSTERS) {
        uint32_t nx=ex_next_cluster(v,c); ex_bmap_set(v,c,false); ex_fat_write(v,c,0); c=nx;
    }
}

/* NameHash per exFAT spec (ASCII upcase approximation for ASCII filenames). */
static uint16_t ex_name_hash(const uint16_t *u, int n) {
    uint16_t h=0;
    for (int i=0;i<n;i++) {
        uint16_t cu=u[i]; if (cu>='a'&&cu<='z') cu-=32;
        h=((h>>1)|(h<<15))+(uint8_t)cu;
        h=((h>>1)|(h<<15))+(uint8_t)(cu>>8);
    }
    return h;
}

/* Entry-set checksum per exFAT spec §6.3.
 * Bytes 2-3 of entry[0] (SetChecksum field) are excluded. */
static uint16_t ex_set_cs(const uint8_t *ents, int n) {
    uint16_t cs=0;
    for (int i=0;i<n*32;i++) {
        if (i==2||i==3) continue;
        cs=((cs>>1)|(cs<<15))+ents[i];
    }
    return cs;
}

/* UTF-8 → UTF-16LE (BMP only). Returns code-unit count written. */
static int ex_u8_to_u16(const char *s, uint16_t *d, int cap) {
    int n=0;
    while (*s && n<cap) {
        unsigned char c=(unsigned char)*s;
        uint32_t cu;
        if      (c<0x80)  { cu=c; s++; }
        else if (c<0xE0)  { cu=((c&0x1F)<<6)|(s[1]&0x3F); s+=2; }
        else              { cu=((c&0x0F)<<12)|((s[1]&0x3F)<<6)|(s[2]&0x3F); s+=3; }
        d[n++]=(uint16_t)cu;
    }
    return n;
}

#define EX_TS  ((uint32_t)((44u<<25)|(1u<<21)|(1u<<16)))  /* 2024-01-01 */
#define EX_UTC 0x80u

/* Build a complete directory entry set (0x85+0xC0+0xC1..) into buf[].
 * `sec_flags` = GeneralSecondaryFlags for the stream entry (0x01 = FAT
 * chain, 0x03 = NoFatChain/contiguous).  Returns number of 32-byte
 * entries, or 0 on error. */
static int ex_build_set(uint8_t *buf, const char *name, uint16_t attr,
                        uint32_t first_clus, uint64_t data_len,
                        uint8_t sec_flags) {
    uint16_t u[255]; int nlen=ex_u8_to_u16(name,u,255);
    if (nlen<1) return 0;
    int nn=(nlen+14)/15;            /* name entry count */
    int total=1+1+nn;
    memset(buf,0,total*32);

    /* 0x85 File entry */
    buf[0]=EX_ENT_FILE; buf[1]=(uint8_t)(1+nn);
    buf[4]=(uint8_t)(attr?attr:0x20); buf[5]=(uint8_t)((attr?attr:0x20)>>8);
    uint32_t ts=EX_TS;
    for (int i=0;i<3;i++) { *(uint32_t*)(buf+8+i*4)=ts; }
    buf[22]=EX_UTC; buf[23]=EX_UTC; buf[24]=EX_UTC;

    /* 0xC0 Stream extension */
    uint8_t *s=buf+32;
    s[0]=EX_ENT_STREAM; s[1]=sec_flags;
    s[3]=(uint8_t)nlen;
    uint16_t nh=ex_name_hash(u,nlen); s[4]=(uint8_t)nh; s[5]=(uint8_t)(nh>>8);
    *(uint64_t*)(s+8)=data_len;      /* ValidDataLength */
    *(uint32_t*)(s+20)=first_clus;   /* FirstCluster */
    *(uint64_t*)(s+24)=data_len;     /* DataLength */

    /* 0xC1 Name entries */
    int ni=0;
    for (int k=0;k<nn;k++) {
        uint8_t *ne=buf+(2+k)*32;
        ne[0]=EX_ENT_NAME; ne[1]=0x01;
        for (int j=0;j<15&&ni<nlen;j++,ni++) {
            ne[2+j*2]=(uint8_t)u[ni]; ne[3+j*2]=(uint8_t)(u[ni]>>8);
        }
    }

    /* Checksum */
    uint16_t cs=ex_set_cs(buf,total); buf[2]=(uint8_t)cs; buf[3]=(uint8_t)(cs>>8);
    return total;
}

/* Write data to a freshly-chained cluster list.
 * clusters: array of cluster numbers in order, nclus: count. */
static int ex_write_data(exfat_volume_t *v, const uint8_t *data, uint32_t len,
                         const uint32_t *clusters, int nclus) {
    uint32_t clus_bytes=v->sec_per_clus*EXSEC;
    uint32_t written=0;
    uint8_t sec[EXSEC];
    for (int ci=0;ci<nclus;ci++) {
        uint32_t base=clus_lba(v,clusters[ci]);
        for (uint32_t s=0;s<v->sec_per_clus;s++) {
            uint32_t off=ci*clus_bytes+s*EXSEC;
            uint32_t want=off<len ? (len-off>=EXSEC?EXSEC:len-off) : 0;
            if (want>0) memcpy(sec,data+off,want); else want=0;
            if (want<EXSEC) memset(sec+want,0,EXSEC-want);
            if (ex_wr(v,base+s,sec)!=0) return -1;
            written+=want;
        }
    }
    (void)written;
    return 0;
}

/* Directory entry location */
typedef struct { uint32_t lba; uint32_t off; } ex_eloc_t;
#define EX_MAX_SET_ENTS 19

/* Scan a directory for a matching entry and/or end-of-directory.
 * On return, if found: scan.n_ents > 0 and scan.locs[] has entry locations.
 * eod_found: true when we saw a type==0x00 entry (scan.eod_lba/off give it).
 * last_clus: last cluster in the directory chain. */
typedef struct {
    const char *want;
    /* Hit */
    bool        found;
    bool        is_dir;
    uint32_t    first_clus;
    uint64_t    size;
    bool        contig;
    int         n_ents;
    ex_eloc_t   locs[EX_MAX_SET_ENTS];
    /* End-of-directory */
    bool        eod_found;
    ex_eloc_t   eod;
    /* Last cluster in chain (for extension) */
    uint32_t    last_clus;
} ex_scan_t;

static int ex_scan_dir(exfat_volume_t *v, uint32_t dir_clus, bool dir_contig,
                       ex_scan_t *sc) {
    uint32_t cluster=dir_clus, guard=0;
    /* in-flight entry-set assembly */
    bool  in_set=false;
    int   sec_left=0;     /* secondary entries still to collect */
    int   ni=0;           /* name index inside current set */
    int   nlen=0;         /* total name length expected */
    char  name[256]; int  namelen=0;
    bool  is_dir=false; bool contig=false;
    uint32_t fclus=0; uint64_t fsize=0;

    while (cluster>=2 && cluster<EX_EOC && guard++<EX_MAX_CLUSTERS) {
        sc->last_clus=cluster;
        uint32_t base=clus_lba(v,cluster);
        for (uint32_t s=0;s<v->sec_per_clus;s++) {
            uint8_t sec[EXSEC];
            if (ex_rd(v,base+s,sec)!=0) return -1;
            for (int e=0;e<(int)(EXSEC/32);e++) {
                uint8_t *ent=sec+e*32;
                uint8_t type=ent[0];
                uint32_t cur_lba=base+s; uint32_t cur_off=(uint32_t)e*32;

                if (type==0x00) {
                    sc->eod_found=true; sc->eod.lba=cur_lba; sc->eod.off=cur_off;
                    return 0;
                }
                if (type==EX_ENT_FILE) {
                    in_set=true; sec_left=ent[1];
                    uint16_t attr=rd16(ent+4); is_dir=(attr&EX_ATTR_DIR)!=0;
                    fclus=0; fsize=0; contig=false; ni=0; nlen=0; namelen=0; name[0]=0;
                    sc->n_ents=0;          /* fresh set starts here       */
                    sc->locs[0].lba=cur_lba; sc->locs[0].off=cur_off;
                    sc->n_ents=1;
                    continue;
                }
                if (!in_set) continue;
                if (type==EX_ENT_STREAM) {
                    contig=(ent[1]&EX_FLAG_NOFATCHAIN)!=0;
                    nlen=ent[3]; fclus=rd32(ent+20);
                    fsize=(uint64_t)rd32(ent+24)|((uint64_t)rd32(ent+28)<<32);
                    sec_left--;
                    if (sc->n_ents<EX_MAX_SET_ENTS)
                        sc->locs[sc->n_ents].lba=cur_lba, sc->locs[sc->n_ents].off=cur_off;
                    sc->n_ents++;
                    continue;
                }
                if (type==EX_ENT_NAME) {
                    if (sc->n_ents<EX_MAX_SET_ENTS)
                        sc->locs[sc->n_ents].lba=cur_lba, sc->locs[sc->n_ents].off=cur_off;
                    sc->n_ents++;
                    for (int k=0;k<15&&ni<nlen;k++,ni++)
                        namelen=utf8_put(name,namelen,(int)sizeof(name),rd16(ent+2+k*2));
                    sec_left--;
                    if (sec_left<=0 && ni>=nlen) {
                        name[namelen]=0;
                        in_set=false;
                        if (sc->want && name_eq(name,sc->want)) {
                            sc->found=true; sc->is_dir=is_dir;
                            sc->first_clus=fclus; sc->size=fsize; sc->contig=contig;
                            return 0;
                        }
                        sc->n_ents=0;  /* reset for next set */
                    }
                    continue;
                }
                if (type&0x80) { if (in_set) { sec_left--;
                    if (sc->n_ents<EX_MAX_SET_ENTS)
                        sc->locs[sc->n_ents].lba=cur_lba, sc->locs[sc->n_ents].off=cur_off;
                    sc->n_ents++; } }
            }
        }
        if (dir_contig) cluster++;
        else            cluster=ex_next_cluster(v,cluster);
    }
    return 0;
}

/* Split a path into dirname + basename. */
static void ex_split_path(const char *path, char *dir, char *base, int cap) {
    const char *last=path;
    for (const char *p=path; *p; p++) if (*p=='/') last=p;
    if (last==path) { dir[0]='/'; dir[1]=0; strncpy(base,path+(path[0]=='/'?1:0),cap-1); base[cap-1]=0; }
    else {
        int dlen=(int)(last-path); if (dlen==0) dlen=1;
        int d=dlen<cap-1?dlen:cap-2; memcpy(dir,path,d); dir[d]=0;
        strncpy(base,last+1,cap-1); base[cap-1]=0;
    }
}

/* ---- Directory reference -------------------------------------------------
 * A directory plus (for non-root dirs) the on-disk location of its OWN
 * entry set in the parent.  Extending a directory must keep the entry
 * set's DataLength + checksum in sync or fsck flags the volume. */
typedef struct {
    uint32_t  first_clus;
    bool      contig;
    uint32_t  alloc_bytes;     /* DataLength (dirs are always < 4 GiB)   */
    bool      is_root;
    int       n_ents;
    ex_eloc_t locs[EX_MAX_SET_ENTS];
} ex_dirref_t;

static int ex_dirref_resolve(exfat_volume_t *v, const char *dirpath,
                             ex_dirref_t *out) {
    memset(out, 0, sizeof(*out));
    if (!dirpath || !dirpath[0] || (dirpath[0]=='/' && !dirpath[1])) {
        out->first_clus = v->root_cluster;
        out->contig     = false;          /* root always walks the FAT   */
        out->is_root    = true;
        return 0;
    }
    char pdir[256], base[256];
    ex_split_path(dirpath, pdir, base, 256);
    ex_dirref_t parent;
    if (ex_dirref_resolve(v, pdir, &parent) != 0) return -1;
    ex_scan_t sc; memset(&sc,0,sizeof(sc)); sc.want=base;
    if (ex_scan_dir(v, parent.first_clus, parent.contig, &sc) != 0) return -1;
    if (!sc.found || !sc.is_dir) return -1;
    out->first_clus  = sc.first_clus;
    out->contig      = sc.contig;
    out->alloc_bytes = (uint32_t)sc.size;
    out->is_root     = false;
    out->n_ents      = sc.n_ents;
    memcpy(out->locs, sc.locs, sizeof(out->locs));
    return 0;
}

/* Rewrite the directory's own stream entry after growth: DataLength,
 * ValidDataLength, optional NoFatChain clear, fresh set checksum. */
static int ex_dir_update_meta(exfat_volume_t *v, ex_dirref_t *dr,
                              uint32_t new_bytes, bool clear_contig) {
    if (dr->is_root) return 0;
    if (dr->n_ents < 2) return -1;
    uint8_t set[EX_MAX_SET_ENTS*32];
    uint8_t sec[EXSEC];
    for (int i=0;i<dr->n_ents;i++) {
        if (ex_rd(v, dr->locs[i].lba, sec)!=0) return -1;
        memcpy(set+i*32, sec+dr->locs[i].off, 32);
    }
    uint8_t *s = set+32;
    *(uint64_t*)(s+8)  = new_bytes;       /* ValidDataLength             */
    *(uint64_t*)(s+24) = new_bytes;       /* DataLength                  */
    if (clear_contig) s[1] &= (uint8_t)~EX_FLAG_NOFATCHAIN;
    uint16_t cs = ex_set_cs(set, dr->n_ents);
    set[2]=(uint8_t)cs; set[3]=(uint8_t)(cs>>8);
    for (int i=0;i<dr->n_ents;i++) {
        if (ex_rd(v, dr->locs[i].lba, sec)!=0) return -1;
        memcpy(sec+dr->locs[i].off, set+i*32, 32);
        if (ex_wr(v, dr->locs[i].lba, sec)!=0) return -1;
    }
    dr->alloc_bytes = new_bytes;
    if (clear_contig) dr->contig = false;
    return 0;
}

/* Next cluster of a directory WITHOUT growing it (0 = none). */
static uint32_t ex_dir_peek_next(exfat_volume_t *v, const ex_dirref_t *dr,
                                 uint32_t cur) {
    if (!dr->contig) {
        uint32_t nx = ex_next_cluster(v, cur);
        return (nx >= 2 && nx < EX_EOC) ? nx : 0;
    }
    uint32_t clus_bytes = v->sec_per_clus * EXSEC;
    uint32_t nclus = (dr->alloc_bytes + clus_bytes - 1) / clus_bytes;
    uint32_t idx   = cur - dr->first_clus + 1;
    return (idx < nclus) ? cur + 1 : 0;
}

/* Next cluster of a directory, growing the allocation when needed.
 * FAT-chained dirs (root + everything WE create) extend anywhere;
 * contiguous (Windows-written) dirs extend only when the next physical
 * cluster is free - converting NoFatChain to a real chain is out of
 * scope, we fail cleanly instead of corrupting. */
static uint32_t ex_dir_next_or_grow(exfat_volume_t *v, ex_dirref_t *dr,
                                    uint32_t cur) {
    uint32_t nx = ex_dir_peek_next(v, dr, cur);
    if (nx) return nx;
    uint32_t clus_bytes = v->sec_per_clus * EXSEC;
    uint8_t  z[EXSEC];
    if (!dr->contig) {
        uint32_t nc = ex_alloc_clus(v);
        if (!nc) return 0;
        if (ex_fat_write(v, cur, nc) != 0) return 0;
        memset(z, 0, EXSEC);
        uint32_t b = clus_lba(v, nc);
        for (uint32_t s = 0; s < v->sec_per_clus; s++) ex_wr(v, b+s, z);
        if (!dr->is_root &&
            ex_dir_update_meta(v, dr, dr->alloc_bytes + clus_bytes,
                               false) != 0) return 0;
        debug_printf("[exfat] extended dir: +cluster %u\n", nc);
        return nc;
    }
    uint32_t nclus = (dr->alloc_bytes + clus_bytes - 1) / clus_bytes;
    uint32_t cand  = dr->first_clus + nclus;
    if (cand < v->cluster_count + 2 && !ex_bmap_get(v, cand)) {
        if (ex_bmap_set(v, cand, true) != 0) return 0;
        memset(z, 0, EXSEC);
        uint32_t b = clus_lba(v, cand);
        for (uint32_t s = 0; s < v->sec_per_clus; s++) ex_wr(v, b+s, z);
        if (ex_dir_update_meta(v, dr, dr->alloc_bytes + clus_bytes,
                               false) != 0) return 0;
        debug_printf("[exfat] extended contiguous dir: +cluster %u\n", cand);
        return cand;
    }
    debug_printf("[exfat] cannot extend contiguous dir (cluster %u busy)\n",
                 cand);
    return 0;
}

/* Write N 32-byte entries at the end-of-directory + a fresh terminator,
 * spanning sectors AND clusters (the directory grows on demand). */
static int ex_append_entries(exfat_volume_t *v, ex_dirref_t *dr,
                             uint32_t eod_clus, ex_eloc_t eod,
                             const uint8_t *ents, int n_ents) {
    uint32_t lba = eod.lba, off = eod.off;
    uint32_t cur = eod_clus;

    for (int ei = 0; ei <= n_ents; ei++) {
        uint8_t sec[EXSEC];
        if (ex_rd(v, lba, sec) != 0) return -1;
        if (ei < n_ents) memcpy(sec+off, ents+ei*32, 32);
        else             memset(sec+off, 0, 32);  /* terminator           */
        if (ex_wr(v, lba, sec) != 0) return -1;

        off += 32;
        if (off >= EXSEC) {
            off = 0;
            lba++;
            if (lba >= clus_lba(v, cur) + v->sec_per_clus) {
                if (ei == n_ents) break;   /* terminator can end with the
                                            * chain - nothing follows     */
                uint32_t nx;
                if (ei == n_ents - 1) {
                    /* Only the terminator left: write it only where a
                     * cluster ALREADY exists (stale bytes there would
                     * derail the walker); end-of-chain terminates too. */
                    nx = ex_dir_peek_next(v, dr, cur);
                    if (!nx) return 0;
                } else {
                    nx = ex_dir_next_or_grow(v, dr, cur);
                    if (!nx) return -1;
                }
                cur = nx;
                lba = clus_lba(v, cur);
            }
        }
    }
    return 0;
}

/* ---- Public write API ---------------------------------------------------- */

static void ex_rc_invalidate(exfat_volume_t *v) { v->rc_path[0] = 0; }

/* Delete a single entry.  -2 = directory not empty. */
static int ex_delete_one(exfat_volume_t *v, const char *path) {
    char dir[256], base[256];
    ex_split_path(path, dir, base, 256);
    ex_dirref_t dr;
    if (ex_dirref_resolve(v, dir, &dr) != 0) return -1;

    ex_scan_t sc; memset(&sc,0,sizeof(sc)); sc.want=base;
    if (ex_scan_dir(v, dr.first_clus, dr.contig, &sc) != 0) return -1;
    if (!sc.found) return -1;

    if (sc.is_dir) {
        fat_entry_t probe[1];
        if (exfat_list(v, path, probe, 1) > 0) return -2;
    }

    ex_free_chain(v, sc.first_clus, sc.contig, sc.size);
    for (int i=0; i<sc.n_ents; i++) {
        uint8_t sec[EXSEC];
        if (ex_rd(v, sc.locs[i].lba, sec) != 0) continue;
        sec[sc.locs[i].off] &= ~0x80u;
        ex_wr(v, sc.locs[i].lba, sec);
    }
    return 0;
}

int exfat_delete(exfat_volume_t *v, const char *path) {
    if (!v || !v->mounted || !v->write || !path) return -1;
    ex_rc_invalidate(v);
    int r = ex_delete_one(v, path);
    if (r != -2) return r;

    /* Non-empty directory: deepest-leaf-first iterative delete (same
     * pattern as fat_delete - recursion would stack 512-byte sector
     * buffers per level and the tree depth is unbounded). */
    static fat_entry_t tree_ents[64];      /* single-threaded FS layer    */
    char cur[300];
    for (int guard = 0; guard < 4096; guard++) {
        strncpy(cur, path, sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = 0;
        bool descended = true;
        while (descended) {
            descended = false;
            int n = exfat_list(v, cur, tree_ents, 64);
            if (n < 0) return -1;
            for (int i = 0; i < n; i++) {
                size_t cl = strlen(cur);
                if (cl + strlen(tree_ents[i].name) + 2 >= sizeof(cur))
                    return -1;
                if (cl == 0 || cur[cl - 1] != '/') strcat(cur, "/");
                strcat(cur, tree_ents[i].name);
                if (tree_ents[i].is_dir) descended = true;
                break;                     /* take the FIRST entry        */
            }
            if (!descended && strcmp(cur, path) != 0) {
                if (ex_delete_one(v, cur) != 0) return -1;
            }
        }
        r = ex_delete_one(v, path);
        if (r == 0) {
            debug_printf("[exfat] deleted tree '%s'\n", path);
            return 0;
        }
        if (r != -2) return r;
    }
    return -1;
}

/* ---- Streaming write ------------------------------------------------------
 * Cluster-accumulation buffer sized for the biggest cluster real sticks
 * use (128 KiB on >32 GiB volumes).  One open stream per volume; the
 * entry set appears only at commit so aborted copies leave nothing. */
static uint8_t g_ex_ws_buf[128 * 1024];

static int ex_ws_flush_cluster(exfat_volume_t *v) {
    uint32_t c = ex_alloc_clus(v);
    if (!c) return -1;
    if (v->ws_prev && ex_fat_write(v, v->ws_prev, c) != 0) return -1;
    if (!v->ws_first) v->ws_first = c;
    uint32_t base = clus_lba(v, c);
    uint8_t  sec[EXSEC];
    for (uint32_t s = 0; s < v->sec_per_clus; s++) {
        uint32_t off  = s * EXSEC;
        uint32_t want = (off < v->ws_buffered)
                      ? (v->ws_buffered - off >= EXSEC ? EXSEC
                                                       : v->ws_buffered - off)
                      : 0;
        if (want) memcpy(sec, g_ex_ws_buf + off, want);
        if (want < EXSEC) memset(sec + want, 0, EXSEC - want);
        if (ex_wr(v, base + s, sec) != 0) return -1;
    }
    v->ws_prev = c;
    v->ws_buffered = 0;
    return 0;
}

int exfat_write_open(exfat_volume_t *v, const char *path) {
    if (!v || !v->mounted || !v->write || !path) return -1;
    if (v->ws_active) return -1;
    if (v->sec_per_clus * EXSEC > sizeof(g_ex_ws_buf)) {
        debug_printf("[exfat] cluster size %u exceeds stream buffer\n",
                     v->sec_per_clus * EXSEC);
        return -1;
    }
    /* Overwrite semantics: retire an existing FILE up front. */
    int r = ex_delete_one(v, path);
    if (r == -2) return -1;               /* target is a non-empty dir   */

    v->ws_active = true;
    strncpy(v->ws_path, path, sizeof(v->ws_path) - 1);
    v->ws_path[sizeof(v->ws_path) - 1] = 0;
    v->ws_first = v->ws_prev = 0;
    v->ws_len = 0;
    v->ws_buffered = 0;
    ex_rc_invalidate(v);
    return 0;
}

int exfat_write_append(exfat_volume_t *v, const void *data, uint32_t len) {
    if (!v || !v->ws_active) return -1;
    uint32_t clus_bytes = v->sec_per_clus * EXSEC;
    const uint8_t *src = (const uint8_t *)data;
    while (len > 0) {
        uint32_t space = clus_bytes - v->ws_buffered;
        uint32_t take  = (len < space) ? len : space;
        memcpy(g_ex_ws_buf + v->ws_buffered, src, take);
        v->ws_buffered += take;
        v->ws_len      += take;
        src += take;
        len -= take;
        if (v->ws_buffered == clus_bytes) {
            if (ex_ws_flush_cluster(v) != 0) {
                exfat_write_close(v, false);
                return -1;
            }
        }
    }
    return 0;
}

int exfat_write_close(exfat_volume_t *v, bool commit) {
    if (!v || !v->ws_active) return -1;
    v->ws_active = false;
    ex_rc_invalidate(v);
    if (!commit) {
        if (v->ws_first) ex_free_chain(v, v->ws_first, false, 0);
        return 0;
    }
    if (v->ws_buffered > 0) {
        if (ex_ws_flush_cluster(v) != 0) {
            if (v->ws_first) ex_free_chain(v, v->ws_first, false, 0);
            return -1;
        }
    }
    char dir[256], base[256];
    ex_split_path(v->ws_path, dir, base, 256);
    ex_dirref_t dr;
    if (ex_dirref_resolve(v, dir, &dr) != 0) goto fail;
    {
        ex_scan_t sc; memset(&sc,0,sizeof(sc));
        if (ex_scan_dir(v, dr.first_clus, dr.contig, &sc) != 0) goto fail;
        if (!sc.eod_found) goto fail;
        uint8_t set_buf[EX_MAX_SET_ENTS*32];
        int n_ents = ex_build_set(set_buf, base, 0x20,
                                  v->ws_first, v->ws_len, 0x01);
        if (n_ents <= 0) goto fail;
        if (ex_append_entries(v, &dr, sc.last_clus, sc.eod,
                              set_buf, n_ents) != 0) goto fail;
    }
    debug_printf("[exfat] streamed file '%s' (%u bytes, cluster %u)\n",
                 base, v->ws_len, v->ws_first);
    return 0;
fail:
    if (v->ws_first) ex_free_chain(v, v->ws_first, false, 0);
    return -1;
}

int exfat_write_file(exfat_volume_t *v, const char *path,
                     const void *data, uint32_t len) {
    if (exfat_write_open(v, path) != 0) return -1;
    if (exfat_write_append(v, data, len) != 0) return -1;
    return exfat_write_close(v, true);
}

int exfat_mkdir(exfat_volume_t *v, const char *path) {
    if (!v || !v->mounted || !v->write || !path) return -1;
    ex_rc_invalidate(v);

    char dir[256], base[256];
    ex_split_path(path, dir, base, 256);
    ex_dirref_t dr;
    if (ex_dirref_resolve(v, dir, &dr) != 0) return -1;

    /* Refuse when the name is taken (the old code silently deleted). */
    {
        ex_scan_t sc; memset(&sc,0,sizeof(sc)); sc.want=base;
        if (ex_scan_dir(v, dr.first_clus, dr.contig, &sc) != 0) return -1;
        if (sc.found) return -1;
    }

    ex_scan_t sc; memset(&sc,0,sizeof(sc));
    if (ex_scan_dir(v, dr.first_clus, dr.contig, &sc) != 0) return -1;
    if (!sc.eod_found) return -1;

    uint32_t dc = ex_alloc_clus(v);
    if (!dc) return -1;
    uint8_t zero[EXSEC]; memset(zero,0,EXSEC);
    uint32_t b = clus_lba(v, dc);
    for (uint32_t s = 0; s < v->sec_per_clus; s++) ex_wr(v, b+s, zero);

    /* Directories carry their allocation in DataLength (one cluster) -
     * DataLength 0 with a FirstCluster set trips fsck. */
    uint8_t set_buf[EX_MAX_SET_ENTS*32];
    int n_ents = ex_build_set(set_buf, base, EX_ATTR_DIR, dc,
                              v->sec_per_clus * EXSEC, 0x01);
    if (n_ents <= 0) { ex_bmap_set(v,dc,false); ex_fat_write(v,dc,0); return -1; }
    return ex_append_entries(v, &dr, sc.last_clus, sc.eod, set_buf, n_ents);
}

/* ---- Rename ---------------------------------------------------------------
 * Same-directory: write a fresh entry set (new name, same data chain +
 * flags), then retire the old set.  Crash-safe ordering: a transient
 * duplicate is possible, a lost file is not. */
int exfat_rename(exfat_volume_t *v, const char *path, const char *new_name) {
    if (!v || !v->mounted || !v->write || !path) return -1;
    if (!new_name || !new_name[0]) return -2;
    for (const char *p = new_name; *p; p++) {
        char c = *p;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' ||
            (unsigned char)c < 0x20) return -2;
    }
    if (strlen(new_name) > 200) return -2;
    ex_rc_invalidate(v);

    char dir[256], base[256];
    ex_split_path(path, dir, base, 256);
    ex_dirref_t dr;
    if (ex_dirref_resolve(v, dir, &dr) != 0) return -1;

    ex_scan_t dup; memset(&dup,0,sizeof(dup)); dup.want=new_name;
    if (ex_scan_dir(v, dr.first_clus, dr.contig, &dup) != 0) return -1;
    if (dup.found) return -3;

    ex_scan_t old; memset(&old,0,sizeof(old)); old.want=base;
    if (ex_scan_dir(v, dr.first_clus, dr.contig, &old) != 0) return -1;
    if (!old.found) return -1;

    ex_scan_t eod; memset(&eod,0,sizeof(eod));
    if (ex_scan_dir(v, dr.first_clus, dr.contig, &eod) != 0) return -1;
    if (!eod.eod_found) return -1;

    uint8_t set_buf[EX_MAX_SET_ENTS*32];
    int n_ents = ex_build_set(set_buf, new_name,
                              old.is_dir ? EX_ATTR_DIR : 0x20,
                              old.first_clus, old.size,
                              old.contig ? (0x01|EX_FLAG_NOFATCHAIN) : 0x01);
    if (n_ents <= 0) return -2;
    if (ex_append_entries(v, &dr, eod.last_clus, eod.eod,
                          set_buf, n_ents) != 0) return -1;

    for (int i = 0; i < old.n_ents; i++) {
        uint8_t sec[EXSEC];
        if (ex_rd(v, old.locs[i].lba, sec) != 0) return -1;
        sec[old.locs[i].off] &= ~0x80u;
        if (ex_wr(v, old.locs[i].lba, sec) != 0) return -1;
    }
    debug_printf("[exfat] renamed '%s' -> '%s'\n", base, new_name);
    return 0;
}

/* ---- Chunked read with a sequential cursor ------------------------------ */
int exfat_read_at(exfat_volume_t *v, const char *path, uint32_t offset,
                  void *buf, uint32_t len) {
    if (!v || !v->mounted || !path || !buf) return -1;

    uint32_t clus_bytes = v->sec_per_clus * EXSEC;
    bool same = (v->rc_path[0] && strcmp(v->rc_path, path) == 0);
    if (!same || offset < v->rc_pos) {
        uint32_t first; uint64_t size; bool contig;
        if (!ex_resolve(v, path, false, &first, &size, &contig)) return -1;
        strncpy(v->rc_path, path, sizeof(v->rc_path) - 1);
        v->rc_path[sizeof(v->rc_path) - 1] = 0;
        v->rc_first   = first;
        v->rc_size    = size;
        v->rc_contig  = contig;
        v->rc_cluster = first;
        v->rc_pos     = 0;
    }
    uint32_t total = (v->rc_size > 0xFFFFFFFFu) ? 0xFFFFFFFFu
                                                : (uint32_t)v->rc_size;
    if (offset >= total) return 0;
    if (len > total - offset) len = total - offset;
    if (len == 0) return 0;

    while (v->rc_cluster >= 2 && v->rc_cluster < EX_EOC &&
           v->rc_pos + clus_bytes <= offset) {
        v->rc_cluster = v->rc_contig ? v->rc_cluster + 1
                                     : ex_next_cluster(v, v->rc_cluster);
        v->rc_pos    += clus_bytes;
    }
    if (v->rc_cluster < 2 || v->rc_cluster >= EX_EOC) return -1;

    uint32_t copied = 0;
    uint8_t  sec[EXSEC];
    while (copied < len &&
           v->rc_cluster >= 2 && v->rc_cluster < EX_EOC) {
        uint32_t in_off = offset + copied - v->rc_pos;       /* in cluster */
        uint32_t s      = in_off / EXSEC;
        uint32_t s_off  = in_off % EXSEC;
        if (ex_rd(v, clus_lba(v, v->rc_cluster) + s, sec) != 0)
            return (int)(copied ? copied : (uint32_t)-1);
        uint32_t chunk = EXSEC - s_off;
        if (chunk > len - copied) chunk = len - copied;
        memcpy((uint8_t *)buf + copied, sec + s_off, chunk);
        copied += chunk;
        if (in_off + chunk >= clus_bytes) {
            v->rc_cluster = v->rc_contig ? v->rc_cluster + 1
                                         : ex_next_cluster(v, v->rc_cluster);
            v->rc_pos    += clus_bytes;
        }
    }
    return (int)copied;
}

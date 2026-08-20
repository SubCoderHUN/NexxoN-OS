/* ============================================================================
 * NexxoN OS - Baseline JPEG (JFIF) decoder  (v2: correct chroma + RST + fast)
 * ----------------------------------------------------------------------------
 * Implements the subset that covers real-world JFIF images: SOF0/SOF1,
 * 8-bit precision, 1 or 3 components, 4:4:4 / 4:2:2 / 4:2:0 chroma
 * subsampling, Huffman entropy coding, DRI/RSTn restart intervals.
 *
 * v2 rewrite, driven by the thin-client browser symptoms on real pages:
 *   * CHROMA WAS SAMPLED WRONG: the old per-pixel mapping
 *     `sx = px*hsamp/mcu_w` evaluates to 0 for EVERY pixel, so the whole
 *     MCU was coloured from chroma sample (0,0).  Flat test images hid it
 *     (uniform chroma); real pages showed blocky, shifted colours.  The
 *     correct map is px*hsamp/max_hs (precomputed as a shift below).
 *   * RESTART MARKERS were not handled in the entropy stream despite the
 *     old header comment claiming so - any encoder using DRI (Chromium
 *     does on some paths) made the whole frame fail with
 *     "JPEG decode failed".  DRI is parsed and RSTn boundaries re-align
 *     the bit reader + reset DC predictors.
 *   * SPEED: the old reader fetched ONE BIT PER CALL through two layers
 *     of functions (~1M+ calls per frame -> hundreds of ms per 1024x768
 *     frame).  v2 keeps a 32-bit bit-buffer with bytewise stuffing-aware
 *     refill and decodes most Huffman symbols through a 9-bit fast table.
 *   * The ~30 KiB decoder context moved off the 64 KiB kernel stack into
 *     a static instance (single-threaded kernel).
 *
 * Output is 32-bpp ARGB top-down with alpha = 0xFF.
 * ============================================================================ */
#include "jpeg.h"
#include "string.h"
#include "debug.h"
#include "sched.h"

/* ZZ-zigzag order for an 8x8 quantisation/coefficient block. */
static const uint8_t ZZ[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

#define JPEG_FAST_BITS 9

typedef struct {
    int      n;
    int      vals[256];
    int      mincode[17];
    int      maxcode[17];
    int      valptr[17];
    /* Fast path: index by the next JPEG_FAST_BITS bits; low byte = symbol,
     * high byte = code length (0 = not resolvable in FAST_BITS, take the
     * canonical slow path). */
    uint16_t fast[1 << JPEG_FAST_BITS];
} jpeg_huff_t;

typedef struct {
    int id, qt, hd, ha, hsamp, vsamp;
    int dc_pred;
} jpeg_comp_t;

typedef struct {
    const uint8_t *src;
    uint32_t       src_len;
    uint32_t       p;
    /* Bit reader: bits sit left-aligned in bit_buf's top `bit_count` bits. */
    uint32_t       bit_buf;
    int            bit_count;
    bool           hit_marker;      /* refill stopped at an 0xFF marker     */
    /* Image params. */
    uint32_t       width, height;
    int            ncomp;
    jpeg_comp_t    comp[3];
    int            restart_interval;
    /* Tables. */
    int            qt[4][64];
    jpeg_huff_t    huff_dc[4];
    jpeg_huff_t    huff_ac[4];
    int            max_hs, max_vs;
} jpeg_ctx_t;

/* ~30 KiB incl. the Huffman fast tables: keep it OFF the kernel stack.
 * The kernel is single-threaded co-operative, so one instance is safe. */
static jpeg_ctx_t g_jctx;

static int read_u16(jpeg_ctx_t *c) {
    if (c->p + 2 > c->src_len) return -1;
    int v = (c->src[c->p] << 8) | c->src[c->p + 1];
    c->p += 2;
    return v;
}

static int huff_build_jpeg(jpeg_huff_t *h, const uint8_t *counts,
                           const uint8_t *vals) {
    int k = 0;
    int code = 0;
    memset(h->fast, 0, sizeof(h->fast));
    for (int len = 1; len <= 16; len++) {
        h->valptr[len]  = k;
        h->mincode[len] = code;
        for (int i = 0; i < counts[len - 1]; i++) {
            h->vals[k] = vals[k];
            /* Populate every fast-table slot whose top `len` bits equal
             * this code. */
            if (len <= JPEG_FAST_BITS) {
                uint32_t first = (uint32_t)code << (JPEG_FAST_BITS - len);
                uint32_t span  = 1u << (JPEG_FAST_BITS - len);
                for (uint32_t s = 0; s < span; s++)
                    h->fast[first + s] = (uint16_t)((len << 8) | vals[k]);
            }
            k++;
            code++;
        }
        h->maxcode[len] = code - 1;
        code <<= 1;
    }
    h->n = k;
    return 0;
}

/* Refill the bit buffer to >= 25 bits (or until a marker / end of data).
 * 0xFF00 byte stuffing is resolved here; a real marker leaves the reader
 * positioned ON the 0xFF and sets hit_marker. */
static void bits_refill(jpeg_ctx_t *c) {
    while (c->bit_count <= 24) {
        if (c->hit_marker || c->p >= c->src_len) {
            /* Feed zeros so trailing codes can finish; decoders relying on
             * these bits fail later via marker/bounds checks. */
            c->bit_count += 8;
            continue;
        }
        uint8_t b = c->src[c->p++];
        if (b == 0xFF) {
            uint8_t nxt = (c->p < c->src_len) ? c->src[c->p] : 0xD9;
            if (nxt == 0x00) {
                c->p++;                       /* stuffed 0xFF data byte    */
            } else {
                c->p--;                       /* sit on the marker         */
                c->hit_marker = true;
                c->bit_count += 8;            /* zero-pad                  */
                continue;
            }
        }
        c->bit_buf |= (uint32_t)b << (24 - c->bit_count);
        c->bit_count += 8;
    }
}

static inline uint32_t bits_peek(jpeg_ctx_t *c, int n) {
    if (c->bit_count < n) bits_refill(c);
    return (c->bit_buf >> (32 - n)) & ((1u << n) - 1u);
}

static inline void bits_consume(jpeg_ctx_t *c, int n) {
    c->bit_buf <<= n;
    c->bit_count -= n;
}

/* Re-align to a byte boundary and consume an expected RSTn marker.
 * Markers may legally be preceded by any number of 0xFF fill bytes. */
static bool bits_restart(jpeg_ctx_t *c, int idx) {
    c->bit_buf = 0;
    c->bit_count = 0;
    c->hit_marker = false;
    if (c->p + 2 > c->src_len) return false;
    if (c->src[c->p] != 0xFF) return false;
    while (c->p + 2 < c->src_len && c->src[c->p + 1] == 0xFF) c->p++;
    uint8_t m = c->src[c->p + 1];
    if (m != (uint8_t)(0xD0 + (idx & 7))) return false;
    c->p += 2;
    return true;
}

static int decode_huff(jpeg_ctx_t *c, jpeg_huff_t *h) {
    uint32_t look = bits_peek(c, JPEG_FAST_BITS);
    uint16_t f = h->fast[look];
    if (f) {
        bits_consume(c, f >> 8);
        return (int)(f & 0xFF);
    }
    /* Slow path: walk lengths FAST_BITS+1..16 against the canonical codes. */
    int code = (int)look;
    int len  = JPEG_FAST_BITS;
    bits_consume(c, JPEG_FAST_BITS);
    while (len < 16) {
        code = (code << 1) | (int)bits_peek(c, 1);
        bits_consume(c, 1);
        len++;
        if (h->maxcode[len] >= h->mincode[len] && code <= h->maxcode[len] &&
            code >= h->mincode[len]) {
            return h->vals[h->valptr[len] + (code - h->mincode[len])];
        }
    }
    return -1;
}

static inline int receive_extend(jpeg_ctx_t *c, int n) {
    if (n == 0) return 0;
    int v = (int)bits_peek(c, n);
    bits_consume(c, n);
    int vt = 1 << (n - 1);
    return v < vt ? v - (1 << n) + 1 : v;
}

/* AAN integer IDCT. */
static const int IDCT_CONST_W1 = 2841;
static const int IDCT_CONST_W2 = 2676;
static const int IDCT_CONST_W3 = 2408;
static const int IDCT_CONST_W5 = 1609;
static const int IDCT_CONST_W6 = 1108;
static const int IDCT_CONST_W7 =  565;

static void idct_row(int *blk) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (((x1 = blk[4] << 11) | (x2 = blk[6]) | (x3 = blk[2]) | (x4 = blk[1])
        | (x5 = blk[7]) | (x6 = blk[5]) | (x7 = blk[3])) == 0) {
        blk[0] = blk[1] = blk[2] = blk[3] = blk[4] = blk[5] = blk[6] = blk[7]
              = blk[0] << 3;
        return;
    }
    x0 = (blk[0] << 11) + 128;
    x8 = IDCT_CONST_W7 * (x4 + x5);
    x4 = x8 + (IDCT_CONST_W1 - IDCT_CONST_W7) * x4;
    x5 = x8 - (IDCT_CONST_W1 + IDCT_CONST_W7) * x5;
    x8 = IDCT_CONST_W3 * (x6 + x7);
    x6 = x8 - (IDCT_CONST_W3 - IDCT_CONST_W5) * x6;
    x7 = x8 - (IDCT_CONST_W3 + IDCT_CONST_W5) * x7;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = IDCT_CONST_W6 * (x3 + x2);
    x2 = x1 - (IDCT_CONST_W2 + IDCT_CONST_W6) * x2;
    x3 = x1 + (IDCT_CONST_W2 - IDCT_CONST_W6) * x3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    blk[0] = (x7 + x1) >> 8;
    blk[1] = (x3 + x2) >> 8;
    blk[2] = (x0 + x4) >> 8;
    blk[3] = (x8 + x6) >> 8;
    blk[4] = (x8 - x6) >> 8;
    blk[5] = (x0 - x4) >> 8;
    blk[6] = (x3 - x2) >> 8;
    blk[7] = (x7 - x1) >> 8;
}

static void idct_col(int *blk) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (((x1 = blk[8*4] << 8) | (x2 = blk[8*6]) | (x3 = blk[8*2])
        | (x4 = blk[8*1]) | (x5 = blk[8*7]) | (x6 = blk[8*5])
        | (x7 = blk[8*3])) == 0) {
        int v = (blk[0] + 32) >> 6;
        for (int i = 0; i < 8; i++) blk[i*8] = v;
        return;
    }
    x0 = (blk[0] << 8) + 8192;
    x8 = IDCT_CONST_W7 * (x4 + x5) + 4;
    x4 = (x8 + (IDCT_CONST_W1 - IDCT_CONST_W7) * x4) >> 3;
    x5 = (x8 - (IDCT_CONST_W1 + IDCT_CONST_W7) * x5) >> 3;
    x8 = IDCT_CONST_W3 * (x6 + x7) + 4;
    x6 = (x8 - (IDCT_CONST_W3 - IDCT_CONST_W5) * x6) >> 3;
    x7 = (x8 - (IDCT_CONST_W3 + IDCT_CONST_W5) * x7) >> 3;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = IDCT_CONST_W6 * (x3 + x2) + 4;
    x2 = (x1 - (IDCT_CONST_W2 + IDCT_CONST_W6) * x2) >> 3;
    x3 = (x1 + (IDCT_CONST_W2 - IDCT_CONST_W6) * x3) >> 3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    blk[8*0] = (x7 + x1) >> 14;
    blk[8*1] = (x3 + x2) >> 14;
    blk[8*2] = (x0 + x4) >> 14;
    blk[8*3] = (x8 + x6) >> 14;
    blk[8*4] = (x8 - x6) >> 14;
    blk[8*5] = (x0 - x4) >> 14;
    blk[8*6] = (x3 - x2) >> 14;
    blk[8*7] = (x7 - x1) >> 14;
}

static uint8_t clamp8(int v) {
    if (v < 0)  return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static int decode_block(jpeg_ctx_t *c, jpeg_comp_t *comp, int *block) {
    for (int i = 0; i < 64; i++) block[i] = 0;
    /* DC. */
    int t = decode_huff(c, &c->huff_dc[comp->hd]);
    if (t < 0 || t > 15) return -1;
    comp->dc_pred += receive_extend(c, t);
    block[0] = comp->dc_pred * c->qt[comp->qt][0];
    /* AC. */
    int k = 1;
    while (k < 64) {
        int rs = decode_huff(c, &c->huff_ac[comp->ha]);
        if (rs < 0) return -1;
        int s = rs & 0x0F;
        int r = rs >> 4;
        if (s == 0) {
            if (r != 15) break;
            k += 16;
            continue;
        }
        k += r;
        if (k >= 64) return -1;
        block[ZZ[k]] = receive_extend(c, s) * c->qt[comp->qt][ZZ[k]];
        k++;
    }
    /* The block is laid out row-major already (we wrote at ZZ[k]); run the
     * 2D IDCT in place. */
    for (int row = 0; row < 8; row++) idct_row(block + row * 8);
    for (int col = 0; col < 8; col++) idct_col(block + col);
    return 0;
}

static bool jpeg_decode_inner(const uint8_t *src, uint32_t src_len,
                              uint8_t *dst_argb, uint32_t dst_cap,
                              jpeg_info_t *info) {
    if (!src || src_len < 8 || !dst_argb || !info) return false;
    /* The context is ~20 KiB (Huffman tables) - far too big for the 8 KiB
     * scheduler task stacks now that the browser decodes JPEGs in a worker
     * task.  g_jctx is a single static instance; jpeg_decode (the public
     * wrapper below) serialises every caller so sharing it is safe. */
    jpeg_ctx_t *c = &g_jctx;
    memset(c, 0, sizeof(*c));
    c->src = src; c->src_len = src_len;
    /* SOI. */
    if (src[0] != 0xFF || src[1] != 0xD8) return false;
    c->p = 2;
    while (c->p + 4 <= src_len) {
        if (src[c->p] != 0xFF) return false;
        uint8_t m = src[c->p + 1];
        c->p += 2;
        if (m == 0xD9) break;                /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        int seg_len = read_u16(c);
        if (seg_len < 2) return false;
        uint32_t segend = c->p + (uint32_t)seg_len - 2;
        if (segend > src_len) return false;
        switch (m) {
            case 0xDB: {     /* DQT */
                while (c->p < segend) {
                    uint8_t info_byte = src[c->p++];
                    int prec = info_byte >> 4;
                    int id   = info_byte & 0x0F;
                    if (prec != 0 || id > 3) return false;
                    if (c->p + 64 > segend) return false;
                    for (int i = 0; i < 64; i++) c->qt[id][ZZ[i]] = src[c->p++];
                }
                break;
            }
            case 0xC4: {     /* DHT */
                while (c->p < segend) {
                    uint8_t info_byte = src[c->p++];
                    int klass = info_byte >> 4;
                    int id    = info_byte & 0x0F;
                    if (klass > 1 || id > 3) return false;
                    if (c->p + 16 > segend) return false;
                    uint8_t counts[16];
                    int total = 0;
                    for (int i = 0; i < 16; i++) {
                        counts[i] = src[c->p++];
                        total += counts[i];
                    }
                    if (total > 256 || c->p + (uint32_t)total > segend)
                        return false;
                    uint8_t vals[256];
                    for (int i = 0; i < total; i++) vals[i] = src[c->p++];
                    if (klass == 0) huff_build_jpeg(&c->huff_dc[id], counts, vals);
                    else            huff_build_jpeg(&c->huff_ac[id], counts, vals);
                }
                break;
            }
            case 0xDD: {     /* DRI - restart interval (in MCUs) */
                if (segend - c->p < 2) return false;
                c->restart_interval = (src[c->p] << 8) | src[c->p + 1];
                break;
            }
            case 0xC2:       /* SOF2 progressive: unsupported, fail FAST */
                debug_printf("[jpeg] progressive JPEG not supported\n");
                return false;
            case 0xC1:       /* SOF1 extended sequential = same decode path */
            case 0xC0: {     /* SOF0 baseline */
                if (segend - c->p < 6) return false;
                uint8_t prec = src[c->p++];
                if (prec != 8) return false;
                c->height = ((uint32_t)src[c->p] << 8) | src[c->p + 1];
                c->p += 2;
                c->width  = ((uint32_t)src[c->p] << 8) | src[c->p + 1];
                c->p += 2;
                c->ncomp = src[c->p++];
                if (c->ncomp != 1 && c->ncomp != 3) return false;
                if (c->width == 0 || c->height == 0) return false;
                c->max_hs = c->max_vs = 1;
                for (int i = 0; i < c->ncomp; i++) {
                    c->comp[i].id   = src[c->p++];
                    uint8_t f       = src[c->p++];
                    c->comp[i].hsamp = f >> 4;
                    c->comp[i].vsamp = f & 0x0F;
                    if (c->comp[i].hsamp < 1 || c->comp[i].hsamp > 2 ||
                        c->comp[i].vsamp < 1 || c->comp[i].vsamp > 2)
                        return false;        /* 4:4:4 / 4:2:2 / 4:2:0 only */
                    if (i > 0 && (c->comp[i].hsamp != 1 ||
                                  c->comp[i].vsamp != 1))
                        return false;        /* chroma must be 1x1 (JFIF)  */
                    if (c->comp[i].hsamp > c->max_hs) c->max_hs = c->comp[i].hsamp;
                    if (c->comp[i].vsamp > c->max_vs) c->max_vs = c->comp[i].vsamp;
                    c->comp[i].qt   = src[c->p++] & 3;
                }
                break;
            }
            case 0xDA: {     /* SOS */
                int ns = src[c->p++];
                if (ns != c->ncomp) return false;
                for (int i = 0; i < ns; i++) {
                    uint8_t cs = src[c->p++];
                    uint8_t td = src[c->p++];
                    int idx = -1;
                    for (int j = 0; j < c->ncomp; j++)
                        if (c->comp[j].id == cs) { idx = j; break; }
                    if (idx < 0) return false;
                    c->comp[idx].hd = (td >> 4) & 3;
                    c->comp[idx].ha = td & 0x03;
                }
                c->p = segend;
                /* Entropy-coded segment starts here. */
                info->width  = c->width;
                info->height = c->height;
                info->components = c->ncomp;
                info->pixel_bytes = c->width * c->height * 4;
                if (info->pixel_bytes > dst_cap) return false;
                for (int i = 0; i < c->ncomp; i++) c->comp[i].dc_pred = 0;
                c->bit_buf = 0; c->bit_count = 0; c->hit_marker = false;
                int mcu_w = c->max_hs * 8;
                int mcu_h = c->max_vs * 8;
                int mcus_x = (int)((c->width  + (uint32_t)mcu_w - 1) / (uint32_t)mcu_w);
                int mcus_y = (int)((c->height + (uint32_t)mcu_h - 1) / (uint32_t)mcu_h);
                /* Luma -> chroma coordinate shifts (hsamp/vsamp are 1 or 2,
                 * so the ratio max/comp is 1 or 2 = shift 0 or 1).  THIS is
                 * the fixed sampling map: chroma pixel = luma pixel >> shift. */
                int cxs = 0, cys = 0;
                if (c->ncomp == 3) {
                    cxs = (c->max_hs / c->comp[1].hsamp == 2) ? 1 : 0;
                    cys = (c->max_vs / c->comp[1].vsamp == 2) ? 1 : 0;
                }
                int yblocks = c->comp[0].hsamp * c->comp[0].vsamp;
                if (yblocks > 4) return false;
                static int Yblk[4][64];
                static int Cbblk[64];
                static int Crblk[64];
                int rst_countdown = c->restart_interval;
                int rst_idx = 0;
                for (int my = 0; my < mcus_y; my++) {
                    for (int mx = 0; mx < mcus_x; mx++) {
                        /* Restart-interval boundary: byte-align on the RSTn
                         * marker and reset every DC predictor. */
                        if (c->restart_interval &&
                            rst_countdown == 0) {
                            if (!bits_restart(c, rst_idx)) return false;
                            rst_idx = (rst_idx + 1) & 7;
                            for (int i = 0; i < c->ncomp; i++)
                                c->comp[i].dc_pred = 0;
                            rst_countdown = c->restart_interval;
                        }
                        if (c->restart_interval) rst_countdown--;

                        int yi = 0;
                        for (int yv = 0; yv < c->comp[0].vsamp; yv++)
                            for (int yu = 0; yu < c->comp[0].hsamp; yu++)
                                if (decode_block(c, &c->comp[0], Yblk[yi++]) != 0)
                                    return false;
                        if (c->ncomp == 3) {
                            if (decode_block(c, &c->comp[1], Cbblk) != 0) return false;
                            if (decode_block(c, &c->comp[2], Crblk) != 0) return false;
                        }
                        /* Convert pixels (row-banded; bounds hoisted). */
                        int gx0 = mx * mcu_w;
                        int gy0 = my * mcu_h;
                        int wlim = (int)c->width  - gx0;
                        int hlim = (int)c->height - gy0;
                        int pxn = mcu_w < wlim ? mcu_w : wlim;
                        int pyn = mcu_h < hlim ? mcu_h : hlim;
                        for (int py = 0; py < pyn; py++) {
                            uint8_t *dp = dst_argb +
                                (((uint32_t)(gy0 + py)) * c->width + (uint32_t)gx0) * 4;
                            int yrow_base = (py & 7) * 8;
                            int yb_row    = (py >> 3) * c->comp[0].hsamp;
                            if (c->ncomp == 3) {
                                const int *cbrow = Cbblk + ((py >> cys) & 7) * 8;
                                const int *crrow = Crblk + ((py >> cys) & 7) * 8;
                                for (int px = 0; px < pxn; px++) {
                                    int Y  = Yblk[yb_row + (px >> 3)]
                                                 [yrow_base + (px & 7)] + 128;
                                    int Cb = cbrow[(px >> cxs) & 7];
                                    int Cr = crrow[(px >> cxs) & 7];
                                    int R = Y + ((91881 * Cr + 32768) >> 16);
                                    int G = Y - ((22554 * Cb + 46802 * Cr
                                                  + 32768) >> 16);
                                    int B = Y + ((116130 * Cb + 32768) >> 16);
                                    dp[0] = clamp8(B);
                                    dp[1] = clamp8(G);
                                    dp[2] = clamp8(R);
                                    dp[3] = 0xFF;
                                    dp += 4;
                                }
                            } else {
                                for (int px = 0; px < pxn; px++) {
                                    uint8_t Y = clamp8(
                                        Yblk[yb_row + (px >> 3)]
                                            [yrow_base + (px & 7)] + 128);
                                    dp[0] = Y; dp[1] = Y; dp[2] = Y;
                                    dp[3] = 0xFF;
                                    dp += 4;
                                }
                            }
                        }
                    }
                }
                return true;
            }
            default:
                c->p = segend;
                break;
        }
        c->p = segend;
    }
    return false;
}

/* Public entry: serialises decoders WITHOUT disabling interrupts (kspin
 * would cli for the whole 50+ ms decode).  Contenders yield so the lock
 * holder - possibly the browser's preemptive worker task - keeps making
 * progress.  Protects the static context + Yblk scratch above. */
static volatile uint32_t g_jpeg_busy = 0;

bool jpeg_decode(const uint8_t *src, uint32_t src_len,
                 uint8_t *dst_argb, uint32_t dst_cap,
                 jpeg_info_t *info) {
    while (__atomic_exchange_n(&g_jpeg_busy, 1, __ATOMIC_ACQUIRE)) {
        if (sched_running()) sched_yield();
        else __asm__ volatile ("pause");
    }
    bool ok = jpeg_decode_inner(src, src_len, dst_argb, dst_cap, info);
    __atomic_store_n(&g_jpeg_busy, 0, __ATOMIC_RELEASE);
    return ok;
}

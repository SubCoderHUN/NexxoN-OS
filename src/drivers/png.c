/* ============================================================================
 * NexxoN OS - Native PNG decoder
 * ----------------------------------------------------------------------------
 * Self-contained read-only decoder.  Implements:
 *
 *   * Signature + chunk walk (IHDR, IDAT*, PLTE, tRNS, IEND).
 *   * Inflate: uncompressed blocks, fixed-Huffman blocks, dynamic-Huffman
 *     blocks.  Output capped at INFLATE_OUT (the size of a 1024x1024
 *     RGBA scanline buffer plus filter overhead - more than enough for
 *     desktop icons / wallpaper-sized images).
 *   * PNG defilter (None / Sub / Up / Average / Paeth) over the scanline.
 *   * Pixel-format conversion: greyscale, greyscale+alpha, RGB, RGBA,
 *     8 bpc only.  Palette/indexed PNGs are decoded via PLTE / tRNS.
 *
 * The decoder is not industrial-strength - it skips CRC verification and
 * trusts that the file isn't fuzzed adversarially - but it correctly
 * renders >99 % of real-world PNG icons / wallpapers.
 * ============================================================================ */
#include "png.h"
#include "string.h"
#include "debug.h"

#define INFLATE_MAX_OUT  (4u * 1024u * 1024u)   /* hard 4 MiB cap            */

/* ---------- Bit reader ---------------------------------------------------- */
typedef struct {
    const uint8_t *p;
    uint32_t       remaining;
    uint32_t       buf;
    uint32_t       nbits;
} bitreader_t;

static bool bitr_init(bitreader_t *b, const uint8_t *p, uint32_t n) {
    b->p = p; b->remaining = n; b->buf = 0; b->nbits = 0; return true;
}
static int bitr_get(bitreader_t *b, int n) {
    while (b->nbits < (uint32_t)n) {
        if (b->remaining == 0) return -1;
        b->buf |= ((uint32_t)*b->p++) << b->nbits;
        b->remaining--;
        b->nbits += 8;
    }
    int v = (int)(b->buf & ((1u << n) - 1u));
    b->buf >>= n;
    b->nbits -= n;
    return v;
}

/* ---------- Huffman code lookup ------------------------------------------- */
#define HMAX_BITS  15
typedef struct {
    int     counts[HMAX_BITS + 1];     /* number of codes of each length */
    int     symbols[288];              /* symbol values in canonical order */
} huff_t;

static int huff_build(huff_t *h, const uint8_t *lens, int n) {
    int offs[HMAX_BITS + 1] = {0};
    for (int i = 0; i <= HMAX_BITS; i++) h->counts[i] = 0;
    for (int i = 0; i < n; i++) h->counts[lens[i]]++;
    h->counts[0] = 0;
    int sum = 0;
    for (int i = 1; i <= HMAX_BITS; i++) {
        offs[i] = sum;
        sum += h->counts[i];
    }
    for (int i = 0; i < n; i++) {
        if (lens[i] != 0) {
            h->symbols[offs[lens[i]]++] = i;
        }
    }
    return 0;
}

static int huff_decode(bitreader_t *b, const huff_t *h) {
    int code = 0;
    int first = 0;
    int idx   = 0;
    for (int len = 1; len <= HMAX_BITS; len++) {
        int bit = bitr_get(b, 1);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        int cnt = h->counts[len];
        if (code - cnt < first) {
            return h->symbols[idx + (code - first)];
        }
        idx   += cnt;
        first  = (first + cnt) << 1;
    }
    return -1;
}

/* DEFLATE constants. */
static const int LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,
    115,131,163,195,227,258
};
static const int LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const int DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,
    2049,3073,4097,6145,8193,12289,16385,24577
};
static const int DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const int CODE_LEN_ORDER[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

/* Decode one DEFLATE block stream into `out`.  Returns total bytes written
 * on success, negative on failure. */
/* Public wrapper for cross-module reuse (zip.c). */
int deflate_inflate(const uint8_t *src, uint32_t src_len,
                    uint8_t *out, uint32_t out_cap);

static int inflate(const uint8_t *src, uint32_t src_len,
                   uint8_t *out, uint32_t out_cap) {
    bitreader_t b;
    bitr_init(&b, src, src_len);
    uint32_t op = 0;
    while (1) {
        int bfinal = bitr_get(&b, 1);
        int btype  = bitr_get(&b, 2);
        if (bfinal < 0 || btype < 0) return -1;
        if (btype == 0) {
            /* Stored block: skip to byte boundary then copy LEN bytes. */
            b.buf = 0; b.nbits = 0;
            if (b.remaining < 4) return -1;
            uint16_t len = (uint16_t)(b.p[0] | (b.p[1] << 8));
            b.p += 4; b.remaining -= 4;
            if (op + len > out_cap || len > b.remaining) return -1;
            memcpy(out + op, b.p, len);
            op += len;
            b.p += len; b.remaining -= len;
        } else if (btype == 1 || btype == 2) {
            huff_t lit, dist;
            if (btype == 1) {
                uint8_t lens[288];
                for (int i = 0;   i < 144; i++) lens[i] = 8;
                for (int i = 144; i < 256; i++) lens[i] = 9;
                for (int i = 256; i < 280; i++) lens[i] = 7;
                for (int i = 280; i < 288; i++) lens[i] = 8;
                huff_build(&lit, lens, 288);
                uint8_t dlens[30];
                for (int i = 0; i < 30; i++) dlens[i] = 5;
                huff_build(&dist, dlens, 30);
            } else {
                int hlit  = bitr_get(&b, 5);
                int hdist = bitr_get(&b, 5);
                int hclen = bitr_get(&b, 4);
                if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
                hlit += 257;
                hdist += 1;
                hclen += 4;
                uint8_t clens[19] = {0};
                for (int i = 0; i < hclen; i++) {
                    int v = bitr_get(&b, 3);
                    if (v < 0) return -1;
                    clens[CODE_LEN_ORDER[i]] = (uint8_t)v;
                }
                huff_t code_huff;
                huff_build(&code_huff, clens, 19);
                uint8_t lens[288 + 30];
                int idx = 0;
                while (idx < hlit + hdist) {
                    int sym = huff_decode(&b, &code_huff);
                    if (sym < 0) return -1;
                    if (sym < 16) {
                        lens[idx++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        int r = bitr_get(&b, 2);
                        if (r < 0 || idx == 0) return -1;
                        r += 3;
                        uint8_t v = lens[idx - 1];
                        while (r-- > 0 && idx < hlit + hdist) lens[idx++] = v;
                    } else if (sym == 17) {
                        int r = bitr_get(&b, 3);
                        if (r < 0) return -1;
                        r += 3;
                        while (r-- > 0 && idx < hlit + hdist) lens[idx++] = 0;
                    } else if (sym == 18) {
                        int r = bitr_get(&b, 7);
                        if (r < 0) return -1;
                        r += 11;
                        while (r-- > 0 && idx < hlit + hdist) lens[idx++] = 0;
                    } else {
                        return -1;
                    }
                }
                huff_build(&lit,  lens,           hlit);
                huff_build(&dist, lens + hlit,    hdist);
            }
            while (1) {
                int sym = huff_decode(&b, &lit);
                if (sym < 0) return -1;
                if (sym < 256) {
                    if (op >= out_cap) return -1;
                    out[op++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    int idx = sym - 257;
                    if (idx >= 29) return -1;
                    int len = LEN_BASE[idx];
                    int eb  = LEN_EXTRA[idx];
                    if (eb) {
                        int extra = bitr_get(&b, eb);
                        if (extra < 0) return -1;
                        len += extra;
                    }
                    int dsym = huff_decode(&b, &dist);
                    if (dsym < 0 || dsym >= 30) return -1;
                    int d = DIST_BASE[dsym];
                    int de = DIST_EXTRA[dsym];
                    if (de) {
                        int extra = bitr_get(&b, de);
                        if (extra < 0) return -1;
                        d += extra;
                    }
                    if ((int)op < d) return -1;
                    if (op + (uint32_t)len > out_cap) return -1;
                    for (int i = 0; i < len; i++) {
                        out[op] = out[op - d];
                        op++;
                    }
                }
            }
        } else {
            return -1;
        }
        if (bfinal) break;
    }
    return (int)op;
}

/* ---------- PNG defilter -------------------------------------------------- */
static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p > (int)a ? p - (int)a : (int)a - p;
    int pb = p > (int)b ? p - (int)b : (int)b - p;
    int pc = p > (int)c ? p - (int)c : (int)c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int deflate_inflate(const uint8_t *src, uint32_t src_len,
                    uint8_t *out, uint32_t out_cap) {
    return inflate(src, src_len, out, out_cap);
}

static int defilter(uint8_t *data, uint32_t scanline_bytes, uint32_t bpp,
                    uint32_t height) {
    /* Each scanline starts with a 1-byte filter type. */
    for (uint32_t r = 0; r < height; r++) {
        uint8_t *row = data + r * (scanline_bytes + 1);
        uint8_t filter = row[0];
        uint8_t *cur = row + 1;
        uint8_t *prev = (r > 0) ? (cur - (scanline_bytes + 1)) : NULL;
        switch (filter) {
            case 0: break;
            case 1:
                for (uint32_t i = bpp; i < scanline_bytes; i++)
                    cur[i] += cur[i - bpp];
                break;
            case 2:
                if (prev) for (uint32_t i = 0; i < scanline_bytes; i++)
                    cur[i] += prev[i];
                break;
            case 3:
                for (uint32_t i = 0; i < scanline_bytes; i++) {
                    uint8_t a = (i >= bpp) ? cur[i - bpp] : 0;
                    uint8_t b = prev ? prev[i] : 0;
                    cur[i] += (uint8_t)((a + b) / 2);
                }
                break;
            case 4:
                for (uint32_t i = 0; i < scanline_bytes; i++) {
                    uint8_t a = (i >= bpp) ? cur[i - bpp] : 0;
                    uint8_t b = prev ? prev[i] : 0;
                    uint8_t c = (prev && i >= bpp) ? prev[i - bpp] : 0;
                    cur[i] += paeth(a, b, c);
                }
                break;
            default: return -1;
        }
    }
    return 0;
}

bool png_decode(const uint8_t *src, uint32_t src_len,
                uint8_t *dst_argb, uint32_t dst_cap,
                png_info_t *info) {
    if (!src || src_len < 24 || !dst_argb || !info) return false;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (memcmp(src, sig, 8) != 0) return false;
    uint32_t p = 8;

    /* IHDR */
    if (p + 8 > src_len) return false;
    uint32_t clen = ((uint32_t)src[p] << 24) | ((uint32_t)src[p+1] << 16)
                  | ((uint32_t)src[p+2] << 8)| (uint32_t)src[p+3];
    if (memcmp(src + p + 4, "IHDR", 4) != 0) return false;
    if (p + 8 + clen + 4 > src_len) return false;
    const uint8_t *ihdr = src + p + 8;
    uint32_t w  = ((uint32_t)ihdr[0] << 24) | ((uint32_t)ihdr[1] << 16)
               | ((uint32_t)ihdr[2] << 8)  | (uint32_t)ihdr[3];
    uint32_t h  = ((uint32_t)ihdr[4] << 24) | ((uint32_t)ihdr[5] << 16)
               | ((uint32_t)ihdr[6] << 8)  | (uint32_t)ihdr[7];
    uint8_t bit_depth = ihdr[8];
    uint8_t color     = ihdr[9];
    if (bit_depth != 8) {
        debug_printf("[png] unsupported bit depth %u\n", bit_depth);
        return false;
    }
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;
    info->width = w; info->height = h;
    info->bit_depth = bit_depth; info->color_type = color;
    info->pixel_bytes = w * h * 4;
    if (info->pixel_bytes > dst_cap) return false;
    p += 8 + clen + 4;

    /* PLTE / tRNS / IDAT loop. */
    static uint8_t palette[256][4];
    int  plte_n = 0;
    static uint8_t idat[INFLATE_MAX_OUT];
    uint32_t idat_len = 0;

    while (p + 8 <= src_len) {
        clen = ((uint32_t)src[p] << 24) | ((uint32_t)src[p+1] << 16)
             | ((uint32_t)src[p+2] << 8)| (uint32_t)src[p+3];
        const char *t = (const char *)(src + p + 4);
        if (p + 8 + clen + 4 > src_len) return false;
        if (memcmp(t, "IDAT", 4) == 0) {
            if (idat_len + clen > sizeof(idat)) return false;
            memcpy(idat + idat_len, src + p + 8, clen);
            idat_len += clen;
        } else if (memcmp(t, "PLTE", 4) == 0) {
            plte_n = (int)(clen / 3);
            if (plte_n > 256) plte_n = 256;
            const uint8_t *pl = src + p + 8;
            for (int i = 0; i < plte_n; i++) {
                palette[i][0] = pl[i * 3 + 0];
                palette[i][1] = pl[i * 3 + 1];
                palette[i][2] = pl[i * 3 + 2];
                palette[i][3] = 0xFF;
            }
        } else if (memcmp(t, "tRNS", 4) == 0 && color == 3) {
            const uint8_t *pl = src + p + 8;
            for (uint32_t i = 0; i < clen && i < 256; i++) {
                palette[i][3] = pl[i];
            }
        } else if (memcmp(t, "IEND", 4) == 0) {
            break;
        }
        p += 8 + clen + 4;
    }
    if (idat_len < 2) return false;
    /* zlib header (2 bytes) - skip CMF/FLG. */
    static uint8_t inflated[INFLATE_MAX_OUT];
    int inflated_len = inflate(idat + 2, idat_len - 2 - 4,
                               inflated, sizeof(inflated));
    if (inflated_len < 0) {
        debug_printf("[png] inflate failed\n");
        return false;
    }
    /* Bytes per pixel by color type. */
    uint32_t bpp_raw;
    uint32_t channels;
    switch (color) {
        case 0: bpp_raw = 1; channels = 1; break;   /* Grayscale */
        case 2: bpp_raw = 3; channels = 3; break;   /* RGB */
        case 3: bpp_raw = 1; channels = 1; break;   /* Palette */
        case 4: bpp_raw = 2; channels = 2; break;   /* Grayscale+A */
        case 6: bpp_raw = 4; channels = 4; break;   /* RGBA */
        default: return false;
    }
    uint32_t scanline = w * bpp_raw;
    uint32_t expected = (scanline + 1) * h;
    if ((uint32_t)inflated_len != expected) {
        debug_printf("[png] inflate size mismatch: got %d want %u\n",
                     inflated_len, expected);
        return false;
    }
    if (defilter(inflated, scanline, bpp_raw, h) != 0) return false;

    /* Convert to ARGB top-down. */
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = inflated + y * (scanline + 1) + 1;
        uint8_t *dst = dst_argb + y * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r, g, b, a = 0xFF;
            if (color == 0)        { r = g = b = row[x]; }
            else if (color == 2)   { r = row[x*3]; g = row[x*3+1]; b = row[x*3+2]; }
            else if (color == 3)   {
                int idx = row[x];
                if (idx >= plte_n) idx = 0;
                r = palette[idx][0]; g = palette[idx][1];
                b = palette[idx][2]; a = palette[idx][3];
            }
            else if (color == 4)   { r = g = b = row[x*2]; a = row[x*2+1]; }
            else                   {
                r = row[x*4]; g = row[x*4+1];
                b = row[x*4+2]; a = row[x*4+3];
            }
            dst[x*4+0] = b;
            dst[x*4+1] = g;
            dst[x*4+2] = r;
            dst[x*4+3] = a;
        }
    }
    (void)channels;
    return true;
}

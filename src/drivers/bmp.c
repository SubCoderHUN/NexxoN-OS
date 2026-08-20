/* ============================================================================
 * NexxoN OS - BMP image decoder
 * ----------------------------------------------------------------------------
 * Minimal but defensively-coded.  Validates the 14-byte file header (BM
 * signature + size + pixel offset), then the 40+ byte info header (size,
 * width, height, planes=1, bpp 24/32, compression=BI_RGB), then walks the
 * pixel rows.  BMP rows are stored little-endian and padded to a 4-byte
 * boundary; a negative `height` means top-down storage (rare but legal).
 *
 * Output is always 32-bpp ARGB with alpha forced to 0xFF.  Allocation is
 * done with a dedicated 6 MiB BSS pool because wallpapers can hit
 * 1024*768*4 = 3 MiB and we deliberately don't share the WM framebuffer
 * pool (a wallpaper outlives every window).
 * ============================================================================ */
#include "bmp.h"
#include "string.h"
#include "debug.h"

#define BMP_POOL_BYTES   (6u * 1024u * 1024u)
ALIGNED(16) static uint8_t g_bmp_pool[BMP_POOL_BYTES];
static uint32_t g_bmp_pool_used = 0;

static uint8_t *bmp_alloc(uint32_t bytes) {
    uint32_t need = (bytes + 15u) & ~15u;
    if (g_bmp_pool_used + need > BMP_POOL_BYTES) return NULL;
    uint8_t *p = g_bmp_pool + g_bmp_pool_used;
    g_bmp_pool_used += need;
    return p;
}

static void bmp_pool_reset(void) { g_bmp_pool_used = 0; }

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)(p[0])        |
           ((uint32_t)p[1] <<  8)  |
           ((uint32_t)p[2] << 16)  |
           ((uint32_t)p[3] << 24);
}

bool bmp_decode(const uint8_t *src, uint32_t src_len, bmp_image_t *out) {
    if (!src || !out) return false;
    memset(out, 0, sizeof(*out));
    /* Minimum: 14-byte file header + 40-byte info header. */
    if (src_len < 54) {
        debug_printf("[bmp] too short (%u bytes)\n", src_len);
        return false;
    }
    if (src[0] != 'B' || src[1] != 'M') {
        debug_printf("[bmp] bad signature %02x %02x\n", src[0], src[1]);
        return false;
    }
    uint32_t pix_off = rd32(src + 0x0A);
    uint32_t dib_sz  = rd32(src + 0x0E);
    int32_t  w       = (int32_t)rd32(src + 0x12);
    int32_t  h_raw   = (int32_t)rd32(src + 0x16);
    uint16_t planes  = rd16(src + 0x1A);
    uint16_t bpp     = rd16(src + 0x1C);
    uint32_t comp    = rd32(src + 0x1E);

    if (dib_sz < 12 || planes != 1) {
        debug_printf("[bmp] unsupported header dib=%u planes=%u\n",
                     dib_sz, planes);
        return false;
    }
    if (bpp != 24 && bpp != 32) {
        debug_printf("[bmp] only 24/32 bpp supported (got %u)\n", bpp);
        return false;
    }
    if (comp != 0 /* BI_RGB */ && !(comp == 3 /* BI_BITFIELDS */ && bpp == 32)) {
        debug_printf("[bmp] unsupported compression %u\n", comp);
        return false;
    }
    if (w <= 0 || w > 4096) {
        debug_printf("[bmp] bad width %d\n", w);
        return false;
    }
    bool bottom_up = (h_raw > 0);
    int32_t h = bottom_up ? h_raw : -h_raw;
    if (h <= 0 || h > 4096) {
        debug_printf("[bmp] bad height %d\n", h);
        return false;
    }
    if (pix_off >= src_len) {
        debug_printf("[bmp] pix_off %u >= src_len %u\n", pix_off, src_len);
        return false;
    }

    /* Row stride: width * bpp/8 rounded up to multiple of 4 bytes. */
    uint32_t bypp   = bpp / 8;
    uint32_t stride = ((uint32_t)w * bypp + 3u) & ~3u;
    uint64_t need64 = (uint64_t)stride * (uint64_t)h;
    if (pix_off + need64 > src_len) {
        debug_printf("[bmp] truncated pixel array "
                     "(need %u bytes, have %u)\n",
                     (uint32_t)need64, src_len - pix_off);
        return false;
    }

    uint32_t out_bytes = (uint32_t)w * (uint32_t)h * 4u;
    uint8_t *dst = bmp_alloc(out_bytes);
    if (!dst) {
        bmp_pool_reset();
        dst = bmp_alloc(out_bytes);
        if (!dst) {
            debug_printf("[bmp] alloc(%u) failed\n", out_bytes);
            return false;
        }
    }

    /* Walk rows.  Source row index depends on storage direction. */
    const uint8_t *base = src + pix_off;
    for (int32_t y = 0; y < h; y++) {
        int32_t src_row = bottom_up ? (h - 1 - y) : y;
        const uint8_t *row = base + (uint32_t)src_row * stride;
        uint32_t      *drow = (uint32_t *)(dst + (uint32_t)y * (uint32_t)w * 4u);
        for (int32_t x = 0; x < w; x++) {
            const uint8_t *px = row + (uint32_t)x * bypp;
            uint8_t b = px[0];
            uint8_t g = px[1];
            uint8_t r = px[2];
            uint8_t a = (bypp == 4) ? px[3] : 0xFF;
            if (a == 0 && bypp == 4) a = 0xFF;  /* alpha 0 = opaque       */
            drow[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                      ((uint32_t)g <<  8) | (uint32_t)b;
        }
    }

    out->width       = (uint32_t)w;
    out->height      = (uint32_t)h;
    out->bpp         = bpp;
    out->pixels      = dst;
    out->pixel_bytes = out_bytes;
    debug_printf("[bmp] decoded %dx%d bpp=%u (%u bytes)\n",
                 w, h, bpp, out_bytes);
    return true;
}

void bmp_free(bmp_image_t *img) {
    /* We don't track individual allocations in the pool - the wallpaper
     * is rebuilt by resetting the entire pool any time a new image is
     * loaded.  Wipe the descriptor so accidental double-frees can't
     * read stale state. */
    if (!img) return;
    memset(img, 0, sizeof(*img));
}

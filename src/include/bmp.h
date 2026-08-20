/* ============================================================================
 * NexxoN OS - 24/32 bpp BMP image parser  (v1.0)
 * ----------------------------------------------------------------------------
 * Reads a Windows BMP file from a contiguous byte buffer and decodes it into
 * a freshly-allocated 32-bpp ARGB pixel array.  Supports BITMAPINFOHEADER
 * variants of 24 bpp (bottom-up, BGR triplets) and 32 bpp (bottom-up OR
 * top-down, BGRA quads).  No compression (BI_RGB only) - that's enough for
 * every screenshot tool and 99% of wallpapers in the wild.
 *
 * Pixel buffer lifetime is owned by the caller via the regular pool allocator
 * exposed by window.c - we deliberately do NOT add another heap.
 * ============================================================================ */
#ifndef NEXXON_BMP_H
#define NEXXON_BMP_H

#include "types.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;          /* 24 or 32                                    */
    uint8_t *pixels;       /* width*height*4 bytes ARGB, top-down rows    */
    uint32_t pixel_bytes;
} bmp_image_t;

/* Decode `src` (a complete BMP file) into `out`.  Returns true on success;
 * out->pixels must be released with bmp_free() when no longer needed.
 *
 * The pixel buffer is allocated through wm pool to avoid introducing a
 * second allocator.  Pass a destination ARGB output buffer big enough for
 * width*height pixels via `dst_pixels` (NULL = the function allocates one).
 *
 * BMP rows are commonly stored bottom-up; the decoder normalises to
 * top-down so the caller can blit directly without flipping. */
bool bmp_decode(const uint8_t *src, uint32_t src_len, bmp_image_t *out);

/* Release the pixel buffer returned by bmp_decode (if any).  Safe to call
 * on a zero-initialised bmp_image_t. */
void bmp_free  (bmp_image_t *img);

#endif /* NEXXON_BMP_H */

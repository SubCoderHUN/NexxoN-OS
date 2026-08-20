/* ============================================================================
 * NexxoN OS - Native PNG decoder  (v1.0)
 * ----------------------------------------------------------------------------
 * A self-contained PNG decoder that handles the on-the-wire format
 * (signature, chunks, IDAT concatenation, CRC verification skipped) and
 * a minimal inflate implementation for uncompressed (BTYPE=00) and fixed
 * Huffman (BTYPE=01) DEFLATE blocks.  Dynamic-Huffman blocks (BTYPE=10)
 * are decoded for the common case of small palette/greyscale PNGs.
 *
 * Output is always 32-bpp ARGB (alpha 0xFF for opaque inputs).  The
 * caller provides a destination buffer big enough for width*height*4
 * bytes; we do not allocate.
 * ============================================================================ */
#ifndef NEXXON_PNG_H
#define NEXXON_PNG_H

#include "types.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t  bit_depth;
    uint8_t  color_type;
    uint32_t pixel_bytes;
} png_info_t;

/* Decode PNG `src`/`src_len` into the caller-supplied `dst_argb` buffer.
 * `dst_cap` must be >= info.pixel_bytes.  Returns true on success and
 * fills `info` with width/height/etc; pixels are top-down ARGB. */
bool png_decode(const uint8_t *src, uint32_t src_len,
                uint8_t *dst_argb, uint32_t dst_cap,
                png_info_t *info);

#endif /* NEXXON_PNG_H */

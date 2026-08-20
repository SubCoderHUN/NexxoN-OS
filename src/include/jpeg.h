/* ============================================================================
 * NexxoN OS - Baseline JPEG (JFIF) decoder  (v1.0)
 * ----------------------------------------------------------------------------
 * Decodes baseline-sequential (SOF0) JFIF JPEG files at 8 bits per
 * component, 4:4:4 or 4:2:0 chroma subsampling.  Implements:
 *
 *   * Marker parsing (SOI / DQT / DHT / SOF0 / SOS / EOI / restart).
 *   * Two quantisation tables (luma + chroma).
 *   * Four Huffman tables (DC luma / AC luma / DC chroma / AC chroma).
 *   * 8x8 IDCT via the AAN integer algorithm.
 *   * MCU rasterisation with YCbCr->RGB and 2x2 chroma upsample.
 *
 * Progressive JPEGs, CMYK, 12-bit, and arithmetic coding are out of
 * scope - 95% of real-world JFIF images are baseline 4:2:0.
 *
 * Output is 32-bpp ARGB, top-down.
 * ============================================================================ */
#ifndef NEXXON_JPEG_H
#define NEXXON_JPEG_H

#include "types.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t components;            /* 1 (grayscale) or 3 (Y/Cb/Cr)        */
    uint32_t pixel_bytes;
} jpeg_info_t;

bool jpeg_decode(const uint8_t *src, uint32_t src_len,
                 uint8_t *dst_argb, uint32_t dst_cap,
                 jpeg_info_t *info);

#endif /* NEXXON_JPEG_H */

/* ============================================================================
 * NexxoN OS - 8x8 monochrome bitmap font
 * ----------------------------------------------------------------------------
 * 256-entry font, each glyph stored as 8 bytes - row 0 first, MSB = leftmost
 * pixel.  Derived from the public-domain IBM VGA 8x8 CP437 font; only ASCII
 * is rendered (other code points are blank stubs).
 * ============================================================================ */
#ifndef NEXXON_FONT_H
#define NEXXON_FONT_H

#include "types.h"

#define FONT_GLYPH_W   8
#define FONT_GLYPH_H   8

extern const uint8_t font8x8[256][FONT_GLYPH_H];

#endif /* NEXXON_FONT_H */

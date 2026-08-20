/* ============================================================================
 * NexxoN OS - VESA / VGA 32 bpp linear-framebuffer driver
 * ----------------------------------------------------------------------------
 * The bootloader (GRUB) is asked to switch to 640x480x32 before our kernel
 * is entered.  We honour whatever resolution it actually delivered.
 *
 * The driver exposes both a low-level putpixel API and a higher-level
 * primitive set (rectangles, lines, char glyphs) used by the terminal.
 * ============================================================================ */
#ifndef NEXXON_VGA_H
#define NEXXON_VGA_H

#include "types.h"
#include "multiboot.h"

#define VGA_W_DEFAULT   1024
#define VGA_H_DEFAULT   768
#define VGA_BPP         32

/* 0xAARRGGBB - we ignore the alpha byte but keep the slot for clarity. */
typedef uint32_t vga_color_t;

#define VGA_BLACK   0xFF000000
#define VGA_WHITE   0xFFFFFFFF
#define VGA_GRAY    0xFF808080
#define VGA_DKGRAY  0xFF202020
#define VGA_LTGRAY  0xFFC0C0C0
#define VGA_RED     0xFFFF4040
#define VGA_GREEN   0xFF40FF80
#define VGA_BLUE    0xFF4080FF
#define VGA_YELLOW  0xFFFFE040
#define VGA_CYAN    0xFF40E0E0
#define VGA_MAGENTA 0xFFFF40FF
#define VGA_ORANGE  0xFFFFA040

bool        vga_init      (const multiboot_info_t *mbi);
uint32_t    vga_width     (void);
uint32_t    vga_height    (void);
uint32_t    vga_bpp       (void);
void       *vga_framebuffer(void);

/* Runtime pixel format introspection.  Multiboot may report a frame
 * buffer where Red/Green/Blue sit at non-standard bit positions.  The
 * browser pixel converter consults vga_red_pos()/vga_blue_pos() and
 * swaps the JPEG output channels if needed instead of dumping garbled
 * BGR pixels straight to the screen. */
uint8_t     vga_red_pos   (void);
uint8_t     vga_green_pos (void);
uint8_t     vga_blue_pos  (void);
bool        vga_is_native_argb(void);
uint32_t    vga_compose_pixel (uint8_t r, uint8_t g, uint8_t b);

void vga_putpixel  (int x, int y, vga_color_t c);
void vga_fill_rect (int x, int y, int w, int h, vga_color_t c);
void vga_clear     (vga_color_t c);
void vga_draw_char (int x, int y, char c, vga_color_t fg, vga_color_t bg);
void vga_draw_string(int x, int y, const char *s, vga_color_t fg, vga_color_t bg);
void vga_scroll_up (int pixels, vga_color_t bg_fill);

/* Runtime VBE mode switching.  Calls the real-mode trampoline to
 * issue INT 10h AX=4F02h, then updates the framebuffer state and
 * broadcasts the change to the WM. */
bool vga_switch_mode(uint16_t width, uint16_t height);

/* True when a Bochs/QEMU/VBox BGA adapter is present (it can set ANY resolution
 * directly).  On bare metal this is false and only firmware-enumerated VBE modes
 * are usable. */
bool vga_bga_available(void);

/* Callback type for resolution-change listeners. */
typedef void (*vga_res_listener_t)(uint32_t new_w, uint32_t new_h);
void vga_register_res_listener(vga_res_listener_t cb);

/* Hardware glyph cell dimensions (font is fixed 8x8). */
#define VGA_FONT_W  8
#define VGA_FONT_H  8

#endif /* NEXXON_VGA_H */

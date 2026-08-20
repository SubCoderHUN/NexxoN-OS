/* ============================================================================
 * NexxoN OS - Verbose serial-port boot debugger
 * ----------------------------------------------------------------------------
 * Mirrors every step-by-step bring-up message to COM1 (0x3F8) so that even
 * when the framebuffer is dead (or before the framebuffer is brought up at
 * all) we still get a record of what the kernel was doing.
 *
 * Run "make run-debug" to attach the host's stdio to the QEMU serial line.
 * ============================================================================ */
#ifndef NEXXON_DEBUG_H
#define NEXXON_DEBUG_H

#include "types.h"

void debug_init   (void);
void debug_putc   (char c);
void debug_puts   (const char *s);
void debug_printf (const char *fmt, ...);

/* High-level helpers used during bring-up so that every subsystem produces
 * a uniform "[ OK ] / [FAIL] / [ .. ]" status line. */
void debug_step   (const char *what);                  /* "[ .. ] what"   */
void debug_ok     (const char *what);                  /* "[ OK ] what"   */
void debug_fail   (const char *what, const char *why); /* "[FAIL] what"   */
void debug_banner (void);                              /* big startup logo */

#endif /* NEXXON_DEBUG_H */

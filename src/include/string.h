/* ============================================================================
 * NexxoN OS - Minimal libc-style string / memory helpers
 * ============================================================================ */
#ifndef NEXXON_STRING_H
#define NEXXON_STRING_H

#include "types.h"
#include <stdarg.h>

/* mem */
void   *memset (void *dst, int val, size_t n);
void   *memcpy (void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
int     memcmp (const void *a, const void *b, size_t n);

/* str */
size_t  strlen (const char *s);
int     strcmp (const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcpy (char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strcat (char *dst, const char *src);
char   *strchr (const char *s, int c);
int     strtok_first(const char *s, char delim, char *out, size_t out_sz);

/* fmt - numeric conversion */
void utoa(uint32_t v, char *out, int base);
void itoa(int32_t  v, char *out, int base);

/* fmt - canonical printf-style formatter.
 *
 * Supports: %c %s %d %i %u %x %X %p %%
 *   Flags  : '-' (left-justify), '0' (zero-pad)
 *   Width  : decimal digits (e.g. %-32s, %08x, %4d)
 *
 * kvsnprintf is the one true formatter; ksnprintf is the variadic wrapper.
 * Every printf-like in the kernel funnels through kvsnprintf to keep
 * behaviour identical across serial / terminal / panic-screen output. */
int kvsnprintf(char *buf, size_t buf_sz, const char *fmt, va_list ap);
int ksnprintf (char *buf, size_t buf_sz, const char *fmt, ...);

#endif /* NEXXON_STRING_H */

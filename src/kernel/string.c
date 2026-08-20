/* ============================================================================
 * NexxoN OS - String / memory primitives (freestanding)
 * ----------------------------------------------------------------------------
 * Plain reference implementations.  GCC, even at -O2, occasionally emits
 * implicit calls to memcpy/memset for struct copies & aggregate inits, so
 * these symbols MUST exist in our kernel even though we never #include them
 * from a hosted libc.
 *
 * The printf-style formatter has been consolidated into kvsnprintf().  All
 * other "printf" entry points in the kernel (term_printf, debug_printf, the
 * panic-screen formatter) call into it so format-spec behaviour - including
 * the '-' left-justify flag and zero/space padding - is identical
 * everywhere.
 * ============================================================================ */
#include "string.h"

/* ---------- memory --------------------------------------------------------- *
 * The bulk paths use `rep movsb` / `rep stosb`: on Ivy Bridge+ (ERMS — the
 * target P8Z77 is the FIRST ERMS generation) the microcoded string ops move
 * cache lines internally and are the fastest portable memcpy/memset for
 * medium/large blocks — exactly what the compositor's multi-MB present path
 * needs.  Under QEMU TCG a rep string op is a single translated helper, far
 * cheaper than an interpreted byte loop.  Blocks under a small threshold use
 * the plain loop (rep has a fixed startup cost).  DF is always cleared by
 * `cld` before `rep` — an ISR could interrupt us mid-copy and these very
 * functions run inside ISRs too, so we never leave DF set (the backward
 * memmove path uses an explicit reverse loop instead of std/rep). */
#define MEM_REP_THRESHOLD 32u

void *memset(void *dst, int val, size_t n) {
    if (n >= MEM_REP_THRESHOLD) {
        void *d = dst;
        __asm__ volatile("cld; rep stosb"
                         : "+D"(d), "+c"(n)
                         : "a"((uint8_t)val)
                         : "memory", "cc");
        return dst;
    }
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    if (n >= MEM_REP_THRESHOLD) {
        void *d = dst;
        const void *s = src;
        __asm__ volatile("cld; rep movsb"
                         : "+D"(d), "+S"(s), "+c"(n)
                         :
                         : "memory", "cc");
        return dst;
    }
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        memcpy(dst, src, n);            /* forward overlap-safe -> fast path */
    } else if (d > s) {
        /* Backward: 8-byte reverse loop (no std/rep — never leave DF set). */
        size_t i = n;
        while (i >= 8) {
            i -= 8;
            *(uint64_t *)(d + i) = *(const uint64_t *)(s + i);
        }
        while (i > 0) { i--; d[i] = s[i]; }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    /* 8-byte strides for the bulk (the compositor's damage tracker memcmps
     * whole rows every present); fall to bytes only to localise a diff or
     * for the tail.  Unaligned 8-byte loads are fine on x86. */
    size_t i = 0;
    while (i + 8 <= n) {
        if (*(const uint64_t *)(x + i) != *(const uint64_t *)(y + i)) break;
        i += 8;
    }
    for (; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

/* ---------- string --------------------------------------------------------- */
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0)
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *r = dst;
    while ((*dst++ = *src++)) {}
    return r;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *r = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++)) {}
    return r;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return NULL;
}

int strtok_first(const char *s, char delim, char *out, size_t out_sz) {
    size_t i = 0;
    while (s[i] && s[i] != delim && i + 1 < out_sz) {
        out[i] = s[i];
        i++;
    }
    out[i] = 0;
    if (s[i] == delim) return (int)i + 1;
    return (int)i;
}

/* ---------- numeric conversion -------------------------------------------- */
static const char digits_lc[] = "0123456789abcdef";

void utoa(uint32_t v, char *out, int base) {
    if (base < 2 || base > 16) { out[0] = 0; return; }
    char tmp[33];
    int  idx = 0;
    if (v == 0) tmp[idx++] = '0';
    while (v) {
        tmp[idx++] = digits_lc[v % (uint32_t)base];
        v /= (uint32_t)base;
    }
    int j = 0;
    while (idx > 0) out[j++] = tmp[--idx];
    out[j] = 0;
}

void itoa(int32_t v, char *out, int base) {
    if (v < 0 && base == 10) {
        out[0] = '-';
        utoa((uint32_t)(-v), out + 1, base);
    } else {
        utoa((uint32_t)v, out, base);
    }
}

/* 64-bit conversions — used by kvsnprintf for the l/ll/z length modifiers and
 * %p so 64-bit addresses print correctly under -m64 (in 32-bit, callers route
 * 32-bit values through here harmlessly). */
static void utoa64(uint64_t v, char *out, int base) {
    if (base < 2 || base > 16) { out[0] = 0; return; }
    char tmp[65];
    int  idx = 0;
    if (v == 0) tmp[idx++] = '0';
    while (v) {
        tmp[idx++] = digits_lc[v % (uint64_t)base];
        v /= (uint64_t)base;
    }
    int j = 0;
    while (idx > 0) out[j++] = tmp[--idx];
    out[j] = 0;
}

static void itoa64(int64_t v, char *out, int base) {
    if (v < 0 && base == 10) {
        out[0] = '-';
        utoa64((uint64_t)(-v), out + 1, base);
    } else {
        utoa64((uint64_t)v, out, base);
    }
}

#if !defined(__x86_64__)
/* The 32-bit freestanding build links no libgcc, so GCC's emitted 64-bit
 * unsigned divide/modulo helpers (used by utoa64's `/` and `%`) are missing.
 * Provide them with a shift-subtract long division that uses ONLY operations
 * GCC decomposes into 32-bit ops (shift / add / sub / compare) — never a
 * 64-bit divide, so there is no recursion back into these helpers.  On x86_64
 * the CPU divides natively and these are not referenced. */
unsigned long long __udivmoddi4(unsigned long long n, unsigned long long d,
                                unsigned long long *rem) {
    unsigned long long q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return ~0ULL; }   /* avoid an infinite loop */
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) { r -= d; q |= (1ULL << i); }
    }
    if (rem) *rem = r;
    return q;
}
unsigned long long __udivdi3(unsigned long long n, unsigned long long d) {
    return __udivmoddi4(n, d, (unsigned long long *)0);
}
unsigned long long __umoddi3(unsigned long long n, unsigned long long d) {
    unsigned long long r;
    __udivmoddi4(n, d, &r);
    return r;
}
#endif

/* ============================================================================
 *                          PROPER  PRINTF  CORE
 * ----------------------------------------------------------------------------
 * Grammar accepted:
 *      "%" flags? width? type
 *
 *   flags : '-'  (left-justify)   |  '0'  (zero-pad, ignored if '-' set)
 *   width : one-or-more decimal digits
 *   type  : c | s | d | i | u | x | X | p | %
 *
 * The previous v1 implementation had two bugs:
 *   1. The '-' flag was misparsed as the start of the conversion-character,
 *      so "%-32s" printed the literal string "%-32s".
 *   2. The '%' parser was duplicated in three places (terminal.c, debug.c,
 *      panic.c) so any fix had to be applied three times.
 *
 * Both are gone: there is now exactly one formatter (kvsnprintf) used by
 * everything in the kernel.
 * ============================================================================ */

/* Small helper: write c to buf if there is room.  Always increments pos so
 * the caller can learn the would-be length of the formatted string. */
static inline void emit(char *buf, size_t buf_sz, size_t *pos, char c) {
    if (*pos + 1 < buf_sz) buf[*pos] = c;
    (*pos)++;
}

/* Emit `s` (length len), padded to `width` columns according to flags. */
static void emit_field(char *buf, size_t buf_sz, size_t *pos,
                       const char *s, int len,
                       int width, char pad_char, bool left_justify) {
    int pad = (width > len) ? (width - len) : 0;
    if (!left_justify) {
        for (int i = 0; i < pad; i++) emit(buf, buf_sz, pos, pad_char);
    }
    for (int i = 0; i < len; i++) emit(buf, buf_sz, pos, s[i]);
    if (left_justify) {
        for (int i = 0; i < pad; i++) emit(buf, buf_sz, pos, ' ');
    }
}

int kvsnprintf(char *buf, size_t buf_sz, const char *fmt, va_list ap) {
    size_t pos  = 0;
    char   num[33];

    while (*fmt) {
        if (*fmt != '%') {
            emit(buf, buf_sz, &pos, *fmt++);
            continue;
        }
        fmt++;  /* past '%' */

        /* ---- Flags --------------------------------------------------- */
        bool left_justify = false;
        char pad_char     = ' ';
        for (;;) {
            if (*fmt == '-') { left_justify = true; fmt++; continue; }
            if (*fmt == '0') { pad_char     = '0'; fmt++; continue; }
            break;
        }
        if (left_justify) pad_char = ' ';  /* zero-pad meaningless when LJ */

        /* ---- Width --------------------------------------------------- */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* ---- Precision (.N) ----------------------------------------- *
         * For strings, '.N' caps the number of characters emitted (then
         * width padding still applies).  This is what column-aligned UIs
         * such as the task manager rely on ("%-22.22s") -- without it the
         * spec was mis-parsed, printed verbatim, and the trailing varargs
         * were consumed by the wrong conversions (garbage memory/CPU
         * columns).  precision < 0 means "unset". */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* ---- Length modifier (l / ll / z; h / hh accepted + ignored) ----- *
         * length: 0 = default (int/uint32), 1 = long, 2 = long long, 3 = size_t.
         * On x86_64 `long`/`size_t`/`uintptr_t` are 64-bit, so this is what lets
         * %lx / %zx / %p print full 64-bit addresses; in 32-bit they stay 32. */
        int length = 0;
        if (*fmt == 'l') {
            fmt++; length = 1;
            if (*fmt == 'l') { fmt++; length = 2; }
        } else if (*fmt == 'z') {
            fmt++; length = 3;
        } else if (*fmt == 'h') {
            fmt++; if (*fmt == 'h') fmt++;   /* h / hh: value still promoted to int */
        }

        /* ---- Conversion --------------------------------------------- */
        char conv = *fmt;
        switch (conv) {
            case 'c': {
                num[0] = (char)va_arg(ap, int);
                emit_field(buf, buf_sz, &pos, num, 1, width, pad_char, left_justify);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int len = (int)strlen(s);
                if (precision >= 0 && len > precision) len = precision;
                emit_field(buf, buf_sz, &pos, s, len, width, pad_char, left_justify);
                break;
            }
            case 'd':
            case 'i': {
                int64_t v;
                if      (length == 2) v = va_arg(ap, long long);
                else if (length == 1) v = (int64_t)va_arg(ap, long);
                else if (length == 3) v = (int64_t)(ssize_t)va_arg(ap, size_t);
                else                  v = (int64_t)va_arg(ap, int);
                itoa64(v, num, 10);
                int len = (int)strlen(num);
                emit_field(buf, buf_sz, &pos, num, len, width, pad_char, left_justify);
                break;
            }
            case 'u': {
                uint64_t v;
                if      (length == 2) v = va_arg(ap, unsigned long long);
                else if (length == 1) v = (uint64_t)va_arg(ap, unsigned long);
                else if (length == 3) v = (uint64_t)va_arg(ap, size_t);
                else                  v = (uint64_t)va_arg(ap, uint32_t);
                utoa64(v, num, 10);
                int len = (int)strlen(num);
                emit_field(buf, buf_sz, &pos, num, len, width, pad_char, left_justify);
                break;
            }
            case 'p': {
                /* %p: "0x" + the full pointer, zero-padded to the native width
                 * (8 hex digits in 32-bit, 16 in 64-bit). */
                emit(buf, buf_sz, &pos, '0');
                emit(buf, buf_sz, &pos, 'x');
                uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
                utoa64(v, num, 16);
                int len = (int)strlen(num);
                emit_field(buf, buf_sz, &pos, num, len,
                           (int)(sizeof(uintptr_t) * 2), '0', false);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v;
                if      (length == 2) v = va_arg(ap, unsigned long long);
                else if (length == 1) v = (uint64_t)va_arg(ap, unsigned long);
                else if (length == 3) v = (uint64_t)va_arg(ap, size_t);
                else                  v = (uint64_t)va_arg(ap, uint32_t);
                utoa64(v, num, 16);
                if (conv == 'X') {
                    for (char *p = num; *p; p++)
                        if (*p >= 'a' && *p <= 'f') *p -= 32;
                }
                int len = (int)strlen(num);
                emit_field(buf, buf_sz, &pos, num, len, width, pad_char, left_justify);
                break;
            }
            case '%':
                emit(buf, buf_sz, &pos, '%');
                break;
            case 0:
                /* Trailing '%' with no conversion - just stop. */
                goto done;
            default:
                /* Unrecognised: print verbatim so bugs are visible, but
                 * never crash. */
                emit(buf, buf_sz, &pos, '%');
                if (left_justify) emit(buf, buf_sz, &pos, '-');
                if (pad_char == '0') emit(buf, buf_sz, &pos, '0');
                if (width > 0) {
                    itoa(width, num, 10);
                    for (char *p = num; *p; p++) emit(buf, buf_sz, &pos, *p);
                }
                emit(buf, buf_sz, &pos, conv);
                break;
        }
        if (conv) fmt++;
    }

done:
    if (buf_sz > 0) {
        buf[pos < buf_sz ? pos : buf_sz - 1] = 0;
    }
    return (int)pos;
}

int ksnprintf(char *buf, size_t buf_sz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, buf_sz, fmt, ap);
    va_end(ap);
    return n;
}

/* ============================================================================
 * NexxoN OS - Core type definitions
 * ----------------------------------------------------------------------------
 * Freestanding fixed-width integer aliases, NULL, bool, and a couple of
 * compiler-portability shims.  The kernel cannot depend on <stdint.h> or
 * <stdbool.h> from a hosted libc, so everything is defined locally.
 * ============================================================================ */
#ifndef NEXXON_TYPES_H
#define NEXXON_TYPES_H

typedef signed   char       int8_t;
typedef unsigned char       uint8_t;
typedef signed   short      int16_t;
typedef unsigned short      uint16_t;
typedef signed   int        int32_t;
typedef unsigned int        uint32_t;
typedef signed   long long  int64_t;
typedef unsigned long long  uint64_t;

/* Pointer- and size-width types track the target word size so the same
 * headers serve BOTH the 32-bit kernel (-m32) and the x86_64 port (-m64).
 * On x86_64 a pointer is 8 bytes, so uintptr_t/size_t MUST be 64-bit or
 * every `(uintptr_t)ptr` cast would truncate addresses above 4 GiB. */
#if defined(__x86_64__) || defined(__LP64__) || (__SIZEOF_POINTER__ == 8)
typedef unsigned long       size_t;     /* 64-bit */
typedef signed   long       ssize_t;
typedef unsigned long       uintptr_t;
typedef signed   long       intptr_t;
#else
typedef uint32_t            size_t;     /* 32-bit */
typedef int32_t             ssize_t;
typedef uint32_t            uintptr_t;
typedef int32_t             intptr_t;
#endif

typedef enum { false = 0, true = 1 } bool;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define PACKED      __attribute__((packed))
#define ALIGNED(x)  __attribute__((aligned(x)))
#define NORETURN    __attribute__((noreturn))
#define UNUSED      __attribute__((unused))
#define INLINE      static inline __attribute__((always_inline))

#endif /* NEXXON_TYPES_H */

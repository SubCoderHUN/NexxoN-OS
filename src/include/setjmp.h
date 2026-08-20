/* ============================================================================
 * NexxoN OS - setjmp / longjmp
 * ----------------------------------------------------------------------------
 * Minimal cooperative non-local goto.  Used by the recoverable-panic path to
 * jump back to a known-good frame (e.g. the top of shell_run) when a CPU
 * exception or driver watchdog fires.  No floating-point or signal state is
 * preserved - this kernel is integer-only and runs with interrupts disabled
 * inside handlers, so just the cdecl callee-saved registers + ESP + return
 * address are enough.
 *
 * Storage layout (6 dwords):
 *   [0]  ebx
 *   [4]  esi
 *   [8]  edi
 *   [12] ebp
 *   [16] esp at the point of the setjmp() call (post-args)
 *   [20] return address that setjmp would have used
 *
 * Behaviour matches C99 setjmp.h:
 *   - setjmp() returns 0 on first entry.
 *   - longjmp(env, val) causes the matching setjmp() to return `val`, with
 *     val of 0 being remapped to 1.
 * ============================================================================ */
#ifndef NEXXON_SETJMP_H
#define NEXXON_SETJMP_H

#include "types.h"

#if defined(__x86_64__)
/* 64-bit: rbx, rbp, r12, r13, r14, r15, rsp, rip (8 x 64-bit). */
typedef uint64_t jmp_buf[8];
#else
/* 32-bit: ebx, esi, edi, ebp, esp, return-addr (6 x 32-bit). */
typedef uint32_t jmp_buf[6];
#endif

int           setjmp (jmp_buf env);
NORETURN void longjmp(jmp_buf env, int val);

#endif /* NEXXON_SETJMP_H */

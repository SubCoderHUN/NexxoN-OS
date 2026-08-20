/* ============================================================================
 * NexxoN OS - Kernel panic & global debugger screen
 * ----------------------------------------------------------------------------
 * Three classes of failure, ordered from "tell the user, then keep running"
 * to "everything is on fire, halt":
 *
 *   recoverable_panic        - User-recoverable error.  Renders a top-most
 *                              red modal window with an OK button.  When
 *                              the user dismisses the modal, longjmps back
 *                              to the shell's recovery point.  Use from
 *                              driver watchdogs and from CPU exceptions
 *                              raised inside cooperative subsystems like
 *                              the editor or NXScript.
 *
 *   panic_from_exception     - Unhandled CPU exception with no installed
 *                              handler.  Tries the recoverable path first
 *                              if recovery is armed; otherwise falls
 *                              through to the BSOD-style halt.
 *
 *   panic                    - Catastrophic, non-recoverable.  Prints the
 *                              BSOD and freezes in cli/hlt.
 *
 * The recovery mechanism is a single global jmp_buf.  Code that wants its
 * stack frame to be the recovery target calls panic_arm_recovery(), which
 * is a thin wrapper around setjmp() that also sets the "armed" flag the
 * panic path checks before attempting to longjmp.
 * ============================================================================ */
#ifndef NEXXON_PANIC_H
#define NEXXON_PANIC_H

#include "types.h"
#include "isr.h"
#include "setjmp.h"

/* Global recovery point.  Shell sets this with setjmp at the top of its
 * main loop.  Only one recovery target exists at a time - the kernel is
 * single-threaded and we don't need nested recovery contexts. */
extern jmp_buf       g_panic_recovery;
extern volatile bool g_panic_recovery_armed;

/* Helper macro for arming recovery from C without exposing the jmp_buf
 * internals to every caller.  Evaluates to 0 on first entry, non-zero
 * when control returns from a longjmp out of recoverable_panic. */
#define PANIC_ARM_RECOVERY()  \
    (g_panic_recovery_armed = true, setjmp(g_panic_recovery))

/* Mark recovery as no-longer-valid (e.g. before the shell hands off to
 * a subsystem that owns the stack for a while). */
void panic_disarm_recovery(void);

NORETURN void panic                       (const char *fmt, ...);
NORETURN void panic_from_exception        (registers_t *r);
NORETURN void recoverable_panic           (const char *fmt, ...);
NORETURN void recoverable_panic_from_exception(registers_t *r);

#endif /* NEXXON_PANIC_H */

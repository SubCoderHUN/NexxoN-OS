/* ============================================================================
 * NexxoN OS - Ring 3 privilege-drop demonstration  (v1.0)
 * ----------------------------------------------------------------------------
 * Step D of the Ring-3 plan: a tiny ring-3 task that calls into the kernel
 * via the int 0x80 dispatcher to prove the privilege boundary is wired up
 * correctly.  The task lives in a dedicated user-mode page (4 KiB), uses a
 * stack carved out of a second user-mode page, and exits back to ring 0
 * by invoking SYS_EXIT.  That syscall path doubles as the verification
 * that the IRET frame we synthesised is well-formed - a malformed frame
 * would have raised #GP, which we'd catch in the panic handler with a
 * clear "user GPF" banner.
 *
 * In a future build the same launcher will be used by the shell, explorer,
 * taskmgr and editor windows once each gets its own user-mode binary
 * image.  For now this is the proof-of-concept.
 * ============================================================================ */
#ifndef NEXXON_USERMODE_H
#define NEXXON_USERMODE_H

#include "types.h"

/* Run the built-in demo ring-3 task: prints "[user] hello from ring 3"
 * via the SYS_TERM_PUTS syscall and exits via SYS_EXIT.  Returns when
 * the task is done.  Used by the `usermode` shell command. */
void usermode_demo(void);

/* ============================================================================
 * app_run_ring3 — generic "launch this app at CPL=3" helper.
 * ----------------------------------------------------------------------------
 * Every program in NexxoN OS (browser, explorer, taskmgr, editor, gephaz,
 * sheet, ...) should respect the ring boundary by entering through this
 * helper.  The flow is:
 *
 *   1. Caller (CPL=0, usually the shell idle loop) hands us a kernel
 *      function pointer + a single uint32_t argument.
 *   2. We synthesise a ring-3 stack, drop to CPL=3 via ring3_enter().
 *   3. The ring-3 stub calls SYS_INVOKE to escalate back into kernel
 *      space for the actual work (window creation, drawing, syscalls).
 *      Privileged instructions inside `fn` itself would fault — that's
 *      exactly the boundary we want enforced.
 *   4. When SYS_INVOKE returns, the stub issues SYS_EXIT which longjmps
 *      out of ring 3.  app_run_ring3 returns the value the kernel
 *      function produced.
 *
 * The helper is intentionally synchronous; long-running TICK loops
 * (browser_tick, taskmgr_tick) still execute on the shell's CPL=0
 * idle thread.  This delivers visible CPL=3 enforcement at the
 * "launch" boundary while we incrementally migrate the per-tick paths
 * onto dedicated syscalls. */
typedef int32_t (*app_fn_t)(uint32_t arg);
int32_t app_run_ring3(app_fn_t fn, uint32_t arg, const char *name);

/* True while a ring-3 task is in flight (between ring3_enter and the
 * matching SYS_EXIT longjmp).  Callers can check this to avoid
 * re-entering app_run_ring3 from inside an app body — re-entry would
 * recurse the setjmp slot and corrupt the return path. */
bool usermode_in_ring3(void);

/* Clear all ring-3 launcher state.  Called by the panic handler after
 * it longjmps back to the shell's recovery point: the crashed app was
 * in mid-launch (g_user_running=true, kernel stack switched to
 * g_ring3_kstack, TSS.ESP0 still aimed at it).  Without this, the
 * next apps_launch would see g_user_running=true and skip the ring 3
 * transition entirely, silently breaking the privilege boundary the
 * user asked us to enforce. */
void usermode_reset_after_panic(void);

#endif /* NEXXON_USERMODE_H */

/* ============================================================================
 * NexxoN OS - Ring 3 demo task and privilege-drop launcher
 * ----------------------------------------------------------------------------
 * Builds a minimal ring-3 program in BSS, drops to ring 3 via the
 * ring3_enter() assembly helper, and waits for the task to issue
 * SYS_EXIT through int 0x80.  Demonstrates every piece of the Ring 3
 * pipeline working together:
 *
 *   1. GDT user code/data selectors are present and DPL=3.
 *   2. TSS.ESP0 / SS0 are loaded so the CPU finds a kernel stack on int.
 *   3. The IRET frame we craft in ring3_enter is consumed correctly by
 *      the CPU, dropping CPL to 3.
 *   4. The ring-3 task's int $0x80 lands on isr128 in idt_flush.asm,
 *      promoting CPL back to 0 with kernel ESP from the TSS.
 *   5. syscall_dispatch() correctly returns through IRET.
 *   6. The task's SYS_EXIT causes the kernel to long-jump out of the
 *      ring-3 context cleanly.
 *
 * Currently we side-step formal task control by using setjmp/longjmp:
 * the demo task lives in the same address space as the kernel (no
 * paging-based isolation in this milestone) and SYS_EXIT longjmps out
 * of the syscall dispatcher's stack frame to recover control.
 * ============================================================================ */
#include "usermode.h"
#include "syscall.h"
#include "debug.h"
#include "terminal.h"
#include "vga.h"
#include "setjmp.h"
#include "string.h"
#include "gdt.h"
#include "apps.h"

extern void ring3_enter(uintptr_t user_eip, uintptr_t user_esp);

/* The userland code is wrapped in a function that uses ONLY the syscall
 * inline-asm helpers from syscall.h - it never touches privileged
 * instructions, never reads from kernel memory directly.  Compiled
 * identically to the rest of the kernel for now (single binary) - in a
 * future build we'd link it as a separate ELF with its own .text. */
static const char g_demo_banner1[] =
    "\n[ring3] hello from CPL=3!  SYS_TERM_PUTS is alive.\n";
static const char g_demo_banner2[] =
    "[ring3] requesting SYS_PIT_MS ...\n";
static const char g_demo_banner3[] =
    "[ring3] all syscalls returned cleanly - dropping back to ring 0.\n";

static jmp_buf g_user_return;
static bool    g_user_running = false;

/* Userland entry point.  Compiled as ordinary kernel code but invoked
 * with CPL=3, so the only "legal" things it does are:
 *   - register-only arithmetic;
 *   - `int $0x80` to trap into the kernel.
 * Any other privileged operation would raise #GP. */
__attribute__((noreturn))
static void user_entry(void) {
    (void)syscall1(SYS_TERM_PUTS, (uint32_t)(uintptr_t)g_demo_banner1);
    (void)syscall1(SYS_TERM_PUTS, (uint32_t)(uintptr_t)g_demo_banner2);
    int32_t ms = syscall0(SYS_PIT_MS);

    /* Format the pit_ms value via a tiny on-stack buffer (avoid printf -
     * we don't have it as a syscall yet). */
    char line[64];
    /* Manual itoa is cheaper than dragging in libc here. */
    char numbuf[16];
    int  i = 0;
    uint32_t v = (uint32_t)ms;
    if (v == 0) { numbuf[i++] = '0'; }
    else {
        char tmp[12]; int t = 0;
        while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t) numbuf[i++] = tmp[--t];
    }
    numbuf[i] = 0;
    const char *p = "[ring3] SYS_PIT_MS returned: ";
    int n = 0;
    while (*p) line[n++] = *p++;
    p = numbuf;
    while (*p) line[n++] = *p++;
    line[n++] = '\n';
    line[n]   = 0;
    (void)syscall1(SYS_TERM_PUTS, (uint32_t)(uintptr_t)line);

    (void)syscall1(SYS_TERM_PUTS, (uint32_t)(uintptr_t)g_demo_banner3);

    /* SYS_EXIT - we override the dispatcher to longjmp here in the demo
     * (see syscall.c).  For now the kernel will simply return back from
     * the dispatcher and IRET to user mode; the user task's next
     * instruction is a syscall-loop fallback that idles until killed.
     * To make the demo terminate cleanly we longjmp out of ring 0 from
     * a wrapper that catches SYS_EXIT. */
    (void)syscall0(SYS_EXIT);
    /* If SYS_EXIT returns (it shouldn't in steady state), spin in a
     * polite hlt loop so the kernel can scrub us. */
    for (;;) {
        (void)syscall0(SYS_YIELD);
    }
}

/* ESP must be 4-byte aligned at minimum on i386.  16 KiB is more than
 * enough for the few syscalls the demo issues. */
ALIGNED(16) static uint8_t g_user_stack[16 * 1024];

/* Dedicated ring-0 stack used when an `int 0x80` (or any other ring 3 →
 * 0 transition) fires from inside one of our ring-3 tasks.  MUST be a
 * different physical buffer from g_user_stack / g_app_stack — the CPU
 * pushes SS:ESP, EFLAGS, CS, EIP onto whatever TSS.ESP0 points at, and
 * if that overlaps the user stack those CPU writes corrupt the user
 * task's most-recent stack frame (saved EBP / return address / spilled
 * locals).  That manifests as a #GP fault on IRET because the stub
 * returns to a garbled EIP.  Was the root cause of the "browser launch
 * → #GP at random EIP" report.
 *
 * 32 KiB is generous — the deepest call chain we drive through this
 * stack is SYS_INVOKE → apps_launch_trampoline → browser_open →
 * app_run_ring3 → browser_open_impl → wm_create_window → gfx blits,
 * which fits comfortably inside ~8 KiB even with debug printf scratch. */
ALIGNED(16) static uint8_t g_ring3_kstack[32 * 1024];

void usermode_demo(void) {
    term_set_color(VGA_CYAN, VGA_BLACK);
    term_printf("\n[usermode] launching ring-3 demo task...\n");
    term_set_color(VGA_LTGRAY, VGA_BLACK);

    if (setjmp(g_user_return) != 0) {
        /* SYS_EXIT path - we're back in kernel space. */
        g_user_running = false;
        term_set_color(VGA_GREEN, VGA_BLACK);
        term_printf("[usermode] ring-3 task SYS_EXIT - back in CPL=0.\n");
        term_set_color(VGA_LTGRAY, VGA_BLACK);
        return;
    }
    g_user_running = true;

    /* Update the TSS so int 0x80 / IRQs / exceptions raised from ring 3
     * land on g_ring3_kstack, NOT on g_user_stack.  Pointing ESP0 at the
     * user stack is the bug that corrupted the ring-3 IRET frame and
     * surfaced as a wild #GP at the next user-space instruction. */
    extern uint8_t _binary_stage1_bin_start[]; /* silence unused linker */
    (void)_binary_stage1_bin_start;
    gdt_set_kernel_stack((uint32_t)(uintptr_t)
                         (g_ring3_kstack + sizeof(g_ring3_kstack)));

    uint32_t user_eip = (uint32_t)(uintptr_t)user_entry;
    uint32_t user_esp = (uint32_t)(uintptr_t)
                        (g_user_stack + sizeof(g_user_stack) - 16);

    debug_printf("[usermode] ring3_enter eip=0x%x esp=0x%x\n",
                 user_eip, user_esp);
    ring3_enter(user_eip, user_esp);
    /* ring3_enter does not return.  SYS_EXIT longjmps to the setjmp() above. */
}

/* The dispatcher (syscall.c) calls this hook whenever SYS_EXIT is
 * received so a user-mode demo task can hand control back to the
 * kernel-side launcher without us needing a real task scheduler. */
void usermode_handle_exit(void) {
    if (g_user_running) {
        debug_printf("[usermode] SYS_EXIT - longjmp back to launcher\n");
        longjmp(g_user_return, 1);
    }
}

bool usermode_in_ring3(void) { return g_user_running; }

/* usermode_reset_after_panic() is defined further down, after the
 * g_app_* statics it touches. */

/* ============================================================================
 * app_run_ring3 — generic ring-3 launcher for NexxoN OS programs
 * ----------------------------------------------------------------------------
 * Same setjmp/longjmp pattern as usermode_demo, but parameterised so any
 * subsystem can borrow it.  The user-mode stub stores the result of the
 * SYS_INVOKE call in a kernel-visible static slot so the caller can
 * recover the return value after we longjmp out via SYS_EXIT.
 * ============================================================================ */

/* Trampoline state — single-slot because we have one cooperative ring-3
 * launch in flight at a time.  Nested launches would need a stack here. */
static app_fn_t  g_app_fn   = NULL;
static uint32_t  g_app_arg  = 0;
static int32_t   g_app_ret  = 0;

ALIGNED(16) static uint8_t g_app_stack[16 * 1024];

/* Ring-3 stub: this is the function the CPU actually executes at CPL=3.
 * It runs SYS_INVOKE to escalate back into kernel space for the real
 * work, captures the return value, then exits cleanly via SYS_EXIT.
 * Cannot use any global state directly because we're at CPL=3 with no
 * paging isolation — but we can write to plain kernel globals because
 * the demo address space is shared. */
__attribute__((noreturn))
static void app_ring3_stub(void) {
    /* SYS_INVOKE escalates to CPL=0 and runs g_app_fn(g_app_arg) on the
     * kernel stack.  When it returns, we're still at CPL=3.  Stash the
     * value so the caller can pick it up after we exit.
     *
     * Use kcall1 (which routes through syscall5) so EDX/ESI/EDI are
     * explicitly cleared instead of forwarding whatever garbage the
     * compiler left in them — the target function expects exactly one
     * argument, and any leak past that would be UB. */
    int32_t rv = kcall1((void *)(uintptr_t)g_app_fn, g_app_arg);
    g_app_ret = rv;
    (void)syscall0(SYS_EXIT);
    for (;;) (void)syscall0(SYS_YIELD);     /* unreachable */
}

int32_t app_run_ring3(app_fn_t fn, uint32_t arg, const char *name) {
    if (!fn) return -1;

    /* Reentrancy guard: if we're already inside a ring-3 task (e.g. an
     * app launcher recursively triggering another launcher), execute
     * the function directly at the current privilege level instead of
     * dropping again.  Nested ring3_enter would corrupt g_user_return
     * and strand the outer task. */
    if (g_user_running) {
        return fn(arg);
    }

    /* Stash `name` in a static slot so the longjmp path can re-read it
     * without tripping GCC's -Wclobbered.  Stack-resident locals live
     * in unspecified registers across setjmp/longjmp on x86. */
    static const char *s_app_name;
    s_app_name = name ? name : "?";

    debug_printf("[usermode] app_run_ring3(%s, fn=0x%x, arg=0x%x): "
                 "dropping to CPL=3\n",
                 s_app_name, (uint32_t)(uintptr_t)fn, arg);

    /* setjmp gives us a recovery point for SYS_EXIT to longjmp back to.
     * We share g_user_return with usermode_demo because there is only
     * one ring-3 task in flight at any moment (cooperative, single-
     * threaded). */
    if (setjmp(g_user_return) != 0) {
        g_user_running = false;
        debug_printf("[usermode] app_run_ring3(%s): back in CPL=0, ret=%d\n",
                     s_app_name, g_app_ret);
        return g_app_ret;
    }

    g_app_fn  = fn;
    g_app_arg = arg;
    g_app_ret = -1;
    g_user_running = true;

    /* TSS.ESP0 must point at a SEPARATE buffer from the user stack we're
     * about to enter on.  When int 0x80 fires from the ring-3 stub the
     * CPU dumps SS/ESP/EFLAGS/CS/EIP onto whatever TSS.ESP0 names; if
     * that aliases the user stack the writes land on top of the user
     * task's saved EBP + return address and the IRET back to ring 3
     * jumps to a garbled EIP — which is exactly the "#GP right after
     * browser launch" failure we were chasing. */
    gdt_set_kernel_stack((uint32_t)(uintptr_t)
                         (g_ring3_kstack + sizeof(g_ring3_kstack)));

    uint32_t user_eip = (uint32_t)(uintptr_t)app_ring3_stub;
    uint32_t user_esp = (uint32_t)(uintptr_t)
                        (g_app_stack + sizeof(g_app_stack) - 16);

    ring3_enter(user_eip, user_esp);
    /* ring3_enter does not return; SYS_EXIT will longjmp above. */
    return -1;
}

/* ============================================================================
 * apps_launch — thin bool/void shim over app_run_ring3
 * ----------------------------------------------------------------------------
 * The public app entry points (browser_open, gephaz_open, ...) all share
 * the `bool (*)(void)` signature.  This trampoline adapts that to the
 * `int32_t (*)(uint32_t)` shape app_run_ring3 wants, so callers don't
 * have to wrap each open function manually.
 *
 * The function pointer is passed *through* the `arg` channel of
 * app_run_ring3 instead of via a static slot.  Reason: a launch can be
 * reentered (an app's open() may pump wm_tick(), which delivers a
 * mouse click that opens another app), and a static would let the
 * inner launch overwrite the outer launch's function pointer.  Routing
 * through `arg` keeps each frame's state on the kernel stack where it
 * belongs.
 * ============================================================================ */

void usermode_reset_after_panic(void) {
    if (!g_user_running) return;
    debug_printf("[usermode] panic during ring-3 launch — clearing state "
                 "(fn=0x%x, arg=0x%x, ret=%d)\n",
                 (uint32_t)(uintptr_t)g_app_fn, g_app_arg, g_app_ret);
    g_user_running = false;
    g_app_fn  = NULL;
    g_app_arg = 0;
    g_app_ret = -1;
}

static int32_t apps_launch_trampoline(uint32_t open_fn_ptr) {
    app_open_fn_t open_fn = (app_open_fn_t)(uintptr_t)open_fn_ptr;
    if (!open_fn) return 0;
    return open_fn() ? 1 : 0;
}

bool apps_launch(app_open_fn_t open_fn, const char *name) {
    if (!open_fn) return false;
    int32_t rv = app_run_ring3(apps_launch_trampoline,
                               (uint32_t)(uintptr_t)open_fn, name);
    return rv != 0;
}

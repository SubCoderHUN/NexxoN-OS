/* ============================================================================
 * NexxoN OS - Ring 3 system-call dispatcher  (v1.0)
 * ----------------------------------------------------------------------------
 * Vector 0x80 is installed as a TRAP gate with DPL=3 so a CPL=3 task can
 * `int 0x80` without faulting.  The stub in boot/idt_flush.asm pushes a
 * registers_t structure identical to the one ISRs/IRQs already use, then
 * jumps into syscall_dispatch_isr() which:
 *
 *   1. Records ESP0 in the TSS (already done at boot).
 *   2. Dispatches based on EAX (the syscall number).
 *   3. Writes the return value back into the saved EAX so it appears in
 *      the user task's EAX after IRET.
 *
 * Pointer-typed args (filenames, buffers) are NOT yet sandboxed against
 * the kernel/user split because paging stays identity-mapped in this
 * release - that's the next step.  A future patch will copy_from_user /
 * copy_to_user every pointer; until then the dispatcher trusts callers.
 * ============================================================================ */
#include "syscall.h"
#include "isr.h"
#include "idt.h"
#include "nxfs.h"
#include "sched.h"
#include "window.h"
#include "terminal.h"
#include "keyboard.h"
#include "mouse.h"
#include "shell.h"
#include "pit.h"
#include "speaker.h"
#include "debug.h"
#include "string.h"
#include "usermode.h"
#include "net.h"
#include "crypto.h"
#include "download.h"
#include "ipc.h"
#include "clipboard.h"
#include "posix.h"
#include "audio.h"
#include "linuxsys.h"

extern void isr128(void);   /* int 0x80 stub */
extern void usermode_handle_exit(void);

void syscall_init(void) {
    debug_step("syscall: installing int 0x80 trap gate (DPL=3)");
    /* 0xEE = present (bit 7), DPL=3 (bits 5..6), type 0xE (32-bit int gate). */
    idt_set_gate(0x80, (uint32_t)(uintptr_t)isr128, 0x08, 0xEE);
    debug_ok("syscall: int 0x80 ready, table has 0x60 entries");
}

/* The dispatcher is called from boot/idt_flush.asm with all five
 * argument registers as positional parameters.  Returning a value here
 * places it in EAX before IRET (the stub stores our return into the
 * saved-EAX slot of the registers_t pushed onto the kernel stack). */
int32_t syscall_dispatch(uint32_t num, uint32_t a0, uint32_t a1,
                         uint32_t a2, uint32_t a3, uint32_t a4) {
    if (linux_compat32_active())
        return linux_compat32_syscall(num, a0, a1, a2, a3, a4);
    switch (num) {
        /* ---- Lifecycle ------------------------------------------------ */
        case SYS_EXIT:
            /* Hand off to the user-mode launcher which longjmps back
             * into kernel space.  The longjmp does not return through
             * the syscall dispatcher path - control resumes in
             * usermode_demo()'s setjmp branch. */
            usermode_handle_exit();
            return 0;
        case SYS_YIELD:
            sched_yield();
            return 0;
        case SYS_GETPID:         return wm_focused_id();
        case SYS_KILL: {
            window_t *wins[WM_MAX_WINDOWS];
            int n = wm_get_windows(wins, WM_MAX_WINDOWS);
            for (int i = 0; i < n; i++) {
                if (wins[i]->id == (int)a0 && !wins[i]->protected) {
                    wm_destroy_window(wins[i]);
                    return 0;
                }
            }
            return -1;
        }
        case SYS_PIT_MS:         return (int32_t)pit_ms();
        case SYS_LAST_INPUT_MS:  return (int32_t)pit_ms();  /* TODO */

        /* ---- Terminal -------------------------------------------------- */
        case SYS_TERM_PUTC:      term_putc((char)a0); return 0;
        case SYS_TERM_PUTS:      term_puts((const char *)(uintptr_t)a0); return 0;
        case SYS_TERM_GETC:      return keyboard_wait_getc();
        case SYS_TERM_HAS_KEY:   return keyboard_has_data() ? 1 : 0;
        case SYS_SHELL_EXEC:
            shell_exec((const char *)(uintptr_t)a0);
            return 0;

        /* ---- NXFS ------------------------------------------------------ */
        case SYS_NX_CWD:         return (int32_t)nxfs_cwd();
        case SYS_NX_PWD:
            return nxfs_pwd_path((char *)(uintptr_t)a0, (size_t)a1);
        case SYS_NX_RESOLVE:
            return nxfs_resolve(a0, (const char *)(uintptr_t)a1,
                                (uint32_t *)(uintptr_t)a2);
        case SYS_NX_READ: {
            uint32_t got = 0;
            int r = nxfs_read_file(a0, (void *)(uintptr_t)a1, a2, &got);
            uint32_t *gotp = (uint32_t *)(uintptr_t)a3;
            if (gotp) *gotp = got;
            return r;
        }
        case SYS_NX_WRITE:
            return nxfs_write_file(a0, (const void *)(uintptr_t)a1, a2);
        case SYS_NX_CREATE:
            return nxfs_create_file(a0, (const char *)(uintptr_t)a1,
                                    (uint32_t *)(uintptr_t)a2);
        case SYS_NX_DELETE:
            return nxfs_delete_file(a0, (const char *)(uintptr_t)a1);
        case SYS_NX_MKDIR:
            return nxfs_create_dir(a0, (const char *)(uintptr_t)a1,
                                   (uint32_t *)(uintptr_t)a2);
        case SYS_NX_RMDIR:
            return nxfs_delete_dir(a0, (const char *)(uintptr_t)a1);
        case SYS_NX_RENAME:
            return nxfs_rename(a0, (const char *)(uintptr_t)a1,
                               (const char *)(uintptr_t)a2);
        case SYS_NX_SETCWD:
            return nxfs_set_cwd(a0);

        /* ---- WM -------------------------------------------------------- */
        case SYS_WM_MARK_DIRTY:  wm_mark_dirty(); return 0;
        case SYS_WM_POOL_STATS: {
            uint32_t used, total;
            wm_pool_stats(&used, &total);
            uint32_t *u = (uint32_t *)(uintptr_t)a0;
            uint32_t *t = (uint32_t *)(uintptr_t)a1;
            if (u) *u = used;
            if (t) *t = total;
            return 0;
        }

        /* ---- Input ----------------------------------------------------- */
        case SYS_MOUSE_POLL: {
            int x, y; uint8_t btn;
            (void)mouse_poll(&x, &y, &btn);
            int *px = (int *)(uintptr_t)a0;
            int *py = (int *)(uintptr_t)a1;
            uint8_t *pb = (uint8_t *)(uintptr_t)a2;
            if (px) *px = x;
            if (py) *py = y;
            if (pb) *pb = btn;
            return 0;
        }
        case SYS_KBD_GETC:       return keyboard_getc();
        case SYS_KBD_HAS_DATA:   return keyboard_has_data() ? 1 : 0;

        /* ---- Audio ----------------------------------------------------- */
        case SYS_SPEAKER_BEEP:   speaker_beep(a0, a1); return 0;
        case SYS_AUDIO_PLAY_PCM:
            return audio_play_pcm((const int16_t *)(uintptr_t)a0,
                                  a1, (int)a2, (int)a3);

        /* ---- Network --------------------------------------------------- */
        case SYS_SOCKET:    return 0;   /* one virtual socket type: TCP    */
        case SYS_CONNECT:   return tcp_connect(a0, (uint16_t)a1, a2);
        case SYS_SEND:      return tcp_send((tcp_handle_t)a0,
                                            (const void *)(uintptr_t)a1, a2);
        case SYS_RECV:      return tcp_recv((tcp_handle_t)a0,
                                            (void *)(uintptr_t)a1, a2, a3);
        case SYS_CLOSE:     tcp_close((tcp_handle_t)a0); return 0;
        case SYS_PING:      return net_ping(a0, a1 ? a1 : 1500);
        case SYS_NET_CONFIG:
            net_get_config((net_config_t *)(uintptr_t)a0);
            return 0;
        case SYS_SECURE_CONNECT:
            return tls_connect(a0, (uint16_t)a1, NULL, a2 ? a2 : 5000);
        case SYS_SECURE_SEND:
            return tls_send((tls_handle_t)a0,
                            (const void *)(uintptr_t)a1, a2);
        case SYS_SECURE_RECV:
            return tls_recv((tls_handle_t)a0,
                            (void *)(uintptr_t)a1, a2, a3);
        case SYS_SECURE_CLOSE:
            tls_close((tls_handle_t)a0); return 0;
        case SYS_DOWNLOAD:
            return download_simple((const char *)(uintptr_t)a0,
                                   (void *)(uintptr_t)a1, a2);

        /* IPC: shared memory + message queues (TASK 21). */
        case SYS_SHM_GET:     return ipc_shm_get(a0, a1, a2);
        case SYS_SHM_ATTACH:  return (int32_t)(uintptr_t)ipc_shm_attach((int)a0);
        case SYS_SHM_DETACH:  return ipc_shm_detach((int)a0);
        case SYS_SHM_DESTROY: return ipc_shm_destroy((int)a0);
        case SYS_MSG_SEND:    return ipc_msg_send((int)a0,
                                                 (const void *)(uintptr_t)a1, a2);
        case SYS_MSG_RECV:    return ipc_msg_recv((int)a0,
                                                 (void *)(uintptr_t)a1, a2);
        case SYS_MSG_PEEK:    return ipc_msg_peek((int)a0);

        /* Clipboard (TASK 25). */
        case SYS_CLIP_SET:    return clipboard_set_mime(
                                  (const char *)(uintptr_t)a0,
                                  (const void *)(uintptr_t)a1, a2);
        case SYS_CLIP_GET:    return clipboard_get_mime(
                                  (const char *)(uintptr_t)a0,
                                  (void *)(uintptr_t)a1, a2);
        case SYS_CLIP_LIST:   return clipboard_list_mimes((int)a0,
                                  (char *)(uintptr_t)a1, a2);

        /* POSIX (TASK 29). */
        case SYS_FORK:           return posix_fork();
        case SYS_EXECVE:         return posix_execve(
                                     (const char *)(uintptr_t)a0,
                                     (char *const *)(uintptr_t)a1,
                                     (char *const *)(uintptr_t)a2);
        case SYS_PTHREAD_CREATE: return posix_pthread_create(
                                     (uint32_t *)(uintptr_t)a0,
                                     (void *(*)(void *))(uintptr_t)a1,
                                     (void *)(uintptr_t)a2);
        case SYS_MMAP:           return (int32_t)(uintptr_t)posix_mmap(
                                     (void *)(uintptr_t)a0, a1, (int)a2,
                                     (int)a3, 0, 0);
        case SYS_MUNMAP:         return posix_munmap((void *)(uintptr_t)a0, a1);
        case SYS_WAITPID:        return posix_waitpid((pid_t)a0,
                                     (int *)(uintptr_t)a1, (int)a2);

        /* ---- Ring-3 escape hatch -------------------------------------- *
         * SYS_INVOKE is the "call any kernel function" bridge used by
         * apps that have already dropped to CPL=3 but still need to
         * reach kernel APIs that don't yet have a dedicated syscall
         * (gfx blits, font rasterisation, JPEG decode, etc.).  The
         * function executes on the kernel stack at CPL=0, then we
         * IRET back to ring 3.  Pointer is taken on trust — the same
         * trust model the kernel already uses for indirect calls. */
        case SYS_INVOKE: {
            if (a0 == 0) return -1;
            typedef int32_t (*kfn_t)(uint32_t, uint32_t, uint32_t, uint32_t);
            kfn_t fn = (kfn_t)(uintptr_t)a0;
            /* App entry points run real, possibly-blocking work: pit_sleep()
             * (Bluetooth scan/pair), network waits (NexxStore package index),
             * etc.  int 0x80 enters through an interrupt gate, so IF=0 here --
             * a pit_sleep()/hlt inside the app would then halt the CPU forever
             * because no timer tick can ever wake it.  That is exactly why the
             * system froze the instant Bluetooth or NexxStore was opened.
             * Re-enable interrupts for the duration of the call so blocking
             * primitives work (and the scheduler can preempt), then restore the
             * IF=0 state the rest of the dispatcher / IRET epilogue expects. */
            __asm__ volatile ("sti");
            int32_t rv = fn(a1, a2, a3, a4);
            __asm__ volatile ("cli");
            return rv;
        }

        default:
            debug_printf("[syscall] unknown number %u (a0=%u a1=%u a2=%u)\n",
                         num, a0, a1, a2);
            return -1;
    }
}

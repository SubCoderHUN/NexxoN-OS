/* ============================================================================
 * NexxoN OS - Ring 3 system-call interface  (v1.0)
 * ----------------------------------------------------------------------------
 * Userland enters the kernel through `int 0x80`.  The convention - chosen
 * for register-pressure economy over GCC's freestanding inline asm - is:
 *
 *   EAX  syscall number (one of SYS_*)
 *   EBX  arg 0
 *   ECX  arg 1
 *   EDX  arg 2
 *   ESI  arg 3
 *   EDI  arg 4
 *
 * The kernel's int-0x80 stub dispatches through syscall_dispatch() which
 * returns a 32-bit value placed back in EAX before IRET.
 *
 * The syscall list below was engineered after auditing the visual / disk /
 * lifecycle needs of every shipping app:
 *
 *   * Shell             - read/write the console, dispatch other shell
 *                         commands, and ask for the cwd path string.
 *   * Explorer          - NXFS list/create/delete/rename + dialog primitives.
 *   * Task Manager      - enumerate windows + kill by id + read pool stats.
 *   * Editor            - read/write a file by inode, plus terminal I/O.
 *   * Pong              - WM blits + mouse polling + pit_ms() timestamp.
 *   * Screensaver       - direct gfx_screen() blits + last-input timestamp.
 *
 * Numbering reserves blocks of 16 per concern so future additions don't
 * renumber existing callers.
 * ============================================================================ */
#ifndef NEXXON_SYSCALL_H
#define NEXXON_SYSCALL_H

#include "types.h"

/* ---- Process / lifecycle  (0x00..0x0F) -------------------------------- */
#define SYS_EXIT          0x00      /* terminate current ring-3 task        */
#define SYS_YIELD         0x01      /* relinquish to the scheduler          */
#define SYS_GETPID        0x02      /* return our window-id-as-pid          */
#define SYS_KILL          0x03      /* destroy another window (taskmgr)     */
#define SYS_PIT_MS        0x04      /* uint32 milliseconds since boot       */
#define SYS_LAST_INPUT_MS 0x05      /* screensaver idle detection           */

/* ---- Terminal / shell  (0x10..0x1F) ----------------------------------- */
#define SYS_TERM_PUTC     0x10
#define SYS_TERM_PUTS     0x11      /* arg0 = const char*                   */
#define SYS_TERM_GETC     0x12      /* blocking; returns int                */
#define SYS_TERM_HAS_KEY  0x13
#define SYS_SHELL_EXEC    0x14      /* arg0 = const char* command           */

/* ---- NXFS  (0x20..0x2F) ----------------------------------------------- */
#define SYS_NX_CWD        0x20
#define SYS_NX_PWD        0x21      /* arg0=buf, arg1=cap                   */
#define SYS_NX_RESOLVE    0x22      /* arg0=cwd, arg1=name, arg2=out_ino    */
#define SYS_NX_READ       0x23      /* arg0=ino, arg1=buf, arg2=cap, arg3=*got */
#define SYS_NX_WRITE      0x24      /* arg0=ino, arg1=buf, arg2=len         */
#define SYS_NX_CREATE     0x25      /* arg0=parent, arg1=name, arg2=*out    */
#define SYS_NX_DELETE     0x26      /* arg0=parent, arg1=name               */
#define SYS_NX_MKDIR      0x27      /* arg0=parent, arg1=name, arg2=*out    */
#define SYS_NX_RMDIR      0x28
#define SYS_NX_RENAME     0x29
#define SYS_NX_LIST       0x2A      /* arg0=dir, arg1=cb, arg2=user         */
#define SYS_NX_SETCWD     0x2B

/* ---- WM / graphics  (0x30..0x3F) -------------------------------------- */
#define SYS_WM_CREATE     0x30      /* arg0..3 = x,y,w,h ; arg4 = title     */
#define SYS_WM_DESTROY    0x31      /* arg0 = id                            */
#define SYS_WM_LIST       0x32      /* arg0 = buffer of window_info_t      */
#define SYS_WM_MARK_DIRTY 0x33
#define SYS_WM_FOCUS      0x34
#define SYS_WM_BLIT       0x35      /* arg0=id, arg1=pixels, arg2=w,h packed*/
#define SYS_WM_POOL_STATS 0x36

/* ---- Input  (0x40..0x4F) ---------------------------------------------- */
#define SYS_MOUSE_POLL    0x40      /* arg0=*x, arg1=*y, arg2=*btn          */
#define SYS_KBD_GETC      0x41
#define SYS_KBD_HAS_DATA  0x42

/* ---- Audio (0x50..0x5F) ----------------------------------------------- */
#define SYS_SPEAKER_BEEP  0x50      /* arg0 = hz, arg1 = ms                 */
#define SYS_AUDIO_PLAY_PCM 0x51     /* arg0=*samples, arg1=frames,
                                       arg2=channels, arg3=rate             */

/* ---- Network (0x60..0x6F) - sockets + ping --------------------------- *
 * Connect/send/recv are 1:1 with the tcp_* helpers in net.h - they exist
 * in this table so a Ring 3 task can drive the network entirely via
 * int 0x80 without touching the NIC MMIO directly (any attempt at that
 * triggers a #GP because the MMIO range is supervisor-only under the
 * Ring 3 isolation model). */
#define SYS_SOCKET        0x60      /* placeholder: returns 0 for TCP      */
#define SYS_CONNECT       0x61      /* arg0=ip, arg1=port, arg2=timeout    */
#define SYS_SEND          0x62      /* arg0=handle, arg1=buf, arg2=len     */
#define SYS_RECV          0x63      /* arg0=handle, arg1=buf, arg2=cap,
                                       arg3=timeout                        */
#define SYS_CLOSE         0x64      /* arg0=handle                         */
#define SYS_PING          0x65      /* arg0=ip, arg1=timeout_ms            */
#define SYS_NET_CONFIG    0x66      /* arg0=out_net_config_t*              */
#define SYS_SECURE_CONNECT 0x67     /* arg0=ip, arg1=port, arg2=timeout   */
#define SYS_SECURE_SEND   0x68
#define SYS_SECURE_RECV   0x69
#define SYS_SECURE_CLOSE  0x6A
#define SYS_DOWNLOAD      0x6B      /* arg0=url, arg1=buf, arg2=cap         */

/* ---- Settings / persistence (0x70..0x7F) ----------------------------- */
#define SYS_SETTINGS_GET  0x70      /* arg0=key, arg1=buf, arg2=cap        */
#define SYS_SETTINGS_SET  0x71      /* arg0=key, arg1=value                */

/* ---- IPC: Shared Memory + Message Queues (0x80..0x8F) ---------------- */
#define SYS_SHM_GET       0x80      /* arg0=key, arg1=size, arg2=flags     */
#define SYS_SHM_ATTACH    0x81      /* arg0=id, returns void* ptr          */
#define SYS_SHM_DETACH    0x82
#define SYS_SHM_DESTROY   0x83
#define SYS_MSG_SEND      0x84      /* arg0=queue, arg1=buf, arg2=len      */
#define SYS_MSG_RECV      0x85      /* arg0=queue, arg1=buf, arg2=cap      */
#define SYS_MSG_PEEK      0x86

/* ---- POSIX-style process / threading (0x90..0x9F) -------------------- */
#define SYS_FORK          0x90
#define SYS_EXECVE        0x91
#define SYS_PTHREAD_CREATE 0x92
#define SYS_MMAP          0x93
#define SYS_MUNMAP        0x94
#define SYS_WAITPID       0x95

/* ---- Clipboard (multi-format, 0xA0..0xAF) ---------------------------- */
#define SYS_CLIP_SET      0xA0      /* arg0=mime, arg1=buf, arg2=len       */
#define SYS_CLIP_GET      0xA1      /* arg0=mime, arg1=buf, arg2=cap       */
#define SYS_CLIP_LIST     0xA2      /* arg0=idx, arg1=out_buf, arg2=cap    */

/* ---- Ring-3 escape hatch (0xB0..0xBF) -------------------------------- *
 * SYS_INVOKE lets a ring-3 application call any kernel function whose
 * address it already knows (single-binary kernel today; once we ship a
 * proper user-space ABI this collapses into per-subsystem syscalls).
 *   arg0 = function pointer (must point into kernel .text)
 *   arg1..arg4 = positional arguments passed verbatim
 * Returns whatever the called function returned, truncated to int32_t.
 *
 * This is the bridge that lets an app run at CPL=3 without rewriting
 * every wm_/gfx_/tcp_/font_ call site as a dedicated syscall.  It
 * is intentionally dangerous (no pointer validation, no access
 * control) — exactly the same trust boundary the kernel already has
 * with itself.  The CPL=3 enforcement still catches privileged
 * instructions, port I/O, CLI/STI, CR0/CR3 access, etc. */
#define SYS_INVOKE        0xB0      /* arg0=fn, arg1..4 = args            */

#define SYS_MAX           0xC0

/* ---- Userland-side inline-asm helpers (compiled into ring 3 apps) ---- *
 * Kept in the public header so the future user-space binaries can include
 * it directly and the kernel can syscall-test by linking the same
 * helpers in (single-binary kernel for now; multi-binary later).        */
static inline int32_t syscall0(uint32_t n) {
    int32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}
static inline int32_t syscall1(uint32_t n, uint32_t a0) {
    int32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a0) : "memory");
    return r;
}
static inline int32_t syscall2(uint32_t n, uint32_t a0, uint32_t a1) {
    int32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a0), "c"(a1) : "memory");
    return r;
}
static inline int32_t syscall3(uint32_t n, uint32_t a0, uint32_t a1, uint32_t a2) {
    int32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a0), "c"(a1), "d"(a2)
                                    : "memory");
    return r;
}
static inline int32_t syscall4(uint32_t n, uint32_t a0, uint32_t a1,
                                uint32_t a2, uint32_t a3) {
    int32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r)
                                  : "a"(n), "b"(a0), "c"(a1), "d"(a2), "S"(a3)
                                  : "memory");
    return r;
}
static inline int32_t syscall5(uint32_t n, uint32_t a0, uint32_t a1,
                                uint32_t a2, uint32_t a3, uint32_t a4) {
    int32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r)
                                  : "a"(n), "b"(a0), "c"(a1), "d"(a2),
                                    "S"(a3), "D"(a4)
                                  : "memory");
    return r;
}

/* SYS_INVOKE convenience wrappers — call an arbitrary kernel function
 * from a ring-3 app.  `fn` is a real kernel function pointer; the
 * dispatcher casts it back and invokes it on the kernel stack.
 *
 * Each wrapper always sends 5 args through syscall5 so EDX/ESI/EDI
 * are explicitly zeroed for unused slots.  Without this, GCC could
 * leave whatever user-mode garbage was last in those registers, and
 * the kernel-side SYS_INVOKE handler would pass that garbage to the
 * target function — exactly the kind of latent UB that surfaces as
 * a phantom #GP / #PF when the target later dereferences it. */
static inline int32_t kcall0(void *fn) {
    return syscall5(SYS_INVOKE, (uint32_t)(uintptr_t)fn, 0, 0, 0, 0);
}
static inline int32_t kcall1(void *fn, uint32_t a0) {
    return syscall5(SYS_INVOKE, (uint32_t)(uintptr_t)fn, a0, 0, 0, 0);
}
static inline int32_t kcall2(void *fn, uint32_t a0, uint32_t a1) {
    return syscall5(SYS_INVOKE, (uint32_t)(uintptr_t)fn, a0, a1, 0, 0);
}
static inline int32_t kcall3(void *fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    return syscall5(SYS_INVOKE, (uint32_t)(uintptr_t)fn, a0, a1, a2, 0);
}
static inline int32_t kcall4(void *fn, uint32_t a0, uint32_t a1,
                              uint32_t a2, uint32_t a3) {
    return syscall5(SYS_INVOKE, (uint32_t)(uintptr_t)fn, a0, a1, a2, a3);
}

/* ---- Kernel-side dispatcher ----------------------------------------- */
struct registers;
void syscall_init    (void);
int32_t syscall_dispatch(uint32_t num, uint32_t a0, uint32_t a1,
                         uint32_t a2, uint32_t a3, uint32_t a4);

#endif /* NEXXON_SYSCALL_H */

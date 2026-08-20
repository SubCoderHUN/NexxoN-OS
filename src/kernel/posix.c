/* ============================================================================
 * NexxoN OS - POSIX compatibility layer
 * ----------------------------------------------------------------------------
 * Stub implementations that surface the syscall numbers so the int 0x80
 * dispatcher in syscall.c can route them.  Real fork/execve will arrive
 * with the per-process page table milestone; until then the calls
 * return -1 with a debug log so user-space binaries can detect "feature
 * not yet available" rather than silently misbehaving.
 * ============================================================================ */
#include "posix.h"
#include "syscall.h"
#include "sched.h"
#include "vmm.h"
#include "debug.h"

int   posix_fork(void) {
    debug_printf("[posix] fork: not yet implemented (per-process MMU pending)\n");
    return -1;
}

int   posix_execve(const char *path, char *const argv[], char *const envp[]) {
    (void)argv; (void)envp;
    debug_printf("[posix] execve('%s'): pending\n", path ? path : "(null)");
    return -1;
}

/* Trampoline that bridges the POSIX void*(*)(void*) calling convention to
 * sched_spawn's task_fn_t = void(*)(uint32_t).  We stash the fn+arg pair in
 * a small heap-on-frame-pool block, then call fn(arg) from the new task. */
typedef struct { void *(*fn)(void *); void *arg; } pthread_arg_t;

static void pthread_kernel_entry(uint32_t raw_arg) {
    pthread_arg_t *pa = (pthread_arg_t *)(uintptr_t)raw_arg;
    void *(*fn)(void *) = pa->fn;
    void *arg = pa->arg;
    vmm_frame_free(raw_arg);   /* release the arg block */
    fn(arg);
}

int   posix_pthread_create(uint32_t *thr, void *(*fn)(void *), void *arg) {
    if (!fn) return -1;

    /* Allocate one physical frame (4 KiB) to hold the pthread_arg_t.
     * With paging enabled and full identity-map, phys == virt. */
    uint32_t phys = vmm_frame_alloc();
    if (!phys) {
        debug_printf("[posix] pthread_create: no frame for arg block\n");
        return -1;
    }
    pthread_arg_t *pa = (pthread_arg_t *)(uintptr_t)phys;
    pa->fn  = fn;
    pa->arg = arg;

    tcb_t *t = sched_spawn("pthread", pthread_kernel_entry, phys,
                            vmm_kernel_pd());
    if (!t) {
        vmm_frame_free(phys);
        debug_printf("[posix] pthread_create: no free task slot\n");
        return -1;
    }
    if (thr) *thr = t->pid;
    debug_printf("[posix] pthread_create: spawned pid=%u\n", t->pid);
    return 0;
}

void *posix_mmap(void *addr, uint32_t length, int prot, int flags,
                 int fd, off_t offset) {
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
    /* Without page tables we can't service a fresh anonymous mapping
     * safely.  Return MAP_FAILED. */
    return (void *)(intptr_t)-1;
}

int   posix_munmap(void *addr, uint32_t length) {
    (void)addr; (void)length;
    return 0;
}

pid_t posix_waitpid(pid_t pid, int *status, int options) {
    (void)pid; (void)options;
    if (status) *status = 0;
    return -1;
}

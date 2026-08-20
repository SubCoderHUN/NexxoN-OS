/* ============================================================================
 * NexxoN OS - POSIX Compatibility Layer / libc bridge  (TASK 29, v1.0)
 * ----------------------------------------------------------------------------
 * Translates a thin subset of the POSIX API into NexxoN syscalls.  Used
 * by user-space binaries compiled against libc.nxl.  Functions sit
 * inside the kernel so libc.nxl is just a relocation table; once the
 * user-space ELF loader is wired we'll move them into the userland
 * library proper.
 *
 * Implemented (mapped to SYS_* via int 0x80):
 *
 *     pid_t  fork(void)                       -> SYS_FORK
 *     int    execve(path, argv, envp)         -> SYS_EXECVE
 *     int    pthread_create(thr, fn, arg)     -> SYS_PTHREAD_CREATE
 *     void  *mmap(addr, len, prot, flags, fd, offset) -> SYS_MMAP
 *     int    munmap(addr, len)                -> SYS_MUNMAP
 *     pid_t  waitpid(pid, status, opts)       -> SYS_WAITPID
 *
 * Plus the obvious wrappers around existing NexxoN syscalls:
 *     read / write / open / close / lseek      -> SYS_NX_*
 *     getpid / exit / sleep                    -> SYS_GETPID / SYS_EXIT / SYS_PIT_MS
 * ============================================================================ */
#ifndef NEXXON_POSIX_H
#define NEXXON_POSIX_H

#include "types.h"

typedef int32_t pid_t;
typedef int32_t off_t;
typedef int32_t mode_t;

int    posix_fork           (void);
int    posix_execve         (const char *path, char *const argv[],
                             char *const envp[]);
int    posix_pthread_create (uint32_t *thr, void *(*fn)(void *), void *arg);
void  *posix_mmap           (void *addr, uint32_t length, int prot,
                             int flags, int fd, off_t offset);
int    posix_munmap         (void *addr, uint32_t length);
pid_t  posix_waitpid        (pid_t pid, int *status, int options);

#endif /* NEXXON_POSIX_H */

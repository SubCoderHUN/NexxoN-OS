/* ============================================================================
 * NexxoN OS - Linux x86_64 compatibility layer
 * ----------------------------------------------------------------------------
 * Runs static (or static-PIE) x86_64 Linux ELF binaries at CPL=3 with a
 * Linux syscall emulation table reached through the native `syscall`
 * instruction (MSR_LSTAR).  The current static-runtime milestone targets
 * musl-static programs in a private CR3: memory, time, terminal, directory and
 * file calls are emulated; descriptors are backed by NXFS plus /dev
 * pseudo-files.  PT_INTERP / musl-dynamic binaries are supported when /lib
 * contains ld-musl-x86_64.so.1 and libc.so.  User VA >=0x20000000 is reserved
 * for PIE/ET_EXEC program images; the interpreter maps in the low window.
 *
 * LIMITATIONS (see docs/NEXT_STEPS_BARE_METAL.md): glibc and full pthread
 * remain future milestones; v10 adds CLONE_THREAD + blocking futex for musl
 * pthread; v9 adds AF_INET socket syscalls bridged to the kernel TCP/UDP stack.
 * ============================================================================ */
#ifndef NEXXON_LINUXSYS_H
#define NEXXON_LINUXSYS_H

#include "types.h"
#include "isr.h"

/* One-time init: program the SYSCALL MSRs (EFER.SCE, STAR, LSTAR, SFMASK). */
void linux_syscall_init(void);

/* Load + run a Linux ELF image already in memory.  argv[0] is `name`.
 * Returns the process exit code, or a negative kernel error. */
int  linux_run_image(const uint8_t *img, uint32_t len,
                     const char *name, char *out_msg, uint32_t out_cap);

/* Load + run a Linux ELF from an NXFS path (e.g. "/programs/hello"). */
int  linux_run_path(const char *path, char *out_msg, uint32_t out_cap);

/* Argument-aware path launcher used by `linux <path> [args...]`. */
int  linux_run_path_args(const char *path, int argc, const char *const *argv,
                         char *out_msg, uint32_t out_cap);

/* Enter the interactive Linux subsystem shell (/bin/sh). */
int  linux_subsystem_enter(char *out_msg, uint32_t out_cap);

/* Recover a CPL=3 guest fault as a process exit instead of panicking the OS.
 * Returns false when no Linux compatibility process is active. */
bool linux_handle_user_exception_frame(registers_t *frame,
                                       uint64_t vector, uint64_t err,
                                       uint64_t rip);

/* IA-32 compatibility-mode int 0x80 bridge used by 32-bit Linux guests. */
bool    linux_compat32_active(void);
int32_t linux_compat32_syscall(uint32_t num, uint32_t a0, uint32_t a1,
                               uint32_t a2, uint32_t a3, uint32_t a4);

/* Per-thread fd snapshots for fork(2) parent/child isolation. */
void lin_fds_snap_thread(int slot);
void lin_fds_load_thread(int slot);
void lin_fds_fork_dup(int parent_slot, int child_slot);

#endif /* NEXXON_LINUXSYS_H */

/* ============================================================================
 * NexxoN OS - Linux pthread / CLONE_THREAD + futex scheduler
 * ============================================================================ */
#pragma once

#include "types.h"

typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags, user_rsp;
} lin_regs_t;

#define LIN_THREAD_SWITCHED  (-999999L)

bool lin_thread_session_active(void);
void lin_thread_reset(void);
void lin_thread_ensure_boot(lin_regs_t *r);

long lin_thread_clone(uint64_t flags, uint64_t child_stack, uint64_t ptid,
                      uint64_t ctid, uint64_t tls, lin_regs_t *r);

long lin_thread_futex_wait(uint64_t uaddr, uint32_t val, uint64_t timeout,
                           lin_regs_t *r, bool *switched);

long lin_thread_futex_wake(uint64_t uaddr, int count);

void lin_thread_yield(lin_regs_t *r, bool *switched);

bool lin_thread_try_exit(lin_regs_t *r, int code, bool exit_group);

bool lin_thread_handle_fault(lin_regs_t *frame, int sig);

uint32_t lin_thread_get_tid(void);
uint32_t lin_thread_set_tid_address(uint64_t addr);

/* IA-32 int 0x80 thread paths used by i386 glibc NPTL (Steam updater). */
long lin_thread_compat32_clone(uint32_t flags, uint32_t child_stack,
                               uint32_t ptid, uint32_t ctid, uint32_t tls);
int32_t lin_thread_compat32_futex_wait(uint32_t uaddr, uint32_t val,
                                       uint32_t timeout_ptr);
int32_t lin_thread_compat32_futex_wake(uint32_t uaddr, int count);
int32_t lin_thread_compat32_yield(void);
bool lin_thread_compat32_try_exit(int code, bool exit_group,
                                  int32_t *resume_rax);
void lin_thread_compat32_note_tls(uint32_t base, uint32_t limit, bool page_gran);
uint32_t lin_compat32_default_thread_stack(void);
uint32_t lin_thread_compat32_fork_spawn(uint32_t child_stack);
bool lin_thread_compat32_fork_exit(int code, int32_t *resume_rax);
int32_t lin_thread_compat32_resume_parent(int32_t *resume_rax);

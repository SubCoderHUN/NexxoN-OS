/* ============================================================================
 * NexxoN OS - Linux pthread / CLONE_THREAD + blocking futex
 * ============================================================================ */
#include "lin_thread.h"
#include "vmm.h"
#include "pit.h"
#include "debug.h"
#include "string.h"
#include "gdt.h"
#include "linuxsys.h"

extern uint32_t lin_compat32_default_thread_stack(void);

#define MSR_FS_BASE  0xC0000100u

#define CLONE_VM              0x00000100u
#define CLONE_SETTLS          0x00080000u
#define CLONE_PARENT_SETTID   0x00100000u
#define CLONE_CHILD_CLEARTID  0x00200000u
#define CLONE_THREAD          0x00010000u

#define L_EAGAIN   11
#define L_EFAULT   14
#define L_EINVAL   22
#define L_ETIMEDOUT 110

#define LIN_COMPAT32_FRAME_BYTES 168u
#define LIN_COMPAT32_RAX_OFF     112u
#define LIN_COMPAT32_RIP_OFF     120u
#define LIN_COMPAT32_RSP_OFF     144u

extern vmm_pd_t *lin_host_pd(void);
extern bool lin_host_running(void);
extern bool lin_host_sync_running(void);
extern bool lin_host_fork_child(void);
extern bool lin_host_pe_running(void);
extern uint32_t lin_host_pid(void);
extern bool linux_compat32_active(void);
extern uint64_t g_lin_compat_frame_rsp;
extern uint32_t g_lin_compat_rip;
extern int lin_ucopy_from(void *dst, uint64_t src, uint32_t len);
extern int lin_ucopy_to(uint64_t dst, const void *src, uint32_t len);
extern int lin_ucopy_u32(uint64_t addr, uint32_t *out);
extern int lin_ucopy_u32_write(uint64_t addr, uint32_t val);

#define LIN_THREADS_MAX 16

typedef enum {
    LIN_THR_FREE = 0,
    LIN_THR_RUNNABLE,
    LIN_THR_BLOCKED_FUTEX,
    LIN_THR_EXITED,
} lin_thr_state_t;

typedef struct {
    bool            used;
    bool            compat32;
    lin_thr_state_t state;
    uint32_t        tid;
    bool            fork_proc;
    uint64_t        fs_base;
    uint64_t        clear_tid;
    uint64_t        futex_addr;
    uint32_t        futex_val;
    int32_t         compat_rax;
    uint32_t        tls_base;
    uint32_t        tls_limit;
    bool            tls_pg;
    uint8_t         compat_frame[LIN_COMPAT32_FRAME_BYTES];
    lin_regs_t      regs;
} lin_thread_t;

static lin_thread_t g_threads[LIN_THREADS_MAX];
static int          g_thread_count;
static int          g_thread_live;
static int          g_current = -1;
static uint32_t     g_main_tid;
static uint32_t     g_next_tid = 1001;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v),
                     "d"((uint32_t)(v >> 32)));
}

bool lin_thread_session_active(void) {
    if (lin_host_pe_running()) return false;
    if (!lin_host_running() && !lin_host_sync_running()) return false;
    if (lin_host_fork_child() && !lin_host_sync_running()) return false;
    return true;
}

void lin_thread_reset(void) {
    memset(g_threads, 0, sizeof(g_threads));
    g_thread_count = 0;
    g_thread_live = 0;
    g_current = -1;
    g_main_tid = 0;
    g_next_tid = lin_host_pid() + 1;
}

static lin_thread_t *lin_cur(void) {
    if (g_current < 0 || g_current >= LIN_THREADS_MAX) return NULL;
    if (!g_threads[g_current].used) return NULL;
    return &g_threads[g_current];
}

static int lin_alloc_slot(void) {
    for (int i = 0; i < LIN_THREADS_MAX; i++) {
        if (!g_threads[i].used) return i;
    }
    return -1;
}

static lin_thread_t *lin_pick_runnable(int skip_slot) {
    for (int i = 0; i < LIN_THREADS_MAX; i++) {
        if (i == skip_slot) continue;
        if (g_threads[i].used && g_threads[i].state == LIN_THR_RUNNABLE)
            return &g_threads[i];
    }
    return NULL;
}

static void lin_compat32_capture(lin_thread_t *t) {
    if (!g_lin_compat_frame_rsp) return;
    memcpy(t->compat_frame, (const void *)(uintptr_t)g_lin_compat_frame_rsp,
           LIN_COMPAT32_FRAME_BYTES);
    t->compat32 = true;
    for (int i = 0; i < LIN_THREADS_MAX; i++) {
        if (&g_threads[i] == t) {
            lin_fds_snap_thread(i);
            break;
        }
    }
}

static int lin_thread_slot(const lin_thread_t *t) {
    if (!t) return -1;
    return (int)(t - g_threads);
}

static void lin_compat32_apply_tls(const lin_thread_t *t) {
    if (t->tls_base || t->tls_limit)
        gdt_set_compat_tls(t->tls_base, t->tls_limit, t->tls_pg);
}

static int32_t lin_compat32_restore(lin_thread_t *t) {
    if (!g_lin_compat_frame_rsp) return 0;
    lin_fds_load_thread(lin_thread_slot(t));
    memcpy((void *)(uintptr_t)g_lin_compat_frame_rsp, t->compat_frame,
           LIN_COMPAT32_FRAME_BYTES);
    g_lin_compat_rip =
        (uint32_t)*(uint64_t *)(t->compat_frame + LIN_COMPAT32_RIP_OFF);
    lin_compat32_apply_tls(t);
    for (int i = 0; i < LIN_THREADS_MAX; i++) {
        if (&g_threads[i] == t) {
            g_current = i;
            break;
        }
    }
    return t->compat_rax;
}

static void lin_compat32_set_frame_u32(lin_thread_t *t, uint32_t off,
                                       uint32_t val) {
    *(uint32_t *)(t->compat_frame + off) = val;
}

static void lin_switch_to(lin_regs_t *r, lin_thread_t *t, long rax) {
    if (t->compat32) {
        t->compat_rax = (int32_t)rax;
        (void)lin_compat32_restore(t);
        return;
    }
    t->regs.rax = (uint64_t)(long)rax;
    *r = t->regs;
    wrmsr(MSR_FS_BASE, t->fs_base);
    for (int i = 0; i < LIN_THREADS_MAX; i++) {
        if (&g_threads[i] == t) {
            g_current = i;
            break;
        }
    }
}

static void lin_save_current(lin_regs_t *r) {
    lin_thread_t *cur = lin_cur();
    if (!cur) return;
    if (cur->compat32) {
        lin_compat32_capture(cur);
        return;
    }
    cur->regs = *r;
    cur->fs_base = rdmsr(MSR_FS_BASE);
}

void lin_thread_ensure_boot(lin_regs_t *r) {
    if (!lin_thread_session_active()) return;
    if (g_thread_count > 0 && g_thread_live > 0) return;
    if (g_thread_count > 0 && g_thread_live == 0)
        lin_thread_reset();
    int slot = lin_alloc_slot();
    if (slot < 0) return;
    lin_thread_t *t = &g_threads[slot];
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->state = LIN_THR_RUNNABLE;
    t->tid = lin_host_pid();
    g_main_tid = t->tid;
    if (linux_compat32_active()) {
        lin_compat32_capture(t);
    } else {
        t->fs_base = rdmsr(MSR_FS_BASE);
        t->regs = *r;
    }
    g_current = slot;
    g_thread_count = 1;
    g_thread_live = 1;
    debug_printf("[linux/thr] main tid=%u booted compat32=%u\n",
                 t->tid, t->compat32 ? 1u : 0u);
}

long lin_thread_clone(uint64_t flags, uint64_t child_stack, uint64_t ptid,
                      uint64_t ctid, uint64_t tls, lin_regs_t *r) {
    debug_printf("[linux/thr] clone req flags=0x%x stack=%p ptid=%p ctid=%p tls=%p\n",
                 (uint32_t)flags, (void *)(uintptr_t)child_stack,
                 (void *)(uintptr_t)ptid, (void *)(uintptr_t)ctid,
                 (void *)(uintptr_t)tls);
    if (!(flags & CLONE_THREAD)) return -L_EINVAL;
    if (!(flags & CLONE_VM)) return -L_EINVAL;
    if (!child_stack) return -L_EINVAL;

    lin_thread_ensure_boot(r);
    int slot = lin_alloc_slot();
    if (slot < 0) return -L_EINVAL;

    uint32_t tid = g_next_tid++;
    lin_thread_t *t = &g_threads[slot];
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->state = LIN_THR_RUNNABLE;
    t->tid = tid;
    t->regs = *r;
    t->regs.rax = 0;
    t->regs.user_rsp = child_stack;
    if (flags & CLONE_SETTLS)
        t->fs_base = tls;
    else
        t->fs_base = rdmsr(MSR_FS_BASE);
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_tid = ctid;

    if (ptid && (flags & CLONE_PARENT_SETTID))
        (void)lin_ucopy_u32_write(ptid, tid);
    if (ctid && (flags & CLONE_CHILD_CLEARTID))
        (void)lin_ucopy_u32_write(ctid, tid);

    g_thread_count++;
    g_thread_live++;
    debug_printf("[linux/thr] clone tid=%u stack=%p tls=%p\n",
                 tid, (void *)(uintptr_t)child_stack,
                 (void *)(uintptr_t)t->fs_base);
    return (long)tid;
}

uint32_t lin_thread_compat32_fork_spawn(uint32_t child_stack) {
    if (!child_stack)
        child_stack = lin_compat32_default_thread_stack();
    if (!child_stack)
        return 0;

    lin_thread_ensure_boot(NULL);
    lin_thread_t *parent = lin_cur();
    if (parent)
        lin_compat32_capture(parent);

    int slot = lin_alloc_slot();
    if (slot < 0)
        return 0;

    uint32_t pid = g_next_tid++;
    lin_thread_t *child = &g_threads[slot];
    memset(child, 0, sizeof(*child));
    child->used = true;
    child->compat32 = true;
    child->fork_proc = true;
    child->state = LIN_THR_RUNNABLE;
    child->tid = pid;
    child->compat_rax = 0;
    if (parent) {
        memcpy(child->compat_frame, parent->compat_frame,
               LIN_COMPAT32_FRAME_BYTES);
        child->tls_base = parent->tls_base;
        child->tls_limit = parent->tls_limit;
        child->tls_pg = parent->tls_pg;
    } else {
        lin_compat32_capture(child);
    }
    lin_compat32_set_frame_u32(child, LIN_COMPAT32_RAX_OFF, 0);
    lin_compat32_set_frame_u32(child, LIN_COMPAT32_RSP_OFF, child_stack);
    if (parent)
        lin_fds_fork_dup(lin_thread_slot(parent), slot);
    else
        lin_fds_fork_dup(0, slot);

    g_thread_count++;
    g_thread_live++;
    debug_printf("[linux/thr/i386] fork spawn pid=%u stack=0x%x live=%d\n",
                 pid, child_stack, g_thread_live);
    return pid;
}

bool lin_thread_compat32_fork_exit(int code, int32_t *resume_rax) {
    lin_thread_t *me = lin_cur();
    if (!me || !me->fork_proc)
        return false;
    me->state = LIN_THR_EXITED;
    me->used = false;
    g_thread_live--;
    debug_printf("[linux/thr/i386] fork proc pid=%u exit=%d live=%d\n",
                 me->tid, code, g_thread_live);
    lin_thread_t *next = lin_pick_runnable(-1);
    if (next && resume_rax) {
        *resume_rax = lin_compat32_restore(next);
        return true;
    }
    return false;
}

int32_t lin_thread_compat32_resume_parent(int32_t *resume_rax) {
    lin_thread_t *me = lin_cur();
    if (me) {
        me->state = LIN_THR_EXITED;
        me->used = false;
        g_thread_live--;
        debug_printf("[linux/thr/i386] fork exec done tid=%u live=%d\n",
                     me->tid, g_thread_live);
    }
    lin_thread_t *next = lin_pick_runnable(-1);
    if (next && resume_rax) {
        *resume_rax = lin_compat32_restore(next);
        return 1;
    }
    return 0;
}

long lin_thread_compat32_clone(uint32_t flags, uint32_t child_stack,
                               uint32_t ptid, uint32_t ctid, uint32_t tls) {
    debug_printf("[linux/thr/i386] clone flags=0x%x stack=0x%x ptid=0x%x "
                 "ctid=0x%x tls=0x%x\n",
                 flags, child_stack, ptid, ctid, tls);
    if (!(flags & CLONE_THREAD)) return -L_EINVAL;
    if (!child_stack)
        child_stack = lin_compat32_default_thread_stack();
    if (!child_stack) return -L_EINVAL;

    lin_thread_ensure_boot(NULL);
    int slot = lin_alloc_slot();
    if (slot < 0) return -L_EINVAL;

    uint32_t tid = g_next_tid++;
    lin_thread_t *t = &g_threads[slot];
    lin_thread_t *parent = lin_cur();
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->compat32 = true;
    t->state = LIN_THR_RUNNABLE;
    t->tid = tid;
    t->compat_rax = 0;
    if (parent) {
        lin_compat32_capture(parent);
        memcpy(t->compat_frame, parent->compat_frame,
               LIN_COMPAT32_FRAME_BYTES);
        t->tls_base = parent->tls_base;
        t->tls_limit = parent->tls_limit;
        t->tls_pg = parent->tls_pg;
    } else {
        lin_compat32_capture(t);
    }
    lin_compat32_set_frame_u32(t, LIN_COMPAT32_RAX_OFF, 0);
    lin_compat32_set_frame_u32(t, LIN_COMPAT32_RSP_OFF, child_stack);
    if (flags & CLONE_SETTLS && tls) {
        uint32_t desc[4];
        if (lin_ucopy_from(desc, tls, sizeof(desc)) == 0) {
            t->tls_base = desc[1];
            t->tls_limit = desc[2];
            t->tls_pg = (desc[3] & (1u << 4)) != 0;
        }
    }
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_tid = ctid;
    if (ptid && (flags & CLONE_PARENT_SETTID))
        (void)lin_ucopy_u32_write(ptid, tid);
    if (ctid && (flags & CLONE_CHILD_CLEARTID))
        (void)lin_ucopy_u32_write(ctid, tid);

    g_thread_count++;
    g_thread_live++;
    debug_printf("[linux/thr/i386] clone tid=%u stack=0x%x live=%d\n",
                 tid, child_stack, g_thread_live);
    return (long)tid;
}

void lin_thread_compat32_note_tls(uint32_t base, uint32_t limit, bool page_gran) {
    lin_thread_t *t = lin_cur();
    if (!t) return;
    t->tls_base = base;
    t->tls_limit = limit;
    t->tls_pg = page_gran;
}

static bool lin_futex_value_changed(uint64_t addr, uint32_t expected) {
    uint32_t cur = expected;
    if (lin_ucopy_u32(addr, &cur) != 0) return true;
    return cur != expected;
}

long lin_thread_futex_wake(uint64_t uaddr, int count) {
    int woke = 0;
    for (int i = 0; i < LIN_THREADS_MAX && woke < count; i++) {
        lin_thread_t *t = &g_threads[i];
        if (!t->used || t->state != LIN_THR_BLOCKED_FUTEX) continue;
        if (t->futex_addr != uaddr) continue;
        t->state = LIN_THR_RUNNABLE;
        if (t->compat32)
            t->compat_rax = 0;
        else
            t->regs.rax = 0;
        woke++;
    }
    if (woke > 0)
        debug_printf("[linux/thr] futex wake addr=%p n=%d\n",
                     (void *)(uintptr_t)uaddr, woke);
    return woke;
}

int32_t lin_thread_compat32_futex_wake(uint32_t uaddr, int count) {
    return (int32_t)lin_thread_futex_wake(uaddr, count);
}

static void lin_thread_schedule_away(lin_regs_t *r, bool *switched) {
    lin_save_current(r);
    lin_thread_t *next = lin_pick_runnable(g_current);
    if (!next) return;
    lin_switch_to(r, next, next->compat32 ? next->compat_rax : next->regs.rax);
    if (switched) *switched = true;
}

long lin_thread_futex_wait(uint64_t uaddr, uint32_t val, uint64_t timeout,
                           lin_regs_t *r, bool *switched) {
    uint32_t cur = 0;
    if (lin_ucopy_u32(uaddr, &cur) != 0) return -L_EFAULT;
    if (cur != val) return -L_EAGAIN;

    lin_thread_ensure_boot(r);
    lin_thread_t *me = lin_cur();
    if (!me) return -L_EAGAIN;

    lin_save_current(r);
    me->state = LIN_THR_BLOCKED_FUTEX;
    me->futex_addr = uaddr;
    me->futex_val = val;

    uint32_t deadline = 0;
    if (timeout) {
        uint64_t nsec = timeout;
        uint32_t ms = (uint32_t)(nsec / 1000000ull);
        if (ms < 1) ms = 1;
        deadline = pit_ms() + ms;
    }

    for (;;) {
        lin_thread_t *next = lin_pick_runnable(g_current);
        if (next) {
            lin_switch_to(r, next,
                          next->compat32 ? next->compat_rax : next->regs.rax);
            if (switched) *switched = true;
            return LIN_THREAD_SWITCHED;
        }
        if (lin_futex_value_changed(uaddr, val)) {
            me->state = LIN_THR_RUNNABLE;
            return -L_EAGAIN;
        }
        if (deadline && (int32_t)(pit_ms() - deadline) >= 0) {
            me->state = LIN_THR_RUNNABLE;
            return -L_ETIMEDOUT;
        }
        __asm__ volatile("sti; hlt");
    }
}

int32_t lin_thread_compat32_futex_wait(uint32_t uaddr, uint32_t val,
                                       uint32_t timeout_ptr) {
    uint64_t timeout_ns = 0;
    if (timeout_ptr) {
        uint32_t ts[2];
        if (lin_ucopy_from(ts, timeout_ptr, sizeof(ts)) != 0)
            return -L_EFAULT;
        timeout_ns = (uint64_t)ts[0] * 1000000000ull + ts[1];
    }

    lin_thread_ensure_boot(NULL);
    lin_thread_t *me = lin_cur();
    if (!me) return -L_EAGAIN;

    uint32_t cur = val;
    if (lin_ucopy_u32(uaddr, &cur) != 0) return -L_EFAULT;
    if (cur != val) return -L_EAGAIN;

    lin_compat32_capture(me);
    me->state = LIN_THR_BLOCKED_FUTEX;
    me->futex_addr = uaddr;
    me->futex_val = val;

    uint32_t deadline = 0;
    if (timeout_ns) {
        uint32_t ms = (uint32_t)(timeout_ns / 1000000ull);
        if (ms < 1) ms = 1;
        deadline = pit_ms() + ms;
    }

    for (;;) {
        lin_thread_t *next = lin_pick_runnable(g_current);
        if (next)
            return lin_compat32_restore(next);
        if (lin_futex_value_changed(uaddr, val)) {
            me->state = LIN_THR_RUNNABLE;
            return -L_EAGAIN;
        }
        if (deadline && (int32_t)(pit_ms() - deadline) >= 0) {
            me->state = LIN_THR_RUNNABLE;
            return -L_ETIMEDOUT;
        }
        __asm__ volatile("sti; hlt");
    }
}

void lin_thread_yield(lin_regs_t *r, bool *switched) {
    if (!lin_thread_session_active() || g_thread_live < 2) return;
    lin_thread_ensure_boot(r);
    lin_thread_schedule_away(r, switched);
}

int32_t lin_thread_compat32_yield(void) {
    if (!lin_thread_session_active() || g_thread_live < 2)
        return 0;
    lin_thread_ensure_boot(NULL);
    lin_thread_t *me = lin_cur();
    if (!me) return 0;
    lin_compat32_capture(me);
    lin_thread_t *next = lin_pick_runnable(g_current);
    if (!next) return 0;
    return lin_compat32_restore(next);
}

uint32_t lin_thread_get_tid(void) {
    lin_thread_t *t = lin_cur();
    return t ? t->tid : lin_host_pid();
}

uint32_t lin_thread_set_tid_address(uint64_t addr) {
    lin_thread_t *t = lin_cur();
    if (t) t->clear_tid = addr;
    return lin_thread_get_tid();
}

static void lin_thread_wake_clear_tid(lin_thread_t *t) {
    if (!t->clear_tid) return;
    (void)lin_ucopy_u32_write(t->clear_tid, 0);
    (void)lin_thread_futex_wake(t->clear_tid, 1);
}

bool lin_thread_try_exit(lin_regs_t *r, int code, bool exit_group) {
    if (!lin_thread_session_active()) return false;
    lin_thread_ensure_boot(r);

    lin_thread_t *me = lin_cur();
    if (!me) return false;

    if (!exit_group && g_thread_live > 1 && me->tid != g_main_tid) {
        lin_save_current(r);
        lin_thread_wake_clear_tid(me);
        me->state = LIN_THR_EXITED;
        me->used = false;
        g_thread_live--;
        debug_printf("[linux/thr] thread tid=%u exit=%d live=%d\n",
                     me->tid, code, g_thread_live);

        lin_thread_t *next = lin_pick_runnable(-1);
        if (next) {
            lin_switch_to(r, next,
                          next->compat32 ? next->compat_rax : next->regs.rax);
            return true;
        }
    }
    return false;
}

bool lin_thread_compat32_try_exit(int code, bool exit_group,
                                  int32_t *resume_rax) {
    if (!lin_thread_session_active()) return false;
    lin_thread_ensure_boot(NULL);

    lin_thread_t *me = lin_cur();
    if (!me) return false;

    if (!exit_group && g_thread_live > 1 && me->tid != g_main_tid) {
        lin_thread_wake_clear_tid(me);
        me->state = LIN_THR_EXITED;
        me->used = false;
        g_thread_live--;
        debug_printf("[linux/thr/i386] thread tid=%u exit=%d live=%d\n",
                     me->tid, code, g_thread_live);

        lin_thread_t *next = lin_pick_runnable(-1);
        if (next) {
            if (resume_rax)
                *resume_rax = lin_compat32_restore(next);
            return true;
        }
    }
    return false;
}

bool lin_thread_handle_fault(lin_regs_t *frame, int sig) {
    (void)sig;
    if (!lin_thread_session_active() || g_thread_live < 2) return false;
    lin_thread_t *me = lin_cur();
    if (!me || me->tid == g_main_tid) return false;
    me->state = LIN_THR_EXITED;
    me->used = false;
    g_thread_live--;
    lin_thread_wake_clear_tid(me);
    lin_thread_t *next = lin_pick_runnable(-1);
    if (next) {
        if (next->compat32) {
            (void)lin_compat32_restore(next);
            return true;
        }
        *frame = next->regs;
        wrmsr(MSR_FS_BASE, next->fs_base);
        g_current = (int)(next - g_threads);
        return true;
    }
    return false;
}

/* ============================================================================
 * NexxoN OS - Linux x86_64 compatibility runtime
 * ----------------------------------------------------------------------------
 * Static and static-PIE Linux programs run at CPL=3 and enter this layer with
 * the native x86_64 `syscall` instruction.  This milestone supplies the libc
 * startup/memory/time surface plus an NXFS-backed per-run descriptor table.
 *
 * The runtime is deliberately single-process, but execution has a private CR3:
 * kernel identity mappings are supervisor-only and user ELF/stack/heap/mmap
 * pages are backed by owned VMM frames.
 * ============================================================================ */
#include "linuxsys.h"
#include "elf64.h"
#include "string.h"
#include "debug.h"
#include "terminal.h"
#include "setjmp.h"
#include "gdt.h"
#include "nxfs.h"
#include "pit.h"
#include "keyboard.h"
#include "i18n.h"
#include "sched.h"
#include "isr.h"
#include "download.h"
#include "net.h"
#include "pe_loader.h"
#include "pe_win32.h"
#include "lin_sock.h"
#include "lin_unix.h"
#include "lin_thread.h"
#include "audio.h"

typedef int32_t lin_pid_t;

/* ---- Model-specific registers ------------------------------------------ */
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v),
                     "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

#define MSR_EFER     0xC0000080u
#define MSR_STAR     0xC0000081u
#define MSR_LSTAR    0xC0000082u
#define MSR_SFMASK   0xC0000084u
#define MSR_FS_BASE  0xC0000100u
#define MSR_GS_BASE  0xC0000101u

extern void ring3_enter(uintptr_t user_rip, uintptr_t user_rsp);
extern void ring3_enter32(uintptr_t user_eip, uintptr_t user_esp);
extern void linux_syscall_entry(void);

extern const uint8_t _binary_build_userland_musl_libc_so_start[];
extern const uint8_t _binary_build_userland_musl_libc_so_end[];
extern const uint8_t _binary_build_userland_linuxsh_elf_start[];
extern const uint8_t _binary_build_userland_linuxsh_elf_end[];
extern const uint8_t _binary_build_userland_linuxdemo_elf_start[];
extern const uint8_t _binary_build_userland_linuxdemo_elf_end[];
extern const uint8_t _binary_build_userland_linuxnet_elf_start[];
extern const uint8_t _binary_build_userland_linuxnet_elf_end[];
extern const uint8_t _binary_build_userland_linuxpthread_elf_start[];
extern const uint8_t _binary_build_userland_linuxpthread_elf_end[];
extern const uint8_t _binary_build_userland_linuxepoll_elf_start[];
extern const uint8_t _binary_build_userland_linuxepoll_elf_end[];
extern const uint8_t _binary_build_userland_linuxglibc_elf_start[];
extern const uint8_t _binary_build_userland_linuxglibc_elf_end[];
extern const uint8_t _binary_build_userland_linuxdri_elf_start[];
extern const uint8_t _binary_build_userland_linuxdri_elf_end[];
extern const uint8_t _binary_build_userland_linuxmmap_elf_start[];
extern const uint8_t _binary_build_userland_linuxmmap_elf_end[];
extern const uint8_t _binary_build_userland_linuxdrmfb_elf_start[];
extern const uint8_t _binary_build_userland_linuxdrmfb_elf_end[];
extern const uint8_t _binary_build_userland_linuxalsa_elf_start[];
extern const uint8_t _binary_build_userland_linuxalsa_elf_end[];
extern const uint8_t _binary_build_userland_linuxx11_elf_start[];
extern const uint8_t _binary_build_userland_linuxx11_elf_end[];
extern const uint8_t _binary_build_userland_linuxxlib_elf_start[];
extern const uint8_t _binary_build_userland_linuxxlib_elf_end[];
extern const uint8_t _binary_build_userland_linuxxevent_elf_start[];
extern const uint8_t _binary_build_userland_linuxxevent_elf_end[];
extern const uint8_t _binary_build_userland_linuxvulkan_elf_start[];
extern const uint8_t _binary_build_userland_linuxvulkan_elf_end[];
extern const uint8_t _binary_build_userland_linuxvulkanso_elf_start[];
extern const uint8_t _binary_build_userland_linuxvulkanso_elf_end[];
extern const uint8_t _binary_build_userland_linuxvkloader_elf_start[];
extern const uint8_t _binary_build_userland_linuxvkloader_elf_end[];
extern const uint8_t _binary_build_userland_linuxvkinstance_elf_start[];
extern const uint8_t _binary_build_userland_linuxvkinstance_elf_end[];
extern const uint8_t _binary_build_userland_linuxvkqueue_elf_start[];
extern const uint8_t _binary_build_userland_linuxvkqueue_elf_end[];
extern const uint8_t _binary_build_userland_linuxvkcommand_elf_start[];
extern const uint8_t _binary_build_userland_linuxvkcommand_elf_end[];
extern const uint8_t _binary_build_userland_linuxvksurface_elf_start[];
extern const uint8_t _binary_build_userland_linuxvksurface_elf_end[];
extern const uint8_t _binary_build_userland_linuxvkswapchain_elf_start[];
extern const uint8_t _binary_build_userland_linuxvkswapchain_elf_end[];
extern const uint8_t _binary_build_userland_linuxpulse_elf_start[];
extern const uint8_t _binary_build_userland_linuxpulse_elf_end[];
extern const uint8_t _binary_build_userland_linux32_elf_start[];
extern const uint8_t _binary_build_userland_linux32_elf_end[];
extern const uint8_t _binary_build_userland_linux32glibc_elf_start[];
extern const uint8_t _binary_build_userland_linux32glibc_elf_end[];
extern const uint8_t _binary_build_userland_linux32pie_elf_start[];
extern const uint8_t _binary_build_userland_linux32pie_elf_end[];
extern const uint8_t _binary_build_userland_linux32pthread_elf_start[];
extern const uint8_t _binary_build_userland_linux32pthread_elf_end[];
extern const uint8_t _binary_build_userland_libvulkan_nexxon_so_start[];
extern const uint8_t _binary_build_userland_libvulkan_nexxon_so_end[];
extern const uint8_t _binary_build_userland_linuxdyn_elf_start[];
extern const uint8_t _binary_build_userland_linuxdyn_elf_end[];
extern const uint8_t _binary_build_userland_linuxexec_elf_start[];
extern const uint8_t _binary_build_userland_linuxexec_elf_end[];
extern const uint8_t _binary_build_userland_winestub_elf_start[];
extern const uint8_t _binary_build_userland_winestub_elf_end[];
extern const uint8_t _binary_tools_hello_pe_bin_start[];
extern const uint8_t _binary_tools_hello_pe_bin_end[];

/* NexxoN-private syscalls (see docs §5.22). */
#define LSYS_nexxon_apt  456
#define LSYS_nexxon_run  458
#define LSYS_nexxon_wine 459
#define LSYS_nexxon_pe_exit 460

#define NEXXON_APT_REGISTRY_URL "http://apt.nexxon:8000/"
#define LIN_PE_BASE  0x40000000ULL
#define LIN_PE_LIMIT 0x48000000ULL

/* The assembly entry stub references these symbols directly. */
ALIGNED(16) static uint8_t g_lin_kstack[32 * 1024];
ALIGNED(16) static uint8_t g_lin_sync_kstack[16 * 1024];

uint64_t g_lin_kstack_top = 0;
uint64_t g_lin_user_rsp   = 0;
uint8_t  g_lin_syscall_frame_override = 0;
uint8_t  g_lin_return_compat32 = 0;
lin_regs_t g_lin_syscall_return_frame;
uint32_t g_lin_compat_arg5 = 0;
uint32_t g_lin_compat_rip = 0;
uint64_t g_lin_compat_frame_rsp = 0;

/* ---- Per-run process state --------------------------------------------- */
#define LIN_STACK_BYTES  (1024u * 1024u)
#define LIN_IMAGE_BYTES  (16u * 1024u * 1024u)
#define LIN_PATH_MAX     256
#define LIN_MAX_ARGS     32
/* execve argv elements are NOT paths: a `sh -c "<long command>"` argument
 * easily exceeds LIN_PATH_MAX (the Steam updater's spawn did — the silent
 * ENAMETOOLONG surfaced as posix_spawn exit 127). */
#define LIN_ARG_MAX      1024
#define LIN_FD_MAX       32
#define LIN_MAP_MAX      64

#define LIN_USER_LOW     0x00000000ULL
#define LIN_INTERP_BASE  0x08000000ULL
#define LIN_INTERP_LIMIT 0x09000000ULL
#define LIN_PIE_BASE     0x20000000ULL
#define LIN_BRK_LIMIT    0x50000000ULL
#define LIN_MMAP_TOP     0x7FC00000ULL
#define LIN_MMAP_LAZY_MIN (256u * 1024u)

/* x86_64 AT_HWCAP / AT_HWCAP2 — glibc IFUNC resolvers need non-zero caps. */
#define LIN_HWCAP_X86 ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | \
                       (1u << 4) | (1u << 5) | (1u << 9))
#define LIN_HWCAP2_X86 ((1u << 0) | (1u << 5) | (1u << 8))
#define LIN_STACK_TOP    0x7FF00000ULL
#define LIN_STACK_BASE   (LIN_STACK_TOP - LIN_STACK_BYTES)
#define LIN_CHILD_STACK_TOP  (LIN_STACK_BASE - 0x1000ULL)
#define LIN_CHILD_STACK_BASE (LIN_CHILD_STACK_TOP - LIN_STACK_BYTES)
#define LIN_SYNC_STACK_TOP   (LIN_CHILD_STACK_BASE - 0x1000ULL)
#define LIN_SYNC_STACK_BASE  (LIN_SYNC_STACK_TOP - LIN_STACK_BYTES)
#define LIN_USER_MIN     LIN_PIE_BASE
#define LIN_USER_MAX     0x80000000ULL
#define LIN_MAX_MAP_BYTES (0x81000000ULL) /* 2 GiB + slack for i386 pthread guards */

/* NexxoN headless DRM shim — enough for libdrm version/cap probes. */
#define DRM_IOCTL_VERSION 0xC0406400u
#define DRM_IOCTL_GET_CAP 0xC010640Cu
#define DRM_CAP_DUMB_BUFFER 0x1u
#define DRM_IOCTL_MODE_CREATE_DUMB 0xC02064B2u
#define DRM_IOCTL_MODE_MAP_DUMB    0xC01064B3u
#define DRM_IOCTL_MODE_DESTROY_DUMB 0xC00464B4u

#define LIN_DUMB_SLOTS    8
#define LIN_DUMB_MAX_PG   1024u   /* 4 MiB / buffer */
#define LIN_DUMB_MAP_BASE 0x01000000ULL

ALIGNED(4096) static uint8_t g_lin_file_image[LIN_IMAGE_BYTES];
ALIGNED(4096) static uint8_t g_lin_aux_image[LIN_IMAGE_BYTES];

static jmp_buf  g_lin_return;
static jmp_buf  g_lin_sync_dispatch_jmp;
static bool     g_lin_sync_dispatch_active;
static bool     g_lin_sync_running;
static int      g_lin_sync_exit_code;
static bool     g_lin_sync_faulted;
static jmp_buf  g_lin_pe_return;
static bool     g_lin_pe_running;
static int      g_lin_pe_exit_code;
static bool     g_lin_pe_used_win32;
static lin_regs_t g_lin_wine_saved_regs;
static bool     g_lin_wine_saved_valid;
static uint64_t g_lin_wine_saved_fs;
static uint64_t g_lin_wine_saved_gs;
static bool     g_lin_running;
static bool     g_lin_compat32;
static bool     g_lin_faulted;
static bool     g_lin_private_cr3;
static int      g_lin_exit_code;
static vmm_pd_t *g_lin_pd;
static uint64_t g_lin_brk;
static uint64_t g_lin_brk_floor;
static uint64_t g_lin_load_lo;
static uint64_t g_lin_load_hi;
static char     g_lin_exec_path[LIN_PATH_MAX];
static char     g_lin_cwd[LIN_PATH_MAX];
static uint32_t g_lin_cwd_ino;
static uint32_t g_lin_image_size;

static lin_pid_t  g_lin_pid = 1000;
static lin_pid_t  g_lin_ppid = 1;
static lin_pid_t  g_lin_next_pid = 1001;
#define LIN_COMPAT32_FRAME_BYTES 168u
#define LIN_COMPAT32_RAX_OFF     112u
#define LIN_COMPAT32_RSP_OFF     144u

static uint8_t g_lin_fork_parent_compat_frame[LIN_COMPAT32_FRAME_BYTES];
static bool     g_lin_fork_child;
static lin_regs_t g_lin_fork_parent_regs;
static lin_pid_t  g_lin_fork_parent_pid;
static vmm_pd_t  *g_lin_fork_parent_pd;
static lin_pid_t  g_lin_fork_child_pid;
static int      g_lin_fork_child_status;
static bool     g_lin_fork_child_done;
static bool     g_lin_fork_child_exec;

static char     g_lin_tty_line[256];
static size_t   g_lin_tty_line_len;
static size_t   g_lin_tty_line_pos;

/* Linux errno values; syscall failures return the negative value. */
#define L_EPERM         1
#define L_ENOENT        2
#define L_EIO           5
#define L_ENXIO         6
#define L_E2BIG         7
#define L_EBADF         9
#define L_EAGAIN       11
#define L_ENOMEM       12
#define L_EACCES       13
#define L_EFAULT       14
#define L_EBUSY        16
#define L_EEXIST       17
#define L_ENODEV       19
#define L_ENOTDIR      20
#define L_EISDIR       21
#define L_EINVAL       22
#define L_EMFILE       24
#define L_ENOTTY       25
#define L_EFBIG        27
#define L_ENOSPC       28
#define L_ESPIPE       29
#define L_EROFS        30
#define L_ERANGE       34
#define L_ENAMETOOLONG 36
#define L_ENOSYS       38
#define L_ENOTEMPTY    39
#define L_EINTR         4
#define L_ECHILD       10
#define L_EOPNOTSUPP   95
#define L_ETIMEDOUT   110
#define L_ECONNREFUSED 111
#define L_EAFNOSUPPORT 97
#define L_EPIPE          32

/* Linux x86_64 syscall numbers used by static musl-class programs. */
enum {
    LSYS_read = 0, LSYS_write = 1, LSYS_open = 2, LSYS_close = 3,
    LSYS_stat = 4, LSYS_fstat = 5, LSYS_poll = 7, LSYS_lseek = 8,
    LSYS_mmap = 9, LSYS_mprotect = 10, LSYS_munmap = 11, LSYS_brk = 12,
    LSYS_rt_sigaction = 13, LSYS_rt_sigprocmask = 14, LSYS_ioctl = 16,
    LSYS_pread64 = 17, LSYS_pwrite64 = 18, LSYS_readv = 19,
    LSYS_writev = 20, LSYS_access = 21, LSYS_pipe = 22,
    LSYS_sched_yield = 24,
    LSYS_mremap = 25, LSYS_madvise = 28, LSYS_dup = 32, LSYS_dup2 = 33,
    LSYS_nanosleep = 35, LSYS_getpid = 39, LSYS_exit = 60,
    LSYS_uname = 63, LSYS_fcntl = 72, LSYS_getcwd = 79, LSYS_chdir = 80,
    LSYS_fchdir = 81, LSYS_readlink = 89, LSYS_gettimeofday = 96,
    LSYS_getrlimit = 97, LSYS_getuid = 102, LSYS_getgid = 104,
    LSYS_geteuid = 107, LSYS_getegid = 108, LSYS_arch_prctl = 158,
    LSYS_prctl = 157, LSYS_gettid = 186, LSYS_time = 201, LSYS_futex = 202,
    LSYS_sched_getaffinity = 204, LSYS_getdents64 = 217,
    LSYS_set_tid_address = 218, LSYS_clock_gettime = 228,
    LSYS_exit_group = 231, LSYS_openat = 257, LSYS_newfstatat = 262,
    LSYS_readlinkat = 267, LSYS_faccessat = 269,
    LSYS_set_robust_list = 273, LSYS_dup3 = 292, LSYS_pipe2 = 293,
    LSYS_prlimit64 = 302, LSYS_epoll_create1 = 291, LSYS_epoll_wait = 232,
    LSYS_epoll_ctl = 233, LSYS_epoll_pwait = 281,
    LSYS_getrandom = 318, LSYS_rseq = 334, LSYS_faccessat2 = 439,
    LSYS_clone = 56, LSYS_fork = 57, LSYS_execve = 59, LSYS_wait4 = 61,
    LSYS_socket = 41, LSYS_connect = 42, LSYS_accept = 43,
    LSYS_sendto = 44, LSYS_recvfrom = 45, LSYS_sendmsg = 46,
    LSYS_recvmsg = 47, LSYS_shutdown = 48,
    LSYS_bind = 49, LSYS_listen = 50, LSYS_getsockname = 51,
    LSYS_getpeername = 52, LSYS_socketpair = 53, LSYS_setsockopt = 54,
    LSYS_getsockopt = 55,
    LSYS_getppid = 110,     LSYS_nexxon_apt_install = LSYS_nexxon_apt,
    LSYS_nexxon_run_sync    = LSYS_nexxon_run,
    LSYS_nexxon_wine_run    = LSYS_nexxon_wine,
    LSYS_pe_exit            = LSYS_nexxon_pe_exit,
};

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

#define L_AT_FDCWD       (-100)
#define L_O_ACCMODE      3
#define L_O_RDONLY       0
#define L_O_WRONLY       1
#define L_O_RDWR         2
#define L_O_CREAT        0100
#define L_O_EXCL         0200
#define L_O_TRUNC        01000
#define L_O_APPEND       02000
#define L_O_NONBLOCK     04000
#define L_O_DIRECTORY    0200000
#define L_O_CLOEXEC      02000000

#define L_SEEK_SET 0
#define L_SEEK_CUR 1
#define L_SEEK_END 2

#define L_PROT_READ   1
#define L_PROT_WRITE  2
#define L_PROT_EXEC   4
#define L_MAP_SHARED  1
#define L_MAP_PRIVATE 2
#define L_MAP_FIXED   0x10
#define L_MAP_ANON    0x20

#define L_S_IFMT   0170000u
#define L_S_IFCHR  0020000u
#define L_S_IFDIR  0040000u
#define L_S_IFREG  0100000u

/* ---- Guest pointer and string validation ------------------------------- */
static bool range_inside(uint64_t p, uint64_t n, uint64_t lo, uint64_t hi) {
    if (p < lo || p > hi) return false;
    if (n > hi - p) return false;
    return true;
}

static bool uptr_ok(uint64_t p, uint64_t n) {
    return g_lin_pd && range_inside(p, n, LIN_USER_LOW, LIN_USER_MAX) &&
           vmm_user_range_ok(g_lin_pd, (uintptr_t)p, (size_t)n, false);
}

static bool uptr_write_ok(uint64_t p, uint64_t n) {
    return g_lin_pd && range_inside(p, n, LIN_USER_LOW, LIN_USER_MAX) &&
           vmm_user_range_ok(g_lin_pd, (uintptr_t)p, (size_t)n, true);
}

static int user_copy_to(vmm_pd_t *pd, uint64_t dst,
                        const void *src, uint64_t len) {
    const uint8_t *s = (const uint8_t *)src;
    while (len) {
        uintptr_t pa = vmm_resolve(pd, (uintptr_t)dst);
        if (!pa) return -1;
        uint32_t chunk = VMM_PAGE_SIZE -
                         (uint32_t)(dst & (VMM_PAGE_SIZE - 1u));
        if ((uint64_t)chunk > len) chunk = (uint32_t)len;
        memcpy((void *)pa, s, chunk);
        dst += chunk;
        s += chunk;
        len -= chunk;
    }
    return 0;
}

static int user_copy_from(vmm_pd_t *pd, void *dst, uint64_t src,
                          uint64_t len) {
    uint8_t *d = (uint8_t *)dst;
    while (len) {
        uintptr_t pa = vmm_resolve(pd, (uintptr_t)src);
        if (!pa) return -1;
        uint32_t chunk = VMM_PAGE_SIZE -
                         (uint32_t)(src & (VMM_PAGE_SIZE - 1u));
        if ((uint64_t)chunk > len) chunk = (uint32_t)len;
        memcpy(d, (void *)pa, chunk);
        d += chunk;
        src += chunk;
        len -= chunk;
    }
    return 0;
}

static int map_user_pages(uint64_t addr, uint64_t len, uintptr_t flags) {
    if (!g_lin_pd || !range_inside(addr, len, LIN_USER_MIN, LIN_USER_MAX))
        return -1;
    uint64_t page = addr & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    uint64_t first = page;
    uint64_t end = (addr + len + VMM_PAGE_SIZE - 1u) &
                   ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    for (; page < end; page += VMM_PAGE_SIZE) {
        uintptr_t old_flags = 0;
        if (vmm_query_page(g_lin_pd, (uintptr_t)page, NULL, &old_flags) == 0 &&
            (old_flags & VMM_FLAG_USER))
            continue;
        uint32_t pa = vmm_frame_alloc();
        if (!pa) goto rollback;
        if (vmm_map_page(g_lin_pd, (uintptr_t)page, pa,
                         flags | VMM_FLAG_USER) != 0) {
            vmm_frame_free(pa);
            goto rollback;
        }
    }
    return 0;
rollback:
    for (uint64_t undo = first; undo < page; undo += VMM_PAGE_SIZE)
        (void)vmm_unmap_page(g_lin_pd, (uintptr_t)undo);
    return -1;
}

static int copy_user_cstr(uint64_t p, char *out, uint32_t cap) {
    if (!out || cap < 2 || !p) return -L_EFAULT;
    for (uint32_t i = 0; i < cap; i++) {
        if (!uptr_ok(p + i, 1)) return -L_EFAULT;
        out[i] = *(const char *)(uintptr_t)(p + i);
        if (!out[i]) return 0;
    }
    out[cap - 1] = 0;
    return -L_ENAMETOOLONG;
}

static void out_copy(char *out, uint32_t cap, const char *s) {
    if (!out || cap == 0) return;
    strncpy(out, s ? s : "", cap - 1);
    out[cap - 1] = 0;
}

/* Canonicalise against `base`.  `..` is allowed but cannot escape root. */
static int normalize_path(const char *base, const char *path,
                          char *out, uint32_t cap) {
    if (!path || !path[0] || !out || cap < 2) return -L_ENOENT;
    uint32_t n = 0;
    if (path[0] == '/') {
        out[n++] = '/';
    } else {
        const char *b = (base && base[0]) ? base : "/";
        size_t bl = strlen(b);
        if (bl >= cap) return -L_ENAMETOOLONG;
        memcpy(out, b, bl);
        n = (uint32_t)bl;
        if (n == 0 || out[0] != '/') {
            out[0] = '/';
            n = 1;
        }
    }
    out[n] = 0;

    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        char comp[NXFS_NAME_MAX];
        uint32_t cn = 0;
        while (*p && *p != '/') {
            if (cn + 1 >= sizeof(comp)) return -L_ENAMETOOLONG;
            comp[cn++] = *p++;
        }
        comp[cn] = 0;
        while (*p == '/') p++;
        if (cn == 0 || strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            while (n > 1 && out[n - 1] == '/') n--;
            while (n > 1 && out[n - 1] != '/') n--;
            if (n > 1 && out[n - 1] == '/') n--;
            if (n == 0) n = 1;
            out[n] = 0;
            continue;
        }
        if (n > 1 && out[n - 1] == '/') n--;
        uint32_t need = n + (n > 1 ? 1u : 0u) + cn + 1u;
        if (need > cap) return -L_ENAMETOOLONG;
        if (n > 1) out[n++] = '/';
        memcpy(out + n, comp, cn);
        n += cn;
        out[n] = 0;
    }
    if (n == 0) {
        out[0] = '/';
        out[1] = 0;
    }
    return 0;
}

static int resolve_nx_path(const char *path, uint32_t *ino) {
    if (strcmp(path, "/") == 0) {
        *ino = 0;
        return 0;
    }
    return nxfs_resolve_path(path, ino) == NXFS_OK ? 0 : -L_ENOENT;
}

static int split_parent(const char *path, char *parent, uint32_t parent_cap,
                        char *name, uint32_t name_cap) {
    size_t len = strlen(path);
    if (len < 2) return -L_EINVAL;
    size_t slash = len;
    while (slash > 0 && path[slash - 1] != '/') slash--;
    if (slash == 0 || len - slash + 1 > name_cap) return -L_ENAMETOOLONG;
    memcpy(name, path + slash, len - slash);
    name[len - slash] = 0;
    size_t plen = slash > 1 ? slash - 1 : 1;
    if (plen + 1 > parent_cap) return -L_ENAMETOOLONG;
    memcpy(parent, path, plen);
    parent[plen] = 0;
    return 0;
}

/* ---- Descriptor table -------------------------------------------------- */
typedef enum {
    LFD_FREE = 0, LFD_TTY, LFD_NX_FILE, LFD_NX_DIR,
    LFD_DEV_NULL, LFD_DEV_ZERO, LFD_DEV_RANDOM, LFD_DEV_DRI,
    LFD_DEV_SND_PCM, LFD_DEV_SND_CTL,
    LFD_SOCK, LFD_PIPE, LFD_EPOLL, LFD_EVENTFD,
} lin_fd_kind_t;

#define LIN_PIPE_PAIRS    8
#define LIN_PIPE_BUF      4096
#define LIN_EPOLL_MAX_ENT 16

typedef struct {
    uint8_t  buf[LIN_PIPE_BUF];
    uint32_t len;
    bool     r_open, w_open;
    int      r_fd, w_fd;
} lin_pipe_pair_t;

typedef struct {
    int32_t  fd;
    uint32_t events;
    uint64_t data;
} lin_epoll_ent_t;

typedef struct {
    lin_fd_kind_t kind;
    int flags;
    uint32_t inode;
    uint64_t offset;
    uint64_t size;
    uint32_t dir_pos;
    bool write_stream;
    bool cloexec;
    char path[LIN_PATH_MAX];
    lin_sock_t sock;
    int16_t    pipe_ix;
    bool       pipe_wr;
    uint64_t   event_count;
} lin_fd_t;

static lin_pipe_pair_t g_lin_pipes[LIN_PIPE_PAIRS];
static lin_epoll_ent_t g_lin_epoll_tab[LIN_FD_MAX][LIN_EPOLL_MAX_ENT];
static uint8_t         g_lin_epoll_cnt[LIN_FD_MAX];

typedef struct {
    bool     used;
    int      dri_fd;
    uint32_t handle;
    uint32_t width, height, bpp, pitch, flags;
    uint64_t size;
    uint64_t map_off;
    uint32_t npages;
    uint32_t pa[LIN_DUMB_MAX_PG];
} lin_dumb_t;

static lin_dumb_t g_lin_dumbs[LIN_DUMB_SLOTS];
static uint32_t   g_lin_dumb_next_handle = 1;
static uint64_t   g_lin_dumb_next_map_off = LIN_DUMB_MAP_BASE;

static lin_dumb_t *lin_dumb_find_handle(int dri_fd, uint32_t handle) {
    for (int i = 0; i < LIN_DUMB_SLOTS; i++) {
        lin_dumb_t *d = &g_lin_dumbs[i];
        if (d->used && d->dri_fd == dri_fd && d->handle == handle)
            return d;
    }
    return NULL;
}

static lin_dumb_t *lin_dumb_find_mapoff(int dri_fd, uint64_t offset) {
    for (int i = 0; i < LIN_DUMB_SLOTS; i++) {
        lin_dumb_t *d = &g_lin_dumbs[i];
        if (d->used && d->dri_fd == dri_fd &&
            offset >= d->map_off && offset < d->map_off + d->size)
            return d;
    }
    return NULL;
}

static void lin_dumb_free_pages(lin_dumb_t *d) {
    if (!d) return;
    for (uint32_t i = 0; i < d->npages; i++) {
        if (d->pa[i]) {
            vmm_frame_free(d->pa[i]);
            d->pa[i] = 0;
        }
    }
    d->npages = 0;
}

static void lin_dumb_destroy_slot(lin_dumb_t *d) {
    if (!d || !d->used) return;
    lin_dumb_free_pages(d);
    memset(d, 0, sizeof(*d));
}

static void lin_dumb_destroy_fd(int dri_fd) {
    for (int i = 0; i < LIN_DUMB_SLOTS; i++)
        if (g_lin_dumbs[i].used && g_lin_dumbs[i].dri_fd == dri_fd)
            lin_dumb_destroy_slot(&g_lin_dumbs[i]);
}

static lin_dumb_t *lin_dumb_alloc_slot(void) {
    for (int i = 0; i < LIN_DUMB_SLOTS; i++)
        if (!g_lin_dumbs[i].used) return &g_lin_dumbs[i];
    return NULL;
}

static lin_fd_t g_lin_fds[LIN_FD_MAX];
static int g_lin_write_fd = -1;

#define LIN_THR_FD_SNAPS 16
static struct {
    lin_fd_t fds[LIN_FD_MAX];
    int      write_fd;
    bool     valid;
} g_thr_fd_snap[LIN_THR_FD_SNAPS];

static void lin_fds_bump_socket_refs(const lin_fd_t *fds) {
    for (int i = 3; i < LIN_FD_MAX; i++) {
        if (fds[i].kind != LFD_SOCK ||
            fds[i].sock.domain != LIN_AF_UNIX ||
            fds[i].sock.unix_link < 0)
            continue;
        lin_unix_link_dup_refs(fds[i].sock.unix_link);
    }
}

void lin_fds_snap_thread(int slot) {
    if (slot < 0 || slot >= LIN_THR_FD_SNAPS)
        return;
    memcpy(g_thr_fd_snap[slot].fds, g_lin_fds, sizeof(g_lin_fds));
    g_thr_fd_snap[slot].write_fd = g_lin_write_fd;
    g_thr_fd_snap[slot].valid = true;
}

void lin_fds_load_thread(int slot) {
    if (slot < 0 || slot >= LIN_THR_FD_SNAPS ||
        !g_thr_fd_snap[slot].valid)
        return;
    memcpy(g_lin_fds, g_thr_fd_snap[slot].fds, sizeof(g_lin_fds));
    g_lin_write_fd = g_thr_fd_snap[slot].write_fd;
}

void lin_fds_fork_dup(int parent_slot, int child_slot) {
    if (parent_slot < 0 || parent_slot >= LIN_THR_FD_SNAPS ||
        child_slot < 0 || child_slot >= LIN_THR_FD_SNAPS)
        return;
    lin_fds_snap_thread(parent_slot);
    memcpy(g_thr_fd_snap[child_slot].fds, g_thr_fd_snap[parent_slot].fds,
           sizeof(g_lin_fds));
    g_thr_fd_snap[child_slot].write_fd = g_thr_fd_snap[parent_slot].write_fd;
    g_thr_fd_snap[child_slot].valid = true;
    lin_fds_bump_socket_refs(g_thr_fd_snap[child_slot].fds);
}

static void fd_table_init(void) {
    if (g_lin_write_fd >= 0) (void)nxfs_write_end(false);
    memset(g_lin_fds, 0, sizeof(g_lin_fds));
    g_lin_write_fd = -1;
    for (int i = 0; i < 3; i++) {
        g_lin_fds[i].kind = LFD_TTY;
        g_lin_fds[i].flags = (i == 0) ? L_O_RDONLY : L_O_WRONLY;
        strcpy(g_lin_fds[i].path, "/dev/tty");
    }
}

static int fd_alloc_from(int first) {
    if (first < 3) first = 3;
    for (int i = first; i < LIN_FD_MAX; i++)
        if (g_lin_fds[i].kind == LFD_FREE) return i;
    return -L_EMFILE;
}

static int fd_close(int fd, bool commit) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE)
        return -L_EBADF;
    if (g_lin_fds[fd].write_stream) {
        int rc = nxfs_write_end(commit);
        g_lin_write_fd = -1;
        if (rc != NXFS_OK) {
            if (g_lin_fds[fd].kind == LFD_SOCK)
                lin_sock_close(&g_lin_fds[fd].sock);
            memset(&g_lin_fds[fd], 0, sizeof(g_lin_fds[fd]));
            return -L_EIO;
        }
    }
    if (g_lin_fds[fd].kind == LFD_SOCK)
        lin_sock_close(&g_lin_fds[fd].sock);
    if (g_lin_fds[fd].kind == LFD_PIPE) {
        int ix = g_lin_fds[fd].pipe_ix;
        if (ix >= 0 && ix < LIN_PIPE_PAIRS) {
            lin_pipe_pair_t *pp = &g_lin_pipes[ix];
            if (g_lin_fds[fd].pipe_wr)
                pp->w_open = false;
            else
                pp->r_open = false;
            if (!pp->r_open && !pp->w_open)
                memset(pp, 0, sizeof(*pp));
        }
    }
    if (g_lin_fds[fd].kind == LFD_EPOLL)
        g_lin_epoll_cnt[fd] = 0;
    if (g_lin_fds[fd].kind == LFD_DEV_DRI)
        lin_dumb_destroy_fd(fd);
    memset(&g_lin_fds[fd], 0, sizeof(g_lin_fds[fd]));
    return 0;
}

static void fd_table_finish(bool commit) {
    for (int i = 3; i < LIN_FD_MAX; i++)
        if (g_lin_fds[i].kind != LFD_FREE) (void)fd_close(i, commit);
}

static int fd_base_path(int dirfd, const char **base) {
    if (dirfd == L_AT_FDCWD) {
        *base = g_lin_cwd;
        return 0;
    }
    if (dirfd < 0 || dirfd >= LIN_FD_MAX ||
        g_lin_fds[dirfd].kind != LFD_NX_DIR) return -L_EBADF;
    *base = g_lin_fds[dirfd].path;
    return 0;
}

static int pseudo_kind(const char *path) {
    if (strcmp(path, "/dev/null") == 0) return LFD_DEV_NULL;
    if (strcmp(path, "/dev/zero") == 0) return LFD_DEV_ZERO;
    if (strcmp(path, "/dev/urandom") == 0 ||
        strcmp(path, "/dev/random") == 0) return LFD_DEV_RANDOM;
    if (strcmp(path, "/dev/tty") == 0 ||
        strcmp(path, "/dev/console") == 0) return LFD_TTY;
    if (strcmp(path, "/dev/dri/card0") == 0 ||
        strcmp(path, "/dev/dri/renderD128") == 0)
        return LFD_DEV_DRI;
    if (strcmp(path, "/dev/snd/pcmC0D0p") == 0)
        return LFD_DEV_SND_PCM;
    if (strcmp(path, "/dev/snd/controlC0") == 0)
        return LFD_DEV_SND_CTL;
    return LFD_FREE;
}

static int do_open_path(const char *path, int flags, uint32_t mode) {
    (void)mode;
    int access = flags & L_O_ACCMODE;
    if (access > L_O_RDWR) return -L_EINVAL;

    int pk = pseudo_kind(path);
    if (pk != LFD_FREE) {
        if ((flags & L_O_DIRECTORY) != 0) return -L_ENOTDIR;
        int fd = fd_alloc_from(3);
        if (fd < 0) return fd;
        g_lin_fds[fd].kind = (lin_fd_kind_t)pk;
        g_lin_fds[fd].flags = flags;
        g_lin_fds[fd].cloexec = (flags & L_O_CLOEXEC) != 0;
        strncpy(g_lin_fds[fd].path, path, sizeof(g_lin_fds[fd].path) - 1);
        return fd;
    }

    /* /proc/self/exe is a synthetic symlink to the launched NXFS image. */
    const char *actual = path;
    if (strcmp(path, "/proc/self/exe") == 0 && g_lin_exec_path[0] == '/')
        actual = g_lin_exec_path;

    uint32_t ino = 0;
    int rr = resolve_nx_path(actual, &ino);
    if (rr < 0 && (flags & L_O_CREAT)) {
        char parent[LIN_PATH_MAX], name[NXFS_NAME_MAX];
        uint32_t pino;
        int sr = split_parent(actual, parent, sizeof(parent), name, sizeof(name));
        if (sr < 0) return sr;
        if (resolve_nx_path(parent, &pino) < 0) return -L_ENOENT;
        int cr = nxfs_create_file(pino, name, &ino);
        if (cr == NXFS_ERR_EXISTS) return -L_EEXIST;
        if (cr == NXFS_ERR_NOSPACE || cr == NXFS_ERR_FULL) return -L_ENOSPC;
        if (cr != NXFS_OK) return -L_EIO;
    } else if (rr < 0) {
        return rr;
    } else if ((flags & L_O_CREAT) && (flags & L_O_EXCL)) {
        return -L_EEXIST;
    }

    nxfs_inode_t node;
    if (nxfs_read_inode(ino, &node) != NXFS_OK) return -L_EIO;
    bool is_dir = node.type == NXFS_TYPE_DIR;
    if ((flags & L_O_DIRECTORY) && !is_dir) return -L_ENOTDIR;
    if (is_dir && access != L_O_RDONLY) return -L_EISDIR;
    if (!is_dir && node.type != NXFS_TYPE_FILE) return -L_ENODEV;

    int fd = fd_alloc_from(3);
    if (fd < 0) return fd;
    lin_fd_t *f = &g_lin_fds[fd];
    f->kind = is_dir ? LFD_NX_DIR : LFD_NX_FILE;
    f->flags = flags;
    f->inode = ino;
    f->size = ((uint64_t)node.size_hi << 32) | node.size;
    f->cloexec = (flags & L_O_CLOEXEC) != 0;
    strncpy(f->path, actual, sizeof(f->path) - 1);

    if (!is_dir && access != L_O_RDONLY) {
        if (g_lin_write_fd >= 0) {
            memset(f, 0, sizeof(*f));
            return -L_EBUSY;
        }
        if ((flags & L_O_APPEND) || (!(flags & L_O_TRUNC) && f->size != 0)) {
            memset(f, 0, sizeof(*f));
            return -L_EOPNOTSUPP;
        }
        if (nxfs_write_begin(ino) != NXFS_OK) {
            memset(f, 0, sizeof(*f));
            return -L_EIO;
        }
        f->write_stream = true;
        f->size = 0;
        g_lin_write_fd = fd;
    }
    debug_printf("[linux] open '%s' -> fd=%d kind=%d\n", actual, fd, f->kind);
    return fd;
}

static int do_openat(int dirfd, uint64_t upath, int flags, uint32_t mode) {
    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    int r = copy_user_cstr(upath, path, sizeof(path));
    if (r < 0) return r;
    const char *base = "/";
    if (path[0] != '/') {
        r = fd_base_path(dirfd, &base);
        if (r < 0) return r;
    }
    r = normalize_path(base, path, canon, sizeof(canon));
    if (r < 0) return r;
    return do_open_path(canon, flags, mode);
}

static int copy_user_range(uint64_t dst, uint64_t src, uint64_t len);

static long do_read(int fd, void *buf, uint64_t len) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE)
        return -L_EBADF;
    if (len == 0) return 0;
    if (!uptr_write_ok((uint64_t)(uintptr_t)buf, len)) return -L_EFAULT;
    if (len > 0x7FFFFFFFu) len = 0x7FFFFFFFu;
    lin_fd_t *f = &g_lin_fds[fd];
    if ((f->flags & L_O_ACCMODE) == L_O_WRONLY) return -L_EBADF;
    switch (f->kind) {
    case LFD_TTY: {
        if (g_lin_tty_line_pos < g_lin_tty_line_len) {
            uint64_t avail = g_lin_tty_line_len - g_lin_tty_line_pos;
            uint64_t chunk = len < avail ? len : avail;
            memcpy(buf, g_lin_tty_line + g_lin_tty_line_pos, (size_t)chunk);
            g_lin_tty_line_pos += chunk;
            if (g_lin_tty_line_pos >= g_lin_tty_line_len) {
                g_lin_tty_line_len = 0;
                g_lin_tty_line_pos = 0;
            }
            return (long)chunk;
        }
        size_t i = 0;
        while (i + 1 < len && i + 1 < sizeof(g_lin_tty_line)) {
            if ((f->flags & L_O_NONBLOCK) && !keyboard_has_data())
                return i ? (long)i : -L_EAGAIN;
            int c = keyboard_wait_getc();
            if (c == KEY_CTRL_C) {
                term_putc('^'); term_putc('C'); term_putc('\n');
                g_lin_tty_line_len = 0;
                g_lin_tty_line_pos = 0;
                return i ? (long)i : -L_EINTR;
            }
            if (c == KEY_BACKSPACE || c == 127) {
                if (i > 0) {
                    i--;
                    term_putc('\b'); term_putc(' '); term_putc('\b');
                }
                continue;
            }
            if (c == KEY_ENTER) {
                term_putc('\n');
                g_lin_tty_line[i++] = '\n';
                break;
            }
            if (c >= 32 && c < 127) {
                term_putc((char)c);
                g_lin_tty_line[i++] = (char)c;
            }
        }
        if (i == 0) return -L_EAGAIN;
        memcpy(buf, g_lin_tty_line, i);
        g_lin_tty_line_len = i;
        g_lin_tty_line_pos = i;
        return (long)i;
    }
    case LFD_DEV_NULL:
        return 0;
    case LFD_DEV_ZERO:
        memset(buf, 0, (size_t)len);
        return (long)len;
    case LFD_DEV_RANDOM: {
        uint8_t *p = (uint8_t *)buf;
        uint32_t x = pit_ms() ^ (uint32_t)f->offset ^ 0x9E3779B9u;
        for (uint64_t i = 0; i < len; i++) {
            x = x * 1664525u + 1013904223u;
            p[i] = (uint8_t)(x >> 24);
        }
        f->offset += len;
        return (long)len;
    }
    case LFD_NX_DIR:
        return -L_EISDIR;
    case LFD_NX_FILE: {
        int got = nxfs_read_at(f->inode, f->offset, buf, (uint32_t)len);
        if (got < 0) return -L_EIO;
        f->offset += (uint32_t)got;
        return got;
    }
    case LFD_SOCK: {
        bool nb = (f->flags & L_O_NONBLOCK) != 0;
        uint8_t stack[4096];
        uint64_t left = len;
        uint64_t off = 0;
        long total = 0;
        while (left > 0) {
            uint32_t chunk = left > sizeof(stack) ? (uint32_t)sizeof(stack)
                                                  : (uint32_t)left;
            long rc = lin_sock_read(&f->sock, stack, chunk, nb);
            if (rc > 0) {
                debug_printf("[linux/sock] read %ld bytes from socket\n", rc);
                if (copy_user_range((uint64_t)(uintptr_t)buf + off,
                                    (uint64_t)(uintptr_t)stack,
                                    (uint32_t)rc) != 0)
                    return total > 0 ? total : -L_EFAULT;
                total += rc;
                off += (uint64_t)rc;
                left -= (uint64_t)rc;
                if ((uint64_t)rc < chunk) break;
                continue;
            }
            if (rc == 0) return total > 0 ? total : 0;
            if (rc == -11) return total > 0 ? total : -L_EAGAIN;
            if (rc == -110) return total > 0 ? total : (nb ? -L_EAGAIN : -L_ETIMEDOUT);
            return total > 0 ? total : -L_EIO;
        }
        return total;
    }
    case LFD_PIPE: {
        if (f->pipe_wr || f->pipe_ix < 0 || f->pipe_ix >= LIN_PIPE_PAIRS)
            return -L_EBADF;
        lin_pipe_pair_t *pp = &g_lin_pipes[f->pipe_ix];
        if (!pp->r_open) return 0;
        if (pp->len == 0) {
            if (!pp->w_open) return 0;
            if (f->flags & L_O_NONBLOCK) return -L_EAGAIN;
            return 0;
        }
        uint64_t chunk = len < pp->len ? len : pp->len;
        if (copy_user_range((uint64_t)(uintptr_t)buf,
                            (uint64_t)(uintptr_t)pp->buf, chunk) != 0)
            return -L_EFAULT;
        memmove(pp->buf, pp->buf + chunk, pp->len - (uint32_t)chunk);
        pp->len -= (uint32_t)chunk;
        return (long)chunk;
    }
    case LFD_EVENTFD: {
        if (len < 8) return -L_EINVAL;
        if (f->event_count == 0) return -L_EAGAIN;
        uint64_t value = f->event_count;
        f->event_count = 0;
        return user_copy_to(g_lin_pd, (uint64_t)(uintptr_t)buf,
                            &value, 8) == 0 ? 8 : -L_EFAULT;
    }
    default:
        return -L_EBADF;
    }
}

static long do_write(int fd, const void *buf, uint64_t len) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE)
        return -L_EBADF;
    if (len == 0) return 0;
    if (!uptr_ok((uint64_t)(uintptr_t)buf, len)) return -L_EFAULT;
    if (len > 0x7FFFFFFFu) len = 0x7FFFFFFFu;
    lin_fd_t *f = &g_lin_fds[fd];
    if ((f->flags & L_O_ACCMODE) == L_O_RDONLY && f->kind != LFD_DEV_ZERO)
        return -L_EBADF;
    switch (f->kind) {
    case LFD_TTY:
        for (uint64_t i = 0; i < len; i++) term_putc(((const char *)buf)[i]);
        return (long)len;
    case LFD_DEV_NULL:
        return (long)len;
    case LFD_DEV_ZERO:
        return -L_EBADF;
    case LFD_NX_DIR:
        return -L_EISDIR;
    case LFD_NX_FILE:
        if (!f->write_stream || g_lin_write_fd != fd) return -L_EBADF;
        if (nxfs_write_append(buf, (uint32_t)len) != NXFS_OK) return -L_EIO;
        f->offset += len;
        if (f->offset > f->size) f->size = f->offset;
        return (long)len;
    case LFD_SOCK: {
        uint8_t stack[4096];
        uint64_t off = 0;
        long total = 0;
        const uint8_t *src = (const uint8_t *)buf;
        while (off < len) {
            uint32_t chunk = (uint32_t)((len - off) > sizeof(stack)
                                     ? sizeof(stack) : (len - off));
            memcpy(stack, src + off, chunk);
            long rc = lin_sock_write(&f->sock, stack, chunk);
            if (rc < 0) return total > 0 ? total : (rc == -22 ? -L_EINVAL : -L_EIO);
            total += rc;
            off += (uint64_t)rc;
            if ((uint64_t)rc < chunk) break;
        }
        return total;
    }
    case LFD_PIPE: {
        if (!f->pipe_wr || f->pipe_ix < 0 || f->pipe_ix >= LIN_PIPE_PAIRS)
            return -L_EBADF;
        lin_pipe_pair_t *pp = &g_lin_pipes[f->pipe_ix];
        if (!pp->w_open) return -L_EPIPE;
        uint64_t room = LIN_PIPE_BUF - pp->len;
        if (room == 0) {
            if (f->flags & L_O_NONBLOCK) return -L_EAGAIN;
            return 0;
        }
        uint64_t chunk = len < room ? len : room;
        if (copy_user_range((uint64_t)(uintptr_t)pp->buf + pp->len,
                            (uint64_t)(uintptr_t)buf, chunk) != 0)
            return -L_EFAULT;
        pp->len += (uint32_t)chunk;
        return (long)chunk;
    }
    case LFD_EVENTFD: {
        if (len < 8) return -L_EINVAL;
        uint64_t value = 0;
        if (user_copy_from(g_lin_pd, &value,
                           (uint64_t)(uintptr_t)buf, 8) != 0)
            return -L_EFAULT;
        f->event_count += value;
        return 8;
    }
    case LFD_DEV_SND_PCM: {
        if ((len & 3u) != 0) return -L_EINVAL;
        int16_t stack[2048];
        uint64_t off = 0;
        long total = 0;
        while (off < len) {
            uint32_t chunk = (uint32_t)((len - off) > sizeof(stack)
                                     ? sizeof(stack) : (len - off));
            if (user_copy_from(g_lin_pd, stack,
                               (uint64_t)(uintptr_t)buf + off,
                               chunk) != 0)
                return total > 0 ? total : -L_EFAULT;
            uint32_t fchunk = chunk / 4u;
            int queued = audio_play_pcm(stack, fchunk, 2, 44100);
            if (queued < 0) return total > 0 ? total : -L_EIO;
            total += (long)(queued * 4u);
            off += (uint64_t)queued * 4u;
            if ((uint32_t)queued < fchunk) break;
        }
        if (total > 0)
            debug_printf("[linux/alsa] pcm write %ld bytes (%u frames)\n",
                         total, (uint32_t)(total / 4));
        return total;
    }
    case LFD_DEV_SND_CTL:
        return (long)len;
    default:
        return -L_EBADF;
    }
}

static long do_iov(int fd, uint64_t uiov, uint64_t count, bool write) {
    if (count > 1024 || count > (~0ULL / 16)) return -L_EINVAL;
    if (!uptr_ok(uiov, count * 16)) return -L_EFAULT;
    const uint64_t *iov = (const uint64_t *)(uintptr_t)uiov;
    long total = 0;
    for (uint64_t i = 0; i < count; i++) {
        long n = write ? do_write(fd, (const void *)(uintptr_t)iov[i * 2],
                                  iov[i * 2 + 1])
                       : do_read(fd, (void *)(uintptr_t)iov[i * 2],
                                 iov[i * 2 + 1]);
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint64_t)n < iov[i * 2 + 1]) break;
    }
    return total;
}

typedef struct {
    uint64_t name;
    uint32_t name_len;
    uint32_t pad0;
    uint64_t iov;
    uint64_t iov_len;
    uint64_t control;
    uint64_t control_len;
    uint32_t flags;
    uint32_t pad1;
} lin_msghdr_t;

static long do_msg(int fd, uint64_t umsg, bool write) {
    if (!uptr_ok(umsg, sizeof(lin_msghdr_t)))
        return -L_EFAULT;
    lin_msghdr_t msg;
    if (copy_user_range((uint64_t)(uintptr_t)&msg, umsg, sizeof(msg)) != 0)
        return -L_EFAULT;
    if (msg.iov_len > 1024)
        return -L_EINVAL;
    long ret = do_iov(fd, msg.iov, msg.iov_len, write);
    if (!write && ret >= 0 && uptr_write_ok(umsg + 48, 4))
        *(uint32_t *)(uintptr_t)(umsg + 48) = 0;
    return ret;
}

static long do_lseek(int fd, int64_t off, int whence) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE)
        return -L_EBADF;
    lin_fd_t *f = &g_lin_fds[fd];
    if (f->kind != LFD_NX_FILE || f->write_stream) return -L_ESPIPE;
    int64_t base;
    if (whence == L_SEEK_SET) base = 0;
    else if (whence == L_SEEK_CUR) base = (int64_t)f->offset;
    else if (whence == L_SEEK_END) base = (int64_t)f->size;
    else return -L_EINVAL;
    if (off < 0 && -off > base) return -L_EINVAL;
    uint64_t next = (uint64_t)(base + off);
    f->offset = next;
    return (long)next;
}

static int do_dup_from(int oldfd, int first, bool cloexec) {
    if (oldfd < 0 || oldfd >= LIN_FD_MAX ||
        g_lin_fds[oldfd].kind == LFD_FREE) return -L_EBADF;
    if (g_lin_fds[oldfd].write_stream) return -L_EBUSY;
    if (g_lin_fds[oldfd].kind == LFD_SOCK) return -L_EINVAL;
    int fd = fd_alloc_from(first);
    if (fd < 0) return fd;
    g_lin_fds[fd] = g_lin_fds[oldfd];
    g_lin_fds[fd].cloexec = cloexec;
    return fd;
}

/* ---- Linux stat and directory ABI -------------------------------------- */
typedef struct PACKED {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, pad0;
    uint64_t st_rdev;
    int64_t  st_size, st_blksize, st_blocks;
    uint64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec;
    uint64_t ctime_sec, ctime_nsec;
    int64_t  unused[3];
} lin_stat_t;

static void stat_fill(lin_stat_t *st, uint32_t mode, uint64_t size,
                      uint64_t ino) {
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = ino;
    st->st_nlink = (mode & L_S_IFMT) == L_S_IFDIR ? 2 : 1;
    st->st_mode = mode;
    st->st_size = (int64_t)size;
    st->st_blksize = NXFS_BLOCK_BYTES;
    st->st_blocks = (int64_t)((size + 511) / 512);
}

static int stat_path_kernel(const char *path, lin_stat_t *st) {
    if (strcmp(path, "/dev") == 0 || strcmp(path, "/proc") == 0 ||
        strcmp(path, "/proc/self") == 0 || strcmp(path, "/proc/self/fd") == 0 ||
        strcmp(path, "/dev/dri") == 0 ||
        strcmp(path, "/dev/snd") == 0) {
        stat_fill(st, L_S_IFDIR | 0755u, 0, 2);
        return 0;
    }
    int pk = pseudo_kind(path);
    if (pk != LFD_FREE) {
        stat_fill(st, L_S_IFCHR | 0666u, 0, (uint64_t)pk + 10);
        return 0;
    }
    const char *actual = path;
    if (strcmp(path, "/proc/self/exe") == 0) {
        if (g_lin_exec_path[0] != '/') {
            stat_fill(st, L_S_IFREG | 0555u, g_lin_image_size, 3);
            return 0;
        }
        actual = g_lin_exec_path;
    }
    uint32_t ino;
    if (resolve_nx_path(actual, &ino) < 0) return -L_ENOENT;
    nxfs_inode_t node;
    if (nxfs_read_inode(ino, &node) != NXFS_OK) return -L_EIO;
    uint64_t size = ((uint64_t)node.size_hi << 32) | node.size;
    uint32_t mode = node.type == NXFS_TYPE_DIR
                  ? (L_S_IFDIR | 0755u) : (L_S_IFREG | 0755u);
    stat_fill(st, mode, node.type == NXFS_TYPE_DIR ? 0 : size, (uint64_t)ino + 1);
    return 0;
}

static int do_stat_user(int dirfd, uint64_t upath, uint64_t ust, int flags) {
    (void)flags;
    if (!uptr_write_ok(ust, sizeof(lin_stat_t))) return -L_EFAULT;
    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    int r = copy_user_cstr(upath, path, sizeof(path));
    if (r < 0) return r;
    const char *base = "/";
    if (path[0] != '/') {
        r = fd_base_path(dirfd, &base);
        if (r < 0) return r;
    }
    r = normalize_path(base, path, canon, sizeof(canon));
    if (r < 0) return r;
    return stat_path_kernel(canon, (lin_stat_t *)(uintptr_t)ust);
}

static int do_fstat(int fd, uint64_t ust) {
    if (!uptr_write_ok(ust, sizeof(lin_stat_t))) return -L_EFAULT;
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE)
        return -L_EBADF;
    lin_fd_t *f = &g_lin_fds[fd];
    lin_stat_t *st = (lin_stat_t *)(uintptr_t)ust;
    if (f->kind == LFD_NX_FILE)
        stat_fill(st, L_S_IFREG | 0755u, f->size, (uint64_t)f->inode + 1);
    else if (f->kind == LFD_NX_DIR)
        stat_fill(st, L_S_IFDIR | 0755u, 0, (uint64_t)f->inode + 1);
    else
        stat_fill(st, L_S_IFCHR | 0666u, 0, (uint64_t)f->kind + 10);
    return 0;
}

static long do_getdents64(int fd, uint64_t ubuf, uint64_t cap) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind != LFD_NX_DIR)
        return -L_EBADF;
    if (!uptr_write_ok(ubuf, cap)) return -L_EFAULT;
    nxfs_inode_t dir;
    if (nxfs_read_inode(g_lin_fds[fd].inode, &dir) != NXFS_OK) return -L_EIO;
    uint8_t *dst = (uint8_t *)(uintptr_t)ubuf;
    uint32_t used = 0;
    while (g_lin_fds[fd].dir_pos < dir.child_count) {
        uint32_t idx = g_lin_fds[fd].dir_pos;
        nxfs_inode_t child;
        if (nxfs_read_inode(dir.children[idx], &child) != NXFS_OK) return -L_EIO;
        uint32_t nl = (uint32_t)strlen(child.name);
        uint16_t reclen = (uint16_t)((19u + nl + 1u + 7u) & ~7u);
        if ((uint64_t)used + reclen > cap) break;
        uint8_t *d = dst + used;
        *(uint64_t *)(void *)(d + 0) = (uint64_t)dir.children[idx] + 1;
        *(int64_t  *)(void *)(d + 8) = (int64_t)idx + 1;
        *(uint16_t *)(void *)(d + 16) = reclen;
        d[18] = child.type == NXFS_TYPE_DIR ? 4 : 8; /* DT_DIR / DT_REG */
        memcpy(d + 19, child.name, nl + 1);
        if (reclen > 19 + nl + 1) memset(d + 19 + nl + 1, 0,
                                         reclen - 19 - nl - 1);
        used += reclen;
        g_lin_fds[fd].dir_pos++;
    }
    return used;
}

/* ---- Anonymous/file-backed arena mmap ---------------------------------- */
typedef struct {
    bool active;
    bool lazy;
    uint64_t addr;
    uint64_t len;
    int prot;
    int flags;
} lin_map_t;

static lin_map_t g_lin_maps[LIN_MAP_MAX];
static uint32_t g_lin_last_map_stack_top;

static bool map_overlaps(uint64_t addr, uint64_t len, const lin_map_t *m) {
    return m->active && addr < m->addr + m->len && m->addr < addr + len;
}

static bool map_range_free(uint64_t addr, uint64_t len) {
    uint64_t heap_hi = (g_lin_brk + 4095) & ~4095ULL;
    uint64_t map_top = linux_compat32_active() ? LIN_STACK_TOP : LIN_MMAP_TOP;
    if (!range_inside(addr, len, LIN_BRK_LIMIT, map_top) ||
        (addr < g_lin_load_hi && g_lin_load_lo < addr + len) ||
        (addr < LIN_STACK_TOP && addr + len > LIN_STACK_BASE) ||
        (addr < heap_hi && g_lin_brk_floor < addr + len))
        return false;
    for (int i = 0; i < LIN_MAP_MAX; i++)
        if (map_overlaps(addr, len, &g_lin_maps[i])) return false;
    return true;
}

static int map_slot(void) {
    for (int i = 0; i < LIN_MAP_MAX; i++) if (!g_lin_maps[i].active) return i;
    return -1;
}

#define LIN_I386_THR_STACK_SLOTS 16u

static uint64_t lin_i386_pick_thread_stack(uint64_t len) {
    len = (len + 4095) & ~4095ULL;
    for (uint32_t i = 0; i < LIN_I386_THR_STACK_SLOTS; i++) {
        uint64_t top = LIN_STACK_BASE - (uint64_t)(i + 1) * (len + 4096u);
        if (top < len + LIN_BRK_LIMIT) break;
        uint64_t base = top - len;
        if (map_range_free(base, len))
            return base;
    }
    return 0;
}

/* MAP_FIXED may replace part of an existing VMA (glibc ld.so loads PT_LOAD). */
static void map_evict_overlaps(uint64_t addr, uint64_t len) {
    uint64_t end = addr + len;
    for (int i = 0; i < LIN_MAP_MAX; i++) {
        lin_map_t *m = &g_lin_maps[i];
        if (!m->active) continue;
        uint64_t mend = m->addr + m->len;
        if (addr >= mend || end <= m->addr) continue;

        uint64_t o_lo = addr > m->addr ? addr : m->addr;
        uint64_t o_hi = end < mend ? end : mend;
        for (uint64_t p = o_lo; p < o_hi; p += VMM_PAGE_SIZE)
            (void)vmm_unmap_page(g_lin_pd, (uintptr_t)p);

        if (addr <= m->addr && end >= mend) {
            memset(m, 0, sizeof(*m));
        } else if (addr <= m->addr && end < mend) {
            m->addr = end;
            m->len = mend - end;
        } else if (addr > m->addr && end >= mend) {
            m->len = addr - m->addr;
        } else {
            uint64_t tail_addr = end;
            uint64_t tail_len = mend - end;
            int tail = map_slot();
            m->len = addr - m->addr;
            if (tail >= 0) {
                g_lin_maps[tail].active = true;
                g_lin_maps[tail].addr = tail_addr;
                g_lin_maps[tail].len = tail_len;
                g_lin_maps[tail].prot = m->prot;
                g_lin_maps[tail].flags = m->flags;
            }
        }
    }
}

static uint64_t map_lowest(void) {
    uint64_t low = LIN_BRK_LIMIT;
    for (int i = 0; i < LIN_MAP_MAX; i++)
        if (g_lin_maps[i].active && g_lin_maps[i].addr < low) low = g_lin_maps[i].addr;
    return low;
}

/* Demand-page a single 4 KiB page inside a lazy anonymous mmap VMA. */
static bool lin_mmap_materialize(uint64_t fault_va, bool writing) {
    if (!g_lin_pd) return false;
    uint64_t page = fault_va & ~4095ULL;
    for (int i = 0; i < LIN_MAP_MAX; i++) {
        lin_map_t *m = &g_lin_maps[i];
        if (!m->active || !m->lazy) continue;
        if (page < m->addr || page >= m->addr + m->len) continue;
        if (writing && !(m->prot & L_PROT_WRITE)) return false;
        if (vmm_resolve(g_lin_pd, (uintptr_t)page)) {
            if (!writing) return true;
            (void)vmm_unmap_page(g_lin_pd, (uintptr_t)page);
        }

        uint32_t pa = vmm_frame_alloc();
        if (!pa) {
            debug_printf("[linux/mmap] fault-in ENOMEM 0x%x\n", (uint32_t)page);
            return false;
        }
        uintptr_t flags = VMM_FLAG_USER |
                          ((m->prot & L_PROT_WRITE) ? VMM_FLAG_RW : 0) |
                          ((m->prot & L_PROT_EXEC) ? 0 : VMM_FLAG_NX);
        if (vmm_map_page(g_lin_pd, (uintptr_t)page, pa,
                         VMM_FLAG_USER | VMM_FLAG_RW |
                         (flags & VMM_FLAG_NX)) != 0) {
            vmm_frame_free(pa);
            return false;
        }
        memset((void *)(uintptr_t)page, 0, VMM_PAGE_SIZE);
        if (!(m->prot & L_PROT_WRITE) &&
            vmm_protect_page(g_lin_pd, (uintptr_t)page, flags) != 0)
            return false;
        debug_printf("[linux/mmap] fault-in 0x%x (slot=%d)\n",
                     (uint32_t)page, i);
        return true;
    }
    return false;
}

static bool lin_mmap_materialize_prot(uint64_t page, int prot) {
    if (!g_lin_pd) return false;
    page &= ~4095ULL;
    for (int i = 0; i < LIN_MAP_MAX; i++) {
        lin_map_t *m = &g_lin_maps[i];
        if (!m->active || !m->lazy) continue;
        if (page < m->addr || page >= m->addr + m->len) continue;
        m->prot |= (uint32_t)prot;
        if (vmm_resolve(g_lin_pd, (uintptr_t)page)) {
            uintptr_t pf = VMM_FLAG_USER |
                           ((m->prot & L_PROT_WRITE) ? VMM_FLAG_RW : 0) |
                           ((m->prot & L_PROT_EXEC) ? 0 : VMM_FLAG_NX);
            return vmm_protect_page(g_lin_pd, (uintptr_t)page, pf) == 0;
        }
        uint32_t pa = vmm_frame_alloc();
        if (!pa) return false;
        uintptr_t pf = VMM_FLAG_USER |
                       ((m->prot & L_PROT_WRITE) ? VMM_FLAG_RW : 0) |
                       ((m->prot & L_PROT_EXEC) ? 0 : VMM_FLAG_NX);
        if (vmm_map_page(g_lin_pd, (uintptr_t)page, pa,
                         VMM_FLAG_USER | VMM_FLAG_RW |
                         (pf & VMM_FLAG_NX)) != 0) {
            vmm_frame_free(pa);
            return false;
        }
        memset((void *)(uintptr_t)page, 0, VMM_PAGE_SIZE);
        if (vmm_protect_page(g_lin_pd, (uintptr_t)page, pf) != 0)
            return false;
        debug_printf("[linux/mmap] mprotect-in 0x%x (slot=%d prot=0x%x)\n",
                     (uint32_t)page, i, (uint32_t)m->prot);
        return true;
    }
    return false;
}

static int lin_mprotect_user(uint64_t addr, uint64_t len, int prot) {
    if ((addr & 4095u) || len == 0 ||
        (prot & ~(L_PROT_READ | L_PROT_WRITE | L_PROT_EXEC)))
        return -L_EINVAL;
    for (int i = 0; i < LIN_MAP_MAX; i++) {
        lin_map_t *m = &g_lin_maps[i];
        if (!m->active || addr < m->addr || addr >= m->addr + m->len)
            continue;
        uint64_t max = m->addr + m->len - addr;
        if (len > max) len = max;
        break;
    }
    bool lazy_vma = false;
    for (int i = 0; i < LIN_MAP_MAX; i++) {
        lin_map_t *m = &g_lin_maps[i];
        if (!m->active || !m->lazy) continue;
        if (addr >= m->addr && addr + len <= m->addr + m->len) {
            lazy_vma = true;
            break;
        }
    }
    if (!lazy_vma && !uptr_ok(addr, len))
        return -L_EFAULT;
    if (len >= 0x100000u && lazy_vma)
        debug_printf("[linux] mprotect addr=0x%x len=0x%x prot=0x%x\n",
                     (uint32_t)addr, (uint32_t)len, (uint32_t)prot);
    if (lazy_vma) {
        for (int i = 0; i < LIN_MAP_MAX; i++) {
            lin_map_t *m = &g_lin_maps[i];
            if (!m->active || !m->lazy) continue;
            if (addr >= m->addr && addr + len <= m->addr + m->len) {
                m->prot |= (uint32_t)prot;
                return 0;
            }
        }
    }
    uintptr_t pf = VMM_FLAG_USER |
                   ((prot & L_PROT_WRITE) ? VMM_FLAG_RW : 0) |
                   ((prot & L_PROT_EXEC) ? 0 : VMM_FLAG_NX);
    uint64_t range = (len + 4095) & ~4095ULL;
    for (uint64_t p = addr; p < addr + range; p += VMM_PAGE_SIZE) {
        if (!vmm_resolve(g_lin_pd, (uintptr_t)p) &&
            !lin_mmap_materialize_prot(p, prot))
            return -L_ENOMEM;
        if (vmm_protect_page(g_lin_pd, (uintptr_t)p, pf) != 0)
            return -L_ENOMEM;
    }
    return 0;
}

static bool lin_try_page_fault(uint64_t vector, uint64_t err) {
    if (vector != 14 || !(err & 4u)) return false;
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    if (lin_mmap_materialize(cr2, (err & 2u) != 0))
        return true;
    debug_printf("[linux/mmap] #PF cr2=0x%x err=0x%x (unhandled)\n",
                 (uint32_t)cr2, (uint32_t)err);
    return false;
}

static long lin_dumb_mmap(uint64_t hint, uint64_t length, int prot, int flags,
                          int fd, uint64_t offset) {
    lin_dumb_t *d = lin_dumb_find_mapoff(fd, offset);
    if (!d) return -L_EINVAL;
    if (offset != d->map_off || length > d->size) return -L_EINVAL;

    uint64_t len = (length + 4095) & ~4095ULL;
    if (len > d->npages * (uint64_t)VMM_PAGE_SIZE) return -L_EINVAL;

    uint64_t addr = hint & ~4095ULL;
    bool fixed = (flags & L_MAP_FIXED) != 0;
    if (fixed) {
        if (hint != addr ||
            !range_inside(addr, len, LIN_BRK_LIMIT, LIN_MMAP_TOP) ||
            (addr < g_lin_load_hi && g_lin_load_lo < addr + len) ||
            (addr < LIN_STACK_TOP && LIN_STACK_BASE < addr + len) ||
            (addr < ((g_lin_brk + 4095) & ~4095ULL) &&
             g_lin_brk_floor < addr + len))
            return -L_EINVAL;
        map_evict_overlaps(addr, len);
    } else if (!addr || !map_range_free(addr, len)) {
        uint64_t hi = LIN_MMAP_TOP;
        for (;;) {
            if (hi < len) return -L_ENOMEM;
            addr = (hi - len) & ~4095ULL;
            if (map_range_free(addr, len)) break;
            uint64_t next_hi = addr;
            for (int i = 0; i < LIN_MAP_MAX; i++)
                if (map_overlaps(addr, len, &g_lin_maps[i]) &&
                    g_lin_maps[i].addr < next_hi)
                    next_hi = g_lin_maps[i].addr;
            if (next_hi >= hi) return -L_ENOMEM;
            hi = next_hi;
        }
    }

    int slot = map_slot();
    if (slot < 0) return -L_ENOMEM;

    uintptr_t final_flags = ((prot & L_PROT_WRITE) ? VMM_FLAG_RW : 0) |
                            ((prot & L_PROT_EXEC) ? 0 : VMM_FLAG_NX);
    uintptr_t pte_flags = VMM_FLAG_USER | VMM_FLAG_RW |
                          (final_flags & VMM_FLAG_NX);

    for (uint64_t pg = 0; pg < len; pg += VMM_PAGE_SIZE) {
        uint32_t dpi = (uint32_t)(pg / VMM_PAGE_SIZE);
        if (dpi >= d->npages) return -L_EINVAL;
        uint32_t pa = d->pa[dpi];
        if (!pa || vmm_map_page(g_lin_pd, (uintptr_t)(addr + pg), pa,
                                pte_flags) != 0) {
            for (uint64_t undo = 0; undo < pg; undo += VMM_PAGE_SIZE)
                (void)vmm_unmap_page(g_lin_pd, (uintptr_t)(addr + undo));
            return -L_ENOMEM;
        }
    }
    if (!(prot & L_PROT_WRITE))
        for (uint64_t p = addr; p < addr + len; p += VMM_PAGE_SIZE)
            if (vmm_protect_page(g_lin_pd, (uintptr_t)p, final_flags) != 0) {
                for (uint64_t undo = 0; undo < len; undo += VMM_PAGE_SIZE)
                    (void)vmm_unmap_page(g_lin_pd, (uintptr_t)(addr + undo));
                return -L_EIO;
            }

    g_lin_maps[slot].active = true;
    g_lin_maps[slot].lazy = false;
    g_lin_maps[slot].addr = addr;
    g_lin_maps[slot].len = len;
    g_lin_maps[slot].prot = prot;
    g_lin_maps[slot].flags = flags;
    debug_printf("[linux/dri] mmap dumb h=%u off=0x%x -> 0x%x (%u bytes)\n",
                 d->handle, (uint32_t)offset, (uint32_t)addr, (uint32_t)len);
    return (long)addr;
}

static long do_mmap(uint64_t hint, uint64_t length, int prot, int flags,
                    int fd, uint64_t offset) {
    bool dri_map = !(flags & L_MAP_ANON) && fd >= 0 && fd < LIN_FD_MAX &&
                   g_lin_fds[fd].kind == LFD_DEV_DRI;
    if ((flags & (L_MAP_SHARED | L_MAP_PRIVATE)) == 0)
        return -L_EINVAL;
    if (!dri_map && (flags & (L_MAP_SHARED | L_MAP_PRIVATE)) != L_MAP_PRIVATE)
        return -L_EOPNOTSUPP;
    if (dri_map)
        return lin_dumb_mmap(hint, length, prot, flags, fd, offset);
    if (length >= 0x100000u && (flags & 0x20000u))
        debug_printf("[linux] mmap req hint=0x%x len=0x%x prot=0x%x flags=0x%x\n",
                     (uint32_t)hint, (uint32_t)length, (uint32_t)prot,
                     (uint32_t)flags);
    if (linux_compat32_active() && (flags & 0x20000u) &&
        length > 2u * 1024u * 1024u)
        length = 2u * 1024u * 1024u;
    if (length == 0 || length > LIN_MAX_MAP_BYTES) {
        debug_printf("[linux] mmap reject hint=0x%x len=%u flags=0x%x\n",
                     (uint32_t)hint, (uint32_t)length, (uint32_t)flags);
        return -L_EINVAL;
    }
    /* PROT_NONE (0) is valid: musl maps the full thread stack VMA no-access,
     * then mprotect(map+guard, size-guard, RW) for the usable portion. */
    if (prot & ~(L_PROT_READ | L_PROT_WRITE | L_PROT_EXEC))
        return -L_EINVAL;
    if (offset & 4095u) return -L_EINVAL;
    if (!(flags & L_MAP_ANON) &&
        (fd < 0 || fd >= LIN_FD_MAX ||
         g_lin_fds[fd].kind != LFD_NX_FILE))
        return -L_EBADF;
    uint64_t len = (length + 4095) & ~4095ULL;

    uint64_t addr = hint & ~4095ULL;
    bool fixed = (flags & L_MAP_FIXED) != 0;
    uint64_t map_top = LIN_MMAP_TOP;
    if (linux_compat32_active() && (flags & 0x20000u))
        map_top = LIN_STACK_TOP;
    if (!fixed && linux_compat32_active() && (flags & 0x20000u) && !addr) {
        addr = lin_i386_pick_thread_stack(len);
        if (!addr) {
            debug_printf("[linux] MAP_STACK alloc len=0x%x failed\n",
                         (uint32_t)len);
            return -L_ENOMEM;
        }
    } else if (fixed) {
        if (hint != addr ||
            !range_inside(addr, len, LIN_BRK_LIMIT, map_top) ||
            (addr < g_lin_load_hi && g_lin_load_lo < addr + len) ||
            (!(linux_compat32_active() && (flags & 0x20000u)) &&
             addr < LIN_STACK_TOP && LIN_STACK_BASE < addr + len) ||
            (addr < ((g_lin_brk + 4095) & ~4095ULL) &&
             g_lin_brk_floor < addr + len))
            return -L_EINVAL;
        map_evict_overlaps(addr, len);
    } else if (!addr || !map_range_free(addr, len)) {
        uint64_t hi = map_top;
        for (;;) {
            if (hi < len) {
                debug_printf("[linux] mmap ENOMEM hi=0x%x len=0x%x flags=0x%x\n",
                             (uint32_t)hi, (uint32_t)len, (uint32_t)flags);
                return -L_ENOMEM;
            }
            if (!addr)
                addr = (hi - len) & ~4095ULL;
            if (map_range_free(addr, len)) break;
            addr = 0;
            uint64_t next_hi = (hi - len) & ~4095ULL;
            for (int i = 0; i < LIN_MAP_MAX; i++)
                if (map_overlaps((hi - len) & ~4095ULL, len, &g_lin_maps[i]) &&
                    g_lin_maps[i].addr < next_hi)
                    next_hi = g_lin_maps[i].addr;
            if (next_hi >= hi) return -L_ENOMEM;
            hi = next_hi;
        }
    }
    int slot = map_slot();
    if (slot < 0) return -L_ENOMEM;

    bool lazy = (flags & L_MAP_ANON) && len >= LIN_MMAP_LAZY_MIN;
    if (linux_compat32_active() && (flags & 0x20000u))
        lazy = true;
    uintptr_t final_flags = ((prot & L_PROT_WRITE) ? VMM_FLAG_RW : 0) |
                            ((prot & L_PROT_EXEC) ? 0 : VMM_FLAG_NX);
    uintptr_t pte_flags = VMM_FLAG_USER | VMM_FLAG_RW |
                          (final_flags & VMM_FLAG_NX);

    if (!lazy) {
        uint64_t mapped = 0;
        for (; mapped < len; mapped += VMM_PAGE_SIZE) {
            uint32_t pa = vmm_frame_alloc();
            if (!pa ||
                vmm_map_page(g_lin_pd, (uintptr_t)(addr + mapped), pa,
                             pte_flags) != 0) {
                if (pa) vmm_frame_free(pa);
                for (uint64_t undo = 0; undo < mapped; undo += VMM_PAGE_SIZE)
                    (void)vmm_unmap_page(g_lin_pd, (uintptr_t)(addr + undo));
                debug_printf("[linux] mmap ENOMEM addr=0x%x len=%u\n",
                             (uint32_t)addr, (uint32_t)len);
                return -L_ENOMEM;
            }
        }
        memset((void *)(uintptr_t)addr, 0, (size_t)len);
    }
    if (!(flags & L_MAP_ANON)) {
        uint64_t done = 0;
        while (done < length) {
            uint32_t chunk = (uint32_t)((length - done) > 65536
                                     ? 65536 : (length - done));
            int got = nxfs_read_at(g_lin_fds[fd].inode, offset + done,
                                   (void *)(uintptr_t)(addr + done), chunk);
            if (got < 0) {
                for (uint64_t undo = 0; undo < len; undo += VMM_PAGE_SIZE)
                    (void)vmm_unmap_page(g_lin_pd, (uintptr_t)(addr + undo));
                return -L_EIO;
            }
            if (got == 0) break;
            done += (uint32_t)got;
        }
    }
    if (!lazy && !(prot & L_PROT_WRITE))
        for (uint64_t p = addr; p < addr + len; p += VMM_PAGE_SIZE)
            if (vmm_protect_page(g_lin_pd, (uintptr_t)p, final_flags) != 0) {
                for (uint64_t undo = 0; undo < len; undo += VMM_PAGE_SIZE)
                    (void)vmm_unmap_page(g_lin_pd,
                                         (uintptr_t)(addr + undo));
                return -L_EIO;
            }
    g_lin_maps[slot].active = true;
    g_lin_maps[slot].lazy = lazy;
    g_lin_maps[slot].addr = addr;
    g_lin_maps[slot].len = len;
    g_lin_maps[slot].prot = prot;
    g_lin_maps[slot].flags = flags;
    if (linux_compat32_active() && (flags & 0x20000u))
        g_lin_last_map_stack_top = (uint32_t)(addr + len - 4096u);
    debug_printf("[linux] mmap len=%u prot=0x%x flags=0x%x%s -> 0x%x (slot=%d)\n",
                 (uint32_t)len, (uint32_t)prot, (uint32_t)flags,
                 lazy ? " lazy" : "", (uint32_t)addr, slot);
    return (long)addr;
}

static int do_munmap(uint64_t addr, uint64_t length) {
    if ((addr & 4095u) || length == 0) return -L_EINVAL;
    uint64_t len = (length + 4095) & ~4095ULL;
    for (int i = 0; i < LIN_MAP_MAX; i++) {
        if (!g_lin_maps[i].active) continue;
        if (addr >= g_lin_maps[i].addr &&
            addr < g_lin_maps[i].addr + g_lin_maps[i].len) {
            uint64_t max = g_lin_maps[i].addr + g_lin_maps[i].len - addr;
            if (len > max) len = max;
            break;
        }
    }
    bool found = false;
    for (int i = 0; i < LIN_MAP_MAX; i++) {
        if (!g_lin_maps[i].active) continue;
        if (addr >= g_lin_maps[i].addr &&
            addr + len >= addr &&
            addr + len <= g_lin_maps[i].addr + g_lin_maps[i].len) {
            uint64_t old_end = g_lin_maps[i].addr + g_lin_maps[i].len;
            found = true;
            if (addr == g_lin_maps[i].addr && addr + len == old_end) {
                memset(&g_lin_maps[i], 0, sizeof(g_lin_maps[i]));
            } else if (addr == g_lin_maps[i].addr) {
                g_lin_maps[i].addr += len;
                g_lin_maps[i].len -= len;
            } else if (addr + len == old_end) {
                g_lin_maps[i].len = addr - g_lin_maps[i].addr;
            } else {
                int tail = map_slot();
                if (tail < 0) return -L_ENOMEM;
                g_lin_maps[tail] = g_lin_maps[i];
                g_lin_maps[tail].addr = addr + len;
                g_lin_maps[tail].len = old_end - (addr + len);
                g_lin_maps[i].len = addr - g_lin_maps[i].addr;
            }
            break;
        }
    }
    if (!found) return -L_EINVAL;
    for (uint64_t p = addr; p < addr + len; p += VMM_PAGE_SIZE)
        (void)vmm_unmap_page(g_lin_pd, (uintptr_t)p);
    return 0;
}

/* ---- Miscellaneous ABI helpers ----------------------------------------- */
static long do_readlink(uint64_t upath, uint64_t ubuf, uint64_t cap) {
    if (cap == 0 || !uptr_write_ok(ubuf, cap)) return -L_EFAULT;
    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    int r = copy_user_cstr(upath, path, sizeof(path));
    if (r < 0) return r;
    r = normalize_path(g_lin_cwd, path, canon, sizeof(canon));
    if (r < 0) return r;
    if (strcmp(canon, "/proc/self/exe") != 0) return -L_EINVAL;
    size_t n = strlen(g_lin_exec_path);
    if (n > cap) n = cap;
    memcpy((void *)(uintptr_t)ubuf, g_lin_exec_path, n);
    return (long)n;
}

static int do_access(uint64_t upath) {
    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    int r = copy_user_cstr(upath, path, sizeof(path));
    if (r < 0) return r;
    r = normalize_path(g_lin_cwd, path, canon, sizeof(canon));
    if (r < 0) return r;
    lin_stat_t st;
    return stat_path_kernel(canon, &st);
}

static int do_clock_gettime(uint64_t uts) {
    if (!uptr_write_ok(uts, 16)) return -L_EFAULT;
    uint64_t *ts = (uint64_t *)(uintptr_t)uts;
    uint32_t ms = pit_ms();
    ts[0] = ms / 1000u;
    ts[1] = (uint64_t)(ms % 1000u) * 1000000ull;
    return 0;
}

static int do_nanosleep(uint64_t ureq, uint64_t urem) {
    if (!uptr_ok(ureq, 16)) return -L_EFAULT;
    const uint64_t *ts = (const uint64_t *)(uintptr_t)ureq;
    if (ts[1] >= 1000000000ull || ts[0] > 86400ull) return -L_EINVAL;
    uint64_t ms = ts[0] * 1000ull + (ts[1] + 999999ull) / 1000000ull;
    while (ms) {
        uint32_t chunk = ms > 1000 ? 1000 : (uint32_t)ms;
        pit_sleep(chunk);
        ms -= chunk;
    }
    if (urem && uptr_write_ok(urem, 16)) memset((void *)(uintptr_t)urem, 0, 16);
    return 0;
}

uint32_t lin_compat32_default_thread_stack(void) {
    if (g_lin_last_map_stack_top)
        return g_lin_last_map_stack_top;
    uint64_t base = lin_i386_pick_thread_stack(2u * 1024u * 1024u);
    if (!base)
        return 0;
    if (map_user_pages(base, 2u * 1024u * 1024u,
                       VMM_FLAG_RW | VMM_FLAG_NX) != 0)
        return 0;
    return (uint32_t)(base + 2u * 1024u * 1024u);
}

static int do_ioctl(int fd, uint64_t req, uint64_t arg) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE)
        return -L_EBADF;

    if (g_lin_fds[fd].kind == LFD_DEV_DRI) {
        typedef struct {
            int32_t version_major, version_minor, version_patchlevel;
            uint32_t _pad;
            uint64_t name_len, name;
            uint64_t date_len, date;
            uint64_t desc_len, desc;
        } drm_version_t;
        typedef struct {
            uint64_t capability, value;
        } drm_get_cap_t;

        if ((uint32_t)req == DRM_IOCTL_VERSION) {
            if (!uptr_write_ok(arg, sizeof(drm_version_t))) return -L_EFAULT;
            drm_version_t ver;
            if (user_copy_from(g_lin_pd, &ver, arg, sizeof(ver)) != 0)
                return -L_EFAULT;
            ver.version_major = 1;
            ver.version_minor = 0;
            ver.version_patchlevel = 0;
            static const char kname[] = "nexxon-drm";
            static const char kdate[] = "20260711";
            static const char kdesc[] = "NexxoN headless DRM shim";
            if (ver.name && ver.name_len) {
                uint64_t n = ver.name_len - 1;
                if (n > sizeof(kname)) n = sizeof(kname);
                if (user_copy_to(g_lin_pd, ver.name, kname, n + 1) != 0)
                    return -L_EFAULT;
            }
            if (ver.date && ver.date_len) {
                uint64_t n = ver.date_len - 1;
                if (n > sizeof(kdate)) n = sizeof(kdate);
                if (user_copy_to(g_lin_pd, ver.date, kdate, n + 1) != 0)
                    return -L_EFAULT;
            }
            if (ver.desc && ver.desc_len) {
                uint64_t n = ver.desc_len - 1;
                if (n > sizeof(kdesc)) n = sizeof(kdesc);
                if (user_copy_to(g_lin_pd, ver.desc, kdesc, n + 1) != 0)
                    return -L_EFAULT;
            }
            if (user_copy_to(g_lin_pd, arg, &ver, sizeof(ver)) != 0)
                return -L_EFAULT;
            debug_printf("[linux/dri] DRM_VERSION %d.%d.%d\n",
                         ver.version_major, ver.version_minor,
                         ver.version_patchlevel);
            return 0;
        }
        if ((uint32_t)req == DRM_IOCTL_GET_CAP) {
            if (!uptr_write_ok(arg, sizeof(drm_get_cap_t))) return -L_EFAULT;
            drm_get_cap_t cap;
            if (user_copy_from(g_lin_pd, &cap, arg, sizeof(cap)) != 0)
                return -L_EFAULT;
            cap.value = (cap.capability == DRM_CAP_DUMB_BUFFER) ? 1 : 0;
            if (user_copy_to(g_lin_pd, arg, &cap, sizeof(cap)) != 0)
                return -L_EFAULT;
            debug_printf("[linux/dri] GET_CAP cap=%u -> %u\n",
                         (uint32_t)cap.capability, (uint32_t)cap.value);
            return 0;
        }
        if ((uint32_t)req == DRM_IOCTL_MODE_CREATE_DUMB) {
            typedef struct {
                uint32_t height, width, bpp, flags, handle, pitch;
                uint64_t size;
            } drm_create_dumb_t;
            if (!uptr_write_ok(arg, sizeof(drm_create_dumb_t))) return -L_EFAULT;
            drm_create_dumb_t cre;
            if (user_copy_from(g_lin_pd, &cre, arg, sizeof(cre)) != 0)
                return -L_EFAULT;
            if (!cre.width || !cre.height || !cre.bpp || cre.bpp > 32)
                return -L_EINVAL;
            lin_dumb_t *d = lin_dumb_alloc_slot();
            if (!d) return -L_ENOMEM;
            uint32_t pitch = (cre.width * cre.bpp + 7u) / 8u;
            pitch = (pitch + 63u) & ~63u;
            uint64_t size = (uint64_t)pitch * cre.height;
            uint32_t npages = (uint32_t)((size + VMM_PAGE_SIZE - 1) /
                                         VMM_PAGE_SIZE);
            if (npages == 0 || npages > LIN_DUMB_MAX_PG) return -L_EFBIG;
            memset(d, 0, sizeof(*d));
            for (uint32_t i = 0; i < npages; i++) {
                d->pa[i] = vmm_frame_alloc();
                if (!d->pa[i]) {
                    lin_dumb_destroy_slot(d);
                    return -L_ENOMEM;
                }
                memset((void *)(uintptr_t)d->pa[i], 0, VMM_PAGE_SIZE);
            }
            d->used = true;
            d->dri_fd = fd;
            d->handle = g_lin_dumb_next_handle++;
            d->width = cre.width;
            d->height = cre.height;
            d->bpp = cre.bpp;
            d->flags = cre.flags;
            d->pitch = pitch;
            d->size = size;
            d->npages = npages;
            d->map_off = g_lin_dumb_next_map_off;
            g_lin_dumb_next_map_off += ((size + VMM_PAGE_SIZE - 1) &
                                        ~(uint64_t)(VMM_PAGE_SIZE - 1));
            cre.handle = d->handle;
            cre.pitch = pitch;
            cre.size = size;
            if (user_copy_to(g_lin_pd, arg, &cre, sizeof(cre)) != 0) {
                lin_dumb_destroy_slot(d);
                return -L_EFAULT;
            }
            debug_printf("[linux/dri] CREATE_DUMB %ux%u@%u pitch=%u size=%u h=%u\n",
                         cre.width, cre.height, cre.bpp, cre.pitch,
                         (uint32_t)cre.size, cre.handle);
            return 0;
        }
        if ((uint32_t)req == DRM_IOCTL_MODE_MAP_DUMB) {
            typedef struct {
                uint32_t handle;
                uint32_t pad;
                uint64_t offset;
            } drm_map_dumb_t;
            if (!uptr_write_ok(arg, sizeof(drm_map_dumb_t))) return -L_EFAULT;
            drm_map_dumb_t map;
            if (user_copy_from(g_lin_pd, &map, arg, sizeof(map)) != 0)
                return -L_EFAULT;
            lin_dumb_t *d = lin_dumb_find_handle(fd, map.handle);
            if (!d) return -L_EINVAL;
            map.offset = d->map_off;
            if (user_copy_to(g_lin_pd, arg, &map, sizeof(map)) != 0)
                return -L_EFAULT;
            debug_printf("[linux/dri] MAP_DUMB h=%u -> off=0x%x\n",
                         map.handle, (uint32_t)map.offset);
            return 0;
        }
        if ((uint32_t)req == DRM_IOCTL_MODE_DESTROY_DUMB) {
            typedef struct { uint32_t handle; } drm_destroy_dumb_t;
            if (!uptr_write_ok(arg, sizeof(drm_destroy_dumb_t))) return -L_EFAULT;
            drm_destroy_dumb_t des;
            if (user_copy_from(g_lin_pd, &des, arg, sizeof(des)) != 0)
                return -L_EFAULT;
            lin_dumb_t *d = lin_dumb_find_handle(fd, des.handle);
            if (!d) return -L_EINVAL;
            debug_printf("[linux/dri] DESTROY_DUMB h=%u\n", des.handle);
            lin_dumb_destroy_slot(d);
            return 0;
        }
        return -L_EINVAL;
    }

    if (g_lin_fds[fd].kind != LFD_TTY) return -L_ENOTTY;
    if (req == 0x5413) { /* TIOCGWINSZ */
        if (!uptr_write_ok(arg, 8)) return -L_EFAULT;
        uint16_t *ws = (uint16_t *)(uintptr_t)arg;
        ws[0] = 40; ws[1] = 100; ws[2] = 0; ws[3] = 0;
        return 0;
    }
    if (req == 0x541B) { /* FIONREAD */
        if (!uptr_write_ok(arg, 4)) return -L_EFAULT;
        *(uint32_t *)(uintptr_t)arg = keyboard_has_data() ? 1u : 0u;
        return 0;
    }
    if (req == 0x5401) { /* TCGETS */
        if (!uptr_write_ok(arg, 36)) return -L_EFAULT;
        memset((void *)(uintptr_t)arg, 0, 36);
        *(uint32_t *)((uint8_t *)(uintptr_t)arg + 12) = 0x0000088Bu; /* ICANON|ECHO|ISIG */
        return 0;
    }
    return -L_ENOTTY;
}

typedef struct PACKED { int32_t fd; int16_t events, revents; } lin_pollfd_t;

#define LIN_EPOLLIN  0x001u
#define LIN_EPOLLOUT 0x004u
#define LIN_EPOLLERR 0x008u
#define LIN_EPOLLHUP 0x010u

typedef struct PACKED {
    uint32_t events;
    uint64_t data;
} lin_epoll_event_t;

static int lin_fd_revents(int fd, uint32_t interest) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE)
        return 0x20;
    lin_fd_t *f = &g_lin_fds[fd];
    int rev = 0;
    if (interest & 1) {
        if (f->kind == LFD_NX_FILE || f->kind == LFD_NX_DIR ||
            f->kind == LFD_DEV_ZERO || f->kind == LFD_DEV_NULL)
            rev |= 1;
        else if (f->kind == LFD_TTY && keyboard_has_data())
            rev |= 1;
        else if (f->kind == LFD_SOCK && lin_sock_poll_in(&f->sock))
            rev |= 1;
        else if (f->kind == LFD_EVENTFD && f->event_count > 0)
            rev |= 1;
        else if (f->kind == LFD_PIPE && f->pipe_ix >= 0 &&
                 f->pipe_ix < LIN_PIPE_PAIRS) {
            lin_pipe_pair_t *pp = &g_lin_pipes[f->pipe_ix];
            if (!f->pipe_wr) {
                if (pp->len > 0) rev |= 1;
                if (!pp->w_open && pp->len == 0)
                    rev |= (int)(LIN_EPOLLIN | LIN_EPOLLHUP);
            }
        }
    }
    if (interest & 4) {
        if (f->kind == LFD_TTY || f->kind == LFD_DEV_NULL ||
            f->kind == LFD_EVENTFD ||
            (f->kind == LFD_NX_FILE && f->write_stream) ||
            (f->kind == LFD_SOCK && lin_sock_poll_out(&f->sock)))
            rev |= 4;
        else if (f->kind == LFD_PIPE && f->pipe_ix >= 0 &&
                 f->pipe_ix < LIN_PIPE_PAIRS) {
            lin_pipe_pair_t *pp = &g_lin_pipes[f->pipe_ix];
            if (f->pipe_wr) {
                if (pp->w_open && pp->len < LIN_PIPE_BUF) rev |= 4;
                if (!pp->r_open) rev |= (int)(LIN_EPOLLOUT | LIN_EPOLLHUP);
            }
        }
    }
    return rev;
}

static int poll_scan(lin_pollfd_t *p, uint64_t nfds) {
    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        p[i].revents = 0;
        int fd = p[i].fd;
        if (fd < 0) continue;
        int rev = lin_fd_revents(fd, (uint32_t)p[i].events);
        if (rev & 0x20) {
            p[i].revents = 0x20;
            ready++;
            continue;
        }
        p[i].revents = (int16_t)(rev & p[i].events);
        if (p[i].revents) ready++;
    }
    return ready;
}

static long do_poll(uint64_t ufds, uint64_t nfds, int timeout) {
    if (nfds > LIN_FD_MAX ||
        !uptr_write_ok(ufds, nfds * sizeof(lin_pollfd_t)))
        return -L_EFAULT;
    lin_pollfd_t *p = (lin_pollfd_t *)(uintptr_t)ufds;
    int ready = poll_scan(p, nfds);
    if (!ready && timeout > 0) {
        pit_sleep((uint32_t)timeout);
        ready = poll_scan(p, nfds);
    }
    return ready;
}

#define LIN_EPOLL_CTL_ADD 1
#define LIN_EPOLL_CTL_DEL 2
#define LIN_EPOLL_CTL_MOD 3

static long do_pipe2(uint64_t ufd, int flags) {
    (void)flags; /* O_NONBLOCK/O_CLOEXEC ignored for now */
    if (!uptr_write_ok(ufd, 8)) return -L_EFAULT;
    int pair = -1;
    for (int i = 0; i < LIN_PIPE_PAIRS; i++) {
        if (!g_lin_pipes[i].r_open && !g_lin_pipes[i].w_open) {
            pair = i;
            break;
        }
    }
    if (pair < 0) return -L_EMFILE;
    int rfd = fd_alloc_from(3);
    if (rfd < 0) return rfd;
    int wfd = fd_alloc_from(rfd + 1);
    if (wfd < 0) {
        memset(&g_lin_fds[rfd], 0, sizeof(g_lin_fds[rfd]));
        return wfd;
    }
    lin_pipe_pair_t *pp = &g_lin_pipes[pair];
    memset(pp, 0, sizeof(*pp));
    pp->r_open = pp->w_open = true;
    pp->r_fd = rfd;
    pp->w_fd = wfd;

    g_lin_fds[rfd].kind = LFD_PIPE;
    g_lin_fds[rfd].flags = L_O_RDONLY;
    g_lin_fds[rfd].pipe_ix = (int16_t)pair;
    g_lin_fds[rfd].pipe_wr = false;
    strcpy(g_lin_fds[rfd].path, "pipe:r");

    g_lin_fds[wfd].kind = LFD_PIPE;
    g_lin_fds[wfd].flags = L_O_WRONLY;
    g_lin_fds[wfd].pipe_ix = (int16_t)pair;
    g_lin_fds[wfd].pipe_wr = true;
    strcpy(g_lin_fds[wfd].path, "pipe:w");

    uint32_t out[2] = { (uint32_t)rfd, (uint32_t)wfd };
    if (user_copy_to(g_lin_pd, ufd, out, 8) != 0) {
        (void)fd_close(wfd, true);
        (void)fd_close(rfd, true);
        return -L_EFAULT;
    }
    debug_printf("[linux] pipe2 -> %d %d\n", rfd, wfd);
    return 0;
}

static long do_epoll_create1(int flags) {
    (void)flags;
    int fd = fd_alloc_from(3);
    if (fd < 0) return fd;
    g_lin_fds[fd].kind = LFD_EPOLL;
    g_lin_fds[fd].flags = L_O_RDWR;
    g_lin_epoll_cnt[fd] = 0;
    strcpy(g_lin_fds[fd].path, "epoll");
    return fd;
}

static int epoll_find_slot(int epfd, int target) {
    for (int i = 0; i < g_lin_epoll_cnt[epfd]; i++)
        if (g_lin_epoll_tab[epfd][i].fd == target) return i;
    return -1;
}

static long do_epoll_ctl(int epfd, int op, int fd, uint64_t uev) {
    if (epfd < 0 || epfd >= LIN_FD_MAX ||
        g_lin_fds[epfd].kind != LFD_EPOLL)
        return -L_EBADF;
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE)
        return -L_EBADF;
    if (op != LIN_EPOLL_CTL_DEL &&
        !uptr_ok(uev, sizeof(lin_epoll_event_t)))
        return -L_EFAULT;

    lin_epoll_event_t ev;
    if (op != LIN_EPOLL_CTL_DEL) {
        if (user_copy_from(g_lin_pd, &ev, uev,
                           sizeof(lin_epoll_event_t)) != 0)
            return -L_EFAULT;
    }

    int slot = epoll_find_slot(epfd, fd);
    if (op == LIN_EPOLL_CTL_ADD) {
        if (slot >= 0) return -L_EEXIST;
        if (g_lin_epoll_cnt[epfd] >= LIN_EPOLL_MAX_ENT) return -L_ENOMEM;
        slot = g_lin_epoll_cnt[epfd]++;
        g_lin_epoll_tab[epfd][slot].fd = fd;
        g_lin_epoll_tab[epfd][slot].events = ev.events;
        g_lin_epoll_tab[epfd][slot].data = ev.data;
        return 0;
    }
    if (op == LIN_EPOLL_CTL_DEL) {
        if (slot < 0) return -L_ENOENT;
        int last = --g_lin_epoll_cnt[epfd];
        if (slot != last)
            g_lin_epoll_tab[epfd][slot] = g_lin_epoll_tab[epfd][last];
        return 0;
    }
    if (op == LIN_EPOLL_CTL_MOD) {
        if (slot < 0) return -L_ENOENT;
        g_lin_epoll_tab[epfd][slot].events = ev.events;
        g_lin_epoll_tab[epfd][slot].data = ev.data;
        return 0;
    }
    return -L_EINVAL;
}

static int epoll_collect(int epfd, lin_epoll_event_t *out, int maxev) {
    int n = 0;
    for (int i = 0; i < g_lin_epoll_cnt[epfd] && n < maxev; i++) {
        lin_epoll_ent_t *ent = &g_lin_epoll_tab[epfd][i];
        int rev = lin_fd_revents(ent->fd, ent->events);
        if (rev & 0x20) {
            out[n].events = LIN_EPOLLERR | LIN_EPOLLHUP;
            out[n].data = ent->data;
            n++;
            continue;
        }
        if (rev & (int)ent->events) {
            out[n].events = (uint32_t)(rev & (int)ent->events);
            out[n].data = ent->data;
            n++;
        }
    }
    return n;
}

static long do_epoll_wait(int epfd, uint64_t uevents, int maxev, int timeout,
                          lin_regs_t *r) {
    if (epfd < 0 || epfd >= LIN_FD_MAX ||
        g_lin_fds[epfd].kind != LFD_EPOLL)
        return -L_EBADF;
    if (maxev <= 0) return -L_EINVAL;
    if (!uptr_write_ok(uevents, (uint64_t)maxev * sizeof(lin_epoll_event_t)))
        return -L_EFAULT;

    lin_epoll_event_t stack[LIN_EPOLL_MAX_ENT];
    if (maxev > LIN_EPOLL_MAX_ENT) maxev = LIN_EPOLL_MAX_ENT;

    uint32_t deadline = 0;
    if (timeout > 0) deadline = pit_ms() + (uint32_t)timeout;

    for (;;) {
        int n = epoll_collect(epfd, stack, maxev);
        if (n > 0) {
            if (user_copy_to(g_lin_pd, uevents, stack,
                             (uint64_t)n * sizeof(lin_epoll_event_t)) != 0)
                return -L_EFAULT;
            debug_printf("[linux/epoll] wait epfd=%d -> %d events\n", epfd, n);
            return n;
        }
        if (timeout == 0) return 0;
        if (timeout > 0 && (int32_t)(pit_ms() - deadline) >= 0) return 0;
        if (lin_thread_session_active() && g_lin_epoll_cnt[epfd] > 0) {
            bool switched = false;
            lin_thread_yield(r, &switched);
            if (switched) return LIN_THREAD_SWITCHED;
        }
        __asm__ volatile("sti; hlt");
    }
}

static long sock_errno(long rc) {
    if (rc >= 0) return rc;
    switch (rc) {
    case -11: return -L_EAGAIN;
    case -22: return -L_EINVAL;
    case -110: return -L_ETIMEDOUT;
    case -111: return -L_ECONNREFUSED;
    case -24: return -L_EMFILE;
    case -95: return -L_EOPNOTSUPP;
    case -97: return -L_EAFNOSUPPORT;
    default: return -L_EIO;
    }
}

static long do_socket(int domain, int type, int protocol) {
    lin_sock_t tmp;
    lin_sock_reset(&tmp);
    long rc = lin_sock_create(&tmp, domain, type, protocol);
    if (rc < 0) return sock_errno(rc);
    int fd = fd_alloc_from(3);
    if (fd < 0) return fd;
    g_lin_fds[fd].kind = LFD_SOCK;
    g_lin_fds[fd].flags = L_O_RDWR;
    g_lin_fds[fd].sock = tmp;
    ksnprintf(g_lin_fds[fd].path, sizeof(g_lin_fds[fd].path),
              "socket:%d", type);
    return fd;
}

static long do_sock_connect(int fd, uint64_t addr, int addrlen) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind != LFD_SOCK)
        return -L_EBADF;
    if (!uptr_ok(addr, (uint64_t)addrlen)) return -L_EFAULT;
    uint8_t sa[128];
    if (addrlen > (int)sizeof(sa)) return -L_EINVAL;
    if (copy_user_range((uint64_t)(uintptr_t)sa, addr, (uint64_t)addrlen) != 0)
        return -L_EFAULT;
    return sock_errno(lin_sock_connect(&g_lin_fds[fd].sock, sa, addrlen));
}

static long do_sock_bind(int fd, uint64_t addr, int addrlen) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind != LFD_SOCK)
        return -L_EBADF;
    if (!uptr_ok(addr, (uint64_t)addrlen)) return -L_EFAULT;
    uint8_t sa[128];
    if (addrlen > (int)sizeof(sa)) return -L_EINVAL;
    if (copy_user_range((uint64_t)(uintptr_t)sa, addr, (uint64_t)addrlen) != 0)
        return -L_EFAULT;
    return sock_errno(lin_sock_bind(&g_lin_fds[fd].sock, sa, addrlen));
}

static long do_sock_listen(int fd, int backlog) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind != LFD_SOCK)
        return -L_EBADF;
    return sock_errno(lin_sock_listen(&g_lin_fds[fd].sock, backlog));
}

static long do_sock_accept(int fd, uint64_t addr, uint64_t addrlen) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind != LFD_SOCK)
        return -L_EBADF;
    lin_sock_t tmp;
    lin_sock_reset(&tmp);
    uint8_t sa[128];
    int salen = sizeof(sa);
    long rc = lin_sock_accept(&g_lin_fds[fd].sock, &tmp, addr ? sa : NULL,
                              addr ? &salen : NULL);
    if (rc < 0) return sock_errno(rc);
    int nfd = fd_alloc_from(3);
    if (nfd < 0) return nfd;
    g_lin_fds[nfd].kind = LFD_SOCK;
    g_lin_fds[nfd].flags = L_O_RDWR;
    g_lin_fds[nfd].sock = tmp;
    ksnprintf(g_lin_fds[nfd].path, sizeof(g_lin_fds[nfd].path),
              "socket:unix");
    if (addr && addrlen) {
        if (!uptr_write_ok(addrlen, 4)) return -L_EFAULT;
        if (!uptr_write_ok(addr, (uint64_t)salen)) return -L_EFAULT;
        if (copy_user_range(addr, (uint64_t)(uintptr_t)sa, (uint32_t)salen) != 0)
            return -L_EFAULT;
        *(int *)(uintptr_t)addrlen = salen;
    }
    return nfd;
}

static long do_sock_getsockname(int fd, uint64_t addr, uint64_t addrlen) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind != LFD_SOCK)
        return -L_EBADF;
    if (!addr || !addrlen) return -L_EINVAL;
    if (!uptr_write_ok(addrlen, 4)) return -L_EFAULT;
    uint8_t sa[128];
    int salen = sizeof(sa);
    long rc = lin_sock_getsockname(&g_lin_fds[fd].sock, sa, &salen);
    if (rc < 0) return sock_errno(rc);
    if (!uptr_write_ok(addr, (uint64_t)salen)) return -L_EFAULT;
    if (copy_user_range(addr, (uint64_t)(uintptr_t)sa, (uint32_t)salen) != 0)
        return -L_EFAULT;
    *(int *)(uintptr_t)addrlen = salen;
    return 0;
}

static long do_sendto(int fd, uint64_t buf, uint64_t len, int flags,
                      uint64_t addr, int addrlen) {
    (void)flags;
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind != LFD_SOCK)
        return -L_EBADF;
    if (!uptr_ok(buf, len)) return -L_EFAULT;
    uint8_t sa[128];
    const void *sap = NULL;
    if (addr) {
        if (addrlen > (int)sizeof(sa)) return -L_EINVAL;
        if (!uptr_ok(addr, (uint64_t)addrlen)) return -L_EFAULT;
        if (copy_user_range((uint64_t)(uintptr_t)sa, addr, (uint64_t)addrlen) != 0)
            return -L_EFAULT;
        sap = sa;
    }
    uint8_t stack[4096];
    uint64_t left = len;
    uint64_t off = 0;
    long total = 0;
    while (left) {
        uint32_t chunk = left > sizeof(stack) ? (uint32_t)sizeof(stack)
                                              : (uint32_t)left;
        if (copy_user_range((uint64_t)(uintptr_t)stack, buf + off, chunk) != 0)
            return total > 0 ? total : -L_EFAULT;
        long rc = lin_sock_sendto(&g_lin_fds[fd].sock, stack, chunk,
                                  sap, sap ? addrlen : 0);
        if (rc < 0) return total > 0 ? total : sock_errno(rc);
        total += rc;
        off += (uint64_t)rc;
        left -= (uint64_t)rc;
        if ((uint64_t)rc < chunk) break;
    }
    return total;
}

static long do_recvfrom(int fd, uint64_t buf, uint64_t len, int flags,
                        uint64_t addr, uint64_t addrlen) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind != LFD_SOCK)
        return -L_EBADF;
    if (!uptr_write_ok(buf, len)) return -L_EFAULT;
    bool nb = (flags & 0x40) != 0 ||
              (g_lin_fds[fd].flags & L_O_NONBLOCK) != 0;
    uint8_t stack[4096];
    uint32_t chunk = len > sizeof(stack) ? (uint32_t)sizeof(stack) : (uint32_t)len;
    uint8_t sa[128];
    int salen = sizeof(sa);
    long rc = lin_sock_recvfrom(&g_lin_fds[fd].sock, stack, chunk,
                                addr ? sa : NULL, addr ? &salen : NULL, nb);
    if (rc < 0) return sock_errno(rc);
    if (copy_user_range(buf, (uint64_t)(uintptr_t)stack, (uint32_t)rc) != 0)
        return -L_EFAULT;
    if (addr && addrlen) {
        if (!uptr_write_ok(addrlen, 4)) return -L_EFAULT;
        if (!uptr_write_ok(addr, (uint64_t)salen)) return -L_EFAULT;
        if (copy_user_range(addr, (uint64_t)(uintptr_t)sa, (uint32_t)salen) != 0)
            return -L_EFAULT;
        *(int *)(uintptr_t)addrlen = salen;
    }
    return rc;
}

static long do_socketpair(int domain, int type, int protocol, uint64_t ufd) {
    (void)protocol;
    if (domain != LIN_AF_UNIX || type != LIN_SOCK_STREAM)
        return -L_EAFNOSUPPORT;
    if (!uptr_write_ok(ufd, 8))
        return -L_EFAULT;
    lin_sock_t sa, sb;
    long rc = lin_unix_socketpair(&sa, &sb);
    if (rc < 0)
        return sock_errno(rc);
    int fd0 = fd_alloc_from(3);
    if (fd0 < 0)
        return fd0;
    int fd1 = fd_alloc_from(fd0 + 1);
    if (fd1 < 0) {
        memset(&g_lin_fds[fd0], 0, sizeof(g_lin_fds[fd0]));
        return fd1;
    }
    g_lin_fds[fd0].kind = LFD_SOCK;
    g_lin_fds[fd0].flags = L_O_RDWR;
    g_lin_fds[fd0].sock = sa;
    strcpy(g_lin_fds[fd0].path, "socketpair:0");
    g_lin_fds[fd1].kind = LFD_SOCK;
    g_lin_fds[fd1].flags = L_O_RDWR;
    g_lin_fds[fd1].sock = sb;
    strcpy(g_lin_fds[fd1].path, "socketpair:1");
    uint32_t out[2] = { (uint32_t)fd0, (uint32_t)fd1 };
    if (user_copy_to(g_lin_pd, ufd, out, 8) != 0) {
        (void)fd_close(fd1, true);
        (void)fd_close(fd0, true);
        return -L_EFAULT;
    }
    debug_printf("[linux] socketpair -> %d %d\n", fd0, fd1);
    return 0;
}

/* ---- Process helpers (fork/exec) ------------------------------------- */
static long lin_do_fork(lin_regs_t *r);
static void lin_fork_child_exit(lin_regs_t *r, int code);
static void lin_steam_ensure_cdn(void);
static long lin_do_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp,
                            lin_regs_t *r);
static long lin_apt_install_user(uint64_t op, uint64_t upkg);
static long lin_do_run_sync(uint64_t upath, uint64_t uargv);
static long lin_wine_run_user(uint64_t upath, lin_regs_t *r);
static uint32_t lin_usr_bin_dir(void);
static uint32_t lin_programs_dir(void);
static bool linux_install_file(uint32_t dir, const char *name,
                               const uint8_t *start, const uint8_t *end);
static int linux_read_nxfs(const char *path, uint8_t *buf, uint32_t cap,
                           uint32_t *out_len);
static void lin_clear_user_maps(void);
static int lin_count_user_argv(uint64_t uargv);
static int lin_copy_user_argv(uint64_t uargv, int argc,
                              const char *argv[LIN_MAX_ARGS],
                              char storage[LIN_MAX_ARGS][LIN_PATH_MAX]);
static long lin_map_program(const uint8_t *img, uint32_t len,
                            const char *exec_path, int argc,
                            const char *const *argv,
                            uint64_t *entry_out, uint64_t *rsp_out);

/* ---- Internet apt (HTTP registry at 10.0.2.2 via QEMU slirp) ---------- */
enum { LIN_PKG_PROGRAMS = 0, LIN_PKG_USR_BIN = 1 };

#define LIN_APT_INDEX_MAX  (32u * 1024u)
#define LIN_APT_DL_MAX     (256u * 1024u)

static uint8_t  g_apt_index_buf[LIN_APT_INDEX_MAX];
static uint32_t g_apt_index_len;
static bool     g_apt_index_valid;

static uint8_t  g_apt_dl_buf[LIN_APT_DL_MAX];

static void lin_apt_build_url(char *url, uint32_t cap, const char *suffix) {
    if (!url || cap < 2) return;
    uint32_t n = 0;
    const char *reg = NEXXON_APT_REGISTRY_URL;
    while (reg[n] && n + 1 < cap) { url[n] = reg[n]; n++; }
    if (suffix && suffix[0]) {
        uint32_t k = 0;
        while (suffix[k] && n + 1 < cap) { url[n++] = suffix[k++]; }
    }
    url[n] = 0;
}

static bool lin_apt_index_has_pkg(const char *pkg) {
    if (!g_apt_index_valid || !pkg || !pkg[0]) return false;
    char needle[128];
    ksnprintf(needle, sizeof(needle), "Package: %s", pkg);
    size_t nlen = strlen(needle);
    const char *p = (const char *)g_apt_index_buf;
    const char *end = p + g_apt_index_len;
    while (p + nlen <= end) {
        if (memcmp(p, needle, nlen) == 0 &&
            (p[nlen] == '\n' || p[nlen] == '\r' || p[nlen] == 0))
            return true;
        p++;
    }
    return false;
}

static long lin_apt_update(void) {
    char url[256];
    lin_apt_build_url(url, sizeof(url), "Packages");
    int got = download_simple(url, g_apt_index_buf, sizeof(g_apt_index_buf));
    if (got <= 0) {
        debug_printf("[linux] apt update: download failed\n");
        return -L_EIO;
    }
    g_apt_index_len = (uint32_t)got;
    g_apt_index_valid = true;
        debug_printf("[linux] apt update: fetched Packages (%u bytes) "
                     "NEXXON_APT_DNS_OK\n", g_apt_index_len);
    return 0;
}

static int lin_apt_download_retry(const char *url_name) {
    char url[256];
    lin_apt_build_url(url, sizeof(url), url_name);
    int got = -1;
    for (int attempt = 0; attempt < 4 && got <= 0; attempt++) {
        download_request_t req = { {0}, g_lin_file_image, LIN_IMAGE_BYTES,
                                   120000u };
        download_result_t res;
        int n = 0;
        while (url[n] && n < DOWNLOAD_MAX_URL - 1) {
            req.url[n] = url[n];
            n++;
        }
        got = download_fetch(&req, &res);
        if (got <= 0 && attempt < 3) {
            uint32_t delay = 1000u << attempt;
            debug_printf("[linux] apt fetch retry %s in %u ms\n",
                         url_name, delay);
            pit_sleep(delay);
        }
    }
    return got;
}

static long lin_apt_fetch_into_dir(uint32_t dir, const char *url_name,
                                   const char *dest_name) {
    int got = lin_apt_download_retry(url_name);
    if (got <= 0) {
        debug_printf("[linux] apt fetch failed '%s'\n", url_name);
        return -L_EIO;
    }
    if (!linux_install_file(dir, dest_name, g_lin_file_image,
                            g_lin_file_image + got))
        return -L_EIO;
    debug_printf("[linux] apt fetched %s -> %u bytes\n",
                 dest_name, (uint32_t)got);
    return 0;
}

static uint32_t lin_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static long lin_apt_install_bundle(uint32_t dir, const char *url_name,
                                   const char magic[8]) {
    int got = lin_apt_download_retry(url_name);
    if (got < 12 || memcmp(g_lin_file_image, magic, 8) != 0)
        return -L_EIO;
    uint32_t count = lin_read_le32(g_lin_file_image + 8);
    if (count == 0 || count > 16)
        return -L_EIO;
    uint32_t off = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (off + 8 > (uint32_t)got)
            return -L_EIO;
        uint32_t name_len = lin_read_le32(g_lin_file_image + off);
        uint32_t data_len = lin_read_le32(g_lin_file_image + off + 4);
        off += 8;
        if (name_len == 0 || name_len >= 64 ||
            off + name_len > (uint32_t)got ||
            data_len > (uint32_t)got - off - name_len)
            return -L_EIO;
        char name[64];
        memcpy(name, g_lin_file_image + off, name_len);
        name[name_len] = 0;
        off += name_len;
        if (!linux_install_file(dir, name, g_lin_file_image + off,
                                g_lin_file_image + off + data_len))
            return -L_EIO;
        debug_printf("[linux] apt bundle installed %s -> %u bytes\n",
                     name, data_len);
        off += data_len;
    }
    return 0;
}

static long lin_apt_install_blob(const char *name, const uint8_t *data,
                                 uint32_t len, int dest_kind) {
    if (!name || !data || len == 0) return -L_EINVAL;
    uint32_t dir = 0;
    if (dest_kind == LIN_PKG_PROGRAMS)
        dir = lin_programs_dir();
    else if (dest_kind == LIN_PKG_USR_BIN)
        dir = lin_usr_bin_dir();
    if (!dir) return -L_ENOSPC;
    if (!linux_install_file(dir, name, data, data + len))
        return -L_EIO;
    debug_printf("[linux] apt installed %s -> %u bytes\n", name, len);
    return 0;
}

static long lin_apt_install_net(const char *pkg) {
    if (!pkg || !pkg[0]) return -L_EINVAL;
    if (g_apt_index_valid && !lin_apt_index_has_pkg(pkg)) {
        debug_printf("[linux] apt net: '%s' not in Packages index\n", pkg);
        return -L_ENOENT;
    }
    char url[256];
    lin_apt_build_url(url, sizeof(url), pkg);
    int got = download_simple(url, g_apt_dl_buf, sizeof(g_apt_dl_buf));
    if (got <= 0) {
        debug_printf("[linux] apt net: download failed for '%s'\n", pkg);
        return -L_EIO;
    }
    debug_printf("[linux] apt net: fetched '%s' (%d bytes)\n", pkg, got);
    return lin_apt_install_blob(pkg, g_apt_dl_buf, (uint32_t)got,
                                LIN_PKG_PROGRAMS);
}

/* ---- Synchronous subprocess runner (no fork) --------------------------- */
typedef struct {
    vmm_pd_t *pd;
    uint64_t brk, brk_floor, load_lo, load_hi;
    char     exec_path[LIN_PATH_MAX];
    char     cwd[LIN_PATH_MAX];
    uint32_t cwd_ino, image_size;
    lin_fd_t fds[LIN_FD_MAX];
    int      write_fd;
    lin_map_t maps[LIN_MAP_MAX];
    char     tty_line[256];
    size_t   tty_line_len, tty_line_pos;
    lin_pid_t pid, ppid;
    bool     fork_child, fork_child_done, compat32;
    lin_pid_t fork_parent_pid, fork_child_pid;
    int      fork_child_status;
    vmm_pd_t *fork_parent_pd;
    lin_regs_t regs;
    uint64_t fs_base, gs_base;
} lin_saved_t;

static lin_saved_t g_lin_sync_saved;
static char g_lin_sync_child_path[LIN_PATH_MAX];

static void lin_save_state(lin_saved_t *s, const lin_regs_t *r) {
    memset(s, 0, sizeof(*s));
    s->pd = g_lin_pd;
    s->brk = g_lin_brk;
    s->brk_floor = g_lin_brk_floor;
    s->load_lo = g_lin_load_lo;
    s->load_hi = g_lin_load_hi;
    strncpy(s->exec_path, g_lin_exec_path, sizeof(s->exec_path) - 1);
    strncpy(s->cwd, g_lin_cwd, sizeof(s->cwd) - 1);
    s->cwd_ino = g_lin_cwd_ino;
    s->image_size = g_lin_image_size;
    memcpy(s->fds, g_lin_fds, sizeof(s->fds));
    s->write_fd = g_lin_write_fd;
    memcpy(s->maps, g_lin_maps, sizeof(s->maps));
    memcpy(s->tty_line, g_lin_tty_line, sizeof(s->tty_line));
    s->tty_line_len = g_lin_tty_line_len;
    s->tty_line_pos = g_lin_tty_line_pos;
    s->pid = g_lin_pid;
    s->ppid = g_lin_ppid;
    s->fork_child = g_lin_fork_child;
    s->fork_child_done = g_lin_fork_child_done;
    s->compat32 = g_lin_compat32;
    s->fork_parent_pid = g_lin_fork_parent_pid;
    s->fork_child_pid = g_lin_fork_child_pid;
    s->fork_child_status = g_lin_fork_child_status;
    s->fork_parent_pd = g_lin_fork_parent_pd;
    s->fs_base = rdmsr(MSR_FS_BASE);
    s->gs_base = rdmsr(MSR_GS_BASE);
    if (r) s->regs = *r;
}

static void lin_restore_state(const lin_saved_t *s, lin_regs_t *r) {
    if (g_lin_pd && g_lin_pd != s->pd) {
        vmm_release(g_lin_pd);
    }
    g_lin_pd = s->pd;
    sched_set_current_pd(g_lin_pd);
    vmm_switch(g_lin_pd);
    g_lin_brk = s->brk;
    g_lin_brk_floor = s->brk_floor;
    g_lin_load_lo = s->load_lo;
    g_lin_load_hi = s->load_hi;
    strncpy(g_lin_exec_path, s->exec_path, sizeof(g_lin_exec_path) - 1);
    strncpy(g_lin_cwd, s->cwd, sizeof(g_lin_cwd) - 1);
    g_lin_cwd_ino = s->cwd_ino;
    g_lin_image_size = s->image_size;
    memcpy(g_lin_fds, s->fds, sizeof(g_lin_fds));
    g_lin_write_fd = s->write_fd;
    memcpy(g_lin_maps, s->maps, sizeof(g_lin_maps));
    memcpy(g_lin_tty_line, s->tty_line, sizeof(g_lin_tty_line));
    g_lin_tty_line_len = s->tty_line_len;
    g_lin_tty_line_pos = s->tty_line_pos;
    g_lin_pid = s->pid;
    g_lin_ppid = s->ppid;
    g_lin_fork_child = s->fork_child;
    g_lin_fork_child_done = s->fork_child_done;
    g_lin_compat32 = s->compat32;
    g_lin_fork_parent_pid = s->fork_parent_pid;
    g_lin_fork_child_pid = s->fork_child_pid;
    g_lin_fork_child_status = s->fork_child_status;
    g_lin_fork_parent_pd = s->fork_parent_pd;
    wrmsr(MSR_FS_BASE, s->fs_base);
    wrmsr(MSR_GS_BASE, s->gs_base);
    if (r) *r = s->regs;
}

static void lin_sync_child_exit(int code) {
    g_lin_sync_exit_code = code;
    g_lin_sync_faulted = false;
    g_lin_sync_running = false;
    lin_thread_reset();
    if (g_lin_compat32)
        gdt_set_kernel_stack((uintptr_t)(g_lin_kstack + sizeof(g_lin_kstack)));
    if (g_lin_sync_dispatch_active)
        longjmp(g_lin_sync_dispatch_jmp, 1);
}

bool linux_compat32_active(void) {
    return g_lin_compat32 && (g_lin_sync_running || g_lin_running);
}

static int32_t lin_compat32_iov(int fd, uint32_t uiov, uint32_t count,
                                bool write) {
    if (count > 1024 || !uptr_ok(uiov, (uint64_t)count * 8))
        return -L_EFAULT;
    int32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t pair[2];
        if (user_copy_from(g_lin_pd, pair, uiov + i * 8, 8) != 0)
            return total ? total : -L_EFAULT;
        if (write && fd == 2 && pair[0] && pair[1]) {
            char diag[192];
            uint32_t dn = pair[1] < sizeof(diag) - 1
                        ? pair[1] : (uint32_t)sizeof(diag) - 1;
            if (user_copy_from(g_lin_pd, diag, pair[0], dn) == 0) {
                diag[dn] = 0;
                debug_printf("[linux/i386] stderr: %s", diag);
            }
        }
        long n = write
            ? do_write(fd, (const void *)(uintptr_t)pair[0], pair[1])
            : do_read(fd, (void *)(uintptr_t)pair[0], pair[1]);
        if (n < 0) return total ? total : (int32_t)n;
        total += (int32_t)n;
        if ((uint32_t)n < pair[1]) break;
    }
    return total;
}

static int32_t lin_compat32_put_stat64(uint32_t ubuf, uint32_t mode,
                                      uint64_t size, uint64_t ino) {
    if (!uptr_write_ok(ubuf, 96))
        return -L_EFAULT;
    uint8_t st[96];
    memset(st, 0, sizeof(st));
    *(uint64_t *)(void *)(st + 0) = 1;
    *(uint32_t *)(void *)(st + 12) = (uint32_t)ino;
    *(uint32_t *)(void *)(st + 16) = mode;
    *(uint32_t *)(void *)(st + 20) = 1;
    *(uint64_t *)(void *)(st + 44) = size;
    *(uint32_t *)(void *)(st + 52) = NXFS_BLOCK_BYTES;
    *(uint64_t *)(void *)(st + 56) = (size + 511) / 512;
    *(uint64_t *)(void *)(st + 88) = ino;
    return user_copy_to(g_lin_pd, ubuf, st, sizeof(st)) == 0
        ? 0 : -L_EFAULT;
}

static int32_t lin_compat32_fstat64(int fd, uint32_t ubuf) {
    if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE)
        return -L_EBADF;
    uint32_t mode = L_S_IFREG | 0755u;
    uint64_t size = g_lin_fds[fd].size;
    if (g_lin_fds[fd].kind == LFD_NX_DIR)
        mode = L_S_IFDIR | 0755u;
    else if (g_lin_fds[fd].kind != LFD_NX_FILE)
        mode = L_S_IFCHR | 0666u;
    return lin_compat32_put_stat64(ubuf, mode, size,
                                   (uint64_t)g_lin_fds[fd].inode + 1);
}

static int32_t lin_compat32_stat64(int dirfd, uint32_t upath,
                                   uint32_t ubuf, uint32_t flags) {
    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    int r = copy_user_cstr(upath, path, sizeof(path));
    if (r < 0) return r;
    if (path[0] == 0 && (flags & 0x1000u))
        return lin_compat32_fstat64(dirfd, ubuf);
    const char *base = "/";
    if (path[0] != '/') {
        r = fd_base_path(dirfd, &base);
        if (r < 0) return r;
    }
    r = normalize_path(base, path, canon, sizeof(canon));
    if (r < 0) return r;
    lin_stat_t st;
    r = stat_path_kernel(canon, &st);
    if (r < 0) return r;
    return lin_compat32_put_stat64(ubuf, st.st_mode, st.st_size, st.st_ino);
}

static int32_t lin_compat32_mkdir(uint32_t upath) {
    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    int r = copy_user_cstr(upath, path, sizeof(path));
    if (r < 0) return r;
    r = normalize_path(g_lin_cwd, path, canon, sizeof(canon));
    if (r < 0) return r;
    uint32_t existing;
    if (resolve_nx_path(canon, &existing) == 0)
        return -L_EEXIST;
    char *slash = NULL;
    for (char *p = canon; *p; p++)
        if (*p == '/') slash = p;
    if (!slash || !slash[1]) return -L_EINVAL;
    char name[LIN_PATH_MAX];
    strcpy(name, slash + 1);
    if (slash == canon)
        slash[1] = 0;
    else
        *slash = 0;
    uint32_t parent = 0;
    if (strcmp(canon, "/") != 0 && resolve_nx_path(canon, &parent) < 0)
        return -L_ENOENT;
    uint32_t ino = 0;
    int cr = nxfs_create_dir(parent, name, &ino);
    if (cr == NXFS_OK) return 0;
    debug_printf("[linux/i386] mkdir '%s/%s' parent=%u nxfs_rc=%d\n",
                 canon, name, parent, cr);
    if (cr == NXFS_ERR_EXISTS) return -L_EEXIST;
    if (cr == NXFS_ERR_NOSPACE || cr == NXFS_ERR_FULL) return -L_ENOSPC;
    return -L_EIO;
}

static int32_t lin_compat32_chdir(uint32_t upath) {
    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    int r = copy_user_cstr(upath, path, sizeof(path));
    if (r < 0) return r;
    r = normalize_path(g_lin_cwd, path, canon, sizeof(canon));
    if (r < 0) return r;
    uint32_t ino = 0;
    nxfs_inode_t node;
    if (resolve_nx_path(canon, &ino) < 0 ||
        nxfs_read_inode(ino, &node) != NXFS_OK ||
        node.type != NXFS_TYPE_DIR) {
        debug_printf("[linux/i386] chdir failed '%s'\n", canon);
        return -L_ENOTDIR;
    }
    strcpy(g_lin_cwd, canon);
    g_lin_cwd_ino = ino;
    debug_printf("[linux/i386] chdir '%s'\n", canon);
    return 0;
}

static int lin_compat32_split_path(uint32_t upath, char parent[LIN_PATH_MAX],
                                   char name[LIN_PATH_MAX]) {
    char path[LIN_PATH_MAX];
    int r = copy_user_cstr(upath, path, sizeof(path));
    if (r < 0) return r;
    r = normalize_path(g_lin_cwd, path, parent, LIN_PATH_MAX);
    if (r < 0) return r;
    char *slash = NULL;
    for (char *p = parent; *p; p++)
        if (*p == '/') slash = p;
    if (!slash || !slash[1]) return -L_EINVAL;
    strcpy(name, slash + 1);
    if (slash == parent)
        slash[1] = 0;
    else
        *slash = 0;
    return 0;
}

static int32_t lin_compat32_rename(uint32_t uold, uint32_t unew) {
    char old_parent[LIN_PATH_MAX], new_parent[LIN_PATH_MAX];
    char old_name[LIN_PATH_MAX], new_name[LIN_PATH_MAX];
    int r = lin_compat32_split_path(uold, old_parent, old_name);
    if (r < 0) return r;
    r = lin_compat32_split_path(unew, new_parent, new_name);
    if (r < 0) return r;
    if (strcmp(old_parent, new_parent) != 0)
        return -L_EOPNOTSUPP;
    uint32_t parent = 0;
    if (strcmp(old_parent, "/") != 0 &&
        resolve_nx_path(old_parent, &parent) < 0)
        return -L_ENOENT;
    uint32_t target;
    if (nxfs_resolve(parent, new_name, &target) == NXFS_OK)
        (void)nxfs_delete_file(parent, new_name);
    int nr = nxfs_rename(parent, old_name, new_name);
    debug_printf("[linux/i386] rename '%s/%s' -> '%s' rc=%d\n",
                 old_parent, old_name, new_name, nr);
    return nr == NXFS_OK ? 0 : -L_EIO;
}

static int32_t lin_compat32_unlink(uint32_t upath) {
    char parent_path[LIN_PATH_MAX], name[LIN_PATH_MAX];
    int r = lin_compat32_split_path(upath, parent_path, name);
    if (r < 0) return r;
    uint32_t parent = 0;
    if (strcmp(parent_path, "/") != 0 &&
        resolve_nx_path(parent_path, &parent) < 0)
        return -L_ENOENT;
    return nxfs_delete_file(parent, name) == NXFS_OK ? 0 : -L_ENOENT;
}

static int32_t lin_compat32_socketcall(uint32_t call, uint32_t uargs) {
    uint32_t a[6] = {0};
    if (!uptr_ok(uargs, sizeof(a)) ||
        user_copy_from(g_lin_pd, a, uargs, sizeof(a)) != 0)
        return -L_EFAULT;
    switch (call) {
    case 1: return (int32_t)do_socket((int)a[0], (int)a[1], (int)a[2]);
    case 2: return (int32_t)do_sock_bind((int)a[0], a[1], (int)a[2]);
    case 3: return (int32_t)do_sock_connect((int)a[0], a[1], (int)a[2]);
    case 4: return (int32_t)do_sock_listen((int)a[0], (int)a[1]);
    case 5: return (int32_t)do_sock_accept((int)a[0], a[1], a[2]);
    case 6: return (int32_t)do_sock_getsockname((int)a[0], a[1], a[2]);
    case 8: return (int32_t)do_socketpair((int)a[0], (int)a[1], (int)a[2], a[3]);
    case 9: return (int32_t)do_write((int)a[0],
                                     (const void *)(uintptr_t)a[1], a[2]);
    case 10: return (int32_t)do_read((int)a[0],
                                     (void *)(uintptr_t)a[1], a[2]);
    case 11: return (int32_t)do_sendto((int)a[0], a[1], a[2],
                                       (int)a[3], a[4], (int)a[5]);
    case 12: return (int32_t)do_recvfrom((int)a[0], a[1], a[2],
                                         (int)a[3], a[4], a[5]);
    case 13: case 14: case 15:
        return 0;
    default:
        return -L_EOPNOTSUPP;
    }
}

static bool arg_has_token(const char *arg, const char *tag) {
    if (!arg || !tag || !*tag) return false;
    for (const char *p = arg; *p; p++) {
        const char *a = p;
        const char *b = tag;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

static int arg_parse_fd_after_token(const char *arg, const char *tag) {
    if (!arg_has_token(arg, tag)) return -1;
    const char *p = arg;
    size_t tlen = strlen(tag);
    for (; *p; p++) {
        if (strncmp(p, tag, tlen) == 0) {
            p += tlen;
            while (*p == ' ') p++;
            int fd = 0;
            if (*p < '0' || *p > '9') return -1;
            while (*p >= '0' && *p <= '9') {
                fd = fd * 10 + (*p - '0');
                p++;
            }
            return fd;
        }
    }
    return -1;
}

static void lin_mark_steam_update_ui_arg(const char *arg) {
    if (!arg) return;
    int fd = arg_parse_fd_after_token(arg, "-child-update-ui-socket");
    if (fd < 0 || fd >= LIN_FD_MAX ||
        g_lin_fds[fd].kind != LFD_SOCK ||
        g_lin_fds[fd].sock.unix_link < 0)
        return;
    lin_unix_mark_steam_ui(g_lin_fds[fd].sock.unix_link);
    debug_printf("[linux] steam update UI socket fd=%d link=%d\n",
                 fd, g_lin_fds[fd].sock.unix_link);
}

static void lin_mark_steam_update_ui_argv(int argc, const char *const *argv) {
    for (int i = 0; i < argc; i++)
        lin_mark_steam_update_ui_arg(argv[i]);
}

/* Single live exec context at a time — a static store keeps 32 KiB of
 * argv strings off the 64 KiB kernel stack. */
static char g_lin_exec_argstore[LIN_MAX_ARGS][LIN_ARG_MAX];

static int32_t lin_compat32_execve(uint32_t upath, uint32_t uargv) {
    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    int r = copy_user_cstr(upath, path, sizeof(path));
    if (r < 0) {
        debug_printf("[linux/i386] execve: bad path ptr=0x%x rc=%d\n",
                     upath, r);
        return r;
    }
    r = normalize_path(g_lin_cwd, path, canon, sizeof(canon));
    if (r < 0) {
        debug_printf("[linux/i386] execve: normalize '%s' rc=%d\n", path, r);
        return r;
    }

    const char *argv[LIN_MAX_ARGS];
    char (*storage)[LIN_ARG_MAX] = g_lin_exec_argstore;
    int argc = 0;
    if (uargv) {
        for (; argc < LIN_MAX_ARGS - 1; argc++) {
            uint32_t ptr = 0;
            if (user_copy_from(g_lin_pd, &ptr, uargv + argc * 4u, 4) != 0)
                return -L_EFAULT;
            if (!ptr) break;
            r = copy_user_cstr(ptr, storage[argc], LIN_ARG_MAX);
            if (r < 0) {
                debug_printf("[linux/i386] execve '%s': argv[%d] rc=%d\n",
                             canon, argc, r);
                return r;
            }
            argv[argc] = storage[argc];
        }
    }
    if (argc == 0) {
        argv[0] = canon;
        argc = 1;
    }
    argv[argc] = NULL;
    debug_printf("[linux/i386] execve '%s' argc=%d\n", canon, argc);
    for (int i = 0; i < argc; i++)
        debug_printf("[linux/i386]   argv[%d]='%s'\n", i, argv[i]);

    lin_mark_steam_update_ui_argv(argc, argv);

    if (g_lin_fork_child_pid && !g_lin_fork_child_done)
        g_lin_fork_child_exec = true;

    uint32_t len = 0;
    if (linux_read_nxfs(canon, g_lin_file_image, LIN_IMAGE_BYTES, &len) != 0)
        return -L_ENOENT;
    for (int i = 3; i < LIN_FD_MAX; i++) {
        if (g_lin_fds[i].kind == LFD_FREE || !g_lin_fds[i].cloexec)
            continue;
        if (g_lin_fds[i].kind == LFD_SOCK &&
            g_lin_fds[i].sock.domain == LIN_AF_UNIX &&
            g_lin_fds[i].sock.unix_link >= 0 &&
            strncmp(g_lin_fds[i].path, "socketpair:", 11) == 0)
            continue;
        (void)fd_close(i, true);
    }

    vmm_pd_t *old = g_lin_pd;
    g_lin_pd = vmm_clone_for_exec();
    if (!g_lin_pd) {
        g_lin_pd = old;
        return -L_ENOMEM;
    }
    sched_set_current_pd(g_lin_pd);
    vmm_switch(g_lin_pd);
    memset(g_lin_maps, 0, sizeof(g_lin_maps));
    uint64_t entry = 0, esp = 0;
    long lr = lin_map_program(g_lin_file_image, len, canon, argc, argv,
                              &entry, &esp);
    if (lr < 0) {
        vmm_pd_t *failed = g_lin_pd;
        g_lin_pd = old;
        sched_set_current_pd(old);
        vmm_switch(old);
        vmm_release(failed);
        return (int32_t)lr;
    }
    vmm_release(old);
    strncpy(g_lin_exec_path, canon, sizeof(g_lin_exec_path) - 1);
    g_lin_exec_path[sizeof(g_lin_exec_path) - 1] = 0;
    g_lin_image_size = len;
    bool target32 = len >= 5 && g_lin_file_image[4] == 1;
    uint64_t *frame = (uint64_t *)(uintptr_t)g_lin_compat_frame_rsp;
    frame[15] = entry;
    frame[16] = target32 ? 0x3Bull : 0x1Bull;
    frame[18] = esp;
    g_lin_return_compat32 = target32 ? 1 : 0;
    if (!g_lin_fork_child_exec)
        lin_thread_reset();
    g_lin_last_map_stack_top = 0;
    debug_printf("[linux/i386] execve -> %s rip=0x%x cs=0x%x\n",
                 target32 ? "i386" : "amd64",
                 (uint32_t)entry, target32 ? 0x3Bu : 0x1Bu);
    return 0;
}

static int32_t lin_compat32_fork_child_exit(int code);
static int32_t lin_compat32_do_fork(uint32_t child_stack);
static int32_t lin_compat32_timespec_sleep(uint32_t ureq, uint32_t urem);

int32_t linux_compat32_syscall(uint32_t num, uint32_t a0, uint32_t a1,
                               uint32_t a2, uint32_t a3, uint32_t a4) {
    (void)a3;
    (void)a4;
    /* Diagnostic: between fork and child-exit only the child runs, so this
     * window is an exact per-syscall trace of the spawned process. */
    if (g_lin_fork_child_pid && !g_lin_fork_child_done &&
        lin_thread_get_tid() == g_lin_fork_child_pid)
        debug_printf("[linux/i386/child] sc %u a0=0x%x a1=0x%x a2=0x%x\n",
                     num, a0, a1, a2);
    switch (num) {
    case 1: /* exit */
    case 252: /* exit_group */
        {
            int32_t resume = 0;
            if (lin_thread_compat32_try_exit((int)(uint8_t)a0,
                                             num == 252, &resume))
                return resume;
            if (lin_thread_compat32_fork_exit((int)(uint8_t)a0, &resume)) {
                g_lin_fork_child_status = ((int)(uint8_t)a0 & 0xff) << 8;
                g_lin_fork_child_done = true;
                return resume;
            }
        }
        if (g_lin_fork_child)
            return lin_compat32_fork_child_exit((int)(uint8_t)a0);
        if (g_lin_fork_child_exec) {
            g_lin_fork_child_status = ((int)(uint8_t)a0 & 0xff) << 8;
            g_lin_fork_child_done = true;
            g_lin_fork_child_exec = false;
            g_lin_pid = g_lin_fork_parent_pid;
            int32_t resume = 0;
            if (lin_thread_compat32_resume_parent(&resume))
                return resume;
            return 0;
        }
        if (g_lin_sync_running)
            lin_sync_child_exit((int)(uint8_t)a0);
        g_lin_exit_code = (int)(uint8_t)a0;
        longjmp(g_lin_return, 1);
        return 0;
    case 3: /* read */
        return (int32_t)do_read((int)a0, (void *)(uintptr_t)a1, a2);
    case 4: /* write */
        if ((a0 == 1 || a0 == 2) && a1 && a2) {
            char diag[192];
            uint32_t dn = a2 < sizeof(diag) - 1
                        ? a2 : (uint32_t)sizeof(diag) - 1;
            if (user_copy_from(g_lin_pd, diag, a1, dn) == 0) {
                diag[dn] = 0;
                debug_printf("[linux/i386] output: %s", diag);
            }
        }
        return (int32_t)do_write((int)a0, (const void *)(uintptr_t)a1, a2);
    case 5: /* open */
        return (int32_t)do_openat(L_AT_FDCWD, a0, (int)a1, a2);
    case 6: /* close */
        return (int32_t)fd_close((int)a0, true);
    case 11: /* execve */
        return lin_compat32_execve(a0, a1);
    case 33: /* access */
        return (int32_t)do_access(a0);
    case 15: /* chmod */
        return 0;
    case 39: /* mkdir */
        return lin_compat32_mkdir(a0);
    case 296: /* mkdirat(dirfd, path, mode) — modern glibc mkdir() path */
        /* AT_FDCWD paths and absolute paths both normalise against cwd in
         * lin_compat32_mkdir; a real dirfd base is not used by Steam. */
        return lin_compat32_mkdir(a1);
    case 38: /* rename */
        return lin_compat32_rename(a0, a1);
    case 12: /* chdir */
        return lin_compat32_chdir(a0);
    case 20: /* getpid */
        return (int32_t)g_lin_pid;
    case 45: /* brk */
        return (int32_t)g_lin_brk;
    case 146: /* writev */
        return lin_compat32_iov((int)a0, a1, a2, true);
    case 158: /* sched_yield */
        return lin_thread_compat32_yield();
    case 168: /* poll */
        return (int32_t)do_poll(a0, a1, (int)a2);
    case 118: /* fsync */
    case 148: /* fdatasync */
    case 143: /* flock */
        return 0;
    case 2: /* fork */
    case 190: /* vfork */
        return lin_compat32_do_fork(0);
    case 120: /* clone */
        /* i386 sys_clone register ABI: ebx=flags, ecx=stack, edx=ptid,
         * esi=tls (user_desc*), edi=ctid — a3 is TLS, a4 is CTID. */
        if (a0 & 0x00010000u) /* CLONE_THREAD */
            return (int32_t)lin_thread_compat32_clone(a0, a1, a2,
                                                      a4 /* ctid */,
                                                      a3 /* tls */);
        return lin_compat32_do_fork(a1);
    case 7: /* waitpid */
    case 114: /* wait4 */
        while (!g_lin_fork_child_done) {
            (void)lin_thread_compat32_yield();
            pit_sleep(1);
        }
        if (a0 > 0 && (lin_pid_t)a0 != g_lin_fork_child_pid)
            return -L_ECHILD;
        if (a1 && uptr_write_ok(a1, 4)) {
            uint32_t st = (uint32_t)g_lin_fork_child_status;
            if (user_copy_to(g_lin_pd, a1, &st, 4) != 0)
                return -L_EFAULT;
        }
        {
            int32_t pid = (int32_t)g_lin_fork_child_pid;
            g_lin_fork_child_done = false;
            return pid;
        }
    case 172: /* prctl */
        return 0;
    case 162: /* nanosleep */
        return lin_compat32_timespec_sleep(a0, a1);
    case 219: /* madvise */
        return uptr_ok(a0, a1) ? 0 : -L_EFAULT;
    case 267: /* clock_nanosleep */
        return lin_compat32_timespec_sleep(a2, a3);
    case 407: { /* clock_nanosleep_time64 */
        if (!uptr_ok(a2, 16))
            return -L_EFAULT;
        uint64_t ts[2];
        if (user_copy_from(g_lin_pd, ts, a2, 16) != 0)
            return -L_EFAULT;
        uint64_t ms = ts[0] * 1000ull + (ts[1] + 999999ull) / 1000000ull;
        while (ms) {
            if (lin_thread_session_active())
                (void)lin_thread_compat32_yield();
            uint32_t chunk = ms > 1000 ? 1000 : (uint32_t)ms;
            pit_sleep(chunk);
            ms -= chunk;
        }
        if (a3 && uptr_write_ok(a3, 16))
            memset((void *)(uintptr_t)a3, 0, 16);
        return 0;
    }
    case 435: { /* clone3 */
        if (!uptr_ok(a0, 64))
            return -L_EFAULT;
        /* struct clone_args (linux/sched.h): flags@0, pidfd@8,
         * child_tid@16, parent_tid@24, exit_signal@32, stack@40 (LOW
         * address), stack_size@48, tls@56.  The old reader took stack
         * from @32 — the child "stack" became exit_signal (SIGCHLD=0x11)
         * and the resumed context popped from ESP=0x11 (CR2=0x11 crash in
         * glibc __clone3); tls from @40 pointed at the stack low address,
         * which is what kept CLONE_SETTLS threads on the fallback path. */
        uint64_t ca_flags = 0, ca_child_tid = 0, ca_parent_tid = 0;
        uint64_t ca_stack = 0, ca_stack_size = 0, ca_tls = 0;
        if (user_copy_from(g_lin_pd, &ca_flags, a0, 8) != 0 ||
            user_copy_from(g_lin_pd, &ca_child_tid, a0 + 16, 8) != 0 ||
            user_copy_from(g_lin_pd, &ca_parent_tid, a0 + 24, 8) != 0 ||
            user_copy_from(g_lin_pd, &ca_stack, a0 + 40, 8) != 0 ||
            user_copy_from(g_lin_pd, &ca_stack_size, a0 + 48, 8) != 0 ||
            user_copy_from(g_lin_pd, &ca_tls, a0 + 56, 8) != 0)
            return -L_EFAULT;
        uint32_t stack_top = ca_stack
            ? (uint32_t)(ca_stack + ca_stack_size) : 0;
        if ((uint32_t)ca_flags & 0x00010000u)
            return (int32_t)lin_thread_compat32_clone((uint32_t)ca_flags,
                stack_top, (uint32_t)ca_parent_tid, (uint32_t)ca_child_tid,
                (uint32_t)ca_tls);
        return lin_compat32_do_fork(stack_top);
    }
    case 197: /* fstat64 */
        return lin_compat32_fstat64((int)a0, a1);
    case 243: { /* set_thread_area */
        if (!uptr_write_ok(a0, 16))
            return -L_EFAULT;
        uint32_t desc[4];
        if (user_copy_from(g_lin_pd, desc, a0, sizeof(desc)) != 0)
            return -L_EFAULT;
        if (desc[0] != 0xFFFFFFFFu && desc[0] != 8u)
            return -L_EINVAL;
        gdt_set_compat_tls(desc[1], desc[2], (desc[3] & (1u << 4)) != 0);
        lin_thread_compat32_note_tls(desc[1], desc[2],
                                     (desc[3] & (1u << 4)) != 0);
        desc[0] = 8;
        return user_copy_to(g_lin_pd, a0, desc, sizeof(desc)) == 0
            ? 0 : -L_EFAULT;
    }
    case 258: /* set_tid_address */
        return (int32_t)lin_thread_set_tid_address(a0);
    case 311: /* set_robust_list */
        return 0;
    case 174: /* rt_sigaction */
    case 175: /* rt_sigprocmask */
    case 186: /* sigaltstack */
        return 0;
    case 183: { /* getcwd */
        uint32_t n = (uint32_t)strlen(g_lin_cwd) + 1;
        if (a1 < n || !uptr_write_ok(a0, n)) return -L_ERANGE;
        return user_copy_to(g_lin_pd, a0, g_lin_cwd, n) == 0
            ? (int32_t)n : -L_EFAULT;
    }
    case 191: { /* ugetrlimit */
        if (!uptr_write_ok(a1, 8)) return -L_EFAULT;
        uint32_t lim[2] = { 0x7FFFFFFFu, 0x7FFFFFFFu };
        return user_copy_to(g_lin_pd, a1, lim, sizeof(lim)) == 0
            ? 0 : -L_EFAULT;
    }
    case 224: /* gettid */
        return (int32_t)(lin_thread_session_active()
            ? lin_thread_get_tid() : g_lin_pid);
    case 240: { /* futex */
        uint32_t op = a1 & 0x7Fu;
        if (op == 1 || op == 3 || op == 4 || op == 5 || op == 10)
            return lin_thread_compat32_futex_wake(a0, (int)a2);
        if (op == 0 || op == 9) {
            uint32_t timeout_ptr = (op == 9) ? a3 : a3;
            return lin_thread_compat32_futex_wait(a0, a2, timeout_ptr);
        }
        return -L_ENOSYS;
    }
    case 265: { /* clock_gettime */
        if (!uptr_write_ok(a1, 8)) return -L_EFAULT;
        uint32_t ts[2] = { pit_ms() / 1000u,
                           (pit_ms() % 1000u) * 1000000u };
        return user_copy_to(g_lin_pd, a1, ts, sizeof(ts)) == 0
            ? 0 : -L_EFAULT;
    }
    case 270: /* tgkill: signals are not yet delivered */
        return 0;
    case 196: /* lstat64 */
        return lin_compat32_stat64(L_AT_FDCWD, a0, a1, 0);
    case 195: /* stat64 */
        return lin_compat32_stat64(L_AT_FDCWD, a0, a1, 0);
    case 220: /* getdents64 */
        return (int32_t)do_getdents64((int)a0, a1, a2);
    case 221: { /* fcntl64 */
        int fd = (int)a0, cmd = (int)a1;
        if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE)
            return -L_EBADF;
        if (cmd == 0 || cmd == 1030)
            return do_dup_from(fd, (int)a2, cmd == 1030);
        if (cmd == 1) return g_lin_fds[fd].cloexec ? 1 : 0;
        if (cmd == 2) {
            g_lin_fds[fd].cloexec = (a2 & 1) != 0;
            return 0;
        }
        if (cmd == 3) return g_lin_fds[fd].flags;
        if (cmd == 4) {
            g_lin_fds[fd].flags =
                (g_lin_fds[fd].flags & ~L_O_NONBLOCK) |
                ((int)a2 & L_O_NONBLOCK);
            return 0;
        }
        return -L_EINVAL;
    }
    case 254: /* epoll_create */
        return (int32_t)do_epoll_create1(0);
    case 255: /* epoll_ctl */
        return (int32_t)do_epoll_ctl((int)a0, (int)a1, (int)a2, a3);
    case 256: /* epoll_wait: no events in synchronous compat path */
        return 0;
    case 54: /* ioctl */
        return (int32_t)do_ioctl((int)a0, a1, a2);
    case 85: /* readlink */
        return (int32_t)do_readlink(a0, a1, a2);
    case 102: /* socketcall */
        return lin_compat32_socketcall(a0, a1);
    case 297: { /* mknodat: represent FIFOs as NXFS rendezvous files */
        long fd = do_openat((int)a0, a1,
                            L_O_CREAT | L_O_EXCL | L_O_WRONLY, a2);
        if (fd < 0) return (int32_t)fd;
        (void)fd_close((int)fd, true);
        return 0;
    }
    case 328: { /* eventfd2 */
        int fd = fd_alloc_from(3);
        if (fd < 0) return fd;
        g_lin_fds[fd].kind = LFD_EVENTFD;
        g_lin_fds[fd].flags = L_O_RDWR | ((a1 & 0x800u) ? L_O_NONBLOCK : 0);
        g_lin_fds[fd].cloexec = (a1 & L_O_CLOEXEC) != 0;
        g_lin_fds[fd].event_count = a0;
        strcpy(g_lin_fds[fd].path, "eventfd");
        return fd;
    }
    case 340: { /* prlimit64 */
        if (a3) {
            if (!uptr_write_ok(a3, 16)) return -L_EFAULT;
            uint64_t lim[2] = { 0x7FFFFFFFull, 0x7FFFFFFFull };
            if (user_copy_to(g_lin_pd, a3, lim, sizeof(lim)) != 0)
                return -L_EFAULT;
        }
        return 0;
    }
    case 305: /* readlinkat */
        return (int32_t)do_readlink(a1, a2, a3);
    case 355: { /* getrandom */
        if (!uptr_write_ok(a0, a1)) return -L_EFAULT;
        uint8_t *p = (uint8_t *)(uintptr_t)a0;
        uint32_t x = pit_ms() ^ 0x4E58584Fu;
        for (uint32_t i = 0; i < a1; i++) {
            x = x * 1664525u + 1013904223u;
            p[i] = (uint8_t)(x >> 24);
        }
        return (int32_t)a1;
    }
    case 268:   /* statfs64 (path, sz, buf) */
    case 269: { /* fstatfs64 (fd, sz, buf) */
        /* struct statfs64 (i386, 84 bytes): f_type@0, f_bsize@4,
         * f_blocks@8 (u64), f_bfree@16 (u64), f_bavail@24 (u64),
         * f_files@32 (u64), f_ffree@40 (u64), f_fsid@48 (8),
         * f_namelen@56, f_frsize@60, f_flags@64, f_spare@68 (5*4).
         * Steam divides f_bavail*f_bsize to gauge free space — report the
         * live NXFS figures so the updater sees real capacity. */
        uint64_t ubuf = a2;
        if (!uptr_write_ok(ubuf, 84)) return -L_EFAULT;
        uint32_t bsize = NXFS_BLOCK_BYTES;
        uint32_t iu = 0, bu = 0;
        nxfs_stats(&iu, &bu);
        uint64_t bfree = nxfs_free_blocks();
        /* Floor the advertised free space to 1 GiB: the Steam updater
         * refuses to proceed under 250 MB, and the LIVE RAMFS volume is far
         * smaller.  The subsystem streams the client rather than storing the
         * full multi-hundred-MB download, so a generous figure here lets the
         * graphical update flow run.  (An INSTALLED NXFS/SATA volume reports
         * its real, larger capacity through the same path.) */
        uint64_t floor_blocks = (1024ULL * 1024ULL * 1024ULL) / bsize;
        if (bfree < floor_blocks) bfree = floor_blocks;
        uint64_t btotal = (uint64_t)bu + bfree;
        if (btotal == 0) btotal = 1;
        uint8_t st[84];
        memset(st, 0, sizeof(st));
        *(uint32_t *)(void *)(st + 0)  = 0x6e657878u;    /* f_type 'nexx' */
        *(uint32_t *)(void *)(st + 4)  = bsize;           /* f_bsize */
        *(uint64_t *)(void *)(st + 8)  = btotal;          /* f_blocks */
        *(uint64_t *)(void *)(st + 16) = bfree;           /* f_bfree */
        *(uint64_t *)(void *)(st + 24) = bfree;           /* f_bavail */
        *(uint64_t *)(void *)(st + 32) = 0x100000ULL;     /* f_files */
        *(uint64_t *)(void *)(st + 40) = 0x100000ULL - iu;/* f_ffree */
        *(uint32_t *)(void *)(st + 56) = 255;             /* f_namelen */
        *(uint32_t *)(void *)(st + 60) = bsize;           /* f_frsize */
        return user_copy_to(g_lin_pd, ubuf, st, sizeof(st)) == 0
            ? 0 : -L_EFAULT;
    }
    case 386: /* rseq: libc optional fast path */
        return -L_ENOSYS;
    case 403: { /* clock_gettime64 */
        if (!uptr_write_ok(a1, 16)) return -L_EFAULT;
        uint64_t ts[2] = { pit_ms() / 1000u,
                           (uint64_t)(pit_ms() % 1000u) * 1000000u };
        return user_copy_to(g_lin_pd, a1, ts, sizeof(ts)) == 0
            ? 0 : -L_EFAULT;
    }
    case 439: /* faccessat2 */
        return (int32_t)do_access(a1);
    case 10: /* unlink */
        return lin_compat32_unlink(a0);
    case 117: { /* ipc: SysV semaphore subset */
        uint32_t call = a0 & 0xFFFFu;
        if (call == 2) /* SEMGET */
            return 1;
        if (call == 1 || call == 3) /* SEMOP / SEMCTL */
            return 0;
        return -L_ENOSYS;
    }
    case 198: /* lchown32 */
        return 0;
    case 199: /* getuid32 */
        return 1000;
    case 200: /* getgid32 */
        return 1000;
    case 201: /* geteuid32 */
        return 1000;
    case 202: /* getegid32 */
        return 1000;
    case 192: /* mmap2 (offset in 4 KiB pages) */
        return (int32_t)do_mmap(a0, a1, (int)a2, (int)a3, (int)a4,
                                (uint64_t)g_lin_compat_arg5 << 12);
    case 91: /* munmap */
        return (int32_t)do_munmap(a0, a1);
    case 125: /* mprotect */
        return lin_mprotect_user(a0, a1, (int)a2);
    case 295: /* openat */
        return (int32_t)do_openat((int)a0, a1, (int)a2, a3);
    case 300: /* fstatat64 */
        return lin_compat32_stat64((int)a0, a1, a2, a3);
    case 383: /* statx */
        return -L_ENOSYS;
    default:
        debug_printf("[linux/i386] unimplemented int80 syscall %u\n", num);
        return -L_ENOSYS;
    }
}

static long lin_do_run_sync(uint64_t upath, uint64_t uargv) {
    if (g_lin_sync_running || g_lin_fork_child) return -L_EAGAIN;

    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    int pr = copy_user_cstr(upath, path, sizeof(path));
    if (pr < 0) return pr;
    pr = normalize_path(g_lin_cwd, path, canon, sizeof(canon));
    if (pr < 0) return pr;
    if (strcmp(canon, "/programs/steam") == 0)
        strcpy(canon, "/programs/ubuntu12_32/steam");

    int argc = lin_count_user_argv(uargv);
    if (argc < 0 || argc > LIN_MAX_ARGS) return -L_EINVAL;
    char argbuf[LIN_MAX_ARGS][LIN_PATH_MAX];
    const char *argv[LIN_MAX_ARGS];
    int ar = lin_copy_user_argv(uargv, argc, argv, argbuf);
    if (ar < 0) return ar;
    if (argc == 0) {
        argv[0] = canon;
        argc = 1;
    } else {
        argv[0] = canon;
    }

    if (strcmp(canon, "/programs/ubuntu12_32/steam") == 0)
        lin_steam_ensure_cdn();

    uint32_t len = 0;
    if (linux_read_nxfs(canon, g_lin_file_image, LIN_IMAGE_BYTES, &len) != 0)
        return -L_ENOENT;

    /* Parent state must live outside g_lin_kstack: child syscalls reuse that
     * stack and would clobber a stack-local lin_saved_t before longjmp back. */
    lin_save_state(&g_lin_sync_saved, NULL);

    vmm_pd_t *run_pd = vmm_clone_for_exec();
    if (!run_pd) {
        lin_restore_state(&g_lin_sync_saved, NULL);
        return -L_ENOMEM;
    }
    g_lin_pd = run_pd;
    sched_set_current_pd(g_lin_pd);
    vmm_switch(g_lin_pd);
    lin_clear_user_maps();

    for (int i = 3; i < LIN_FD_MAX; i++)
        if (g_lin_fds[i].kind != LFD_FREE)
            (void)fd_close(i, true);
    g_lin_tty_line_len = 0;
    g_lin_tty_line_pos = 0;
    if (strcmp(canon, "/programs/ubuntu12_32/steam") == 0) {
        uint32_t programs_ino = 0;
        if (resolve_nx_path("/programs", &programs_ino) == 0) {
            strcpy(g_lin_cwd, "/programs");
            g_lin_cwd_ino = programs_ino;
        }
    }

    uint64_t entry = 0, rsp = 0;
    long lr = lin_map_program(g_lin_file_image, len, canon, argc, argv,
                              &entry, &rsp);
    if (lr < 0) {
        lin_restore_state(&g_lin_sync_saved, NULL);
        return lr;
    }

    strncpy(g_lin_exec_path, canon, sizeof(g_lin_exec_path) - 1);
    strncpy(g_lin_sync_child_path, canon, sizeof(g_lin_sync_child_path) - 1);
    g_lin_image_size = len;

    lin_thread_reset();

    g_lin_sync_running = true;
    g_lin_sync_exit_code = 0;
    g_lin_sync_faulted = false;
    gdt_set_kernel_stack((uintptr_t)(g_lin_kstack +
        (g_lin_compat32 ? sizeof(g_lin_kstack) / 2 : sizeof(g_lin_kstack))));

    debug_printf("[linux] sync run '%s' argc=%d entry=%p rsp=%p\n",
                 canon, argc, (void *)(uintptr_t)entry,
                 (void *)(uintptr_t)rsp);
    if (g_lin_compat32)
        ring3_enter32((uintptr_t)entry, (uintptr_t)rsp);
    else
        ring3_enter((uintptr_t)entry, (uintptr_t)rsp);
    g_lin_sync_exit_code = -5;
    g_lin_sync_running = false;

    lin_restore_state(&g_lin_sync_saved, NULL);
    lin_thread_reset();
    debug_printf("[linux] sync run '%s' finished code=%d fault=%u\n",
                 canon, g_lin_sync_exit_code, g_lin_sync_faulted ? 1u : 0u);
    return g_lin_sync_exit_code;
}

static void lin_pe_guest_exit(int code) {
    g_lin_pe_exit_code = code;
    longjmp(g_lin_pe_return, 1);
}

static long lin_wine_run_user(uint64_t upath, lin_regs_t *r) {
    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    g_lin_wine_saved_fs = rdmsr(MSR_FS_BASE);
    g_lin_wine_saved_gs = rdmsr(MSR_GS_BASE);
    if (r) {
        g_lin_wine_saved_regs = *r;
        g_lin_wine_saved_valid = true;
    } else {
        g_lin_wine_saved_valid = false;
    }

    int pr = copy_user_cstr(upath, path, sizeof(path));
    if (pr < 0) return pr;
    pr = normalize_path(g_lin_cwd, path, canon, sizeof(canon));
    if (pr < 0) return pr;

    uint32_t len = 0;
    if (linux_read_nxfs(canon, g_lin_file_image, LIN_IMAGE_BYTES, &len) != 0) {
        debug_printf("[linux] wine: ENOENT '%s'\n", canon);
        return -L_ENOENT;
    }
    if (len < 64) return -L_EINVAL;

    pe_image_t pe;
    if (!pe_parse(g_lin_file_image, len, &pe)) {
        debug_printf("[linux] wine: PE parse failed '%s' (%u bytes): %s\n",
                     canon, len, pe.err);
        return -L_EINVAL;
    }

    if (!pe_map_user(g_lin_file_image, len, g_lin_pd,
                     LIN_PE_BASE, LIN_USER_LOW, LIN_USER_MAX, &pe)) {
        debug_printf("[linux] wine: PE map failed '%s': %s\n", canon, pe.err);
        return -L_EINVAL;
    }

    uint64_t stub = pe_win32_map_stubs(g_lin_pd, LIN_PE_BASE, pe.size_of_image);
    if (!stub) return -L_ENOMEM;

    if (!pe_win32_bind_imports(g_lin_file_image, len, g_lin_pd,
                               LIN_PE_BASE, stub, &pe)) {
        debug_printf("[linux] wine: import bind failed '%s'\n", canon);
        return -L_EINVAL;
    }

    if (!pe_win32_inject_entry(g_lin_pd, LIN_PE_BASE, pe.entry_rva, stub)) {
        debug_printf("[linux] wine: entry inject failed '%s'\n", canon);
        return -L_EINVAL;
    }

    uint64_t entry = 0, rsp = 0;
    if (!pe_win32_prepare_run(g_lin_pd, LIN_PE_BASE, pe.size_of_image,
                              pe.entry_rva, &entry, &rsp))
        return -L_ENOMEM;

    debug_printf("[linux] wine PE load OK '%s' entry=0x%x machine=0x%x "
                 "NEXXON_PE_LOAD_OK\n",
                 canon, (uint32_t)entry, pe.machine);

    g_lin_pe_running = true;
    g_lin_pe_exit_code = 0;
    g_lin_pe_used_win32 = false;
    gdt_set_kernel_stack((uintptr_t)(g_lin_kstack + sizeof(g_lin_kstack)));

    if (setjmp(g_lin_pe_return) == 0) {
        debug_printf("[linux] wine PE run entry=0x%x rsp=0x%x\n",
                     (uint32_t)entry, (uint32_t)rsp);
        ring3_enter((uintptr_t)entry, (uintptr_t)rsp);
        g_lin_pe_exit_code = -5;
    }

    g_lin_pe_running = false;
    int pe_code = g_lin_pe_exit_code;
    bool pe_win32 = g_lin_pe_used_win32;

    wrmsr(MSR_FS_BASE, g_lin_wine_saved_fs);
    wrmsr(MSR_GS_BASE, g_lin_wine_saved_gs);

    if (r && g_lin_wine_saved_valid) {
        *r = g_lin_wine_saved_regs;
        r->rax = (uint64_t)(long)pe_code;
    }

    debug_printf("[linux] wine PE finished code=%d win32=%u NEXXON_PE_RUN_OK\n",
                 pe_code, pe_win32 ? 1u : 0u);
    if (pe_win32)
        debug_printf("[linux] NEXXON_WIN32_EXITPROCESS_OK\n");

    return pe_code;
}

/* ---- Host accessors for lin_thread.c ----------------------------------- */
vmm_pd_t *lin_host_pd(void) { return g_lin_pd; }
bool lin_host_running(void) { return g_lin_running; }
bool lin_host_sync_running(void) { return g_lin_sync_running; }
bool lin_host_fork_child(void) { return g_lin_fork_child; }
bool lin_host_pe_running(void) { return g_lin_pe_running; }
uint32_t lin_host_pid(void) { return (uint32_t)g_lin_pid; }

int lin_ucopy_from(void *dst, uint64_t src, uint32_t len) {
    return user_copy_from(g_lin_pd, dst, src, len);
}

int lin_ucopy_to(uint64_t dst, const void *src, uint32_t len) {
    return user_copy_to(g_lin_pd, dst, src, len);
}

int lin_ucopy_u32(uint64_t addr, uint32_t *out) {
    return user_copy_from(g_lin_pd, out, addr, 4);
}

int lin_ucopy_u32_write(uint64_t addr, uint32_t val) {
    return user_copy_to(g_lin_pd, addr, &val, 4);
}

/* ---- Syscall dispatcher ------------------------------------------------ */
void linux_syscall_dispatch(lin_regs_t *r) {
    bool switched = false;
    g_lin_syscall_frame_override = 0;
    g_lin_return_compat32 = 0;
    if (lin_thread_session_active())
        lin_thread_ensure_boot(r);

    uint64_t n = r->rax;
    uint64_t a0 = r->rdi, a1 = r->rsi, a2 = r->rdx;
    uint64_t a3 = r->r10, a4 = r->r8, a5 = r->r9;
    long ret = -L_ENOSYS;

    switch (n) {
    case LSYS_read:
        ret = do_read((int)a0, (void *)(uintptr_t)a1, a2);
        break;
    case LSYS_write:
        ret = do_write((int)a0, (const void *)(uintptr_t)a1, a2);
        break;
    case LSYS_readv:
        ret = do_iov((int)a0, a1, a2, false);
        break;
    case LSYS_writev:
        ret = do_iov((int)a0, a1, a2, true);
        break;
    case LSYS_open:
        ret = do_openat(L_AT_FDCWD, a0, (int)a1, (uint32_t)a2);
        break;
    case LSYS_openat:
        ret = do_openat((int)a0, a1, (int)a2, (uint32_t)a3);
        break;
    case LSYS_close:
        ret = fd_close((int)a0, true);
        break;
    case LSYS_lseek:
        ret = do_lseek((int)a0, (int64_t)a1, (int)a2);
        break;
    case LSYS_pread64: {
        int fd = (int)a0;
        if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind != LFD_NX_FILE) {
            ret = -L_EBADF;
            break;
        }
        uint64_t save = g_lin_fds[fd].offset;
        g_lin_fds[fd].offset = a3;
        ret = do_read(fd, (void *)(uintptr_t)a1, a2);
        g_lin_fds[fd].offset = save;
        break;
    }
    case LSYS_pwrite64:
        ret = -L_EOPNOTSUPP;
        break;
    case LSYS_stat:
        ret = do_stat_user(L_AT_FDCWD, a0, a1, 0);
        break;
    case LSYS_fstat:
        ret = do_fstat((int)a0, a1);
        break;
    case LSYS_newfstatat:
        ret = do_stat_user((int)a0, a1, a2, (int)a3);
        break;
    case LSYS_getdents64:
        ret = do_getdents64((int)a0, a1, a2);
        break;
    case LSYS_access:
        ret = do_access(a0);
        break;
    case LSYS_faccessat:
    case LSYS_faccessat2:
        ret = do_access(a1);
        break;
    case LSYS_getcwd: {
        size_t len = strlen(g_lin_cwd) + 1;
        if (a1 < len) ret = -L_ENOMEM;
        else if (!uptr_write_ok(a0, len)) ret = -L_EFAULT;
        else { memcpy((void *)(uintptr_t)a0, g_lin_cwd, len); ret = (long)len; }
        break;
    }
    case LSYS_chdir: {
        char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
        int x = copy_user_cstr(a0, path, sizeof(path));
        if (x == 0) x = normalize_path(g_lin_cwd, path, canon, sizeof(canon));
        uint32_t ino = 0;
        nxfs_inode_t node;
        if (x == 0) x = resolve_nx_path(canon, &ino);
        if (x == 0 && (nxfs_read_inode(ino, &node) != NXFS_OK ||
                       node.type != NXFS_TYPE_DIR)) x = -L_ENOTDIR;
        if (x == 0) {
            strcpy(g_lin_cwd, canon);
            g_lin_cwd_ino = ino;
        }
        ret = x;
        break;
    }
    case LSYS_fchdir:
        if ((int)a0 < 0 || (int)a0 >= LIN_FD_MAX ||
            g_lin_fds[(int)a0].kind != LFD_NX_DIR) ret = -L_EBADF;
        else {
            strcpy(g_lin_cwd, g_lin_fds[(int)a0].path);
            g_lin_cwd_ino = g_lin_fds[(int)a0].inode;
            ret = 0;
        }
        break;
    case LSYS_readlink:
        ret = do_readlink(a0, a1, a2);
        break;
    case LSYS_readlinkat:
        ret = do_readlink(a1, a2, a3);
        break;
    case LSYS_dup:
        ret = do_dup_from((int)a0, 3, false);
        break;
    case LSYS_dup2:
    case LSYS_dup3: {
        int oldfd = (int)a0, newfd = (int)a1;
        if (oldfd < 0 || oldfd >= LIN_FD_MAX ||
            g_lin_fds[oldfd].kind == LFD_FREE ||
            newfd < 0 || newfd >= LIN_FD_MAX) {
            ret = -L_EBADF;
        } else if (oldfd == newfd) {
            ret = (n == LSYS_dup3) ? -L_EINVAL : newfd;
        } else if (g_lin_fds[oldfd].write_stream) {
            ret = -L_EBUSY;
        } else {
            if (g_lin_fds[newfd].kind != LFD_FREE) (void)fd_close(newfd, true);
            g_lin_fds[newfd] = g_lin_fds[oldfd];
            g_lin_fds[newfd].cloexec = (n == LSYS_dup3) && (a2 & L_O_CLOEXEC);
            ret = newfd;
        }
        break;
    }
    case LSYS_fcntl: {
        int fd = (int)a0, cmd = (int)a1;
        if (fd < 0 || fd >= LIN_FD_MAX || g_lin_fds[fd].kind == LFD_FREE) {
            ret = -L_EBADF;
        } else if (cmd == 0 || cmd == 1030) { /* F_DUPFD[_CLOEXEC] */
            ret = do_dup_from(fd, (int)a2, cmd == 1030);
        } else if (cmd == 1) {               /* F_GETFD */
            ret = g_lin_fds[fd].cloexec ? 1 : 0;
        } else if (cmd == 2) {               /* F_SETFD */
            g_lin_fds[fd].cloexec = (a2 & 1) != 0; ret = 0;
        } else if (cmd == 3) {               /* F_GETFL */
            ret = g_lin_fds[fd].flags;
        } else if (cmd == 4) {               /* F_SETFL */
            g_lin_fds[fd].flags = (g_lin_fds[fd].flags & ~L_O_NONBLOCK) |
                                  ((int)a2 & L_O_NONBLOCK); ret = 0;
        } else ret = -L_EINVAL;
        break;
    }
    case LSYS_ioctl:
        ret = do_ioctl((int)a0, a1, a2);
        break;
    case LSYS_poll:
        ret = do_poll(a0, a1, (int)a2);
        break;
    case LSYS_pipe:
    case LSYS_pipe2:
        ret = do_pipe2(a0, n == LSYS_pipe2 ? (int)a1 : 0);
        break;
    case LSYS_epoll_create1:
        ret = do_epoll_create1((int)a0);
        break;
    case LSYS_epoll_ctl:
        ret = do_epoll_ctl((int)a0, (int)a1, (int)a2, a3);
        break;
    case LSYS_epoll_wait:
    case LSYS_epoll_pwait:
        ret = do_epoll_wait((int)a0, a1, (int)a2, (int)a3, r);
        if (ret == LIN_THREAD_SWITCHED) return;
        break;

    case LSYS_brk: {
        if (a0 == 0) {
            ret = (long)g_lin_brk;
            break;
        }
        uint64_t max = map_lowest();
        if (a0 >= g_lin_brk_floor && a0 <= max) {
            uint64_t old_page = (g_lin_brk + 4095) & ~4095ULL;
            uint64_t new_page = (a0 + 4095) & ~4095ULL;
            bool ok = true;
            if (new_page > old_page)
                ok = map_user_pages(old_page, new_page - old_page,
                                    VMM_FLAG_RW | VMM_FLAG_NX) == 0;
            if (ok) {
                if (a0 > g_lin_brk)
                    memset((void *)(uintptr_t)g_lin_brk, 0,
                           (size_t)(a0 - g_lin_brk));
                else if (new_page < old_page)
                    for (uint64_t p = new_page; p < old_page;
                         p += VMM_PAGE_SIZE)
                        (void)vmm_unmap_page(g_lin_pd, (uintptr_t)p);
                g_lin_brk = a0;
            }
        }
        ret = (long)g_lin_brk;
        break;
    }
    case LSYS_mmap:
        ret = do_mmap(a0, a1, (int)a2, (int)a3, (int)a4, a5);
        break;
    case LSYS_munmap:
        ret = do_munmap(a0, a1);
        break;
    case LSYS_mprotect:
        ret = lin_mprotect_user(a0, a1, (int)a2);
        break;
    case LSYS_madvise:
        ret = uptr_ok(a0, a1) ? 0 : -L_EFAULT;
        break;
    case LSYS_mremap:
        ret = -L_EOPNOTSUPP;
        break;

    case LSYS_arch_prctl:
        if (a0 == ARCH_SET_FS && (a1 == 0 || uptr_ok(a1, 1))) {
            wrmsr(MSR_FS_BASE, a1); ret = 0;
        } else if (a0 == ARCH_SET_GS && (a1 == 0 || uptr_ok(a1, 1))) {
            wrmsr(MSR_GS_BASE, a1); ret = 0;
        } else if (a0 == ARCH_GET_FS && uptr_write_ok(a1, 8)) {
            *(uint64_t *)(uintptr_t)a1 = rdmsr(MSR_FS_BASE); ret = 0;
        } else if (a0 == ARCH_GET_GS && uptr_write_ok(a1, 8)) {
            *(uint64_t *)(uintptr_t)a1 = rdmsr(MSR_GS_BASE); ret = 0;
        } else ret = -L_EINVAL;
        break;
    case LSYS_rt_sigaction:
    case LSYS_rt_sigprocmask:
    case LSYS_prctl:
    case LSYS_set_robust_list:
        ret = 0;
        break;
    case LSYS_set_tid_address:
        ret = (long)lin_thread_set_tid_address(a0);
        break;
    case LSYS_getpid:
        ret = (long)g_lin_pid;
        break;
    case LSYS_gettid:
        ret = lin_thread_session_active()
            ? (long)lin_thread_get_tid() : (long)g_lin_pid;
        break;
    case LSYS_getppid:
        ret = (long)g_lin_ppid;
        break;
    case LSYS_nexxon_apt_install:
        ret = lin_apt_install_user(a0, a1);
        break;
    case LSYS_nexxon_run_sync: {
        g_lin_syscall_return_frame = *r;
        g_lin_sync_dispatch_active = true;
        uint64_t sync_top = (uint64_t)(uintptr_t)(g_lin_sync_kstack +
                                                  sizeof(g_lin_sync_kstack));
        uint64_t saved_rsp;
        __asm__ volatile("mov %%rsp, %0" : "=r"(saved_rsp));
        __asm__ volatile("mov %0, %%rsp" :: "r"(sync_top) : "memory");
        if (setjmp(g_lin_sync_dispatch_jmp) == 0)
            ret = lin_do_run_sync(a0, a1);
        else {
            ret = g_lin_sync_exit_code;
            __asm__ volatile("sti");
            vmm_pd_t *child = g_lin_pd;
            g_lin_pd = g_lin_sync_saved.pd;
            sched_set_current_pd(g_lin_pd);
            vmm_switch(g_lin_pd);
            if (child && child != g_lin_pd)
                vmm_release(child);
            lin_restore_state(&g_lin_sync_saved, NULL);
            g_lin_return_compat32 = 0;
            gdt_set_kernel_stack(
                (uintptr_t)(g_lin_kstack + sizeof(g_lin_kstack)));
            lin_thread_reset();
            debug_printf("[linux] sync run '%s' finished code=%d fault=%u\n",
                         g_lin_sync_child_path, g_lin_sync_exit_code,
                         g_lin_sync_faulted ? 1u : 0u);
            debug_printf("[linux] sync return rip=0x%x rsp=0x%x compat=%u\n",
                         (uint32_t)g_lin_syscall_return_frame.rip,
                         (uint32_t)g_lin_syscall_return_frame.user_rsp,
                         g_lin_return_compat32);
        }
        __asm__ volatile("mov %0, %%rsp" :: "r"(saved_rsp) : "memory");
        g_lin_sync_dispatch_active = false;
        g_lin_syscall_return_frame.rax = (uint64_t)ret;
        g_lin_syscall_frame_override = 1;
        break;
    }
    case LSYS_nexxon_wine_run:
        ret = lin_wine_run_user(a0, r);
        break;
    case LSYS_pe_exit:
        if (!g_lin_pe_running) { ret = -L_EINVAL; break; }
        g_lin_pe_used_win32 = true;
        lin_pe_guest_exit((int)(uint32_t)a0);
        break;
    case LSYS_socket:
        ret = do_socket((int)a0, (int)a1, (int)a2);
        break;
    case LSYS_connect:
        ret = do_sock_connect((int)a0, a1, (int)a2);
        break;
    case LSYS_bind:
        ret = do_sock_bind((int)a0, a1, (int)a2);
        break;
    case LSYS_sendto:
        ret = do_sendto((int)a0, a1, a2, (int)a3, a4, (int)a5);
        break;
    case LSYS_recvfrom:
        ret = do_recvfrom((int)a0, a1, a2, (int)a3, a4, a5);
        break;
    case LSYS_sendmsg:
        ret = do_msg((int)a0, a1, true);
        break;
    case LSYS_recvmsg:
        ret = do_msg((int)a0, a1, false);
        break;
    case LSYS_shutdown:
        if ((int)a0 < 0 || (int)a0 >= LIN_FD_MAX ||
            g_lin_fds[(int)a0].kind != LFD_SOCK) {
            ret = -L_EBADF;
        } else if (g_lin_fds[(int)a0].sock.kind == LSK_STREAM &&
                   g_lin_fds[(int)a0].sock.tcp != TCP_INVALID) {
            tcp_close(g_lin_fds[(int)a0].sock.tcp);
            g_lin_fds[(int)a0].sock.tcp = TCP_INVALID;
            ret = 0;
        } else {
            ret = 0;
        }
        break;
    case LSYS_listen:
        ret = do_sock_listen((int)a0, (int)a1);
        break;
    case LSYS_accept:
        ret = do_sock_accept((int)a0, a1, a2);
        break;
    case LSYS_getsockname:
        ret = do_sock_getsockname((int)a0, a1, a2);
        break;
    case LSYS_getpeername:
    case LSYS_setsockopt:
    case LSYS_getsockopt:
        ret = -L_EOPNOTSUPP;
        break;
    case LSYS_socketpair:
        ret = do_socketpair((int)a0, (int)a1, (int)a2, a3);
        break;
    case LSYS_fork:
    case LSYS_clone:
        if (n == LSYS_clone && (a0 & 0x00010000ULL)) /* CLONE_THREAD */
            ret = lin_thread_clone(a0, a1, a2, a3, a4, r);
        else
            ret = lin_do_fork(r);
        break;
    case LSYS_execve:
        ret = lin_do_execve(a0, a1, a2, r);
        break;
    case LSYS_wait4: {
        lin_pid_t want = (lin_pid_t)(int64_t)a0;
        if (!g_lin_fork_child_done) {
            ret = -L_ECHILD;
            break;
        }
        if (want > 0 && want != g_lin_fork_child_pid) {
            ret = -L_ECHILD;
            break;
        }
        if (a1 && uptr_write_ok(a1, 4))
            *(int *)(uintptr_t)a1 = g_lin_fork_child_status;
        ret = (long)g_lin_fork_child_pid;
        g_lin_fork_child_done = false;
        break;
    }
    case LSYS_getuid: case LSYS_geteuid:
    case LSYS_getgid: case LSYS_getegid:
        ret = 0;
        break;
    case LSYS_sched_yield:
        lin_thread_yield(r, &switched);
        if (switched) return;
        __asm__ volatile("sti; hlt");
        ret = 0;
        break;
    case LSYS_sched_getaffinity:
        if (a1 < 8 || !uptr_write_ok(a2, a1)) ret = -L_EINVAL;
        else { memset((void *)(uintptr_t)a2, 0, a1);
               *(uint64_t *)(uintptr_t)a2 = 1; ret = 8; }
        break;
    case LSYS_futex: {
        if (!uptr_ok(a0, 4)) { ret = -L_EFAULT; break; }
        uint32_t op = (uint32_t)a1 & 0x7Fu;
        if (op == 1) {
            ret = lin_thread_futex_wake(a0, (int)a2);
        } else if (op == 0) {
            uint64_t timeout_ns = 0;
            if (a3) {
                uint64_t ts[2];
                if (user_copy_from(g_lin_pd, ts, a3, 16) != 0) {
                    ret = -L_EFAULT;
                    break;
                }
                timeout_ns = ts[0] * 1000000000ull + ts[1];
            }
            ret = lin_thread_futex_wait(a0, (uint32_t)a2, timeout_ns, r,
                                        &switched);
            if (ret == LIN_THREAD_SWITCHED) return;
            if (ret == -110) ret = -L_ETIMEDOUT;
        } else {
            ret = -L_ENOSYS;
        }
        break;
    }
    case LSYS_rseq:
        ret = -L_ENOSYS;                            /* libc's optional fast path */
        break;

    case LSYS_uname: {
        char *u = (char *)(uintptr_t)a0;
        if (!uptr_write_ok(a0, 6 * 65)) { ret = -L_EFAULT; break; }
        memset(u, 0, 6 * 65);
        strcpy(u + 0 * 65, "Linux");
        strcpy(u + 1 * 65, "nexxon");
        strcpy(u + 2 * 65, "6.1.0-nexxon");
        strcpy(u + 3 * 65, "NexxoN Linux ABI v34");
        strcpy(u + 4 * 65, "x86_64");
        ret = 0;
        break;
    }
    case LSYS_clock_gettime:
        ret = do_clock_gettime(a1);
        break;
    case LSYS_gettimeofday:
        if (!a0) ret = 0;
        else if (!uptr_write_ok(a0, 16)) ret = -L_EFAULT;
        else {
            uint64_t *tv = (uint64_t *)(uintptr_t)a0;
            uint32_t ms = pit_ms();
            tv[0] = ms / 1000u;
            tv[1] = (uint64_t)(ms % 1000u) * 1000ull;
            ret = 0;
        }
        break;
    case LSYS_time: {
        uint64_t sec = pit_ms() / 1000u;
        if (a0) {
            if (!uptr_write_ok(a0, 8)) { ret = -L_EFAULT; break; }
            *(uint64_t *)(uintptr_t)a0 = sec;
        }
        ret = (long)sec;
        break;
    }
    case LSYS_nanosleep:
        ret = do_nanosleep(a0, a1);
        break;
    case LSYS_getrandom: {
        if (!uptr_write_ok(a0, a1)) { ret = -L_EFAULT; break; }
        uint8_t *b = (uint8_t *)(uintptr_t)a0;
        uint32_t seed = pit_ms() ^ (uint32_t)(uintptr_t)b ^ 0x9E3779B9u;
        for (uint64_t i = 0; i < a1; i++) {
            seed = seed * 1103515245u + 12345u;
            b[i] = (uint8_t)(seed >> 16);
        }
        ret = (long)a1;
        break;
    }
    case LSYS_getrlimit:
    case LSYS_prlimit64: {
        uint64_t ulimit = n == LSYS_getrlimit ? a1 : a3;
        if (!ulimit) { ret = 0; break; }
        if (!uptr_write_ok(ulimit, 16)) { ret = -L_EFAULT; break; }
        uint64_t *lim = (uint64_t *)(uintptr_t)ulimit;
        lim[0] = ~0ULL; lim[1] = ~0ULL;
        if ((n == LSYS_getrlimit ? a0 : a1) == 3) { /* RLIMIT_STACK */
            lim[0] = LIN_STACK_BYTES; lim[1] = LIN_STACK_BYTES;
        }
        ret = 0;
        break;
    }

    case LSYS_exit:
    case LSYS_exit_group:
        if (g_lin_pe_running) {
            lin_pe_guest_exit((int)(uint8_t)a0);
            return;
        }
        if (g_lin_sync_running) {
            lin_sync_child_exit((int)(uint8_t)a0);
            return;
        }
        if (lin_thread_try_exit(r, (int)(uint8_t)a0, n == LSYS_exit_group)) {
            switched = true;
            return;
        }
        if (g_lin_fork_child) {
            lin_fork_child_exit(r, (int)(uint8_t)a0);
            return;
        }
        g_lin_exit_code = (int)(uint8_t)a0;
        debug_printf("[linux] exit(%d)\n", g_lin_exit_code);
        lin_thread_reset();
        longjmp(g_lin_return, 1);
        break;
    default:
        debug_printf("[linux] unimplemented syscall %u (a0=0x%x)\n",
                     (uint32_t)n, (uint32_t)a0);
        ret = -L_ENOSYS;
        break;
    }
    if (!switched)
        r->rax = (uint64_t)ret;
}

/* Recover a CPL=3 guest fault as a process exit instead of panicking the OS.
 * Returns false when no Linux compatibility process is active. */
static bool lin_linux_compat_active(vmm_pd_t *owner_pd) {
    if (!owner_pd || owner_pd != g_lin_pd) return false;
    return g_lin_running || g_lin_sync_running || g_lin_fork_child ||
           g_lin_pe_running;
}

bool linux_handle_user_exception_frame(registers_t *frame,
                                       uint64_t vector, uint64_t err,
                                       uint64_t rip) {
    tcb_t *owner = sched_current();
    if (!lin_linux_compat_active(owner ? owner->pd : NULL)) return false;

    if (lin_try_page_fault(vector, err))
        return true;

    int sig = (vector == 6) ? 4 : (vector == 0 ? 8 : 11);

    if (g_lin_pe_running) {
        if (!g_lin_pe_used_win32)
            g_lin_pe_exit_code = 128 + sig;
        debug_printf("[linux] wine PE fault vec=%u rip=0x%llx -> exit=%d\n",
                     (uint32_t)vector, (unsigned long long)rip,
                     g_lin_pe_exit_code);
        longjmp(g_lin_pe_return, 1);
        return true;
    }

    if (g_lin_sync_running) {
        g_lin_sync_exit_code = 128 + sig;
        g_lin_sync_faulted = true;
        uintptr_t fault_cr2 = 0;
        if (vector == 14)
            __asm__ volatile("mov %%cr2, %0" : "=r"(fault_cr2));
        debug_printf("[linux] sync run fault vec=%u err=0x%x rip=0x%x "
                     "cr2=%p -> exit=%d\n",
                     (uint32_t)vector, (uint32_t)err, (uint32_t)rip,
                     (void *)fault_cr2, g_lin_sync_exit_code);
        g_lin_sync_running = false;
        if (g_lin_sync_dispatch_active)
            longjmp(g_lin_sync_dispatch_jmp, 1);
        return true;
    }

    if (g_lin_fork_child) {
        g_lin_fork_child_status = ((128 + sig) & 0xff) << 8;
        g_lin_fork_child_done = true;
        g_lin_fork_child = false;
        if (g_lin_pd && g_lin_fork_parent_pd && g_lin_pd != g_lin_fork_parent_pd) {
            vmm_release(g_lin_pd);
            g_lin_pd = g_lin_fork_parent_pd;
            sched_set_current_pd(g_lin_pd);
            vmm_switch(g_lin_pd);
        }
        g_lin_pid = g_lin_fork_parent_pid;
        lin_regs_t *p = &g_lin_fork_parent_regs;
        frame->rax = (uint64_t)g_lin_fork_child_pid;
        frame->rbx = p->rbx;
        frame->rcx = p->rcx;
        frame->rdx = p->rdx;
        frame->rsi = p->rsi;
        frame->rdi = p->rdi;
        frame->rbp = p->rbp;
        frame->r8  = p->r8;
        frame->r9  = p->r9;
        frame->r10 = p->r10;
        frame->r11 = p->r11;
        frame->r12 = p->r12;
        frame->r13 = p->r13;
        frame->r14 = p->r14;
        frame->r15 = p->r15;
        frame->rip = p->rip;
        frame->rsp = p->user_rsp;
        frame->rflags = p->rflags;
        debug_printf("[linux] fork child fault vec=%u err=0x%x rip=0x%x "
                     "-> parent resumes pid=%u\n",
                     (uint32_t)vector, (uint32_t)err, (uint32_t)rip,
                     (unsigned)g_lin_pid);
        return true;
    }

    g_lin_exit_code = 128 + sig;
    g_lin_faulted = true;
    debug_printf("[linux] guest exception vec=%u err=0x%x rip=0x%x -> exit=%d\n",
                 (uint32_t)vector, (uint32_t)err, (uint32_t)rip,
                 g_lin_exit_code);
    if (g_lin_private_cr3) {
        sched_set_current_pd(vmm_kernel_pd());
        vmm_switch(vmm_kernel_pd());
        g_lin_private_cr3 = false;
    }
    __asm__ volatile("sti");
    longjmp(g_lin_return, 1);
    return true;
}

/* ---- One-time MSR setup ------------------------------------------------ */
void linux_syscall_init(void) {
    g_lin_kstack_top =
        (uint64_t)(uintptr_t)(g_lin_kstack + sizeof(g_lin_kstack));
    wrmsr(MSR_STAR, ((uint64_t)0x08 << 32) | ((uint64_t)0x1B << 48));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)linux_syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);                       /* clear IF on entry */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1u);
    debug_printf("[linux] SYSCALL MSRs armed (LSTAR=%p)\n",
                 linux_syscall_entry);
}

/* ---- Linux initial stack ------------------------------------------------ */
static uint64_t stack_put(uint64_t top, const void *data, uint32_t len) {
    uint64_t p = top;
    if (p < LIN_STACK_BASE + len + 4096) return 0;
    p -= len;
    if (user_copy_to(g_lin_pd, p, data, len) != 0) return 0;
    return p;
}

static uint64_t build_init_stack(const elf64_image_t *exec,
                                 const elf64_image_t *interp, bool dynamic,
                                 const char *exec_path,
                                 int argc, const char *const *argv) {
    if (!exec || argc < 1 || argc > LIN_MAX_ARGS) return 0;
    uint64_t top = LIN_STACK_TOP;
    uint64_t argp[LIN_MAX_ARGS];

    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        if (len > LIN_PATH_MAX) return 0;
        top = stack_put(top, argv[i], len);
        if (!top) return 0;
        argp[i] = (uint64_t)(uintptr_t)top;
    }

    const char *lang = i18n_get_language() == LANG_HU
                     ? "LANG=hu_HU.UTF-8" : "LANG=en_US.UTF-8";
    top = stack_put(top, lang, (uint32_t)strlen(lang) + 1);
    if (!top) return 0;
    uint64_t env_lang = top;
    static const char term[] = "TERM=nexxon";
    top = stack_put(top, term, sizeof(term));
    if (!top) return 0;
    uint64_t env_term = top;
    static const char home[] = "HOME=/home/admin";
    top = stack_put(top, home, sizeof(home));
    if (!top) return 0;
    uint64_t env_home = top;
    static const char user[] = "USER=admin";
    top = stack_put(top, user, sizeof(user));
    if (!top) return 0;
    uint64_t env_user = top;
    static const char path[] = "PATH=/bin:/programs:/usr/bin";
    top = stack_put(top, path, sizeof(path));
    if (!top) return 0;
    uint64_t env_path = top;
    static const char steamroot[] = "STEAMROOT=/programs";
    top = stack_put(top, steamroot, sizeof(steamroot));
    if (!top) return 0;
    uint64_t env_steamroot = top;
    static const char steamexe[] = "STEAMEXE=steam";
    top = stack_put(top, steamexe, sizeof(steamexe));
    if (!top) return 0;
    uint64_t env_steamexe = top;
    static const char steamruntime[] = "STEAM_RUNTIME=0";
    top = stack_put(top, steamruntime, sizeof(steamruntime));
    if (!top) return 0;
    uint64_t env_steamruntime = top;
    static const char platform[] = "x86_64";
    top = stack_put(top, platform, sizeof(platform));
    if (!top) return 0;
    uint64_t platform_p = top;

    uint64_t execfn_p = 0;
    if (exec_path && exec_path[0]) {
        uint32_t elen = (uint32_t)strlen(exec_path) + 1;
        top = stack_put(top, exec_path, elen);
        if (!top) return 0;
        execfn_p = top;
    }

    uint8_t random[16];
    uint32_t seed = pit_ms() ^ 0xA5C31F27u;
    for (int i = 0; i < 16; i++) {
        seed = seed * 1664525u + 1013904223u;
        random[i] = (uint8_t)(seed >> 24);
    }
    top = stack_put(top, random, sizeof(random));
    if (!top) return 0;
    uint64_t random_p = top;

    const uint64_t aux[][2] = {
        { 3, exec->phdr }, { 4, exec->phent }, { 5, exec->phnum },
        { 6, 4096 },
        { 7, dynamic && interp ? interp->base : 0 },
        { 9, exec->entry },
        { 11, 0 }, { 12, 0 }, { 13, 0 }, { 14, 0 },
        { 15, platform_p }, { 16, LIN_HWCAP_X86 }, { 17, 100 },
        { 23, 0 }, { 25, random_p }, { 26, LIN_HWCAP2_X86 },
        { 31, execfn_p ? execfn_p : argp[0] },
        { 0, 0 },
    };
    uint32_t words = 1u + (uint32_t)argc + 1u + 8u + 1u +
                     (uint32_t)(sizeof(aux) / sizeof(aux[0])) * 2u;
    uint64_t sp = (top - (uint64_t)words * 8u) & ~0xFULL;
    if (sp < LIN_STACK_BASE + 4096) return 0;
    uint64_t init[LIN_MAX_ARGS + 48];
    uint64_t *w = init;
    *w++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++) *w++ = argp[i];
    *w++ = 0;
    *w++ = env_term;
    *w++ = env_lang;
    *w++ = env_home;
    *w++ = env_user;
    *w++ = env_path;
    *w++ = env_steamroot;
    *w++ = env_steamexe;
    *w++ = env_steamruntime;
    *w++ = 0;
    for (uint32_t i = 0; i < sizeof(aux) / sizeof(aux[0]); i++) {
        *w++ = aux[i][0];
        *w++ = aux[i][1];
    }
    uint32_t bytes = (uint32_t)((uint8_t *)w - (uint8_t *)init);
    if (user_copy_to(g_lin_pd, sp, init, bytes) != 0) return 0;
    return sp;
}

static bool linux_install_file(uint32_t dir, const char *name,
                               const uint8_t *start, const uint8_t *end) {
    uint32_t ino;
    if (nxfs_resolve(dir, name, &ino) != NXFS_OK &&
        nxfs_create_file(dir, name, &ino) != NXFS_OK)
        return false;
    return nxfs_write_file(ino, start, (uint32_t)(end - start)) == NXFS_OK;
}

static uint32_t lin_ensure_child_dir(uint32_t parent, const char *name) {
    uint32_t ino;
    if (nxfs_resolve(parent, name, &ino) == NXFS_OK) return ino;
    if (nxfs_create_dir(parent, name, &ino) == NXFS_OK) return ino;
    return 0;
}

static uint32_t lin_programs_dir(void) {
    return lin_ensure_child_dir(0, "programs");
}

static uint32_t lin_usr_bin_dir(void) {
    uint32_t usr = lin_ensure_child_dir(0, "usr");
    if (!usr) return 0;
    return lin_ensure_child_dir(usr, "bin");
}

static uint32_t lin_wine_demo_dir(void) {
    uint32_t usr = lin_ensure_child_dir(0, "usr");
    if (!usr) return 0;
    uint32_t share = lin_ensure_child_dir(usr, "share");
    if (!share) return 0;
    uint32_t wine = lin_ensure_child_dir(share, "wine");
    if (!wine) return 0;
    return lin_ensure_child_dir(wine, "demo");
}

typedef struct {
    const char *pkg;
    const uint8_t *start;
    const uint8_t *end;
    const char *dest_name;
    int dest;
} lin_pkg_entry_t;

static long lin_steam_write_text(uint32_t dir, const char *name,
                                 const char *text);

static void linux_seed_resolv_conf(void) {
    uint32_t etc = lin_ensure_child_dir(0, "etc");
    if (!etc) return;
    net_config_t nc;
    net_get_config(&nc);
    uint32_t dns = nc.dns ? nc.dns : nc.gateway;
    char ns[32];
    if (dns)
        net_ip_ntoa(dns, ns, sizeof(ns));
    else
        ksnprintf(ns, sizeof(ns), "127.0.0.1");
    char buf[128];
    ksnprintf(buf, sizeof(buf), "nameserver %s\nsearch .\n", ns);
    (void)lin_steam_write_text(etc, "resolv.conf", buf);
    debug_printf("[linux] seeded /etc/resolv.conf (nameserver %s)\n", ns);
}

static void linux_seed_hosts(void) {
    uint32_t etc = lin_ensure_child_dir(0, "etc");
    if (!etc) return;
    net_config_t nc;
    net_get_config(&nc);
    char gw[32] = "127.0.0.1";
    if (nc.gateway)
        net_ip_ntoa(nc.gateway, gw, sizeof(gw));
    char buf[256];
    ksnprintf(buf, sizeof(buf),
              "127.0.0.1 localhost\n"
              "::1 ip6-localhost ip6-loopback\n"
              "# NexxoN apt mirror alias (QEMU user-net gateway)\n"
              "%s apt.nexxon\n",
              gw);
    (void)lin_steam_write_text(etc, "hosts", buf);
    net_hosts_reload();
    debug_printf("[linux] seeded /etc/hosts (apt.nexxon -> %s)\n", gw);
}

static void linux_seed_ssl_certs(void) {
    static const char ca_pem[] =
        "-----BEGIN CERTIFICATE-----\n"
        "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
        "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
        "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
        "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
        "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
        "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
        "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
        "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
        "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
        "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
        "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
        "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
        "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
        "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
        "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
        "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
        "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
        "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
        "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
        "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
        "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
        "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
        "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
        "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
        "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
        "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
        "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
        "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
        "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
        "-----END CERTIFICATE-----\n";
    uint32_t etc = lin_ensure_child_dir(0, "etc");
    if (!etc) return;
    uint32_t ssl = lin_ensure_child_dir(etc, "ssl");
    if (!ssl) return;
    uint32_t certs = lin_ensure_child_dir(ssl, "certs");
    if (!certs) return;
    (void)lin_steam_write_text(certs, "ca-certificates.crt", ca_pem);
    debug_printf("[linux] seeded /etc/ssl/certs/ca-certificates.crt\n");
}

static long lin_steam_cdn_fetch(void) {
    int got = download_simple("http://apt.nexxon:8000/steam-cdn-probe.html",
                              g_lin_file_image, LIN_IMAGE_BYTES);
    if (got <= 0) {
        got = download_simple("https://client-update.steamstatic.com/",
                              g_lin_file_image, LIN_IMAGE_BYTES);
        if (got <= 0) {
            debug_printf("[linux] steam CDN fetch failed\n");
            return -L_EIO;
        }
        debug_printf("[linux] steam CDN fetch via HTTPS (%d bytes)\n", got);
    } else {
        debug_printf("[linux] steam CDN fetch via apt mirror (%d bytes)\n", got);
    }
    debug_printf("[linux] steam CDN fetch ok (%d bytes) NEXXON_STEAM_CDN_OK\n",
                 got);

    uint32_t programs = lin_programs_dir();
    if (!programs) return 0;
    uint32_t platform = lin_ensure_child_dir(programs, "ubuntu12_32");
    if (!platform) return 0;
    uint32_t pkgdir = lin_ensure_child_dir(platform, "package");
    if (!pkgdir) return 0;

    long wr = lin_steam_write_text(pkgdir, "steam_client_ubuntu12.installed",
                                   "installed\n");
    if (wr < 0) return wr;

    if (got > 256) got = 256;
    if (!linux_install_file(pkgdir, "cdn_probe.html",
                            g_lin_file_image, g_lin_file_image + got))
        return -L_EIO;
    debug_printf("[linux] steam depot probe staged (%d bytes)\n", got);
    return 0;
}

static bool g_lin_steam_cdn_cached;

static void lin_steam_ensure_cdn(void) {
    if (g_lin_steam_cdn_cached)
        return;
    uint32_t programs = lin_programs_dir();
    uint32_t platform = programs
        ? lin_ensure_child_dir(programs, "ubuntu12_32") : 0;
    uint32_t pkgdir = platform
        ? lin_ensure_child_dir(platform, "package") : 0;
    uint32_t ino = 0;
    if (pkgdir &&
        nxfs_resolve(pkgdir, "steam_client_ubuntu12.installed", &ino) != NXFS_OK) {
        long sr = lin_apt_install_bundle(pkgdir, "steam-client-seed.bundle",
                                         "NXSTMCL1");
        if (sr < 0)
            (void)lin_steam_write_text(pkgdir, "steam_client_ubuntu12.installed",
                                       "installed\n");
    }
    if (platform &&
        nxfs_resolve(platform, "update_hosts_cached.vdf", &ino) != NXFS_OK) {
        (void)lin_steam_write_text(platform, "update_hosts_cached.vdf",
            "\"hosts\"\n{\n}\n");
    }
    /* The bootstrapper's ILocalize hard-requires
     * STEAMROOT/public/steambootstrapper_english.txt; a missing file left a
     * NULL localize table and crashed the updater (CR2=0x11).  Unknown
     * tokens render as their own names, so an empty table is safe. */
    uint32_t pubdir = programs ? lin_ensure_child_dir(programs, "public") : 0;
    if (pubdir &&
        nxfs_resolve(pubdir, "steambootstrapper_english.txt", &ino) != NXFS_OK) {
        (void)lin_steam_write_text(pubdir, "steambootstrapper_english.txt",
            "\"lang\"\n{\n\"Language\" \"english\"\n\"Tokens\"\n{\n}\n}\n");
    }
    if (nxfs_resolve_path("/programs/ubuntu12_32/package/cdn_probe.html",
                          &ino) == NXFS_OK) {
        g_lin_steam_cdn_cached = true;
        debug_printf("[linux] steam CDN fetch ok (cached) NEXXON_STEAM_CDN_OK\n");
        return;
    }
    if (lin_steam_cdn_fetch() == 0)
        g_lin_steam_cdn_cached = true;
}

static void lin_steam_api_probe(void) {
    const char *url =
        "https://api.steampowered.com/ISteamWebAPI/GetServerInfo/v1/";
    int got = download_simple(url, g_lin_file_image, 4096);
    if (got > 0)
        debug_printf("[linux] steam API HTTPS ok (%d bytes) NEXXON_STEAM_API_OK\n",
                     got);
    else
        debug_printf("[linux] steam API HTTPS probe skipped\n");
}

static bool lin_steam_graphical_enabled(void) {
    /* Explicit opt-in marker only: the gfx shim bundle is also staged for
     * console-mode installs (the bootstrap hard-links libX11), so library
     * presence must not flip the UI mode. */
    uint32_t ino;
    return nxfs_resolve_path("/programs/.steam_graphical", &ino) == NXFS_OK;
}

static long lin_steam_install_platform_gfx(uint32_t platform) {
    if (!platform) return -L_EINVAL;
    long gx = lin_apt_install_bundle(platform, "i386-steam-gfx.bundle",
                                     "NXI386G1");
    if (gx < 0)
        debug_printf("[linux] steam i386 gfx bundle skipped (%ld)\n", gx);
    long wh = lin_apt_fetch_into_dir(platform, "steamwebstub.elf",
                                     "steamwebhelper");
    if (wh < 0)
        debug_printf("[linux] steamwebhelper stub skipped (%ld)\n", wh);
    (void)lin_ensure_child_dir(0, "run");
    uint32_t runuser = lin_ensure_child_dir(lin_ensure_child_dir(0, "run"), "user");
    if (runuser)
        (void)lin_ensure_child_dir(runuser, "0");
    return gx;
}

static long lin_steam_write_text(uint32_t dir, const char *name,
                                 const char *text) {
    if (!dir || !name || !text) return -L_EINVAL;
    const uint8_t *start = (const uint8_t *)text;
    uint32_t len = (uint32_t)strlen(text);
    if (!linux_install_file(dir, name, start, start + len))
        return -L_EIO;
    return 0;
}

static long lin_steam_seed_client(void) {
    uint32_t programs = lin_programs_dir();
    if (!programs) return -L_ENOSPC;
    uint32_t platform = lin_ensure_child_dir(programs, "ubuntu12_32");
    if (!platform) return -L_ENOSPC;
    uint32_t pkgdir = lin_ensure_child_dir(platform, "package");
    if (!pkgdir) return -L_ENOSPC;

    (void)lin_steam_install_platform_gfx(platform);

    long sr = lin_apt_install_bundle(pkgdir, "steam-client-seed.bundle",
                                     "NXSTMCL1");
    if (sr < 0) {
        long wr = lin_steam_write_text(pkgdir, "steam_client_ubuntu12.installed",
                                       "installed\n");
        if (wr < 0) return wr;
    }

    uint32_t home = lin_ensure_child_dir(0, "home");
    uint32_t admin = home ? lin_ensure_child_dir(home, "admin") : 0;
    uint32_t dotsteam = admin ? lin_ensure_child_dir(admin, ".steam") : 0;
    uint32_t steamdir = dotsteam ? lin_ensure_child_dir(dotsteam, "steam") : 0;
    if (steamdir) {
        long wr = lin_steam_write_text(steamdir, "local.vdf",
            "\"UserLocalConfigStore\"\n{\n}\n");
        if (wr < 0) return wr;
    }

    long gr = lin_steam_write_text(programs, ".steam_graphical", "1\n");
    if (gr < 0) return gr;

    debug_printf("[linux] steam client seed complete NEXXON_STEAM_CLIENT_OK\n");
    return 0;
}

static long lin_apt_install_one(const lin_pkg_entry_t *e) {
    uint32_t dir = 0;
    if (e->dest == LIN_PKG_PROGRAMS)
        dir = lin_programs_dir();
    else if (e->dest == LIN_PKG_USR_BIN)
        dir = lin_usr_bin_dir();
    if (!dir) return -L_ENOSPC;
    if (!linux_install_file(dir, e->dest_name, e->start, e->end))
        return -L_EIO;
    debug_printf("[linux] apt installed %s -> %u bytes\n",
                 e->dest_name, (uint32_t)(e->end - e->start));
    return 0;
}

static long lin_apt_install(const char *pkg) {
    if (!pkg || !pkg[0]) return -L_EINVAL;

    long embedded = -L_ENOENT;

    if (strcmp(pkg, "linuxnet") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxnet",
            .start = _binary_build_userland_linuxnet_elf_start,
            .end = _binary_build_userland_linuxnet_elf_end,
            .dest_name = "linuxnet",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxpthread") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxpthread",
            .start = _binary_build_userland_linuxpthread_elf_start,
            .end = _binary_build_userland_linuxpthread_elf_end,
            .dest_name = "linuxpthread",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxepoll") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxepoll",
            .start = _binary_build_userland_linuxepoll_elf_start,
            .end = _binary_build_userland_linuxepoll_elf_end,
            .dest_name = "linuxepoll",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxglibc") == 0) {
        uint32_t lib = lin_ensure_child_dir(0, "lib");
        if (!lib) return -L_ENOSPC;
        uint32_t triplet = lin_ensure_child_dir(lib, "x86_64-linux-gnu");
        if (!triplet) return -L_ENOSPC;
        long gr = lin_apt_install_bundle(triplet, "glibc-libs.bundle",
                                         "NXGLIBC1");
        if (gr < 0) return gr;
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxglibc",
            .start = _binary_build_userland_linuxglibc_elf_start,
            .end = _binary_build_userland_linuxglibc_elf_end,
            .dest_name = "linuxglibc",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxdri") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxdri",
            .start = _binary_build_userland_linuxdri_elf_start,
            .end = _binary_build_userland_linuxdri_elf_end,
            .dest_name = "linuxdri",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxalsa") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxalsa",
            .start = _binary_build_userland_linuxalsa_elf_start,
            .end = _binary_build_userland_linuxalsa_elf_end,
            .dest_name = "linuxalsa",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxx11") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxx11",
            .start = _binary_build_userland_linuxx11_elf_start,
            .end = _binary_build_userland_linuxx11_elf_end,
            .dest_name = "linuxx11",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxxlib") == 0) {
        uint32_t lib = lin_ensure_child_dir(0, "lib");
        if (!lib) return -L_ENOSPC;
        uint32_t triplet = lin_ensure_child_dir(lib, "x86_64-linux-gnu");
        if (!triplet) return -L_ENOSPC;
        uint32_t existing;
        if (nxfs_resolve(triplet, "ld-linux-x86-64.so.2", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-ld.so",
                                             "ld-linux-x86-64.so.2");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libc.so.6", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-libc.so",
                                             "libc.so.6");
            if (lr < 0) return lr;
        }
        long lr = lin_apt_install_bundle(triplet, "x11-libs.bundle",
                                         "NXXLIB1");
        if (lr < 0) return lr;
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxxlib",
            .start = _binary_build_userland_linuxxlib_elf_start,
            .end = _binary_build_userland_linuxxlib_elf_end,
            .dest_name = "linuxxlib",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxxevent") == 0) {
        uint32_t lib = lin_ensure_child_dir(0, "lib");
        if (!lib) return -L_ENOSPC;
        uint32_t triplet = lin_ensure_child_dir(lib, "x86_64-linux-gnu");
        if (!triplet) return -L_ENOSPC;
        uint32_t existing;
        if (nxfs_resolve(triplet, "ld-linux-x86-64.so.2", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-ld.so",
                                             "ld-linux-x86-64.so.2");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libc.so.6", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-libc.so",
                                             "libc.so.6");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libX11.so.6", &existing) != NXFS_OK) {
            long lr = lin_apt_install_bundle(triplet, "x11-libs.bundle",
                                             "NXXLIB1");
            if (lr < 0) return lr;
        }
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxxevent",
            .start = _binary_build_userland_linuxxevent_elf_start,
            .end = _binary_build_userland_linuxxevent_elf_end,
            .dest_name = "linuxxevent",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxvulkan") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxvulkan",
            .start = _binary_build_userland_linuxvulkan_elf_start,
            .end = _binary_build_userland_linuxvulkan_elf_end,
            .dest_name = "linuxvulkan",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxvulkanso") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxvulkanso",
            .start = _binary_build_userland_linuxvulkanso_elf_start,
            .end = _binary_build_userland_linuxvulkanso_elf_end,
            .dest_name = "linuxvulkanso",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxvkloader") == 0) {
        uint32_t lib = lin_ensure_child_dir(0, "lib");
        if (!lib) return -L_ENOSPC;
        uint32_t triplet = lin_ensure_child_dir(lib, "x86_64-linux-gnu");
        if (!triplet) return -L_ENOSPC;
        uint32_t existing;
        if (nxfs_resolve(triplet, "ld-linux-x86-64.so.2", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-ld.so",
                                             "ld-linux-x86-64.so.2");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libc.so.6", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-libc.so",
                                             "libc.so.6");
            if (lr < 0) return lr;
        }
        long lr = lin_apt_install_bundle(triplet, "vkloader-libs.bundle",
                                         "NXVKLIB1");
        if (lr < 0) return lr;
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxvkloader",
            .start = _binary_build_userland_linuxvkloader_elf_start,
            .end = _binary_build_userland_linuxvkloader_elf_end,
            .dest_name = "linuxvkloader",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxvkinstance") == 0) {
        uint32_t lib = lin_ensure_child_dir(0, "lib");
        if (!lib) return -L_ENOSPC;
        uint32_t triplet = lin_ensure_child_dir(lib, "x86_64-linux-gnu");
        if (!triplet) return -L_ENOSPC;
        uint32_t existing;
        if (nxfs_resolve(triplet, "ld-linux-x86-64.so.2", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-ld.so",
                                             "ld-linux-x86-64.so.2");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libc.so.6", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-libc.so",
                                             "libc.so.6");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libvulkan.so.1", &existing) != NXFS_OK) {
            long lr = lin_apt_install_bundle(triplet, "vkloader-libs.bundle",
                                             "NXVKLIB1");
            if (lr < 0) return lr;
        }
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxvkinstance",
            .start = _binary_build_userland_linuxvkinstance_elf_start,
            .end = _binary_build_userland_linuxvkinstance_elf_end,
            .dest_name = "linuxvkinstance",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxvkqueue") == 0) {
        uint32_t lib = lin_ensure_child_dir(0, "lib");
        if (!lib) return -L_ENOSPC;
        uint32_t triplet = lin_ensure_child_dir(lib, "x86_64-linux-gnu");
        if (!triplet) return -L_ENOSPC;
        uint32_t existing;
        if (nxfs_resolve(triplet, "ld-linux-x86-64.so.2", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-ld.so",
                                             "ld-linux-x86-64.so.2");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libc.so.6", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-libc.so",
                                             "libc.so.6");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libvulkan.so.1", &existing) != NXFS_OK) {
            long lr = lin_apt_install_bundle(triplet, "vkloader-libs.bundle",
                                             "NXVKLIB1");
            if (lr < 0) return lr;
        }
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxvkqueue",
            .start = _binary_build_userland_linuxvkqueue_elf_start,
            .end = _binary_build_userland_linuxvkqueue_elf_end,
            .dest_name = "linuxvkqueue",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxvkcommand") == 0) {
        uint32_t lib = lin_ensure_child_dir(0, "lib");
        if (!lib) return -L_ENOSPC;
        uint32_t triplet = lin_ensure_child_dir(lib, "x86_64-linux-gnu");
        if (!triplet) return -L_ENOSPC;
        uint32_t existing;
        if (nxfs_resolve(triplet, "ld-linux-x86-64.so.2", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-ld.so",
                                             "ld-linux-x86-64.so.2");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libc.so.6", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-libc.so",
                                             "libc.so.6");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libvulkan.so.1", &existing) != NXFS_OK) {
            long lr = lin_apt_install_bundle(triplet, "vkloader-libs.bundle",
                                             "NXVKLIB1");
            if (lr < 0) return lr;
        }
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxvkcommand",
            .start = _binary_build_userland_linuxvkcommand_elf_start,
            .end = _binary_build_userland_linuxvkcommand_elf_end,
            .dest_name = "linuxvkcommand",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxvksurface") == 0) {
        uint32_t lib = lin_ensure_child_dir(0, "lib");
        if (!lib) return -L_ENOSPC;
        uint32_t triplet = lin_ensure_child_dir(lib, "x86_64-linux-gnu");
        if (!triplet) return -L_ENOSPC;
        uint32_t existing;
        if (nxfs_resolve(triplet, "ld-linux-x86-64.so.2", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-ld.so",
                                             "ld-linux-x86-64.so.2");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libc.so.6", &existing) != NXFS_OK) {
            long lr = lin_apt_fetch_into_dir(triplet, "glibc-libc.so",
                                             "libc.so.6");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libX11.so.6", &existing) != NXFS_OK) {
            long lr = lin_apt_install_bundle(triplet, "x11-libs.bundle",
                                             "NXXLIB1");
            if (lr < 0) return lr;
        }
        if (nxfs_resolve(triplet, "libvulkan.so.1", &existing) != NXFS_OK) {
            long lr = lin_apt_install_bundle(triplet, "vkloader-libs.bundle",
                                             "NXVKLIB1");
            if (lr < 0) return lr;
        }
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxvksurface",
            .start = _binary_build_userland_linuxvksurface_elf_start,
            .end = _binary_build_userland_linuxvksurface_elf_end,
            .dest_name = "linuxvksurface",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxvkswapchain") == 0) {
        long dep = lin_apt_install("linuxvksurface");
        if (dep < 0) return dep;
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxvkswapchain",
            .start = _binary_build_userland_linuxvkswapchain_elf_start,
            .end = _binary_build_userland_linuxvkswapchain_elf_end,
            .dest_name = "linuxvkswapchain",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxpulse") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxpulse",
            .start = _binary_build_userland_linuxpulse_elf_start,
            .end = _binary_build_userland_linuxpulse_elf_end,
            .dest_name = "linuxpulse",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linux32") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linux32",
            .start = _binary_build_userland_linux32_elf_start,
            .end = _binary_build_userland_linux32_elf_end,
            .dest_name = "linux32",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linux32glibc") == 0) {
        uint32_t lib = lin_ensure_child_dir(0, "lib");
        uint32_t lib32 = lin_ensure_child_dir(0, "lib32");
        if (!lib || !lib32) return -L_ENOSPC;
        uint32_t existing;
        if (nxfs_resolve_path("/lib32/libc.so.6", &existing) != NXFS_OK) {
            long lr = lin_apt_install_bundle(lib32, "i386-libs.bundle",
                                             "NXI386R1");
            if (lr < 0) return lr;
            uint32_t ld_len = 0;
            if (linux_read_nxfs("/lib32/ld-linux.so.2", g_lin_aux_image,
                                LIN_IMAGE_BYTES, &ld_len) != 0 ||
                !linux_install_file(lib, "ld-linux.so.2", g_lin_aux_image,
                                    g_lin_aux_image + ld_len))
                return -L_EIO;
        }
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linux32glibc",
            .start = _binary_build_userland_linux32glibc_elf_start,
            .end = _binary_build_userland_linux32glibc_elf_end,
            .dest_name = "linux32glibc",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linux32pie") == 0) {
        long dep = lin_apt_install("linux32glibc");
        if (dep < 0) return dep;
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linux32pie",
            .start = _binary_build_userland_linux32pie_elf_start,
            .end = _binary_build_userland_linux32pie_elf_end,
            .dest_name = "linux32pie",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linux32pthread") == 0) {
        long dep = lin_apt_install("linux32pie");
        if (dep < 0) return dep;
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linux32pthread",
            .start = _binary_build_userland_linux32pthread_elf_start,
            .end = _binary_build_userland_linux32pthread_elf_end,
            .dest_name = "linux32pthread",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "steam") == 0) {
        long dep = lin_apt_install("steam-bootstrap");
        if (dep < 0) return dep;
        dep = lin_steam_cdn_fetch();
        if (dep < 0) return dep;
        dep = lin_steam_seed_client();
        if (dep < 0) return dep;
        lin_steam_api_probe();
        debug_printf("[linux] apt installed steam\n");
        return 0;
    } else if (strcmp(pkg, "steam-bootstrap") == 0) {
        uint32_t programs = lin_programs_dir();
        uint32_t platform = programs
            ? lin_ensure_child_dir(programs, "ubuntu12_32") : 0;
        if (!platform) return -L_ENOSPC;
        long sr = lin_apt_fetch_into_dir(platform, "steam-bootstrap.elf",
                                         "steam");
        if (sr < 0) return sr;
        /* The official bootstrap hard-links libX11/libstdc++; the shim
         * bundle must resolve NEEDED entries even for the console UI. */
        (void)lin_steam_install_platform_gfx(platform);
        debug_printf("[linux] steam CDN prefetch deferred (bootstrap)\n");
        uint32_t steam_len = 0;
        if (linux_read_nxfs("/programs/ubuntu12_32/steam",
                            g_lin_file_image, LIN_IMAGE_BYTES,
                            &steam_len) != 0 ||
            !linux_install_file(programs, "steam", g_lin_file_image,
                                g_lin_file_image + steam_len))
            return -L_EIO;
        long dep = lin_apt_install("linux32pthread");
        if (dep < 0) return dep;
        debug_printf("[linux] apt installed steam-bootstrap\n");
        return 0;
    } else if (strcmp(pkg, "linuxdrmfb") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxdrmfb",
            .start = _binary_build_userland_linuxdrmfb_elf_start,
            .end = _binary_build_userland_linuxdrmfb_elf_end,
            .dest_name = "linuxdrmfb",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxmmap") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxmmap",
            .start = _binary_build_userland_linuxmmap_elf_start,
            .end = _binary_build_userland_linuxmmap_elf_end,
            .dest_name = "linuxmmap",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxdemo") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxdemo",
            .start = _binary_build_userland_linuxdemo_elf_start,
            .end = _binary_build_userland_linuxdemo_elf_end,
            .dest_name = "linuxdemo",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxdyn") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxdyn",
            .start = _binary_build_userland_linuxdyn_elf_start,
            .end = _binary_build_userland_linuxdyn_elf_end,
            .dest_name = "linuxdyn",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "linuxexec") == 0) {
        embedded = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "linuxexec",
            .start = _binary_build_userland_linuxexec_elf_start,
            .end = _binary_build_userland_linuxexec_elf_end,
            .dest_name = "linuxexec",
            .dest = LIN_PKG_PROGRAMS,
        });
    } else if (strcmp(pkg, "wine") == 0) {
        long wr = lin_apt_install_one(&(lin_pkg_entry_t){
            .pkg = "wine",
            .start = _binary_build_userland_winestub_elf_start,
            .end = _binary_build_userland_winestub_elf_end,
            .dest_name = "wine",
            .dest = LIN_PKG_USR_BIN,
        });
        if (wr < 0) return wr;
        uint32_t demo = lin_wine_demo_dir();
        if (!demo) return -L_ENOSPC;
        if (!linux_install_file(demo, "hello.exe",
                                _binary_tools_hello_pe_bin_start,
                                _binary_tools_hello_pe_bin_end))
            return -L_EIO;
        debug_printf("[linux] apt wine: PE demo at /usr/share/wine/demo/hello.exe\n");
        return 0;
    }

    if (embedded == 0) return 0;
    return lin_apt_install_net(pkg);
}

static long lin_apt_install_user(uint64_t op, uint64_t upkg) {
    if (op == 1) return lin_apt_update();
    if (op != 0) return -L_EINVAL;
    char pkg[NXFS_NAME_MAX];
    int pr = copy_user_cstr(upkg, pkg, sizeof(pkg));
    if (pr < 0) return pr;
    return lin_apt_install(pkg);
}

static void linux_seed_lib_if_needed(void) {
    uint32_t lib_ino;
    if (nxfs_resolve_path("/lib/ld-musl-x86_64.so.1", &lib_ino) == NXFS_OK)
        return;
    if (nxfs_resolve(0, "lib", &lib_ino) != NXFS_OK &&
        nxfs_create_dir(0, "lib", &lib_ino) != NXFS_OK)
        return;
    const uint8_t *rt_s = _binary_build_userland_musl_libc_so_start;
    const uint8_t *rt_e = _binary_build_userland_musl_libc_so_end;
    if (!linux_install_file(lib_ino, "ld-musl-x86_64.so.1", rt_s, rt_e))
        return;
    (void)linux_install_file(lib_ino, "libc.so", rt_s, rt_e);
    debug_printf("[linux] seeded /lib musl runtime (%u bytes)\n",
                 (uint32_t)(rt_e - rt_s));
}

static bool lin_interp_is_glibc(const char *path) {
    if (!path) return false;
    const char *needle = "ld-linux";
    size_t nlen = strlen(needle);
    size_t plen = strlen(path);
    if (plen < nlen) return false;
    for (size_t i = 0; i + nlen <= plen; i++) {
        if (memcmp(path + i, needle, nlen) == 0) return true;
    }
    return false;
}

static int linux_read_nxfs(const char *path, uint8_t *buf, uint32_t cap,
                             uint32_t *out_len) {
    if (!path || !buf || !out_len) return -1;
    uint32_t ino;
    if (nxfs_resolve_path(path, &ino) != NXFS_OK) return -1;
    nxfs_inode_t node;
    if (nxfs_read_inode(ino, &node) != NXFS_OK || node.type != NXFS_TYPE_FILE)
        return -1;
    uint64_t size = ((uint64_t)node.size_hi << 32) | node.size;
    if (size == 0 || size > cap) return -1;
    uint64_t off = 0;
    while (off < size) {
        uint32_t chunk = (uint32_t)((size - off) > 65536 ? 65536 : size - off);
        int got = nxfs_read_at(ino, off, buf + off, chunk);
        if (got <= 0) return -1;
        off += (uint32_t)got;
    }
    *out_len = (uint32_t)size;
    return 0;
}

static void lin_dumb_reset_all(void) {
    for (int i = 0; i < LIN_DUMB_SLOTS; i++)
        lin_dumb_destroy_slot(&g_lin_dumbs[i]);
    g_lin_dumb_next_handle = 1;
    g_lin_dumb_next_map_off = LIN_DUMB_MAP_BASE;
}

static void lin_clear_user_maps(void) {
    lin_dumb_reset_all();
    for (int i = 0; i < LIN_MAP_MAX; i++) {
        if (!g_lin_maps[i].active) continue;
        uint64_t addr = g_lin_maps[i].addr;
        uint64_t len = g_lin_maps[i].len;
        for (uint64_t off = 0; off < len; off += VMM_PAGE_SIZE)
            (void)vmm_unmap_page(g_lin_pd, (uintptr_t)(addr + off));
        memset(&g_lin_maps[i], 0, sizeof(g_lin_maps[i]));
    }
}

static uint64_t g_lin_fork_rsp_bias;

static int copy_user_range(uint64_t dst, uint64_t src, uint64_t len) {
    while (len) {
        uint8_t b;
        if (user_copy_from(g_lin_pd, &b, src, 1) != 0) return -1;
        if (user_copy_to(g_lin_pd, dst, &b, 1) != 0) return -1;
        src++;
        dst++;
        len--;
    }
    return 0;
}

static bool lin_ptr_in_parent_stack(uint64_t p, uint64_t parent_rsp) {
    return p >= parent_rsp && p < LIN_STACK_TOP;
}

static void lin_fixup_stack_ptr(uint64_t *p, uint64_t parent_rsp, int64_t bias) {
    if (lin_ptr_in_parent_stack(*p, parent_rsp))
        *p = (uint64_t)((int64_t)*p + bias);
}

static void lin_fork_fixup_stack(uint64_t child_rsp, uint64_t parent_rsp,
                                 uint64_t used, int64_t bias) {
    for (uint64_t off = 0; off + 8 <= used; off += 8) {
        uint64_t q = 0;
        if (user_copy_from(g_lin_pd, &q, child_rsp + off, 8) != 0)
            continue;
        if (lin_ptr_in_parent_stack(q, parent_rsp)) {
            q = (uint64_t)((int64_t)q + bias);
            (void)user_copy_to(g_lin_pd, child_rsp + off, &q, 8);
        }
    }
}

static long lin_do_fork(lin_regs_t *r) {
    if (g_lin_fork_child) return -L_EAGAIN;
    if (map_user_pages(LIN_CHILD_STACK_BASE, LIN_STACK_BYTES,
                       VMM_FLAG_RW | VMM_FLAG_NX) != 0)
        return -L_ENOMEM;

    uint64_t parent_rsp = r->user_rsp;
    if (parent_rsp < LIN_STACK_BASE || parent_rsp >= LIN_STACK_TOP)
        return -L_EINVAL;
    uint64_t used = LIN_STACK_TOP - parent_rsp;
    uint64_t child_rsp = LIN_CHILD_STACK_TOP - used;
    if (copy_user_range(child_rsp, parent_rsp, used) != 0)
        return -L_EFAULT;

    int64_t bias = (int64_t)child_rsp - (int64_t)parent_rsp;
    lin_fork_fixup_stack(child_rsp, parent_rsp, used, bias);
    lin_fixup_stack_ptr(&r->rbp, parent_rsp, bias);

    g_lin_fork_parent_regs = *r;
    g_lin_fork_parent_pid = g_lin_pid;
    g_lin_fork_parent_pd = g_lin_pd;
    g_lin_fork_child_pid = g_lin_next_pid++;
    g_lin_fork_child = true;
    g_lin_fork_child_done = false;
    g_lin_fork_rsp_bias = (uint64_t)bias;
    g_lin_ppid = g_lin_fork_parent_pid;
    g_lin_pid = g_lin_fork_child_pid;
    r->user_rsp = child_rsp;
    debug_printf("[linux] fork -> child pid=%u rsp %p (parent rsp %p)\n",
                 (unsigned)g_lin_fork_child_pid,
                 (void *)(uintptr_t)child_rsp,
                 (void *)(uintptr_t)parent_rsp);
    return 0;
}

static void lin_fork_child_exit(lin_regs_t *r, int code) {
    if (g_lin_pd && g_lin_fork_parent_pd && g_lin_pd != g_lin_fork_parent_pd) {
        vmm_release(g_lin_pd);
        g_lin_pd = g_lin_fork_parent_pd;
        sched_set_current_pd(g_lin_pd);
        vmm_switch(g_lin_pd);
    }
    g_lin_fork_child_status = (code & 0xff) << 8;
    g_lin_fork_child_done = true;
    g_lin_fork_child = false;
    g_lin_pid = g_lin_fork_parent_pid;
    (void)g_lin_fork_rsp_bias;
    *r = g_lin_fork_parent_regs;
    r->rax = (uint64_t)g_lin_fork_child_pid;
    debug_printf("[linux] fork child exit=%d, parent resumes pid=%u\n",
                 code, (unsigned)g_lin_pid);
}

static bool lin_compat32_ptr_in_parent_stack(uint32_t p, uint32_t parent_rsp) {
    return p >= (uint32_t)LIN_STACK_BASE && p < (uint32_t)LIN_STACK_TOP &&
           p >= parent_rsp;
}

static void lin_compat32_fixup_stack(uint32_t child_rsp, uint32_t parent_rsp,
                                     uint32_t used, int32_t bias) {
    for (uint32_t off = 0; off + 4 <= used; off += 4) {
        uint32_t q = 0;
        if (user_copy_from(g_lin_pd, &q, child_rsp + off, 4) != 0)
            continue;
        if (lin_compat32_ptr_in_parent_stack(q, parent_rsp)) {
            q = (uint32_t)((int32_t)q + bias);
            (void)user_copy_to(g_lin_pd, child_rsp + off, &q, 4);
        }
    }
}

static int32_t lin_compat32_do_fork(uint32_t child_stack) {
    if (!g_lin_compat_frame_rsp)
        return -L_EINVAL;

    uint32_t parent_rsp = 0;
    if (user_copy_from(g_lin_pd, &parent_rsp,
                       g_lin_compat_frame_rsp + LIN_COMPAT32_RSP_OFF, 4) != 0)
        return -L_EFAULT;
    if (parent_rsp < (uint32_t)LIN_STACK_BASE ||
        parent_rsp >= (uint32_t)LIN_STACK_TOP)
        return -L_EINVAL;

    uint32_t child_rsp = child_stack;
    if (!child_rsp) {
        uint32_t used = (uint32_t)LIN_STACK_TOP - parent_rsp;
        if (map_user_pages(LIN_SYNC_STACK_BASE, LIN_STACK_BYTES,
                           VMM_FLAG_RW | VMM_FLAG_NX) != 0)
            return -L_ENOMEM;
        child_rsp = (uint32_t)LIN_SYNC_STACK_TOP - used;
        if (copy_user_range(child_rsp, parent_rsp, used) != 0)
            return -L_EFAULT;
        lin_compat32_fixup_stack(child_rsp, parent_rsp, used,
            (int32_t)child_rsp - (int32_t)parent_rsp);
    }

    g_lin_fork_parent_pid = g_lin_pid;
    memcpy(g_lin_fork_parent_compat_frame,
           (const void *)(uintptr_t)g_lin_compat_frame_rsp,
           LIN_COMPAT32_FRAME_BYTES);
    g_lin_fork_child_pid = lin_thread_compat32_fork_spawn(child_rsp);
    if (!g_lin_fork_child_pid)
        return -L_EAGAIN;
    g_lin_fork_child_done = false;
    g_lin_fork_child_status = 0;
    g_lin_ppid = g_lin_fork_parent_pid;

    debug_printf("[linux/i386] fork -> parent pid=%u child pid=%u rsp=0x%x\n",
                 (unsigned)g_lin_pid, (unsigned)g_lin_fork_child_pid, child_rsp);
    /* Child-runs-first model: the yield swaps the CHILD's frame into the
     * live int80 frame, and our return value lands in that frame's RAX.
     * Returning the pid here handed the pid to the CHILD (it took glibc's
     * parent branch and popped a fresh empty stack — CR2 = stack top).
     * After a successful swap the child's RAX (0) must be returned; the
     * parked parent gets the pid when lin_compat32_fork_child_exit
     * restores its frame. */
    int32_t resume = lin_thread_compat32_yield();
    if (lin_thread_get_tid() == g_lin_fork_child_pid) {
        g_lin_pid = g_lin_fork_child_pid;
        return resume;
    }
    return (int32_t)g_lin_fork_child_pid;
}

static int32_t lin_compat32_fork_child_exit(int code) {
    g_lin_fork_child_status = (code & 0xff) << 8;
    g_lin_fork_child_done = true;
    g_lin_fork_child = false;
    g_lin_pid = g_lin_fork_parent_pid;

    if (!g_lin_compat_frame_rsp)
        return (int32_t)g_lin_fork_child_pid;

    memcpy((void *)(uintptr_t)g_lin_compat_frame_rsp,
           g_lin_fork_parent_compat_frame, LIN_COMPAT32_FRAME_BYTES);
    uint32_t pid = (uint32_t)g_lin_fork_child_pid;
    if (user_copy_to(g_lin_pd, g_lin_compat_frame_rsp + LIN_COMPAT32_RAX_OFF,
                     &pid, 4) != 0)
        return -L_EFAULT;

    debug_printf("[linux/i386] fork child exit=%d, parent resumes pid=%u\n",
                 code, (unsigned)g_lin_pid);
    return (int32_t)g_lin_fork_child_pid;
}

static int32_t lin_compat32_timespec_sleep(uint32_t ureq, uint32_t urem) {
    if (!uptr_ok(ureq, 8))
        return -L_EFAULT;
    uint32_t ts[2];
    if (user_copy_from(g_lin_pd, ts, ureq, 8) != 0)
        return -L_EFAULT;
    uint64_t ms = (uint64_t)ts[0] * 1000ull +
                  ((uint64_t)ts[1] + 999999ull) / 1000000ull;
    while (ms) {
        if (lin_thread_session_active())
            (void)lin_thread_compat32_yield();
        uint32_t chunk = ms > 1000 ? 1000 : (uint32_t)ms;
        pit_sleep(chunk);
        ms -= chunk;
    }
    if (urem && uptr_write_ok(urem, 8)) {
        uint32_t zero[2] = { 0, 0 };
        if (user_copy_to(g_lin_pd, urem, zero, 8) != 0)
            return -L_EFAULT;
    }
    return 0;
}

static int lin_count_user_argv(uint64_t uargv) {
    int argc = 0;
    while (argc < LIN_MAX_ARGS) {
        uint64_t slot = 0;
        if (user_copy_from(g_lin_pd, &slot, uargv + (uint64_t)argc * 8, 8) != 0)
            return -1;
        if (!slot) break;
        argc++;
    }
    return argc;
}

static int lin_copy_user_argv(uint64_t uargv, int argc,
                              const char *argv[LIN_MAX_ARGS],
                              char storage[LIN_MAX_ARGS][LIN_PATH_MAX]) {
    for (int i = 0; i < argc; i++) {
        uint64_t slot = 0;
        if (user_copy_from(g_lin_pd, &slot, uargv + (uint64_t)i * 8, 8) != 0)
            return -L_EFAULT;
        int got = copy_user_cstr(slot, storage[i], LIN_PATH_MAX);
        if (got < 0) return got;
        argv[i] = storage[i];
    }
    argv[argc] = NULL;
    return 0;
}

typedef struct PACKED {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} lin_elf32_ehdr_t;

typedef struct PACKED {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} lin_elf32_phdr_t;

static long lin_map_elf32_object(const uint8_t *img, uint32_t len,
                                 uint32_t base, uint16_t expected_type,
                                 uint32_t *lo_out, uint32_t *hi_out,
                                 uint32_t *entry_out) {
    if (len < sizeof(lin_elf32_ehdr_t))
        return -L_EINVAL;
    const lin_elf32_ehdr_t *eh = (const lin_elf32_ehdr_t *)img;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != 1 || eh->ident[5] != 1 ||
        eh->machine != 3 || eh->type != expected_type ||
        eh->phentsize != sizeof(lin_elf32_phdr_t) ||
        eh->phoff > len ||
        (uint64_t)eh->phnum * eh->phentsize > len - eh->phoff)
        return -L_EINVAL;

    uint64_t load_lo = LIN_USER_MAX, load_hi = 0;
    const lin_elf32_phdr_t *ph =
        (const lin_elf32_phdr_t *)(img + eh->phoff);
    for (uint16_t i = 0; i < eh->phnum; i++) {
        if (ph[i].type != 1)
            continue;
        if (ph[i].filesz > ph[i].memsz ||
            ph[i].offset > len || ph[i].filesz > len - ph[i].offset ||
            (uint64_t)base + ph[i].vaddr < LIN_USER_MIN ||
            (uint64_t)base + ph[i].vaddr + ph[i].memsz > LIN_USER_MAX)
            return -L_EINVAL;
        uint64_t va = (uint64_t)base + ph[i].vaddr;
        uint64_t page = va & ~4095ull;
        uint64_t end = (va + ph[i].memsz + 4095) & ~4095ull;
        uintptr_t flags = VMM_FLAG_RW |
                          ((ph[i].flags & 1u) ? 0 : VMM_FLAG_NX);
        if (end > page && map_user_pages(page, end - page, flags) != 0)
            return -L_ENOMEM;
        memset((void *)(uintptr_t)va, 0, ph[i].memsz);
        memcpy((void *)(uintptr_t)va, img + ph[i].offset,
               ph[i].filesz);
        if (page < load_lo) load_lo = page;
        if (end > load_hi) load_hi = end;
    }
    uint64_t entry = (uint64_t)base + eh->entry;
    if (!load_hi || entry < load_lo || entry >= load_hi)
        return -L_EINVAL;
    *lo_out = (uint32_t)load_lo;
    *hi_out = (uint32_t)load_hi;
    *entry_out = (uint32_t)entry;
    return 0;
}

static void lin_push32(uint32_t *esp, uint32_t value) {
    *esp -= 4;
    *(uint32_t *)(uintptr_t)*esp = value;
}

static long lin_map_program32(const uint8_t *img, uint32_t len,
                              const char *exec_path, int argc,
                              const char *const *argv,
                              uint64_t *entry_out, uint64_t *rsp_out) {
    if (len < sizeof(lin_elf32_ehdr_t))
        return -L_EINVAL;
    const lin_elf32_ehdr_t *eh = (const lin_elf32_ehdr_t *)img;
    if (eh->type != 2 && eh->type != 3)
        return -L_EINVAL;
    uint32_t main_base = eh->type == 3 ? 0x20000000u : 0u;
    uint32_t load_lo = 0, load_hi = 0, main_entry = 0;
    long rc = lin_map_elf32_object(img, len, main_base, eh->type,
                                   &load_lo, &load_hi, &main_entry);
    if (rc < 0) return rc;

    char interp[LIN_PATH_MAX];
    interp[0] = 0;
    uint32_t phdr_addr = 0;
    const lin_elf32_phdr_t *ph =
        (const lin_elf32_phdr_t *)(img + eh->phoff);
    for (uint16_t i = 0; i < eh->phnum; i++) {
        if (ph[i].type == 3 && ph[i].filesz > 1 &&
            ph[i].offset <= len && ph[i].filesz <= len - ph[i].offset &&
            ph[i].filesz < sizeof(interp)) {
            memcpy(interp, img + ph[i].offset, ph[i].filesz);
            interp[ph[i].filesz - 1] = 0;
        }
        if (ph[i].type == 1 && eh->phoff >= ph[i].offset &&
            eh->phoff + (uint32_t)eh->phnum * eh->phentsize <=
                ph[i].offset + ph[i].filesz)
            phdr_addr = main_base + ph[i].vaddr +
                        (eh->phoff - ph[i].offset);
    }

    uint32_t runtime_entry = main_entry, interp_base = 0;
    if (interp[0]) {
        uint32_t ilen = 0, ilo = 0, ihi = 0, ientry = 0;
        if (linux_read_nxfs(interp, g_lin_aux_image, LIN_IMAGE_BYTES,
                            &ilen) != 0)
            return -L_ENOENT;
        interp_base = 0x30000000u;
        rc = lin_map_elf32_object(g_lin_aux_image, ilen, interp_base, 3,
                                  &ilo, &ihi, &ientry);
        if (rc < 0) return rc;
        runtime_entry = ientry;
    }

    if (map_user_pages(LIN_STACK_BASE, LIN_STACK_BYTES + VMM_PAGE_SIZE,
                       VMM_FLAG_RW | VMM_FLAG_NX) != 0)
        return -L_ENOMEM;
    uint32_t esp = (uint32_t)LIN_STACK_TOP;
    uint32_t argv32[LIN_MAX_ARGS];
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t n = (uint32_t)strlen(argv[i]) + 1;
        esp -= n;
        memcpy((void *)(uintptr_t)esp, argv[i], n);
        argv32[i] = esp;
    }
    static const char env_home_s[] = "HOME=/home/admin";
    static const char env_user_s[] = "USER=admin";
    static const char env_path_s[] = "PATH=/bin:/programs:/usr/bin";
    static const char env_term_s[] = "TERM=nexxon";
    static const char env_steamroot_s[] = "STEAMROOT=/programs";
    static const char env_steamexe_s[] = "STEAMEXE=steam";
    static const char env_steamruntime_s[] = "STEAM_RUNTIME=0";
    static const char env_display_s[] = "DISPLAY=:0";
    static const char env_ldpath_s[] = "LD_LIBRARY_PATH=/programs/ubuntu12_32";
    static const char env_xdg_s[] = "XDG_RUNTIME_DIR=/run/user/0";
    const char *env_lang_s = i18n_get_language() == LANG_HU
        ? "LANG=hu_HU.UTF-8" : "LANG=en_US.UTF-8";
    bool steam_exec = exec_path &&
        (strcmp(exec_path, "/programs/ubuntu12_32/steam") == 0 ||
         strcmp(exec_path, "/programs/steam") == 0);
    bool steam_gfx = steam_exec && lin_steam_graphical_enabled();
    /* Like the real steam.sh: the platform dir always joins the library
     * path (the bootstrap hard-links the shim libX11 in console mode too);
     * DISPLAY/XDG only appear when the graphical UI is opted in. */
    const char *envs[12];
    int envc = 0;
    envs[envc++] = env_home_s;
    envs[envc++] = env_user_s;
    envs[envc++] = env_path_s;
    envs[envc++] = env_term_s;
    envs[envc++] = env_lang_s;
    envs[envc++] = env_steamroot_s;
    envs[envc++] = env_steamexe_s;
    envs[envc++] = env_steamruntime_s;
    if (steam_exec)
        envs[envc++] = env_ldpath_s;
    if (steam_gfx) {
        envs[envc++] = env_display_s;
        envs[envc++] = env_xdg_s;
    }
    uint32_t env32[12];
    for (int i = envc - 1; i >= 0; i--) {
        uint32_t n = (uint32_t)strlen(envs[i]) + 1;
        esp -= n;
        memcpy((void *)(uintptr_t)esp, envs[i], n);
        env32[i] = esp;
    }
    const char platform[] = "i686";
    esp -= sizeof(platform);
    memcpy((void *)(uintptr_t)esp, platform, sizeof(platform));
    uint32_t platform_ptr = esp;
    esp -= 16;
    for (int i = 0; i < 16; i++)
        ((uint8_t *)(uintptr_t)esp)[i] = (uint8_t)(0x31 + i);
    uint32_t random_ptr = esp;
    esp &= ~15u;

    lin_push32(&esp, 0); lin_push32(&esp, 0);               /* AT_NULL */
    lin_push32(&esp, random_ptr); lin_push32(&esp, 25);     /* AT_RANDOM */
    lin_push32(&esp, platform_ptr); lin_push32(&esp, 15);   /* AT_PLATFORM */
    lin_push32(&esp, argv32[0]); lin_push32(&esp, 31);      /* AT_EXECFN */
    lin_push32(&esp, 0); lin_push32(&esp, 23);              /* AT_SECURE */
    lin_push32(&esp, main_entry); lin_push32(&esp, 9);      /* AT_ENTRY */
    lin_push32(&esp, interp_base); lin_push32(&esp, 7);     /* AT_BASE */
    lin_push32(&esp, 4096); lin_push32(&esp, 6);            /* AT_PAGESZ */
    lin_push32(&esp, eh->phnum); lin_push32(&esp, 5);       /* AT_PHNUM */
    lin_push32(&esp, eh->phentsize); lin_push32(&esp, 4);   /* AT_PHENT */
    lin_push32(&esp, phdr_addr); lin_push32(&esp, 3);       /* AT_PHDR */
    lin_push32(&esp, 0);                                   /* envp NULL */
    for (int i = envc - 1; i >= 0; i--)
        lin_push32(&esp, env32[i]);
    lin_push32(&esp, 0);                                   /* argv NULL */
    for (int i = argc - 1; i >= 0; i--)
        lin_push32(&esp, argv32[i]);
    lin_push32(&esp, (uint32_t)argc);

    g_lin_load_lo = load_lo;
    g_lin_load_hi = load_hi;
    g_lin_brk_floor = load_hi;
    g_lin_brk = load_hi;
    g_lin_compat32 = true;
    *entry_out = runtime_entry;
    *rsp_out = esp;
    debug_printf("[linux/i386] ELF32 compat entry=0x%x runtime=0x%x "
                 "load=0x%x..0x%x interp='%s'\n",
                 main_entry, runtime_entry, load_lo, load_hi,
                 interp[0] ? interp : "-");
    return 0;
}

static long lin_map_program(const uint8_t *img, uint32_t len,
                            const char *exec_path, int argc,
                            const char *const *argv,
                            uint64_t *entry_out, uint64_t *rsp_out) {
    if (len >= 5 && img[0] == 0x7F && img[1] == 'E' &&
        img[2] == 'L' && img[3] == 'F' && img[4] == 1)
        return lin_map_program32(img, len, exec_path, argc, argv,
                                 entry_out, rsp_out);
    g_lin_compat32 = false;
    elf64_mapped_t mapped;
    elf64_map_exec(img, len, g_lin_pd, LIN_PIE_BASE,
                   LIN_USER_LOW, LIN_USER_MAX, &mapped);
    if (!mapped.exec.ok) return -L_EINVAL;

    elf64_image_t interp = {0};
    uint64_t entry = mapped.exec.entry;
    if (mapped.has_interp) {
        if (!lin_interp_is_glibc(mapped.interp_path))
            linux_seed_lib_if_needed();
        uint32_t ilen = 0;
        if (linux_read_nxfs(mapped.interp_path, g_lin_aux_image,
                            LIN_IMAGE_BYTES, &ilen) != 0)
            return -L_ENOENT;
        elf64_map_so(g_lin_aux_image, ilen, g_lin_pd, LIN_INTERP_BASE,
                     LIN_USER_LOW, LIN_INTERP_LIMIT, &interp);
        if (!interp.ok) return -L_EINVAL;
        entry = interp.entry;
    }

    g_lin_load_lo = mapped.exec.load_lo;
    g_lin_load_hi = mapped.exec.load_hi;
    g_lin_brk_floor = (mapped.exec.brk + 4095) & ~4095ULL;
    g_lin_brk = g_lin_brk_floor;
    if (g_lin_brk_floor >= LIN_BRK_LIMIT ||
        map_user_pages(LIN_STACK_BASE, LIN_STACK_BYTES,
                       VMM_FLAG_RW | VMM_FLAG_NX) != 0)
        return -L_ENOMEM;

    uint64_t rsp = build_init_stack(&mapped.exec,
                                    mapped.has_interp ? &interp : NULL,
                                    mapped.has_interp, exec_path,
                                    argc, argv);
    if (!rsp) return -L_ENOMEM;
    *entry_out = entry;
    *rsp_out = rsp;
    return 0;
}

static long lin_do_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp,
                          lin_regs_t *r) {
    (void)uenvp;
    (void)r;
    char path[LIN_PATH_MAX], canon[LIN_PATH_MAX];
    int pr = copy_user_cstr(upath, path, sizeof(path));
    if (pr < 0) return pr;
    pr = normalize_path(g_lin_cwd, path, canon, sizeof(canon));
    if (pr < 0) return pr;

    int argc = lin_count_user_argv(uargv);
    if (argc < 0 || argc > LIN_MAX_ARGS) return -L_EINVAL;
    const char *argv[LIN_MAX_ARGS];
    /* Long `sh -c` command arguments exceed LIN_PATH_MAX; copy argv into
     * the shared wide store (single live exec context). */
    int ar = 0;
    for (int i = 0; i < argc; i++) {
        uint64_t ptr = 0;
        if (user_copy_from(g_lin_pd, &ptr, uargv + (uint64_t)i * 8u, 8) != 0) {
            ar = -L_EFAULT;
            break;
        }
        if (!ptr) {
            argc = i;
            break;
        }
        ar = copy_user_cstr(ptr, g_lin_exec_argstore[i], LIN_ARG_MAX);
        if (ar < 0) {
            debug_printf("[linux] execve '%s': argv[%d] rc=%d\n",
                         canon, i, ar);
            break;
        }
        argv[i] = g_lin_exec_argstore[i];
    }
    if (ar < 0) return ar;
    if (argc == 0) {
        argv[0] = canon;
        argc = 1;
    }

    lin_mark_steam_update_ui_argv(argc, argv);

    uint32_t len = 0;
    if (linux_read_nxfs(canon, g_lin_file_image, LIN_IMAGE_BYTES, &len) != 0) {
        debug_printf("[linux] execve ENOENT '%s'\n", canon);
        return -L_ENOENT;
    }

    for (int i = 3; i < LIN_FD_MAX; i++) {
        if (g_lin_fds[i].kind == LFD_FREE || !g_lin_fds[i].cloexec)
            continue;
        if (g_lin_fds[i].kind == LFD_SOCK &&
            g_lin_fds[i].sock.domain == LIN_AF_UNIX &&
            g_lin_fds[i].sock.unix_link >= 0 &&
            strncmp(g_lin_fds[i].path, "socketpair:", 11) == 0)
            continue;
        (void)fd_close(i, true);
    }

    vmm_pd_t *old = g_lin_pd;
    vmm_pd_t *next = vmm_clone_for_exec();
    if (!next) {
        return -L_ENOMEM;
    }
    g_lin_pd = next;
    sched_set_current_pd(g_lin_pd);
    vmm_switch(g_lin_pd);
    memset(g_lin_maps, 0, sizeof(g_lin_maps));

    uint64_t entry = 0, rsp = 0;
    long lr = lin_map_program(g_lin_file_image, len, canon, argc, argv,
                              &entry, &rsp);
    if (lr < 0) {
        vmm_pd_t *failed = g_lin_pd;
        g_lin_pd = old;
        sched_set_current_pd(g_lin_pd);
        vmm_switch(g_lin_pd);
        vmm_release(failed);
        return lr;
    }
    if (!g_lin_fork_child)
        vmm_release(old);

    strncpy(g_lin_exec_path, canon, sizeof(g_lin_exec_path) - 1);
    g_lin_exec_path[sizeof(g_lin_exec_path) - 1] = 0;
    g_lin_tty_line_len = 0;
    g_lin_tty_line_pos = 0;
    r->rip = entry;
    r->user_rsp = rsp;
    g_lin_return_compat32 = g_lin_compat32 ? 1 : 0;
    debug_printf("[linux] execve '%s' argc=%d entry=%p rsp=%p\n",
                 canon, argc, (void *)(uintptr_t)entry, (void *)(uintptr_t)rsp);
    return 0;
}

static void linux_seed_vulkan_icd(void) {
    uint32_t lib = lin_ensure_child_dir(0, "lib");
    if (lib) {
        uint32_t triplet = lin_ensure_child_dir(lib, "x86_64-linux-gnu");
        if (triplet) {
            if (linux_install_file(triplet, "libvulkan_nexxon.so",
                    _binary_build_userland_libvulkan_nexxon_so_start,
                    _binary_build_userland_libvulkan_nexxon_so_end))
                debug_printf("[linux/vulkan] seeded ICD library (%u bytes)\n",
                    (uint32_t)(_binary_build_userland_libvulkan_nexxon_so_end -
                                _binary_build_userland_libvulkan_nexxon_so_start));
        }
    }

    uint32_t usr = lin_ensure_child_dir(0, "usr");
    if (!usr) return;
    uint32_t share = lin_ensure_child_dir(usr, "share");
    if (!share) return;
    uint32_t vulkan = lin_ensure_child_dir(share, "vulkan");
    if (!vulkan) return;
    uint32_t icd_d = lin_ensure_child_dir(vulkan, "icd.d");
    if (!icd_d) return;
    static const char icd[] =
        "{\n"
        "    \"file_format_version\": \"1.0.0\",\n"
        "    \"ICD\": {\n"
        "        \"library_path\": \"/lib/x86_64-linux-gnu/libvulkan_nexxon.so\",\n"
        "        \"api_version\": \"1.3.0\"\n"
        "    }\n"
        "}\n";
    if (linux_install_file(icd_d, "nexxon_icd.json",
                           (const uint8_t *)icd,
                           (const uint8_t *)icd + sizeof(icd) - 1))
        debug_printf("[linux/vulkan] seeded ICD manifest\n");
}

static void linux_seed_rootfs(void) {
    linux_seed_lib_if_needed();
    lin_unix_global_init();
    linux_seed_vulkan_icd();
    linux_seed_resolv_conf();
    linux_seed_hosts();
    linux_seed_ssl_certs();
    uint32_t home = lin_ensure_child_dir(0, "home");
    if (home) {
        uint32_t admin = lin_ensure_child_dir(home, "admin");
        if (admin) {
            uint32_t dotsteam = lin_ensure_child_dir(admin, ".steam");
            if (dotsteam)
                (void)lin_ensure_child_dir(dotsteam, "steam");
        }
    }
    uint32_t dev = lin_ensure_child_dir(0, "dev");
    if (dev)
        (void)lin_ensure_child_dir(dev, "shm");

    uint32_t bin_ino;
    if (nxfs_resolve_path("/bin/sh", &bin_ino) != NXFS_OK) {
        if (nxfs_resolve(0, "bin", &bin_ino) != NXFS_OK &&
            nxfs_create_dir(0, "bin", &bin_ino) != NXFS_OK)
            return;
        const uint8_t *sh_s = _binary_build_userland_linuxsh_elf_start;
        const uint8_t *sh_e = _binary_build_userland_linuxsh_elf_end;
        (void)linux_install_file(bin_ino, "sh", sh_s, sh_e);
        (void)linux_install_file(bin_ino, "linuxsh", sh_s, sh_e);
        debug_printf("[linux] seeded rootfs /bin/sh (%u bytes)\n",
                     (uint32_t)(sh_e - sh_s));
    }

    uint32_t wine_ino;
    if (nxfs_resolve_path("/usr/bin/wine", &wine_ino) != NXFS_OK) {
        uint32_t ub = lin_usr_bin_dir();
        if (ub) {
            const uint8_t *ws_s = _binary_build_userland_winestub_elf_start;
            const uint8_t *ws_e = _binary_build_userland_winestub_elf_end;
            if (linux_install_file(ub, "wine", ws_s, ws_e))
                debug_printf("[linux] seeded /usr/bin/wine (%u bytes)\n",
                             (uint32_t)(ws_e - ws_s));
        }
    }
}

static int linux_run_image_argv(const uint8_t *img, uint32_t len,
                                int argc, const char *const *argv,
                                const char *exec_path,
                                char *out_msg, uint32_t out_cap) {
    if (g_lin_running) {
        out_copy(out_msg, out_cap, L(STR_LINUX_ALREADY_RUNNING));
        return -1;
    }
    if (!img || !len || argc < 1 || argc > LIN_MAX_ARGS || !argv) {
        out_copy(out_msg, out_cap, L(STR_LINUX_LOAD_FAILED));
        return -2;
    }
    for (int i = 0; i < argc; i++)
        if (!argv[i]) {
            out_copy(out_msg, out_cap, L(STR_LINUX_LOAD_FAILED));
            return -2;
        }

    g_lin_private_cr3 = false;
    g_lin_pid = 1000;
    g_lin_ppid = 1;
    g_lin_fork_child = false;
    g_lin_fork_child_done = false;
    g_lin_tty_line_len = 0;
    g_lin_tty_line_pos = 0;
    memset(g_lin_maps, 0, sizeof(g_lin_maps));
    lin_thread_reset();
    g_lin_pd = vmm_clone_for_exec();
    if (!g_lin_pd) {
        out_copy(out_msg, out_cap, L(STR_LINUX_LOAD_FAILED));
        return -2;
    }
    elf64_mapped_t mapped;
    elf64_map_exec(img, len, g_lin_pd, LIN_PIE_BASE,
                   LIN_USER_LOW, LIN_USER_MAX, &mapped);
    if (!mapped.exec.ok) {
        debug_printf("[linux] load failed: %s\n", mapped.exec.err);
        vmm_release(g_lin_pd);
        g_lin_pd = NULL;
        out_copy(out_msg, out_cap, L(STR_LINUX_LOAD_FAILED));
        return -2;
    }

    elf64_image_t interp = {0};
    uint64_t entry = mapped.exec.entry;
    if (mapped.has_interp) {
        if (!lin_interp_is_glibc(mapped.interp_path))
            linux_seed_lib_if_needed();
        uint32_t ilen = 0;
        if (linux_read_nxfs(mapped.interp_path, g_lin_aux_image,
                            LIN_IMAGE_BYTES, &ilen) != 0) {
            debug_printf("[linux] interpreter missing: %s\n",
                         mapped.interp_path);
            vmm_release(g_lin_pd);
            g_lin_pd = NULL;
            out_copy(out_msg, out_cap, L(STR_LINUX_INTERP_FAILED));
            return -2;
        }
        elf64_map_so(g_lin_aux_image, ilen, g_lin_pd, LIN_INTERP_BASE,
                     LIN_USER_LOW, LIN_INTERP_LIMIT, &interp);
        if (!interp.ok) {
            debug_printf("[linux] interpreter map failed: %s\n", interp.err);
            vmm_release(g_lin_pd);
            g_lin_pd = NULL;
            out_copy(out_msg, out_cap, L(STR_LINUX_INTERP_FAILED));
            return -2;
        }
        entry = interp.entry;
        debug_printf("[linux] dynamic '%s' via %s base=%p entry=%p\n",
                     mapped.interp_path, mapped.interp_path,
                     (void *)(uintptr_t)interp.base,
                     (void *)(uintptr_t)interp.entry);
    }

    if (entry >= LIN_USER_MAX) {
        vmm_release(g_lin_pd);
        g_lin_pd = NULL;
        out_copy(out_msg, out_cap, L(STR_LINUX_ENTRY_UNSUPPORTED));
        return -3;
    }

    g_lin_load_lo = mapped.exec.load_lo;
    g_lin_load_hi = mapped.exec.load_hi;
    g_lin_brk_floor = (mapped.exec.brk + 4095) & ~4095ULL;
    g_lin_brk = g_lin_brk_floor;
    if (g_lin_brk_floor >= LIN_BRK_LIMIT ||
        map_user_pages(LIN_STACK_BASE, LIN_STACK_BYTES,
                       VMM_FLAG_RW | VMM_FLAG_NX) != 0) {
        vmm_release(g_lin_pd);
        g_lin_pd = NULL;
        out_copy(out_msg, out_cap, L(STR_LINUX_STACK_FAILED));
        return -4;
    }
    g_lin_image_size = len;
    strncpy(g_lin_exec_path, exec_path ? exec_path : argv[0],
            sizeof(g_lin_exec_path) - 1);
    g_lin_exec_path[sizeof(g_lin_exec_path) - 1] = 0;
    strcpy(g_lin_cwd, "/");
    g_lin_cwd_ino = 0;
    fd_table_init();

    uint64_t user_rsp = build_init_stack(&mapped.exec,
                                         mapped.has_interp ? &interp : NULL,
                                         mapped.has_interp, exec_path,
                                         argc, argv);
    if (!user_rsp || user_rsp >= LIN_USER_MAX) {
        fd_table_finish(false);
        vmm_release(g_lin_pd);
        g_lin_pd = NULL;
        out_copy(out_msg, out_cap, L(STR_LINUX_STACK_FAILED));
        return -4;
    }

    g_lin_exit_code = 0;
    g_lin_faulted = false;
    if (setjmp(g_lin_return) != 0) {
        bool clean = !g_lin_faulted;
        if (g_lin_private_cr3) {
            sched_set_current_pd(vmm_kernel_pd());
            vmm_switch(vmm_kernel_pd());
            g_lin_private_cr3 = false;
        }
        g_lin_running = false;
        wrmsr(MSR_FS_BASE, 0);
        wrmsr(MSR_GS_BASE, 0);
        fd_table_finish(clean);
        memset(g_lin_maps, 0, sizeof(g_lin_maps));
        vmm_pd_t *dead_pd = g_lin_pd;
        g_lin_pd = NULL;
        if (dead_pd) vmm_release(dead_pd);
        debug_printf("[linux] '%s' finished, code=%d fault=%u\n",
                     g_lin_exec_path, g_lin_exit_code,
                     g_lin_faulted ? 1u : 0u);
        if (out_msg && out_cap) {
            ksnprintf(out_msg, out_cap,
                      g_lin_faulted ? L(STR_LINUX_TERMINATED_FMT)
                                    : L(STR_LINUX_EXITED_FMT),
                      g_lin_exit_code);
        }
        return g_lin_exit_code;
    }

    wrmsr(MSR_FS_BASE, 0);
    wrmsr(MSR_GS_BASE, 0);
    g_lin_running = true;
    gdt_set_kernel_stack((uintptr_t)(g_lin_kstack + sizeof(g_lin_kstack)));
    sched_set_current_pd(g_lin_pd);
    vmm_switch(g_lin_pd);
    g_lin_private_cr3 = true;
    debug_printf("[linux] entering private CR3 '%s' argc=%d entry=%p rsp=%p "
                 "load=%p..%p brk=%p pd=%p dynamic=%u\n",
                 argv[0], argc, (void *)(uintptr_t)entry,
                 (void *)(uintptr_t)user_rsp,
                 (void *)(uintptr_t)g_lin_load_lo,
                 (void *)(uintptr_t)g_lin_load_hi,
                 (void *)(uintptr_t)g_lin_brk, (void *)g_lin_pd,
                 mapped.has_interp ? 1u : 0u);
    ring3_enter((uintptr_t)entry, (uintptr_t)user_rsp);
    return -5;
}

int linux_run_image(const uint8_t *img, uint32_t len,
                    const char *name, char *out_msg, uint32_t out_cap) {
    const char *argv[1] = { name ? name : "linux" };
    return linux_run_image_argv(img, len, 1, argv, name, out_msg, out_cap);
}

int linux_run_path_args(const char *path, int argc, const char *const *argv,
                        char *out_msg, uint32_t out_cap) {
    if (!path || argc < 1 || !argv) return -1;
    linux_seed_rootfs();
    uint32_t len = 0;
    if (linux_read_nxfs(path, g_lin_file_image, LIN_IMAGE_BYTES, &len) != 0) {
        if (out_msg && out_cap)
            ksnprintf(out_msg, out_cap, L(STR_LINUX_NOT_FOUND_FMT), path);
        return -2;
    }
    return linux_run_image_argv(g_lin_file_image, len,
                                argc, argv, path, out_msg, out_cap);
}

int linux_run_path(const char *path, char *out_msg, uint32_t out_cap) {
    const char *argv[1] = { path };
    return linux_run_path_args(path, 1, argv, out_msg, out_cap);
}

int linux_subsystem_enter(char *out_msg, uint32_t out_cap) {
    linux_seed_rootfs();
    const char *argv[1] = { "/bin/sh" };
    return linux_run_path_args("/bin/sh", 1, argv, out_msg, out_cap);
}

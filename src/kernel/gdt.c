/* ============================================================================
 * NexxoN OS - GDT setup  (v2.0, Ring 3 ready)
 * ----------------------------------------------------------------------------
 * Installs:
 *   0x00  Null
 *   0x08  Kernel Code (DPL=0)
 *   0x10  Kernel Data (DPL=0)
 *   0x18  User Code   (DPL=3)
 *   0x20  User Data   (DPL=3)
 *   0x28  TSS                  - ESP0/SS0 stack switch on ring transition
 *
 * Once gdt_flush() returns, ltr() loads the TSS selector so the CPU knows
 * which ring-0 stack to switch to when a user-mode interrupt occurs.  All
 * subsequent ring transitions (int 0x80, GPFs, IRQs while CPL=3) rely on
 * the ESP0 value we keep up to date through gdt_set_kernel_stack().
 * ============================================================================ */
#include "gdt.h"
#include "string.h"
#include "debug.h"

#if defined(__x86_64__)
/* ============================================================================
 * x86_64 long-mode GDT + 64-bit TSS
 * ----------------------------------------------------------------------------
 * Long mode ignores segment base/limit for code/data, but the descriptors
 * must still carry the right access bits and, crucially, the code segment's
 * L bit (long).  The TSS is a 16-byte SYSTEM descriptor (two GDT slots) and
 * the TSS itself is the IA-32e layout (RSP0..RSP2 + IST1..IST7).  The boot64
 * stub already runs us on a minimal GDT with the SAME selectors (0x08 code,
 * 0x10 data), so reloading CS/DS here is seamless.
 * ============================================================================ */
typedef struct PACKED {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags;          /* high nibble = G/D/L/AVL, low nibble = limit_high */
    uint8_t  base_high;
} gdt_entry_t;

typedef struct PACKED {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

/* IA-32e TSS (Intel SDM Vol.3 7.7).  Only rsp0 is consumed (ring transitions);
 * IST entries stay zero until fault stacks arrive. */
typedef struct PACKED {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

/* Base table plus one IA-32 compatibility TLS descriptor. */
#define GDT_NUM_ENTRIES 9
static gdt_entry_t g_gdt[GDT_NUM_ENTRIES];
static gdt_ptr_t   g_gdt_ptr;
static tss_t       g_tss;

ALIGNED(16) static uint8_t g_kstack[16 * 1024];

extern void gdt_flush(uintptr_t gdt_ptr_addr);  /* boot64/gdt_flush64.asm */
extern void tss_flush(uint16_t tss_sel);        /* boot64/gdt_flush64.asm */

static void set_gate(int idx, uint32_t base, uint32_t limit,
                     uint8_t access, uint8_t flags) {
    g_gdt[idx].base_low  = (uint16_t)(base & 0xFFFF);
    g_gdt[idx].base_mid  = (uint8_t) ((base >> 16) & 0xFF);
    g_gdt[idx].base_high = (uint8_t) ((base >> 24) & 0xFF);
    g_gdt[idx].limit_low = (uint16_t)(limit & 0xFFFF);
    g_gdt[idx].flags     = (uint8_t) (((limit >> 16) & 0x0F) | (flags & 0xF0));
    g_gdt[idx].access    = access;
}

/* A 64-bit TSS descriptor spans two GDT slots (16 bytes): the low half is a
 * normal descriptor with type 0x9 (available 64-bit TSS); the high half holds
 * base bits 32..63. */
static void install_tss_descriptor(int idx, uint64_t base, uint32_t limit) {
    set_gate(idx, (uint32_t)(base & 0xFFFFFFFFu), limit, 0x89, 0x00);
    uint32_t *hi = (uint32_t *)(void *)&g_gdt[idx + 1];
    hi[0] = (uint32_t)(base >> 32);
    hi[1] = 0;
}

void gdt_init(void) {
    debug_step("gdt: building 64-bit descriptor table (long mode, Ring 3 ready)");

    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base  = (uintptr_t)&g_gdt;

    /* flags high nibble: G(7) D/B(6) L(5) AVL(4).  64-bit code => G=1,L=1,D=0
     * -> 0xA0; data => G=1,D/B=1 -> 0xC0 (D/B ignored for data in long mode). */
    set_gate(0, 0, 0,       0,    0);     /* null                 */
    set_gate(1, 0, 0xFFFFF, 0x9A, 0xA0);  /* kernel code (L=1)    */
    set_gate(2, 0, 0xFFFFF, 0x92, 0xC0);  /* kernel data          */
    set_gate(3, 0, 0xFFFFF, 0xFA, 0xA0);  /* user code   (DPL3,L) */
    set_gate(4, 0, 0xFFFFF, 0xF2, 0xC0);  /* user data   (DPL3)   */

    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.rsp0       = (uint64_t)(uintptr_t)(g_kstack + sizeof(g_kstack));
    g_tss.iomap_base = sizeof(g_tss);     /* no I/O bitmap */
    install_tss_descriptor(5, (uint64_t)(uintptr_t)&g_tss, sizeof(g_tss) - 1);
    set_gate(7, 0, 0xFFFFF, 0xFA, 0xC0);  /* user compat (DPL3,D=1,L=0) */
    set_gate(8, 0, 0,       0xF2, 0x40);  /* compat TLS, set_thread_area */

    gdt_flush((uintptr_t)&g_gdt_ptr);     /* lgdt + reload CS/DS/SS/ES/FS/GS */
    tss_flush(GDT_TSS_SEL);               /* ltr 0x28                        */
    debug_ok("gdt: 64-bit GDT + TSS loaded "
             "(kCS=0x08 u64=0x1B u32=0x3B TSS=0x28)");
}

void gdt_set_kernel_stack(uintptr_t esp0) { g_tss.rsp0 = esp0; }

void gdt_set_compat_tls(uint32_t base, uint32_t limit, bool page_granular) {
    set_gate(8, base, limit, 0xF2, page_granular ? 0xC0 : 0x40);
}

#else  /* !__x86_64__ : original 32-bit GDT + 32-bit TSS (unchanged) */

typedef struct PACKED {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags;          /* high nibble = flags, low nibble = limit_high */
    uint8_t  base_high;
} gdt_entry_t;

typedef struct PACKED {
    uint16_t limit;
    uint32_t base;
} gdt_ptr_t;

/* 32-bit TSS as documented in Intel SDM Vol. 3A figure 7-2.  Most fields
 * are unused by NexxoN (we don't use hardware task switching); the ones
 * that matter on a ring transition are ESP0 + SS0, which the CPU loads
 * whenever it goes from CPL>0 to CPL=0 via an interrupt gate. */
typedef struct PACKED {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} tss_t;

#define GDT_NUM_ENTRIES 6
static gdt_entry_t g_gdt[GDT_NUM_ENTRIES];
static gdt_ptr_t   g_gdt_ptr;
static tss_t       g_tss;

/* Static ring-0 stack used during privilege transitions.  Aligned to 16
 * bytes because the SysV ABI expects a 16-byte aligned stack at function
 * entry.  16 KiB is enough for any IRQ that escalates from user mode -
 * we never re-enter user code with this stack still in use. */
ALIGNED(16) static uint8_t g_kstack[16 * 1024];

extern void gdt_flush(uint32_t gdt_ptr_addr);   /* boot/gdt_flush.asm */
extern void tss_flush(uint16_t tss_sel);        /* boot/gdt_flush.asm */

static void set_gate(int idx, uint32_t base, uint32_t limit,
                     uint8_t access, uint8_t flags) {
    g_gdt[idx].base_low  = (uint16_t)(base & 0xFFFF);
    g_gdt[idx].base_mid  = (uint8_t) ((base >> 16) & 0xFF);
    g_gdt[idx].base_high = (uint8_t) ((base >> 24) & 0xFF);

    g_gdt[idx].limit_low = (uint16_t)(limit & 0xFFFF);
    g_gdt[idx].flags     = (uint8_t) ((limit >> 16) & 0x0F) | (flags & 0xF0);
    g_gdt[idx].access    = access;
}

static void install_tss_descriptor(int idx, uint32_t base, uint32_t limit) {
    /* Type = 0x9 (available 32-bit TSS), S=0, present, DPL=0:
     *   access = 0b10001001 = 0x89  (P=1 DPL=0 S=0 type=9)
     * Granularity nibble = 0x00 (byte-granular, 16-bit TSS limit). */
    set_gate(idx, base, limit, 0x89, 0x00);
}

void gdt_init(void) {
    debug_step("gdt: building kernel descriptor table (Ring 3 ready)");

    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base  = (uint32_t)(uintptr_t)&g_gdt;

    set_gate(0, 0, 0, 0, 0);                              /* null      */
    set_gate(1, 0, 0x000FFFFF, 0x9A, 0xCF);               /* k code    */
    set_gate(2, 0, 0x000FFFFF, 0x92, 0xCF);               /* k data    */
    set_gate(3, 0, 0x000FFFFF, 0xFA, 0xCF);               /* u code    */
    set_gate(4, 0, 0x000FFFFF, 0xF2, 0xCF);               /* u data    */

    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.ss0  = GDT_KDATA_SEL;
    g_tss.esp0 = (uint32_t)(uintptr_t)(g_kstack + sizeof(g_kstack));
    g_tss.iomap_base = sizeof(g_tss);   /* no I/O bitmap */
    install_tss_descriptor(5, (uint32_t)(uintptr_t)&g_tss, sizeof(g_tss) - 1);

    gdt_flush((uint32_t)(uintptr_t)&g_gdt_ptr);
    tss_flush(GDT_TSS_SEL);
    debug_ok("gdt: 6 entries loaded "
             "(kCS=0x08 kDS=0x10 uCS=0x1B uDS=0x23 TSS=0x28)");
}

void gdt_set_kernel_stack(uintptr_t esp0) {
    g_tss.esp0 = (uint32_t)esp0;
}

void gdt_set_compat_tls(uint32_t base, uint32_t limit, bool page_granular) {
    (void)base; (void)limit; (void)page_granular;
}

#endif /* __x86_64__ */

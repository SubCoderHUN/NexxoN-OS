; ============================================================================
; NexxoN OS - x86_64 IDT reload + ISR / IRQ low-level stubs  (elf64)
; ----------------------------------------------------------------------------
; 64-bit equivalent of boot/idt_flush.asm.  Provides the EXACT symbols the
; real src/kernel/idt.c expects so the production interrupt code drives long
; mode unchanged:
;
;   idt_flush(rdi = &idt_ptr)   - LIDT (SysV: first arg in RDI)
;   isr0 .. isr31               - CPU-exception stubs -> isr_common -> isr_handler
;   irq0 .. irq15               - hardware-IRQ stubs  -> irq_common -> irq_handler
;
; Each stub normalises the stack to [int_no][err_code] (the CPU pushes an error
; code only for some exceptions; the no-error stubs push a dummy 0), then the
; common path saves all 15 GP registers in the order src/include/isr.h's
; registers_t expects (rax pushed first .. r15 last, so r15 is at the lowest
; address), calls the C dispatcher with RDI = &registers_t, restores, drops the
; synthesised int_no/err_code, and returns via IRETQ.
;
; Long mode runs a single flat code/data segment, so unlike the 32-bit stub
; there is no DS/ES/FS/GS reload.  IDT gates are interrupt gates (type 0x8E),
; which clear IF on entry; IRETQ restores the saved RFLAGS, so no explicit
; cli/sti is needed in the stub.
; ============================================================================
[BITS 64]
section .text
global idt_flush
extern isr_handler
extern irq_handler

idt_flush:
    lidt    [rdi]
    ret

%macro PUSH_ALL 0
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15
%endmacro

%macro POP_ALL 0
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
%endmacro

isr_common_stub:
    PUSH_ALL
    mov     rdi, rsp            ; arg1 = pointer to registers_t
    cld
    call    isr_handler
    POP_ALL
    add     rsp, 16             ; discard synthesised int_no + err_code
    iretq

irq_common_stub:
    PUSH_ALL
    mov     rdi, rsp
    cld
    call    irq_handler
    POP_ALL
    add     rsp, 16
    iretq

;----------------------------------------------------------------------------
; int 0x80 - ring-3 system-call entry (64-bit)
; ----------------------------------------------------------------------------
; The CPU entered through a DPL=3 interrupt gate (set by syscall_init), so it
; loaded RSP0 from the TSS and pushed SS:RSP:RFLAGS:CS:RIP.  The userland
; convention (syscall.h, register constraints a/b/c/d/S/D) is:
;   rax=num  rbx=a0  rcx=a1  rdx=a2  rsi=a3  rdi=a4
; We save the GP frame (rax at +112 .. r15 at +0), marshal the args into the
; SysV registers for syscall_dispatch(num,a0..a4), stash the return value into
; the saved-rax slot, restore, and IRETQ back to ring 3 (rax = return value).
;----------------------------------------------------------------------------
; isr128 (the 0x80 syscall gate) + ring3_enter back syscall.c + usermode.c.
;----------------------------------------------------------------------------
global isr128
extern syscall_dispatch
extern g_lin_compat_arg5
extern g_lin_compat_rip
extern g_lin_compat_frame_rsp
isr128:
    PUSH_ALL
    mov     [rel g_lin_compat_frame_rsp], rsp
    mov     rax, [rsp + 64]     ; i386 sixth argument lives in EBP
    mov     [rel g_lin_compat_arg5], eax
    mov     rax, [rsp + 120]    ; compatibility-mode return EIP
    mov     [rel g_lin_compat_rip], eax
    mov     rdi, [rsp + 112]    ; num <- saved rax
    mov     rsi, [rsp + 104]    ; a0  <- saved rbx
    mov     rdx, [rsp + 96]     ; a1  <- saved rcx
    mov     rcx, [rsp + 88]     ; a2  <- saved rdx
    mov     r8,  [rsp + 80]     ; a3  <- saved rsi
    mov     r9,  [rsp + 72]     ; a4  <- saved rdi
    cld
    call    syscall_dispatch
    mov     [rsp + 112], rax    ; return value -> saved rax slot
    POP_ALL
    iretq

;----------------------------------------------------------------------------
; void ring3_enter(uintptr_t user_rip /*rdi*/, uintptr_t user_rsp /*rsi*/)
; ----------------------------------------------------------------------------
; Synthesise an IRETQ frame and drop to CPL=3.  User selectors come from the
; gdt.c table: uCS=0x18 -> 0x1B (RPL3), uDS=0x20 -> 0x23.  user_rip/user_rsp
; are native-width virtual addresses.  Does not return:
; the ring-3 task leaves via SYS_EXIT, which longjmps back in kernel space.
;----------------------------------------------------------------------------
global ring3_enter
ring3_enter:
    cli
    mov     rcx, rdi            ; user RIP
    mov     rdx, rsi            ; user RSP
    mov     ax, 0x23            ; user data selector (DPL3)
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    push    qword 0x23          ; SS
    push    rdx                 ; RSP (user)
    pushfq
    pop     rax
    or      rax, 0x200          ; IF = 1 (interrupts on in ring 3)
    push    rax                 ; RFLAGS
    push    qword 0x1B          ; CS (DPL3)
    push    rcx                 ; RIP
    iretq

;----------------------------------------------------------------------------
; void ring3_enter32(uintptr_t user_eip, uintptr_t user_esp)
; Enter IA-32e compatibility mode using the D=1,L=0 user code descriptor.
;----------------------------------------------------------------------------
global ring3_enter32
ring3_enter32:
    cli
    mov     ecx, edi
    mov     edx, esi
    mov     ax, 0x23
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    push    qword 0x23
    push    rdx
    pushfq
    pop     rax
    or      rax, 0x200
    push    rax
    push    qword 0x3B          ; 32-bit compatibility user code, RPL3
    push    rcx
    xor     eax, eax
    xor     ebx, ebx
    xor     ecx, ecx
    xor     edx, edx
    xor     esi, esi
    xor     edi, edi
    xor     ebp, ebp
    iretq

;----------------------------------------------------------------------------
; Stub generators (no copy-paste).
;----------------------------------------------------------------------------
%macro ISR_NOERR 1
    global isr%1
    isr%1:
        push    qword 0         ; synthetic error code = 0
        push    qword %1        ; interrupt vector
        jmp     isr_common_stub
%endmacro

%macro ISR_ERR 1
    global isr%1
    isr%1:
        push    qword %1        ; vector (CPU already pushed an error code)
        jmp     isr_common_stub
%endmacro

%macro IRQ 2
    global irq%1
    irq%1:
        push    qword 0         ; no error code for hardware interrupts
        push    qword %2        ; remapped vector (32..47)
        jmp     irq_common_stub
%endmacro

; CPU exceptions.  Error-code pushers per Intel/AMD SDM: 8,10,11,12,13,14,17,21.
ISR_NOERR  0
ISR_NOERR  1
ISR_NOERR  2
ISR_NOERR  3
ISR_NOERR  4
ISR_NOERR  5
ISR_NOERR  6
ISR_NOERR  7
ISR_ERR    8
ISR_NOERR  9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; Hardware IRQs, remapped to vectors 32..47 by the PIC.
IRQ  0, 32          ; PIT timer
IRQ  1, 33          ; PS/2 keyboard
IRQ  2, 34          ; cascade
IRQ  3, 35          ; COM2
IRQ  4, 36          ; COM1
IRQ  5, 37          ; LPT2 / sound
IRQ  6, 38          ; floppy
IRQ  7, 39          ; LPT1 / spurious
IRQ  8, 40          ; RTC
IRQ  9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44          ; PS/2 mouse
IRQ 13, 45          ; FPU
IRQ 14, 46          ; primary ATA
IRQ 15, 47          ; secondary ATA

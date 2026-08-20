;=============================================================================
; NexxoN OS - Linux x86_64 SYSCALL entry
;-----------------------------------------------------------------------------
; The `syscall` instruction (used by every native Linux x86_64 binary) enters
; here via MSR_LSTAR.  Unlike `int 0x80`, `syscall` does NOT switch stacks and
; does NOT push a frame — it only loads CS/SS from STAR, saves RIP->RCX and
; RFLAGS->R11, and masks RFLAGS by SFMASK.  So we:
;   1. stash the user RSP, switch to a private kernel stack,
;   2. save the full register file into a lin_regs_t on that stack,
;   3. call linux_syscall_dispatch(&regs) (C) — it reads args + writes ->rax,
;   4. restore the register file and IRETQ back to CPL=3.
; We return with IRETQ rather than SYSRET so we don't depend on the GDT
; following the strict SYSRET selector-ordering convention.
;=============================================================================
bits 64

global linux_syscall_entry
extern linux_syscall_dispatch

; kernel-visible scratch (defined in linuxsys.c)
extern g_lin_kstack_top
extern g_lin_user_rsp
extern g_lin_syscall_frame_override
extern g_lin_syscall_return_frame
extern g_lin_return_compat32

%define LIN_REGS_QWORDS 19
%define LIN_REGS_BYTES  (LIN_REGS_QWORDS * 8)

linux_syscall_entry:
    mov     [rel g_lin_user_rsp], rsp
    mov     rsp, [rel g_lin_kstack_top]

    ; Build lin_regs_t (see linux.c).  Push high field first so the final
    ; RSP points at offset 0 (rax).  Layout (offsets):
    ;   0 rax  8 rbx 16 rcx 24 rdx 32 rsi 40 rdi 48 rbp
    ;  56 r8  64 r9  72 r10 80 r11 88 r12 96 r13 104 r14 112 r15
    ; 120 rip 128 rflags 136 user_rsp
    push    qword [rel g_lin_user_rsp]  ; user_rsp (136)
    push    r11                          ; rflags   (128)  (syscall saved RFLAGS)
    push    rcx                          ; rip      (120)  (syscall saved RIP)
    push    r15
    push    r14
    push    r13
    push    r12
    push    r11
    push    r10
    push    r9
    push    r8
    push    rbp
    push    rdi
    push    rsi
    push    rdx
    push    rcx
    push    rbx
    push    rax                          ; rax      (0)

    mov     rdi, rsp                     ; &regs
    cld
    ; SFMASK cleared IF on entry.  C handlers such as nanosleep and blocking
    ; terminal read use HLT while waiting for IRQ-driven progress, so re-enable
    ; interrupts only after the private kernel stack and complete save frame
    ; are established.  Disable them again while constructing the IRET frame.
    sti
    call    linux_syscall_dispatch
    cli

    cmp     byte [rel g_lin_syscall_frame_override], 0
    je      .reload_from_kstack
    mov     rsp, [rel g_lin_kstack_top]
    sub     rsp, LIN_REGS_BYTES
    mov     rdi, rsp
    lea     rsi, [rel g_lin_syscall_return_frame]
    mov     rcx, LIN_REGS_QWORDS
    rep     movsq
.reload_from_kstack:
    ; Reload everything from the (possibly modified) struct.  r10 is used as
    ; the struct base pointer until the very end.
    mov     r10, rsp
    mov     rax, [r10 + 0]               ; syscall return value
    mov     rbx, [r10 + 8]
    mov     rdx, [r10 + 24]
    mov     rsi, [r10 + 32]
    mov     rdi, [r10 + 40]
    mov     rbp, [r10 + 48]
    mov     r8,  [r10 + 56]
    mov     r9,  [r10 + 64]
    mov     r12, [r10 + 88]
    mov     r13, [r10 + 96]
    mov     r14, [r10 + 104]
    mov     r15, [r10 + 112]
    mov     rcx, [r10 + 120]             ; user RIP  (clobbered per ABI, ok)
    mov     r11, [r10 + 128]             ; user RFLAGS (clobbered per ABI, ok)

    ; Build the IRETQ frame BELOW the saved lin_regs_t.  Using the absolute
    ; stack top here would make the first push overwrite user_rsp at
    ; [base+136], so the next push would return with RSP=0x23.  Keeping RSP at
    ; the struct base leaves every saved field intact until IRETQ consumes the
    ; new frame.
    mov     rsp, r10
    push    qword 0x23                   ; SS  (user data, RPL3)
    push    qword [r10 + 136]            ; RSP (user)
    push    r11                          ; RFLAGS (IF restored from user)
    cmp     byte [rel g_lin_return_compat32], 0
    je      .push_cs64
    push    qword 0x3B                   ; CS  (IA-32 compatibility, RPL3)
    jmp     .push_rip
.push_cs64:
    push    qword 0x1B                   ; CS  (64-bit user code, RPL3)
.push_rip:
    push    rcx                          ; RIP
    mov     r10, [r10 + 72]              ; restore r10 last (was the temp base)
    iretq

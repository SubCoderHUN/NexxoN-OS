; ============================================================================
; NexxoN OS - x86_64 cooperative/preemptive context switch  (elf64)
; ----------------------------------------------------------------------------
; ctx_switch(uintptr_t *old_rsp_ptr  /* rdi */, uintptr_t new_rsp /* rsi */)
;
; 64-bit rewrite of boot/ctx_switch.asm, providing the SAME `ctx_switch`
; symbol the real src/kernel/sched.c calls.  Saves the SysV AMD64 callee-saved
; registers (rbx, rbp, r12-r15) + RFLAGS on the current kernel stack, stores
; the resulting RSP through old_rsp_ptr, loads the next task's RSP, restores
; its set and `ret`s into it.
; ============================================================================
[BITS 64]
global ctx_switch
section .text

ctx_switch:
    pushfq                    ; save RFLAGS (incl. IF) so a task switched out
    push    rbx               ; cooperatively (IF=1) is NOT resumed with the
    push    rbp               ; ISR's IF=0 -> avoids a hlt-with-IF=0 deadlock
    push    r12
    push    r13
    push    r14
    push    r15
    mov     [rdi], rsp        ; *old_rsp_ptr = current RSP
    mov     rsp, rsi          ; switch to the new task's stack
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    popfq                     ; restore that task's RFLAGS (incl. IF)
    ret                       ; -> resume the new task (or its trampoline)

; ============================================================================
; NexxoN OS - setjmp / longjmp  (SysV AMD64, elf64)
; ----------------------------------------------------------------------------
; 64-bit rewrite of src/kernel/setjmp.asm.  See include/setjmp.h: jmp_buf is
; 8 x 64-bit = rbx, rbp, r12, r13, r14, r15, rsp, rip.
;
;   int  setjmp(jmp_buf env)            ; rdi = env  -> returns 0
;   void longjmp(jmp_buf env, int val)  ; rdi = env, esi = val (never returns)
;
; Only the SysV callee-saved integer registers + rsp + the return address are
; saved (no FP/SSE state — kernel handlers run integer-only).
; ============================================================================
[BITS 64]
section .text
global setjmp
global longjmp

; ---- int setjmp(jmp_buf env) ----------------------------------------------
; On entry [rsp] = return address, rdi = env.
setjmp:
    mov     [rdi + 0],  rbx
    mov     [rdi + 8],  rbp
    mov     [rdi + 16], r12
    mov     [rdi + 24], r13
    mov     [rdi + 32], r14
    mov     [rdi + 40], r15
    lea     rcx, [rsp + 8]          ; caller's RSP after our ret pops the addr
    mov     [rdi + 48], rcx
    mov     rcx, [rsp]              ; saved return address (the setjmp site)
    mov     [rdi + 56], rcx
    xor     eax, eax               ; first-entry return value = 0
    ret

; ---- void longjmp(jmp_buf env, int val) -----------------------------------
longjmp:
    mov     eax, esi               ; val
    test    eax, eax               ; C99: val 0 -> 1
    jnz     .nz
    mov     eax, 1
.nz:
    mov     rbx, [rdi + 0]
    mov     rbp, [rdi + 8]
    mov     r12, [rdi + 16]
    mov     r13, [rdi + 24]
    mov     r14, [rdi + 32]
    mov     r15, [rdi + 40]
    mov     rsp, [rdi + 48]        ; unwind to the setjmp caller's stack
    mov     rcx, [rdi + 56]        ; saved return address
    jmp     rcx                    ; non-local goto (eax = return value)

; ============================================================================
; NexxoN OS - x86_64 GDT reload + TSS load  (elf64)
; ----------------------------------------------------------------------------
; 64-bit equivalent of boot/gdt_flush.asm.  Provides the exact symbols the
; real src/kernel/gdt.c expects:
;
;   gdt_flush(rdi = &gdt_ptr)   - LGDT, reload the data segment registers to
;                                 the kernel data selector (0x10) and reload CS
;                                 to the kernel code selector (0x08) via a far
;                                 return (the only way to load CS in long mode).
;   tss_flush(di  = tss_sel)    - LTR so the CPU honours TSS.RSP0 on ring
;                                 transitions.
;
; SysV AMD64: first integer arg is in RDI.  Long mode treats DS/ES/SS as flat,
; but we still load a valid data selector so descriptor-cache state is sane.
; ============================================================================
[BITS 64]
section .text
global gdt_flush
global tss_flush

gdt_flush:
    lgdt    [rdi]                   ; load the new GDT

    mov     ax, 0x10                ; kernel data selector
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    ; Reload CS = 0x08 via a far return: push the target CS:RIP and lretq.
    push    qword 0x08
    lea     rax, [rel .reload_cs]
    push    rax
    o64 retf                        ; far return -> loads CS:RIP (64-bit)
.reload_cs:
    ret

tss_flush:
    mov     ax, di                  ; TSS selector (0x28)
    ltr     ax
    ret

; ============================================================================
; NexxoN OS - x86_64 long-mode boot stub  (PHASE 1 of the 64-bit port)
; ----------------------------------------------------------------------------
; GRUB (Multiboot v1) loads this elf64 image and enters `_start` in 32-bit
; protected mode (the multiboot spec mandates 32-bit PM hand-off even for an
; elf64 kernel).  Our job:
;
;   1. Verify the CPU supports long mode (CPUID 0x80000001, EDX bit 29).
;   2. Build a 4-level page table that identity-maps the low 4 GiB with
;      2 MiB pages (covers RAM + the VBE/MMIO framebuffer up to 4 GiB).
;   3. Enable PAE (CR4.PAE), set EFER.LME, turn on paging (CR0.PG) -> the
;      CPU is now in IA-32e compatibility mode.
;   4. Load a 64-bit GDT and far-jump into a 64-bit code segment (CS.L=1)
;      -> full 64-bit long mode.
;   5. Set up the stack + segment registers and call the C entry kmain64
;      with the Multiboot magic (RDI) and info pointer (RSI), SysV ABI.
;
; This is a SELF-CONTAINED proof-of-concept: it does not touch the working
; 32-bit kernel.  The real port reuses this exact long-mode bring-up.
; ============================================================================

%define MB_MAGIC        0x1BADB002
%define MB_FLAGS        ((1 << 0) | (1 << 1) | (1 << 2))   ; align|meminfo|video
%define MB_CHECKSUM     -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    dd 0, 0, 0, 0, 0            ; AOUT kludge (unused, ELF describes layout)
    dd 0                        ; mode_type = linear framebuffer
    dd 1024                     ; width
    dd 768                      ; height
    dd 32                       ; depth

; ----------------------------------------------------------------------------
section .bss
alignb 4096
pml4:   resb 4096
pdpt:   resb 4096
pd0:    resb 4096
pd1:    resb 4096
pd2:    resb 4096
pd3:    resb 4096
alignb 16
stack_bottom:
    resb 65536
stack_top:

section .data
mb_magic: dd 0
mb_info:  dd 0

; ----------------------------------------------------------------------------
section .rodata
align 8
gdt64:
    dq 0x0000000000000000          ; 0x00 null
    dq 0x00AF9A000000FFFF          ; 0x08 64-bit code (L=1, present, exec, read)
    dq 0x00AF92000000FFFF          ; 0x10 data        (present, read/write)
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

; ----------------------------------------------------------------------------
section .text
global _start
global read_cr2
extern kernel_main

[BITS 32]
_start:
    cli
    cld
    mov     esp, stack_top                 ; flat 4 GiB segs from GRUB
    mov     [mb_magic], eax
    mov     [mb_info], ebx

    ; --- 1. CPUID: long-mode support? ----------------------------------
    mov     eax, 0x80000000
    cpuid
    cmp     eax, 0x80000001
    jb      .no_long
    mov     eax, 0x80000001
    cpuid
    test    edx, 1 << 29                    ; LM bit
    jz      .no_long

    ; --- 2. Build identity page tables (low 4 GiB, 2 MiB pages) ---------
    mov     edi, pml4                       ; clear 6 tables (24 KiB)
    xor     eax, eax
    mov     ecx, (6 * 4096) / 4
    rep     stosd

    mov     eax, pdpt
    or      eax, 0x07                       ; present | rw
    mov     [pml4], eax

    mov     eax, pd0
    or      eax, 0x07
    mov     [pdpt + 0], eax
    mov     eax, pd1
    or      eax, 0x07
    mov     [pdpt + 8], eax
    mov     eax, pd2
    or      eax, 0x07
    mov     [pdpt + 16], eax
    mov     eax, pd3
    or      eax, 0x07
    mov     [pdpt + 24], eax

    mov     edi, pd0                        ; 2048 entries * 2 MiB = 4 GiB
    mov     eax, 0x87                       ; phys 0 | present | rw | user | PS
    mov     ecx, 2048
.fill_pd:
    mov     [edi], eax                      ; low dword = base | flags
    mov     dword [edi + 4], 0              ; high dword = 0 (phys < 4 GiB)
    add     eax, 0x200000                   ; next 2 MiB
    add     edi, 8
    dec     ecx
    jnz     .fill_pd

    ; --- 3. Enable PAE, long mode, paging ------------------------------
    mov     eax, pml4
    mov     cr3, eax

    mov     eax, cr4
    or      eax, 1 << 5                     ; CR4.PAE
    mov     cr4, eax

    mov     ecx, 0xC0000080                 ; EFER MSR
    rdmsr
    or      eax, 1 << 8                     ; EFER.LME
    wrmsr

    mov     eax, cr0
    or      eax, 0x80000001                 ; CR0.PG | CR0.PE
    mov     cr0, eax

    ; --- 4. Long jump into 64-bit code ---------------------------------
    lgdt    [gdt64_ptr]
    jmp     0x08:long_entry

.no_long:
    ; Print "NL" on COM1 (port 0x3F8, assume BIOS left it usable) + halt.
    mov     dx, 0x3F8
    mov     al, 'N'
    out     dx, al
    mov     al, 'L'
    out     dx, al
.hang32:
    hlt
    jmp     .hang32

[BITS 64]
long_entry:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     rsp, stack_top
    xor     rbp, rbp

    ; --- enable SSE: x86_64 baseline, and the SysV ABI returns float/double
    ;     in XMM registers, so the real kernel's spreadsheet / video code
    ;     (compiled WITHOUT -mno-sse) needs it on before any C runs.
    mov     rax, cr0
    and     ax, 0xFFFB                       ; clear CR0.EM (bit 2)
    or      ax, 0x0002                      ; set   CR0.MP (bit 1)
    mov     cr0, rax
    mov     rax, cr4
    or      rax, (1 << 9) | (1 << 10)       ; CR4.OSFXSR | CR4.OSXMMEXCPT
    mov     cr4, rax

    xor     rdi, rdi
    mov     edi, [mb_magic]                 ; arg1: multiboot magic
    xor     rsi, rsi
    mov     esi, [mb_info]                  ; arg2: multiboot info pointer
    call    kernel_main                     ; the real NexxoN kernel entry

.hang64:
    cli
    hlt
    jmp     .hang64

; ---- read_cr2: page-fault linear address (panic.c needs it) --------------
read_cr2:
    mov     rax, cr2
    ret

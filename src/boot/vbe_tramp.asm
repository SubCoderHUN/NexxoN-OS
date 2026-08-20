; ============================================================================
; NexxoN OS - VBE real-mode trampoline for LONG MODE (elf64)  - endgame item 7
; ----------------------------------------------------------------------------
; 64-bit equivalent of boot/vbe_tramp.asm.  Calling a real-mode BIOS INT 10h
; from long mode needs the full descent and climb:
;
;   long mode (64) --far--> compatibility (32) --clear PG/LME--> legacy PM (32)
;     --[the existing 16-bit blob]--> real mode --INT 10h (VBE)--> back up
;   legacy PM (32) --set PAE/CR3/LME/PG--far--> long mode (64) --> return to C
;
; The middle (legacy-PM -> 16-bit -> real -> INT 10h -> legacy-PM) is the SAME
; blob the 32-bit trampoline uses, copied verbatim except its return target.
; All scratch lives in identity-mapped low memory shared with vbe_table.c /
; vga.c (COMM/blob/ENUM addresses MUST match boot/vbe_tramp.asm).
;
; Stability: the trampoline is only ever entered from vga_switch_mode (Strategy
; 2) or vbe_table's enum, i.e. only on a resolution change or Settings open and
; only when the BGA is absent/forced - never on the boot path - so the desktop
; boots regardless of this code.
; ============================================================================

%define BLOB_ADDR       0x2000
%define COMM_ADDR       0x3000
%define TGDT_ADDR       0x3100
%define TGDT_PTR        0x3140      ; 6-byte (32-bit) GDT ptr for the blob's lgdt
%define RM_IDT_PTR      0x3180      ; real-mode IVT ptr {0x3FF, 0}

%define COMM_MODE       0x3000
%define COMM_RESULT     0x3002
%define COMM_PHYS       0x3004
%define COMM_PITCH      0x3008
%define COMM_WIDTH      0x300A
%define COMM_HEIGHT     0x300C
%define COMM_BPP        0x300E
%define COMM_OP         0x3010
%define COMM_TGT_W      0x3012
%define COMM_TGT_H      0x3014
%define COMM_COUNT      0x3016
%define ENUM_BUF        0x4400

; 64-bit save area (distinct from the 32-bit trampoline's 0x315x..0x318x slots)
%define S_GDT64         0x3200      ; 10 bytes: kernel GDTR (sgdt)
%define S_IDT64         0x3210      ; 10 bytes: kernel IDTR (sidt)
%define S_CR3           0x3220      ; 8 bytes
%define S_RSP           0x3228      ; 8 bytes
%define S_GDTPTR64      0x3230      ; 10 bytes: ptr to the transition GDT (long-mode lgdt)

section .text
global vbe_tramp_switch
global vbe_tramp_switch_res
global vbe_tramp_enum
global vbe_tramp_get_result

; ---- transition GDT (copied to TGDT_ADDR) ---------------------------------
section .rodata
align 8
tgdt64:
    dq 0                                         ; 0x00 null
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00                    ; 0x08 32-bit code
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00                    ; 0x10 32-bit data
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0x00, 0x00                    ; 0x18 16-bit code
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0x8F, 0x00                    ; 0x20 16-bit data (big=0)
    dw 0x0000, 0x0000
    db 0x00, 0x9A, 0x20, 0x00                    ; 0x28 64-bit code (L=1)
tgdt64_end:

; ============================================================================
; The relocatable blob: legacy-PM(32) entry -> 16-bit -> real -> INT 10h ->
; back to legacy-PM(32) -> jump to return_to_long.  Copied to 0x2000.
; This is boot/vbe_tramp.asm's blob VERBATIM except the final jump goes to our
; long-mode climb stub instead of restoring the 32-bit kernel.
; ============================================================================
section .text
align 16
blob_start:
[bits 16]
    mov     ax, 0x20            ; 16-bit data selector
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    ; paging is already OFF (the long-mode descent cleared CR0.PG), but keep the
    ; PE clear sequence identical to the proven 32-bit blob.
    mov     eax, cr0
    and     al, 0xFE            ; clear PE
    mov     cr0, eax
    jmp     0x0000:.real_mode - blob_start + BLOB_ADDR
.real_mode:
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     sp, 0x7C00
    lidt    [RM_IDT_PTR]

    cmp     word [COMM_OP], 2
    je      .enum_list
    cmp     word [COMM_OP], 1
    jne     .do_set
    mov     cx, 0x0100
.enum_loop:
    push    cx
    xor     ax, ax
    mov     es, ax
    mov     di, 0x4200
    mov     ax, 0x4F01
    int     0x10
    pop     cx
    cmp     ax, 0x004F
    jne     .enum_next
    test    byte [0x4200 + 0], 0x80
    jz      .enum_next
    mov     al, [0x4200 + 25]
    cmp     al, 32
    jne     .enum_next
    mov     ax, [0x4200 + 18]
    cmp     ax, [COMM_TGT_W]
    jne     .enum_next
    mov     ax, [0x4200 + 20]
    cmp     ax, [COMM_TGT_H]
    jne     .enum_next
    mov     [COMM_MODE], cx
    jmp     .do_set
.enum_next:
    inc     cx
    cmp     cx, 0x0200
    jb      .enum_loop
    mov     word [COMM_RESULT], 0
    jmp     .ret_to_pm

.enum_list:
    mov     word [COMM_COUNT], 0
    mov     cx, 0x0100
.el_loop:
    push    cx
    xor     ax, ax
    mov     es, ax
    mov     di, 0x4200
    mov     ax, 0x4F01
    int     0x10
    pop     cx
    cmp     ax, 0x004F
    jne     .el_next
    test    byte [0x4200 + 0], 0x80
    jz      .el_next
    cmp     byte [0x4200 + 25], 32
    jne     .el_next
    mov     ax, [COMM_COUNT]
    shl     ax, 2
    mov     di, ENUM_BUF
    add     di, ax
    mov     ax, [0x4200 + 18]
    mov     [di], ax
    mov     ax, [0x4200 + 20]
    mov     [di + 2], ax
    inc     word [COMM_COUNT]
    cmp     word [COMM_COUNT], 64
    jae     .el_done
.el_next:
    inc     cx
    cmp     cx, 0x0200
    jb      .el_loop
.el_done:
    mov     word [COMM_RESULT], 0x004F
    jmp     .ret_to_pm

.do_set:
    mov     bx, [COMM_MODE]
    or      bx, (1 << 14)
    mov     ax, 0x4F02
    int     0x10
    mov     [COMM_RESULT], ax
    mov     cx, [COMM_MODE]
    mov     ax, 0x1220
    mov     es, ax
    xor     di, di
    mov     ax, 0x4F01
    int     0x10
    mov     eax, [es:di + 40]
    xor     bx, bx
    mov     ds, bx
    mov     [COMM_PHYS], eax
    mov     ax, 0x1220
    mov     es, ax
    xor     di, di
    mov     ax, [es:di + 50]
    test    ax, ax
    jnz     .pitch_ok
    mov     ax, [es:di + 16]
.pitch_ok:
    mov     [COMM_PITCH], ax
    mov     ax, [es:di + 18]
    mov     [COMM_WIDTH], ax
    mov     ax, [es:di + 20]
    mov     [COMM_HEIGHT], ax
    mov     al, [es:di + 25]
    mov     [COMM_BPP], al

.ret_to_pm:
    cli
    lgdt    [TGDT_PTR]          ; 32-bit GDT ptr (transition GDT)
    mov     eax, cr0
    or      al, 1               ; set PE -> 32-bit protected mode
    mov     cr0, eax
    jmp     dword 0x08:(.pm32 - blob_start + BLOB_ADDR)
[bits 32]
.pm32:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    ; hand back to the long-mode climb stub.  return_to_long is kernel-resident
    ; (NOT part of this copied-to-0x2000 blob), so jump to its ABSOLUTE address
    ; via a register - a relative jmp would be wrong from the relocated blob.
    mov     eax, return_to_long
    jmp     eax
blob_end:

; ============================================================================
; 64-bit C entry points
; ============================================================================
[bits 64]

vbe_tramp_switch:               ; (uint16_t mode -> DI)
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    mov     [COMM_MODE], di
    mov     word [COMM_OP], 0
    jmp     common64

vbe_tramp_switch_res:           ; (uint16_t w -> DI, uint16_t h -> SI)
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    mov     [COMM_TGT_W], di
    mov     [COMM_TGT_H], si
    mov     word [COMM_OP], 1
    jmp     common64

vbe_tramp_enum:                 ; (void)
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    mov     word [COMM_OP], 2
    jmp     common64

common64:
    cli
    ; ---- save kernel state to restore on the way back ----
    sgdt    [S_GDT64]
    sidt    [S_IDT64]
    mov     rax, cr3
    mov     [S_CR3], rax
    mov     [S_RSP], rsp

    ; ---- real-mode IVT pointer {limit=0x3FF, base=0} at RM_IDT_PTR ----
    mov     word  [RM_IDT_PTR],     0x03FF
    mov     dword [RM_IDT_PTR + 2], 0

    ; ---- copy the transition GDT to TGDT_ADDR ----
    lea     rsi, [rel tgdt64]
    mov     edi, TGDT_ADDR
    mov     ecx, (tgdt64_end - tgdt64)
    rep     movsb

    ; 32-bit GDT ptr (for the blob's lgdt) at TGDT_PTR
    mov     word  [TGDT_PTR],     (tgdt64_end - tgdt64 - 1)
    mov     dword [TGDT_PTR + 2], TGDT_ADDR
    ; 64-bit (10-byte) GDT ptr for the long-mode lgdt below
    mov     word  [S_GDTPTR64],     (tgdt64_end - tgdt64 - 1)
    mov     qword [S_GDTPTR64 + 2], TGDT_ADDR

    mov     word [COMM_RESULT], 0

    ; ---- copy the blob to 0x2000 ----
    lea     rsi, [rel blob_start]
    mov     edi, BLOB_ADDR
    mov     ecx, (blob_end - blob_start)
    rep     movsb

    ; ---- load the transition GDT, far-jump to 32-bit code (compat mode) ----
    lgdt    [S_GDTPTR64]
    push    qword 0x08
    lea     rax, [rel compat32]
    push    rax
    o64 retf                    ; CS:RIP = 0x08:compat32  -> compatibility mode

[bits 32]
compat32:
    ; still in IA-32e (compat), paging on.  Leave long mode:
    mov     eax, cr0
    and     eax, 0x7FFFFFFF     ; clear CR0.PG  (LMA drops, now legacy PM)
    mov     cr0, eax
    mov     ecx, 0xC0000080     ; EFER
    rdmsr
    and     eax, ~0x00000100    ; clear EFER.LME
    wrmsr
    ; flush TLB (paging off now anyway)
    xor     eax, eax
    mov     cr3, eax
    mov     ax, 0x10            ; 32-bit data
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    ; enter the blob (16-bit code segment 0x18)
    jmp     0x18:BLOB_ADDR

; ============================================================================
; 32-bit climb stub: legacy PM -> long mode -> restore kernel -> return to C.
; Reached from the blob's .pm32 (paging off, flat 32-bit segs).
; ============================================================================
[bits 32]
return_to_long:
    cli
    mov     eax, cr4
    or      eax, (1 << 5)       ; CR4.PAE
    mov     cr4, eax
    mov     eax, [S_CR3]        ; restore the kernel PML4 (low 32; < 4 GiB)
    mov     cr3, eax
    mov     ecx, 0xC0000080     ; EFER
    rdmsr
    or      eax, 0x00000100     ; set EFER.LME
    wrmsr
    mov     eax, cr0
    or      eax, 0x80000001     ; CR0.PG | CR0.PE  -> IA-32e active (compat)
    mov     cr0, eax
    ; far-jump to the 64-bit code segment (0x28 in the transition GDT)
    jmp     0x28:long_back

[bits 64]
long_back:
    ; restore the kernel GDT/IDT, reload CS to the kernel code selector
    lgdt    [S_GDT64]
    lidt    [S_IDT64]
    push    qword 0x08          ; kernel 64-bit code selector (gdt.c kCS)
    lea     rax, [rel .csok]
    push    rax
    o64 retf
.csok:
    mov     ax, 0x10            ; kernel data selector
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     rsp, [S_RSP]
    sti
    movzx   eax, word [COMM_RESULT]
    cmp     ax, 0x004F
    je      .ok
    mov     eax, -1
    jmp     .done
.ok:
    xor     eax, eax
.done:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; ---- void vbe_tramp_get_result(uint32_t *phys, uint16_t *w, uint16_t *h,
;                                uint16_t *pitch)   (SysV: rdi,rsi,rdx,rcx) ----
vbe_tramp_get_result:
    test    rdi, rdi
    jz      .nw
    mov     eax, [COMM_PHYS]
    mov     [rdi], eax
.nw:
    test    rsi, rsi
    jz      .nh
    movzx   eax, word [COMM_WIDTH]
    mov     [rsi], ax
.nh:
    test    rdx, rdx
    jz      .np
    movzx   eax, word [COMM_HEIGHT]
    mov     [rdx], ax
.np:
    test    rcx, rcx
    jz      .ng
    movzx   eax, word [COMM_PITCH]
    mov     [rcx], ax
.ng:
    ret

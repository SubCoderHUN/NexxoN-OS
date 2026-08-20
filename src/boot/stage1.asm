; ============================================================================
; NexxoN OS - Stage-1 boot sector (LBA 0 / MBR) - REAL-HARDWARE EDITION
; ----------------------------------------------------------------------------
; Bulletproof revision targeting bare metal (VirtualBox, real PCs).  The
; QEMU-only first cut worked because QEMU is permissive about alignment and
; register state; real silicon enforces the spec strictly.  Changes:
;
;   * Explicit CLD so the lodsb in print_str always advances forward.
;     The BIOS doesn't guarantee DF=0 at boot - some real BIOSes leave
;     it however the last INT was using it.
;   * DAP aligned to 16 bytes - the EDD-3 spec doesn't mandate it but
;     a handful of older real BIOSes (Compaq, some Pheonix variants)
;     refuse the call otherwise.
;   * Visible "[ERROR] MBR DAP READ FAILED" diagnostic on disk-read
;     failure so the user knows WHERE we died on a screen they can see.
;     Previously a silent CF=1 fell straight through to the hang.
;   * Preserve DL across the print_str call - some BIOSes clobber DX
;     in INT 10h AH=0Eh on real hardware (it's not documented but it
;     happens) and we MUST hand the right drive number to stage-2.
; ============================================================================

[BITS 16]
[ORG 0x7C00]

stage1_start:
    cli
    cld                                 ; string ops MUST advance forward
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti

    ; Save the BIOS drive number BEFORE we touch DX.
    mov     [boot_drive], dl

    mov     si, msg_loading
    call    print_str

    ; Reload DL from our saved copy in case print_str / INT 10h scrambled it.
    mov     dl, [boot_drive]
    mov     si, dap
    mov     ah, 0x42
    int     0x13
    jc      stage2_load_fail

    mov     dl, [boot_drive]
    jmp     0x0000:0x8000

stage2_load_fail:
    mov     si, msg_disk_err
    call    print_str
    xor     ax, ax
    int     0x16
    int     0x19
.hang:
    hlt
    jmp     .hang

; -------- 16-bit BIOS teletype string printer ------------------------------
print_str:
    push    bx
    mov     ah, 0x0E
    xor     bx, bx                      ; BH=page 0, BL=fg colour 0
.loop:
    lodsb
    or      al, al
    jz      .done
    int     0x10
    jmp     .loop
.done:
    pop     bx
    ret

; -------- Disk-Address-Packet (DAP) for INT 13h AH=42h ---------------------
; Alignment to 16 bytes is overkill per the EDD spec but matches what we
; observed on a Compaq Phoenix BIOS - the deciding factor was a single
; report from a real Dell box that refused INT 13h AH=42h until the DAP
; was 16-byte aligned.  Cheap insurance.
align 16
dap:
    db      0x10            ; size of this packet
    db      0               ; reserved
    dw      63              ; sector count (whole stage-2 region)
    dw      0x8000          ; offset
    dw      0x0000          ; segment
    dq      1               ; starting LBA (stage-2 begins at sector 1)

boot_drive:  db 0

msg_loading: db "NexxoN MBR: loading stage-2 from disk via INT 13h AH=42h...", 13, 10, 0
msg_disk_err:db "[ERROR] MBR DAP READ FAILED.  Press a key to reboot.", 13, 10, 0

; -------- Pad to 446 bytes + room for partition table + boot signature ----
times 446-($-$$)        db 0

; Partition table (4 entries x 16 bytes).  The installer overwrites these.
times 64                db 0

; MBR signature: 0x55 0xAA at offset 510..511.
dw      0xAA55

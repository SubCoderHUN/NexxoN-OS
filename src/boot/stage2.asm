; ============================================================================
; NexxoN OS - Stage-2 boot loader (LBA 1..63 of the HDD) - REAL-HW EDITION
; ----------------------------------------------------------------------------
; Loaded by stage1.asm at 0x0000:0x8000 with DL = BIOS boot drive number.
; Job: bring up an environment indistinguishable (from the kernel's POV)
; from one a Multiboot-1 loader like GRUB would deliver.
;
; This is the "bulletproof on real silicon" rewrite.  The QEMU-only first
; cut had THREE silent-on-emulator-but-fatal-on-bare-metal bugs:
;
;  (1) get_e820_map set EDI = 0x00011000 and then dereferenced [es:di]
;      (16-bit addressing).  On real BIOS this addressed ES:0x1000 (the
;      BIOS scratch area), corrupting state and hanging the next INT 15h.
;      Fixed by setting ES = (MMAP_ADDR >> 4) and DI = 0 so the BIOS
;      writes to the right place.
;
;  (2) Used `pusha` / `popa` (16-bit) around code that manipulated 32-bit
;      registers - the upper halves of EBP / EDI / EAX leaked between
;      iterations.  Now uses `pushad` / `popad` everywhere.
;
;  (3) Set the VBE mode FIRST then tried to print disk-read progress
;      through INT 10h AH=0Eh, which behaves unpredictably on real
;      hardware once a graphics mode is active.  Re-ordered: E820 ->
;      disk read -> VBE -> PM -> ELF -> jump.
;
; Also: granular [BOOT] progress strings before every major milestone so
; a real-hardware hang has an obvious "I got here, I didn't get there"
; signature.
; ============================================================================

[BITS 16]
[ORG 0x8000]

; Multiboot v1 magic + flag bits (must match include/multiboot.h).
%define MB_MAGIC            0x2BADB002
%define MB_FLAG_MEM         (1 << 0)
%define MB_FLAG_BOOTDEV     (1 << 1)
%define MB_FLAG_MMAP        (1 << 6)
%define MB_FLAG_FRAMEBUFFER (1 << 12)

; Memory layout we own:
;   0x00007C00 - 0x00007E00   stage-1 (us, soon to be discarded)
;   0x00008000 - 0x0000FFFF   stage-2 (32 KiB max)
;   0x00010000 - 0x00010FFF   multiboot_info struct
;   0x00011000 - 0x000117FF   mmap entries (max 23 * 24 bytes = 552)
;   0x00012000 - 0x00012FFF   VBE controller / mode info blocks
;   0x00013000 - 0x000137FF   VBE mode table (up to 2 KiB, 64 entries)
;   0x00020000 - 0x000DFFFF   kernel ELF read buffer (~767 KiB)
;   0x00100000 - ...          final kernel image (set by ELF copy)

%define MBI_ADDR        0x00010000
%define MMAP_ADDR       0x00011000
%define VBE_CTRL_ADDR   0x00012000
%define VBE_MODE_ADDR   0x00012200
%define VBE_TABLE_ADDR  0x00013000
%define VBE_TABLE_MAX   64
%define ELF_BUFFER      0x00020000

; ---- ELF header / program-header field offsets ----------------------------
; Assembled twice: the default (elf32) loads the 32-bit kernel; with -DELF64
; (build64full) it loads the elf64 kernel.  e_entry (0x18, read as the low
; dword) and p_type (0) are at the same offset in both, so only the wider
; fields differ.  The kernel loads at 1 MiB and entry < 4 GiB, so reading the
; low 32 bits of the 64-bit fields is correct.
%ifdef ELF64
%define EH_PHOFF        0x20        ; e_phoff
%define EH_PHENTSIZE    0x36        ; e_phentsize
%define EH_PHNUM        0x38        ; e_phnum
%define PH_OFFSET       8           ; p_offset
%define PH_PADDR        24          ; p_paddr
%define PH_FILESZ       32          ; p_filesz
%define PH_MEMSZ        40          ; p_memsz
%else
%define EH_PHOFF        0x1C
%define EH_PHENTSIZE    0x2A
%define EH_PHNUM        0x2C
%define PH_OFFSET       4
%define PH_PADDR        12
%define PH_FILESZ       16
%define PH_MEMSZ        20
%endif

stage2_start:
    cli
    cld                                 ; defensive: BIOS doesn't guarantee DF=0
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00                  ; reuse stage-1's stack area
    sti

    mov     [boot_drive], dl

    mov     si, msg_stage2_hello
    call    print_str

    ; --------------------------------------------------------------
    ; Step 0: enable the A20 gate.
    ;
    ; ROOT-CAUSE FIX (confirmed from a power-off VCPU dump that showed
    ; EIP=0x00008A1F, CS=0x0008, and `A20 disabled (changed 1 times)`):
    ; the BIOS on VirtualBox/VMware/most real machines leaves A20 in an
    ; implementation-defined state at boot.  Stage1 + the previous
    ; stage2 never touched it.  When PM later copied the kernel ELF
    ; from ELF_BUFFER (0x20000) to its 1 MiB load address, address bit
    ; 20 was forced low — so writes to 0x108000+ wrapped back to
    ; 0x008000+, overwriting stage2's own .copy_dwords loop body and
    ; making the CPU spin on garbage instructions forever.
    ;
    ; QEMU's SeaBIOS enables A20 during POST, which is why this bug
    ; was invisible to `make run` but fatal on every real BIOS.
    ;
    ; Do this BEFORE any other step so subsequent BIOS calls (E820,
    ; INT 13h reads, VBE) also see a flat 1 MB-and-up address space,
    ; not just our own PM copy loop.
    ; --------------------------------------------------------------
    mov     si, msg_step_a20
    call    print_str
    call    enable_a20
    jc      .a20_failed
    mov     si, msg_a20_ok
    call    print_str
    jmp     .a20_done
.a20_failed:
    mov     si, msg_a20_fail
    call    print_str
    jmp     fatal_halt
.a20_done:

    ; --------------------------------------------------------------
    ; Step 1: BIOS memory map (E820).
    ; --------------------------------------------------------------
    mov     si, msg_step_e820
    call    print_str
    call    get_e820_map
    jc      .e820_failed
    mov     si, msg_e820_ok
    call    print_str
    jmp     .e820_done
.e820_failed:
    mov     si, msg_e820_fail
    call    print_str
    call    fabricate_simple_map
.e820_done:

    ; --------------------------------------------------------------
    ; Step 2: kernel ELF read.  Done BEFORE the VBE mode change so the
    ; user sees disk-progress output on the BIOS console.  After VBE
    ; flips us into a graphics framebuffer INT 10h teletype is allowed
    ; by the spec but real-hardware support is patchy.
    ; --------------------------------------------------------------
    mov     si, msg_step_disk
    call    print_str
    call    load_kernel_image
    jc      .disk_err
    mov     si, msg_kernel_ok
    call    print_str

    ; Print ELF e_entry HERE - in text mode, BEFORE VBE switches to
    ; graphics mode.  Once VBE is active, BIOS teletype is unreliable.
    ; A wrong address (anything except 0x00101000 for this build) means
    ; the kernel at LBA 64 is stale - run installsys from the GRUB ISO.
    mov     si, msg_kern_entry
    call    print_str
    mov     eax, [kern_entry_low]
    call    print_hex_dword
    mov     si, msg_newline
    call    print_str

    ; --------------------------------------------------------------
    ; Step 3: VBE mode (1024x768x32 linear FB).  Last visible BIOS
    ; teletype step - everything below either succeeds silently or
    ; halts with a halt-banner.
    ; --------------------------------------------------------------
    mov     si, msg_step_vbe
    call    print_str
    call    vbe_set_mode
    jc      .vbe_failed
    mov     si, msg_vbe_ok
    call    print_str
    jmp     .vbe_done
.vbe_failed:
    mov     si, msg_vbe_fail
    call    print_str
    ; No fallback: the kernel has no text-mode renderer.  Halt with the
    ; user looking at a readable diagnostic instead of a black screen.
    jmp     fatal_halt
.vbe_done:

    ; --------------------------------------------------------------
    ; Step 4: switch to protected mode.  After VBE set the screen is
    ; in graphics mode and BIOS teletype is unreliable, but the
    ; INT 10h call below is the LAST visible string we can render
    ; through the BIOS - emit it via the canonical 0xB8000 text-mode
    ; cell too, so even if BIOS teletype no-ops in graphics mode the
    ; user gets a visible 'P' top-left while we lgdt + flip CR0.PE.
    ; --------------------------------------------------------------
    mov     si, msg_step_pm
    call    print_str

    ; Defensive direct text-mode write at 0xB8000 (page 0).  Harmless
    ; if VBE already mapped graphics memory at that address - the byte
    ; will be invisible but won't fault.
    push    es
    mov     ax, 0xB800
    mov     es, ax
    mov     word [es:0], 0x0F50          ; 'P' (white-on-black, top-left)
    pop     es

    cli
    cld
    lgdt    [gdt_descriptor]
    mov     eax, cr0
    or      al, 1
    mov     cr0, eax
    jmp     0x08:pm_entry

.disk_err:
    mov     si, msg_disk_err
    call    print_str
    jmp     fatal_halt

; -------- 16-bit BIOS teletype string printer ------------------------------
; Preserves BX (the printable string's content may want it preserved by
; the caller; INT 10h shouldn't trash BX but real-hardware BIOSes have
; been observed clobbering it on rare paths). */
print_str:
    push    ax
    push    bx
    push    si
    mov     ah, 0x0E
    xor     bx, bx                      ; BH=page 0, BL=fg 0
.loop:
    lodsb
    or      al, al
    jz      .done
    int     0x10
    jmp     .loop
.done:
    pop     si
    pop     bx
    pop     ax
    ret

; -------- 16-bit BIOS teletype hex printer --------------------------------
; Prints EAX as 8 uppercase hex digits via INT 10h AH=0Eh.
; Preserves all registers.
print_hex_dword:
    push    ax
    push    bx
    push    cx
    push    dx
    push    si
    mov     cx, 8
.phd_loop:
    rol     eax, 4
    push    ax
    and     al, 0x0F
    add     al, '0'
    cmp     al, '9'
    jbe     .phd_emit
    add     al, 7                       ; skip ':' .. '@' gap to reach 'A'
.phd_emit:
    mov     ah, 0x0E
    xor     bx, bx
    int     0x10
    pop     ax
    dec     cx
    jnz     .phd_loop
    pop     si
    pop     dx
    pop     cx
    pop     bx
    pop     ax
    ret

fatal_halt:
    mov     si, msg_fatal
    call    print_str
    xor     ax, ax
    int     0x16
    int     0x19
.hang:
    hlt
    jmp     .hang

; ============================================================================
; ===== enable_a20: turn on the A20 gate using whichever method works  =======
; ============================================================================
; On entry: real mode, interrupts on, segments don't matter.
; On exit : CF=0 on success (A20 verifiably enabled), CF=1 on failure.
;           Preserves all general-purpose registers.
;
; Order of attempts:
;   1. test_a20 - maybe the BIOS already did it
;   2. INT 15h AX=2401h - the canonical BIOS-provided method
;   3. Fast A20 via I/O port 0x92 - works on every PIIX/ICH and most real PCs
;   4. 8042 keyboard controller - the original PC/AT method, slowest but the
;      most universally supported
;
; We test after each attempt and return as soon as A20 reads back enabled.
; If all four checks fail we set CF=1 and let the caller decide whether to
; halt with a readable error or press on regardless.
; ============================================================================
enable_a20:
    pushad

    call    test_a20
    jnc     .ok                          ; already enabled - nothing to do

    ; --- Method 1: BIOS ------------------------------------------------
    mov     ax, 0x2401
    int     0x15
    call    test_a20
    jnc     .ok

    ; --- Method 2: Fast A20 via port 0x92 ------------------------------
    ; CRITICAL: bit 0 of port 0x92 is the "INIT_NOW" reset bit on Intel
    ; chipsets - writing it triggers a full CPU reset.  We MUST mask it
    ; off (and bit 7 in some clone chipsets) before writing back.
    in      al, 0x92
    test    al, 0x02
    jnz     .skip_fast                   ; bit 1 already set; skip the OUT
    or      al, 0x02                     ; set bit 1 = A20 enable
    and     al, 0xFE                     ; clear bit 0 to avoid CPU reset
    out     0x92, al
.skip_fast:
    call    test_a20
    jnc     .ok

    ; --- Method 3: 8042 keyboard controller ---------------------------
    cli
    call    .kbc_wait_in_empty
    mov     al, 0xAD                     ; disable keyboard while we poke it
    out     0x64, al

    call    .kbc_wait_in_empty
    mov     al, 0xD0                     ; "read output port" command
    out     0x64, al

    call    .kbc_wait_out_full
    in      al, 0x60                     ; current output port byte
    push    eax

    call    .kbc_wait_in_empty
    mov     al, 0xD1                     ; "write output port" command
    out     0x64, al

    call    .kbc_wait_in_empty
    pop     eax
    or      al, 0x02                     ; set A20 bit
    out     0x60, al

    call    .kbc_wait_in_empty
    mov     al, 0xAE                     ; re-enable keyboard
    out     0x64, al

    call    .kbc_wait_in_empty
    sti
    call    test_a20
    jnc     .ok

    ; All four methods failed: still wrapping.  Return CF=1.
    popad
    stc
    ret

.ok:
    popad
    clc
    ret

; 8042 helpers: spin until the controller's input buffer (bit 1 of port
; 0x64) is empty / its output buffer (bit 0) is full.  No timeout - if the
; KBC is wedged we'd rather hang here with a recognisable [BOOT] string on
; screen than silently fall through into the unreliable 4th-method path.
.kbc_wait_in_empty:
    in      al, 0x64
    test    al, 0x02
    jnz     .kbc_wait_in_empty
    ret
.kbc_wait_out_full:
    in      al, 0x64
    test    al, 0x01
    jz      .kbc_wait_out_full
    ret

; ============================================================================
; ===== test_a20: probe whether A20 is currently enabled =====================
; ============================================================================
; Method: write 0xAA at 0x0000:0x0500 and 0x55 at 0xFFFF:0x0510 (= physical
; 0x100500 with A20 on, or 0x000500 with A20 off because bit 20 wraps).
; If both addresses read back the same value after the second write, A20
; is wrapping and therefore DISABLED.  Restores the byte at 0x0500 before
; returning so we don't corrupt the BDA scratch area.
;
; Returns CF=0 if A20 enabled, CF=1 if disabled.  Preserves all registers.
; ============================================================================
test_a20:
    pushad
    push    ds
    push    es

    xor     ax, ax
    mov     es, ax                       ; ES:DI -> 0x0000:0x0500
    mov     di, 0x0500

    mov     ax, 0xFFFF
    mov     ds, ax                       ; DS:SI -> 0xFFFF:0x0510 = phys 0x100500
    mov     si, 0x0510

    ; Save what's already at the two test cells so we can restore them.
    mov     dl, [es:di]
    mov     dh, [ds:si]

    mov     byte [es:di], 0xAA           ; phys 0x000500 := 0xAA
    mov     byte [ds:si], 0x55           ; phys 0x100500 := 0x55 (or 0x000500 if A20 off)

    ; A short pause + serialising load so the second write is committed
    ; before we re-read the first cell.  Belt-and-braces; modern x86 is
    ; coherent here but some emulators (Bochs in particular) have been
    ; observed reordering MMIO-ish writes around real-mode segment limits.
    mov     al, [es:di]                  ; AL = byte now at phys 0x000500
    mov     bl, al

    ; Restore original bytes BEFORE branching on result.
    mov     [es:di], dl
    mov     [ds:si], dh

    pop     es
    pop     ds

    cmp     bl, 0x55                     ; if 0x55 reached 0x000500, A20 wraps
    je      .disabled
    popad
    clc                                  ; A20 enabled
    ret
.disabled:
    popad
    stc
    ret

; ============================================================================
; ===== get_e820_map: BIOS E820 -> multiboot mmap entries  ===================
; ============================================================================
; Output buffer at MMAP_ADDR (=0x11000).  Each entry is 24 bytes:
;   +0   uint32 size      = 20 (excludes the size field itself)
;   +4   uint64 base
;   +12  uint64 length
;   +20  uint32 type       (1 = usable RAM)
;
; Returns CF=1 on failure (caller falls back to fabricate_simple_map).
; ============================================================================
get_e820_map:
    pushad                              ; save all 32-bit registers

    ; Set ES:DI to point at the output buffer.  The single most common
    ; real-hardware E820 bug is dereferencing [es:di] when DI doesn't
    ; actually hold the offset of MMAP_ADDR within ES - which is what
    ; the old `mov edi, MMAP_ADDR` style did when di was treated as
    ; a 16-bit operand (it discarded EDI's upper half).  Fixed by
    ; converting MMAP_ADDR to a proper segment value.
    mov     ax, MMAP_ADDR >> 4
    mov     es, ax
    xor     di, di                      ; offset within MMAP segment

    xor     ebx, ebx                    ; continuation token = 0
    xor     ebp, ebp                    ; total bytes written

.next:
    ; Reserve the leading 4-byte size field; BIOS will write its 20 bytes
    ; immediately after.  We pre-fill the type field to "usable" (1) so a
    ; broken BIOS that only returns 16 bytes still yields a sane entry.
    mov     dword [es:di + 0],  20
    mov     dword [es:di + 20], 1

    mov     eax, 0xE820
    mov     edx, 0x534D4150             ; 'SMAP'
    mov     ecx, 20
    push    di
    add     di, 4                       ; BIOS writes at [ES:DI+4]
    int     0x15
    pop     di
    jc      .err
    cmp     eax, 0x534D4150
    jne     .err
    cmp     ecx, 20                     ; entry truncated?
    jb      .skip                       ; skip without advancing (defensive)

    add     di, 24
    add     ebp, 24

.skip:
    test    ebx, ebx
    jz      .done
    cmp     ebp, 23 * 24                ; buffer ceiling
    jae     .done
    jmp     .next

.err:
    popad
    stc
    ret
.done:
    mov     [mmap_length], ebp
    popad
    clc
    ret

; -------- Fabricate a "0..512MB usable" entry if E820 is unavailable ------
fabricate_simple_map:
    pushad
    mov     ax, MMAP_ADDR >> 4
    mov     es, ax
    xor     di, di
    mov     dword [es:di + 0],  20
    mov     dword [es:di + 4],  0           ; base lo
    mov     dword [es:di + 8],  0           ; base hi
    mov     dword [es:di + 12], 0x20000000  ; length lo (512 MiB)
    mov     dword [es:di + 16], 0           ; length hi
    mov     dword [es:di + 20], 1           ; type = usable
    mov     dword [mmap_length], 24
    popad
    ret

; ============================================================================
; ===== load_kernel_image: disk -> ELF_BUFFER  ===============================
; ============================================================================
; Bulletproofed flow against real-hardware quirks (the QEMU-only first cut
; hung on VirtualBox with a flashing cursor after "Loading kernel ELF..."):
;
;   1. Reset segment regs DS=ES=0.  print_str above us went through
;      INT 10h AH=0Eh which on real BIOS has been observed clobbering
;      both DS and ES.  INT 13h reads its DAP from DS:SI - if DS is
;      wrong the BIOS sees garbage in the size byte and either errors
;      out or, fatally, sits in a polling loop waiting for hardware
;      it didn't program.  That looks identical to "infinite loop".
;
;   2. INT 13h AH=41h "Check Extensions Present" before AH=42h.  If
;      the BIOS doesn't advertise LBA support, calling AH=42h is
;      undefined - some return CF=1 cleanly, others hang.  Always
;      check first.  No retry: if extensions aren't there we can't
;      load a kernel anyway.
;
;   3. INT 13h AH=00h "Reset Disk System" before the first read.
;      Puts the controller into a known state regardless of what
;      stage-1 left it in.  Some real BIOSes refuse the first AH=42h
;      until a reset has been issued post-boot-control-transfer.
;
;   4. Granular [BOOT]/[FATAL] prints around every step so the next
;      "stuck after X, before Y" report names the exact instruction.
;
;   5. Every error path prints a distinct diagnostic and sets CF=1
;      so the caller halts with the user looking at a readable
;      reason instead of a blinking cursor.
;
; Two-phase load:
;   (a) Read sector 64 alone, parse program-header table to compute the
;       maximum byte offset we actually need.
;   (b) Read the rest in 63-sector chunks via INT 13h AH=42h.
; ============================================================================
load_kernel_image:
    pushad

    ; --- Defensive segment-register reset ------------------------------
    ; print_str called INT 10h AH=0Eh; real BIOSes sometimes clobber
    ; DS and ES on that path.  Restore them so INT 13h reads the DAP
    ; from the right place.
    xor     ax, ax
    mov     ds, ax
    mov     es, ax

    ; --- Step 1: Check BIOS disk extensions (AH=41h) -------------------
    mov     si, msg_disk_check_ext
    call    print_str
    mov     ah, 0x41
    mov     bx, 0x55AA
    mov     dl, [boot_drive]
    int     0x13
    jc      .no_ext_extensions
    cmp     bx, 0xAA55
    jne     .no_ext_extensions
    test    cx, 1                       ; CX bit 0: AH=42h-family supported
    jz      .no_ext_extensions
    mov     si, msg_disk_ext_ok
    call    print_str

    ; --- Step 2: Reset disk system (AH=00h) ----------------------------
    mov     si, msg_disk_reset
    call    print_str
    xor     ah, ah                      ; AH=0x00 = reset disk
    mov     dl, [boot_drive]
    int     0x13
    ; Don't fail on reset error - some BIOSes return junk in CF for
    ; reset of HDDs.  The next AH=42h call will catch real problems.

    ; --- Step 3: read sector 64 (ELF header + program headers) ---------
    mov     si, msg_disk_phase1
    call    print_str

    xor     ax, ax
    mov     ds, ax                      ; re-set DS before another BIOS call
    mov     es, ax
    mov     word [dap_sectors], 1
    mov     word [dap_off],     (ELF_BUFFER & 0x0F)
    mov     word [dap_seg],     (ELF_BUFFER >> 4)
    mov     dword [dap_lba_lo], 64
    mov     dword [dap_lba_hi], 0
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jc      .read_phase1_failed
    mov     si, msg_disk_phase1_ok
    call    print_str

    ; --- Step 4: parse ELF header + SAVE the program-header table ------
    ; ROOT-CAUSE FIX (the "installed boot crashes after the banner" bug):
    ; the kernel ELF file image is now ~590 KiB.  The old loader staged
    ; the WHOLE file at ELF_BUFFER (0x20000); .rodata's file bytes then
    ; landed at 0x20000+0x70000 = 0x90000.. which runs PAST 0xA0000 into
    ; the reserved video/BIOS hole, silently corrupting the upper .rodata
    ; (kvsnprintf's jump table) -> wild jump -> #GP right after the
    ; banner.  640 KiB conventional RAM can no longer hold the file.
    ;
    ; Fix: never stage the whole file low.  Read sector 64, save the
    ; phdr table to a stage2-local buffer, then for each PT_LOAD bounce
    ; 63-sector chunks through ELF_BUFFER and copy them DIRECTLY to their
    ; >1 MiB p_paddr using a flat (unreal-mode) segment.  GRUB/LIVE is
    ; unaffected (it never runs stage2).
    mov     si, msg_disk_parse
    call    print_str

    mov     ax, ELF_BUFFER >> 4         ; ES = 0x2000 -> phys base 0x20000
    mov     es, ax
    cmp     dword [es:0], 0x464C457F    ; '\x7FELF' magic check
    jne     .bad_elf

    mov     eax, dword [es:0x18]        ; e_entry
    mov     [kern_entry_low], eax
    mov     cx, [es:EH_PHNUM]              ; e_phnum
    cmp     cx, 16
    ja      .bad_elf
    mov     [saved_phnum], cx
    mov     bx, [es:EH_PHENTSIZE]          ; e_phentsize
    mov     [saved_phentsize], bx
    mov     ax, cx
    mul     bx                          ; AX = phnum * phentsize  (small)
    mov     cx, ax                      ; byte count
    mov     si, [es:EH_PHOFF]              ; e_phoff (low 16)
    xor     di, di
    push    ds
    xor     ax, ax
    mov     ds, ax                      ; DS=0 so [saved_ph] is absolute
.copy_ph:
    mov     al, [es:si]
    mov     [saved_ph + di], al
    inc     si
    inc     di
    dec     cx
    jnz     .copy_ph
    pop     ds
    mov     si, msg_disk_parse_ok
    call    print_str

    ; --- Step 5: enable unreal mode so we can write above 1 MiB --------
    call    setup_unreal

    ; --- Step 6: per-PT_LOAD bounce read + flat copy to p_paddr --------
    mov     si, msg_disk_phase2
    call    print_str
    xor     ax, ax
    mov     ds, ax                      ; DS=0 for all [stage2-var] access
    mov     cx, [saved_phnum]
    mov     [ph_left], cx
    mov     word [ph_cur], saved_ph

.ploop:
    cmp     word [ph_left], 0
    je      .read_done
    mov     bx, [ph_cur]
    cmp     dword [bx], 1               ; PT_LOAD?
    jne     .pnext

    mov     eax, [bx + PH_OFFSET]      ; p_offset
    shr     eax, 9
    add     eax, 64                     ; file LBA = 64 + p_offset/512
    mov     [cur_lba], eax
    mov     eax, [bx + PH_PADDR]       ; p_paddr
    mov     [cur_dst], eax
    mov     eax, [bx + PH_FILESZ]      ; p_filesz
    mov     [cur_rem], eax

.seg_read:
    cmp     dword [cur_rem], 0
    je      .seg_zero
    mov     eax, [cur_rem]
    add     eax, 511
    shr     eax, 9                      ; sectors still needed
    cmp     eax, 63
    jbe     .have_sect
    mov     eax, 63
.have_sect:
    mov     [cur_sect], ax
    xor     dx, dx
    mov     ds, dx                      ; DS=0 for the DAP
    mov     word [dap_sectors], ax
    mov     word [dap_off], (ELF_BUFFER & 0x0F)
    mov     word [dap_seg], (ELF_BUFFER >> 4)
    mov     eax, [cur_lba]
    mov     [dap_lba_lo], eax
    mov     dword [dap_lba_hi], 0
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jc      .read_phase2_failed
    ; copy_bytes = min(cur_rem, cur_sect * 512)
    movzx   eax, word [cur_sect]
    shl     eax, 9
    mov     edx, [cur_rem]
    cmp     edx, eax
    jae     .copy_full
    mov     eax, edx
.copy_full:
    mov     esi, ELF_BUFFER
    mov     edi, [cur_dst]
    mov     ecx, eax
    call    copy_flat                   ; preserves all regs (pushad)
    add     [cur_dst], eax
    sub     [cur_rem], eax
    movzx   edx, word [cur_sect]
    add     [cur_lba], edx
    jmp     .seg_read

.seg_zero:
    mov     bx, [ph_cur]
    mov     eax, [bx + PH_MEMSZ]       ; p_memsz
    sub     eax, [bx + PH_FILESZ]      ; - p_filesz = BSS bytes to clear
    jz      .pnext
    mov     edi, [cur_dst]             ; = p_paddr + p_filesz
    mov     ecx, eax
    call    zero_flat

.pnext:
    mov     ax, [ph_cur]
    add     ax, [saved_phentsize]
    mov     [ph_cur], ax
    dec     word [ph_left]
    jmp     .ploop

.read_done:
    mov     si, msg_disk_phase2_ok
    call    print_str
    popad
    clc
    ret

.no_ext_extensions:
    mov     si, msg_disk_no_ext
    call    print_str
    popad
    stc
    ret
.read_phase1_failed:
    mov     si, msg_disk_p1_fail
    call    print_str
    popad
    stc
    ret
.read_phase2_failed:
    mov     si, msg_disk_p2_fail
    call    print_str
    popad
    stc
    ret
.bad_elf:
    mov     si, msg_disk_bad_elf
    call    print_str
    popad
    stc
    ret
.bad_chunk:
    mov     si, msg_disk_zero_chunk
    call    print_str
    popad
    stc
    ret

; ============================================================================
; ===== vbe_set_mode: enumerate VBE modes, pick 1024x768x32 LFB  =============
; ============================================================================
; Walks the controller's VideoModePtr list (0xFFFF-terminated word array)
; and picks the first mode that satisfies our criteria:
;   ModeAttributes bit 0   set: mode supported by hardware
;   ModeAttributes bit 4   set: graphics mode (not text)
;   ModeAttributes bit 7   set: linear framebuffer mode available
;   XResolution          == 1024
;   YResolution          == 768
;   BitsPerPixel         == 32
; Then sets the mode with the LFB bit (mode_num | (1 << 14)) and re-queries
; the mode info into VBE_MODE_ADDR.  Returns CF=1 if no mode matches.
; ============================================================================
vbe_set_mode:
    pushad

    ; --- VBE controller info -------------------------------------------
    mov     ax, VBE_CTRL_ADDR >> 4
    mov     es, ax
    xor     di, di
    mov     dword [es:di], 'VBE2'        ; ask for VBE 2.0+ extended info
    mov     ax, 0x4F00
    int     0x10
    cmp     ax, 0x004F
    jne     .err

    ; The mode-list pointer is a real-mode far pointer at offset 0x0E
    ; (word offset, then word segment).
    mov     ax, [es:di + 0x0E]
    mov     bx, [es:di + 0x10]
    mov     [mode_list_off], ax
    mov     [mode_list_seg], bx

    ; --- Zero the VBE mode table header at VBE_TABLE_ADDR -------------
    ; Use GS = VBE_TABLE_ADDR >> 4 so all table accesses stay within a
    ; 16-bit offset, avoiding #GP on real hardware (segment limit = 0xFFFF).
    push    ax
    mov     ax, VBE_TABLE_ADDR >> 4          ; GS = 0x1300
    mov     gs, ax
    pop     ax
    xor     ax, ax
    mov     [gs:0x0000], ax                  ; count = 0
    mov     [gs:0x0002], ax                  ; padding = 0

    ; --- Walk the mode list --------------------------------------------
    mov     fs, [mode_list_seg]
    mov     si, [mode_list_off]
.next_mode:
    mov     cx, [fs:si]
    cmp     cx, 0xFFFF
    je      .err
    add     si, 2

    ; Dot per mode tried: gives a visible mode-count on the console so
    ; a real-hardware hang shows exactly how many modes were checked.
    push    cx
    push    si
    mov     ah, 0x0E
    mov     al, '.'
    xor     bx, bx
    int     0x10
    pop     si
    pop     cx

    ; Get mode info -> VBE_MODE_ADDR.  Save CX (mode number) and SI
    ; (list cursor) around the INT 10h call: some BIOS implementations
    ; (including VirtualBox's VBE BIOS) clobber these on AX=4F01h.
    push    si
    push    cx
    mov     ax, VBE_MODE_ADDR >> 4
    mov     es, ax
    xor     di, di
    ; CX = mode number (required input for AX=4F01h)
    mov     ax, 0x4F01
    int     0x10
    pop     cx                          ; restore mode number
    pop     si                          ; restore list cursor
    cmp     ax, 0x004F
    jne     .next_mode

    ; Filter on ModeAttributes (offset 0x00, word).  All four bits MUST
    ; be set: real silicon refuses or silently fails INT 10h AX=4F02h
    ; on modes lacking any of them.
    mov     ax, [es:di]
    test    ax, 1                        ; bit 0: supported
    jz      .next_mode
    test    ax, 1 << 4                   ; bit 4: graphics (not text)
    jz      .next_mode
    test    ax, 1 << 7                   ; bit 7: linear FB available
    jz      .next_mode

    ; --- Store 32bpp LFB graphics modes in the VBE mode table ----------
    ; At this point the mode has attributes bits 0, 4, 7 set (supported,
    ; graphics, LFB).  Now check for 32bpp before adding to the table.
    cmp     byte [es:di + 0x19], 32      ; BitsPerPixel == 32?
    jne     .skip_table_store            ; not 32bpp -> skip table, skip 1024x768 match

    ; Check if table is full (max VBE_TABLE_MAX entries).
    ; GS = VBE_TABLE_ADDR >> 4, so GS:0 = VBE_TABLE_ADDR (set before loop).
    movzx   eax, word [gs:0x0000]
    cmp     ax, VBE_TABLE_MAX
    jge     .table_full                  ; table full -> skip store, still try 1024x768

    ; Compute entry offset within GS segment: 4 + (count * 16)
    ; EAX = count (already loaded above)
    shl     ax, 4                        ; count * 16
    add     ax, 4                        ; skip 4-byte header

    ; Store mode_number (CX)
    push    bx
    mov     bx, ax                       ; BX = offset into GS segment

    mov     [gs:bx + 0x00], cx

    ; Store XResolution (width) from VBE mode info offset 0x12
    mov     ax, [es:di + 0x12]
    mov     [gs:bx + 0x02], ax

    ; Store YResolution (height) from VBE mode info offset 0x14
    mov     ax, [es:di + 0x14]
    mov     [gs:bx + 0x04], ax

    ; Store BytesPerScanLine (pitch) from VBE mode info offset 0x10
    mov     ax, [es:di + 0x10]
    mov     [gs:bx + 0x06], ax

    ; Store PhysBasePtr (phys_base) from VBE mode info offset 0x28
    mov     eax, [es:di + 0x28]
    mov     [gs:bx + 0x08], eax

    ; Store BitsPerPixel (bpp) from VBE mode info offset 0x19
    mov     al, [es:di + 0x19]
    mov     [gs:bx + 0x0C], al

    ; Store reserved[3] = 0
    mov     byte [gs:bx + 0x0D], 0
    mov     word [gs:bx + 0x0E], 0

    pop     bx

    ; Increment the mode count
    inc     word [gs:0x0000]

.table_full:
    ; --- Fall through to the 1024x768x32 match check ------------------

    ; Filter: 1024x768x32.
    cmp     word [es:di + 0x12], 1024    ; XResolution
    jne     .next_mode
    cmp     word [es:di + 0x14], 768     ; YResolution
    jne     .next_mode
    ; BitsPerPixel is already known to be 32 (checked above for table store)
    ; so no need to re-check it here.
    jmp     .mode_matched

.skip_table_store:
    ; Mode is not 32bpp -> cannot match 1024x768x32, skip to next mode.
    jmp     .next_mode

.mode_matched:

    ; Match!  Set the mode with the LFB bit.  Bare-metal-defensive:
    ;   - Print "[BOOT] Setting VBE Mode..." BEFORE the call so a
    ;     hang inside INT 10h AX=4F02h is visibly attributable.
    ;   - Mask BX explicitly to 16 bits via `movzx ebx, bx` so the
    ;     upper half of EBX (left over from previous BIOS calls or
    ;     pushad-restored garbage) cannot leak into the BIOS handler
    ;     and trigger an exception on strict silicon.
    ;   - Bit 14 (linear-FB) is the documented LFB selector.  We do
    ;     NOT set bit 15 (preserve FB content) - we want a clean FB.
    push    cx
    mov     si, msg_vbe_setting
    call    print_str
    pop     cx
    mov     [vbe_match_mode], cx         ; stash mode# - 4F02h may clobber CX
    mov     bx, cx
    or      bx, 1 << 14
    movzx   ebx, bx                      ; clear upper EBX (defensive)
    mov     ax, 0x4F02
    int     0x10
    cmp     ax, 0x004F
    jne     .err
    mov     si, msg_vbe_set_ok
    call    print_str

    ; Re-query post-set so MBI fill-in reads canonical framebuffer base.
    ; CX must be the mode number for AX=4F01h; restore from the stash
    ; because both 4F02h and print_str can clobber CX.
    mov     cx, [vbe_match_mode]
    mov     ax, VBE_MODE_ADDR >> 4
    mov     es, ax
    xor     di, di
    mov     ax, 0x4F01
    int     0x10

    popad
    clc
    ret
.err:
    popad
    stc
    ret

; ============================================================================
; ===== setup_unreal: enter "unreal mode" (flat 4 GiB FS) ====================
; ============================================================================
; Briefly flip to PM, load FS with the flat 4 GiB data descriptor (sel 0x10),
; then drop back to real mode WITHOUT reloading FS.  The cached descriptor
; (base 0, limit 4 GiB) persists, so copy_flat/zero_flat below can address
; physical memory above 1 MiB with 32-bit offsets via FS while INT 13h still
; reads to a low bounce buffer in plain real mode.  BIOS calls never touch
; FS, so the flat limit survives across the interleaved disk reads.
; Preserves every general register; leaves DS/ES/SS real-mode.
; ============================================================================
setup_unreal:
    pushad
    cli
    push    ds
    push    es
    lgdt    [gdt_descriptor]
    mov     eax, cr0
    or      al, 1
    mov     cr0, eax
    jmp     .flush1
.flush1:
    mov     bx, 0x10
    mov     fs, bx                      ; FS := flat 4 GiB descriptor
    mov     eax, cr0
    and     al, 0xFE
    mov     cr0, eax
    jmp     .flush2
.flush2:
    pop     es
    pop     ds
    sti
    popad
    ret

; ============================================================================
; ===== copy_flat: copy ECX bytes ELF buffer -> high RAM (flat FS) ===========
; ============================================================================
; ESI = source linear addr, EDI = dest linear addr, ECX = byte count.
; Both addressed through FS (flat, base 0) so EDI may be >= 1 MiB.
; Preserves every register.
; ============================================================================
copy_flat:
    pushad
    mov     ebx, ecx
    shr     ecx, 2                      ; dword count
    jz      .ctail
.cdw:
    mov     eax, [fs:esi]
    mov     [fs:edi], eax
    add     esi, 4
    add     edi, 4
    dec     ecx
    jnz     .cdw
.ctail:
    mov     ecx, ebx
    and     ecx, 3
    jz      .cdone
.cb:
    mov     al, [fs:esi]
    mov     [fs:edi], al
    inc     esi
    inc     edi
    dec     ecx
    jnz     .cb
.cdone:
    popad
    ret

; ============================================================================
; ===== zero_flat: zero ECX bytes at EDI (flat FS) ===========================
; ============================================================================
zero_flat:
    pushad
    xor     eax, eax
    mov     ebx, ecx
    shr     ecx, 2
    jz      .ztail
.zdw:
    mov     [fs:edi], eax
    add     edi, 4
    dec     ecx
    jnz     .zdw
.ztail:
    mov     ecx, ebx
    and     ecx, 3
    jz      .zdone
.zb:
    mov     [fs:edi], al
    inc     edi
    dec     ecx
    jnz     .zb
.zdone:
    popad
    ret

mode_list_off:  dw 0
mode_list_seg:  dw 0
vbe_match_mode: dw 0

; ---- Saved program-header table + per-segment load cursors --------------
align 4
saved_ph:        times 512 db 0
saved_phnum:     dw 0
saved_phentsize: dw 0
ph_left:         dw 0
ph_cur:          dw 0
cur_lba:         dd 0
cur_dst:         dd 0
cur_rem:         dd 0
cur_sect:        dw 0

; -------- Data ----------------------------------------------------------------
align 4
gdt_table:
    ; Null descriptor
    dq      0
    ; 0x08: 32-bit flat code  base=0 limit=0xFFFFF (4 GiB w/ G=1)
    dw      0xFFFF, 0x0000
    db      0x00, 0x9A, 0xCF, 0x00
    ; 0x10: 32-bit flat data
    dw      0xFFFF, 0x0000
    db      0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_descriptor:
    dw      gdt_end - gdt_table - 1
    dd      gdt_table

; The DAP for INT 13h AH=42h, aligned to 16 for the same defensive reason
; stage-1's DAP is aligned: a few real BIOSes refuse otherwise.
align 16
dap:
    db      0x10
    db      0
dap_sectors:    dw      0
dap_off:        dw      0
dap_seg:        dw      0
dap_lba_lo:     dd      0
dap_lba_hi:     dd      0

align 4
load_lba:       dq      0
load_dest:      dd      0
load_remain:    dw      0
load_chunk:     dw      0
mmap_length:    dd      0
boot_drive:     db      0
kern_entry_low: dd      0

msg_kern_entry: db "[BOOT] ELF e_entry=0x", 0
msg_newline:    db 13, 10, 0

msg_stage2_hello:db "[BOOT] NexxoN stage-2 active.",                       13, 10, 0
msg_step_a20:    db "[BOOT] Enabling A20 gate (BIOS->Fast->KBC)...",        13, 10, 0
msg_a20_ok:      db "[BOOT] A20 gate enabled.",                             13, 10, 0
msg_a20_fail:    db "[FATAL] A20 gate refused all enable methods.",         13, 10, 0
msg_step_e820:   db "[BOOT] Querying BIOS memory map via INT 15h E820...", 13, 10, 0
msg_e820_ok:     db "[BOOT] E820 memory map saved.",                       13, 10, 0
msg_e820_fail:   db "[WARN] E820 unavailable - using fallback map.",       13, 10, 0
msg_step_disk:   db "[BOOT] Loading kernel ELF from disk LBA 64...",       13, 10, 0
msg_disk_check_ext:db "[BOOT] AH=41h: check BIOS disk extensions...",      13, 10, 0
msg_disk_ext_ok: db "[BOOT] AH=41h: extensions supported.",                13, 10, 0
msg_disk_no_ext: db "[FATAL] AH=41h: BIOS has no LBA disk extensions.",    13, 10, 0
msg_disk_reset:  db "[BOOT] AH=00h: resetting disk controller...",         13, 10, 0
msg_disk_phase1: db "[BOOT] AH=42h: reading sector 64 (ELF header)...",    13, 10, 0
msg_disk_phase1_ok:db "[BOOT] Sector 64 read.",                            13, 10, 0
msg_disk_p1_fail:db "[FATAL] AH=42h: read of ELF header failed.",          13, 10, 0
msg_disk_parse:  db "[BOOT] Parsing ELF program-header table...",          13, 10, 0
msg_disk_parse_ok:db "[BOOT] ELF program-header table parsed.",            13, 10, 0
msg_disk_bad_elf:db "[FATAL] No \x7FELF magic at LBA 64 (disk image bad).",13, 10, 0
msg_disk_phase2: db "[BOOT] AH=42h: reading remaining kernel sectors...",  13, 10, 0
msg_disk_phase2_ok:db "[BOOT] Kernel image fully loaded.",                 13, 10, 0
msg_disk_p2_fail:db "[FATAL] AH=42h: kernel-tail read failed.",            13, 10, 0
msg_disk_zero_chunk:db "[FATAL] read loop produced a zero-sector chunk.",  13, 10, 0
msg_kernel_ok:   db "[BOOT] Kernel loaded to low memory.",                 13, 10, 0
msg_step_vbe:    db "[BOOT] Enumerating VBE modes for 1024x768x32 LFB...", 13, 10, 0
msg_vbe_setting: db "[BOOT] Setting VBE Mode...",                          13, 10, 0
msg_vbe_set_ok:  db "[BOOT] VBE Mode Set Success.",                        13, 10, 0
msg_vbe_ok:      db "[BOOT] VBE LFB validated.",                           13, 10, 0
msg_vbe_fail:    db "[FATAL] No 1024x768x32 LFB VBE mode on this BIOS.",   13, 10, 0
msg_step_pm:     db "[BOOT] Entering 32-bit PM...",                        13, 10, 0
msg_disk_err:    db "[FATAL] disk read error.",                            13, 10, 0
msg_fatal:       db "Stage-2 fatal error.  Press a key to reboot.",        13, 10, 0

; ============================================================================
; ===== 32-bit PROTECTED-MODE entry point  ===================================
; ============================================================================
[BITS 32]
pm_entry:
    ; ============================================================
    ; ROOT-CAUSE FIX - confirmed from VBox.log (GURU MEDITATION):
    ; ------------------------------------------------------------
    ;   EIP = 0x88F0  (first instruction of the OLD pm_entry)
    ;   CS  = 0x0008  (flat PM code selector - we ARE in PM)
    ;   CR0 = 0x11    (PE set)
    ;   DS  = 0x0000  (stale real-mode shadow, limit = 0xFFFF)
    ;   Xcpt #GP errcd=0x0
    ;
    ; After `jmp 0x08:pm_entry`, CS holds the flat 32-bit code
    ; selector (limit = 4 GB), but DS / ES / FS / GS / SS still
    ; carry their real-mode shadow descriptors (base = 0,
    ; limit = 0xFFFF).  In protected mode the processor enforces
    ; segment limits strictly.  0xB8000 > 0xFFFF, so the very
    ; first "proof" write triggered #GP.  The IDT is not set up
    ; yet (IDTR still holds the real-mode IVT at 0x0000:0xFFFF),
    ; so the #GP cannot be dispatched -> double fault -> triple
    ; fault -> GURU MEDITATION.
    ;
    ; FIX: load PM data selectors FIRST, BEFORE any memory access
    ; above 64 KB.  The proof-of-flush write comes after.
    ; ============================================================
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     esp, 0x9F000                 ; high in conventional RAM
    cld

    ; Proof-of-pipeline-flush: safe now that DS covers 4 GB.
    ; 'P' at top-left of VGA text RAM + '3' on COM1.  If we never
    ; reach the kernel but these appear the PM transition worked.
    mov     byte [0xB8000], 'P'
    mov     byte [0xB8001], 0x0F
    mov     byte [0xB8002], '3'
    mov     byte [0xB8003], 0x0F
    mov     dx, 0x3F8
    mov     al, '3'
    out     dx, al

    ; --------------------------------------------------------------
    ; The kernel image is ALREADY resident at its p_paddr addresses:
    ; load_kernel_image (real/unreal mode) copied every PT_LOAD
    ; directly to >1 MiB and zeroed the BSS.  So PM does NOT parse or
    ; copy the ELF anymore - it only emits a diagnostic and hands off.
    ; (This is the fix for the post-banner #GP: the old PM copy read
    ; from ELF_BUFFER which could never hold the now-590 KiB file.)
    ; --------------------------------------------------------------
    mov     eax, [kern_entry_low]        ; e_entry, saved during the load
    cmp     eax, 0x100000                ; paranoia: reject a bad entry
    jb      elf_bad

    ; PM diagnostic: emit e_entry as 8 hex chars + CR/LF on COM1 so a
    ; serial terminal shows exactly which address we will jump to.
    push    eax
    mov     ecx, 8
.ke_hex:
    rol     eax, 4
    push    eax
    and     al, 0x0F
    add     al, '0'
    cmp     al, '9'
    jbe     .ke_emit
    add     al, 7
.ke_emit:
    mov     dx, 0x3F8
    out     dx, al
    pop     eax
    dec     ecx
    jnz     .ke_hex
    mov     al, 0x0D
    out     dx, al
    mov     al, 0x0A
    out     dx, al
    pop     eax

    ; --------------------------------------------------------------
    ; Step 6: build the multiboot_info struct at MBI_ADDR.
    ; --------------------------------------------------------------
    mov     edi, MBI_ADDR
    xor     eax, eax
    mov     ecx, 128 / 4
    rep     stosd

    ; flags
    mov     dword [MBI_ADDR + 0],  \
        MB_FLAG_MEM | MB_FLAG_BOOTDEV | MB_FLAG_MMAP | MB_FLAG_FRAMEBUFFER

    ; mem_lower = 640 KiB conventional
    mov     dword [MBI_ADDR + 4], 640
    ; mem_upper - the kernel only uses this for the banner; the real
    ; truth lives in mbi->mmap_*.
    mov     dword [MBI_ADDR + 8], 64 * 1024

    ; boot_device = (drive << 24)
    movzx   eax, byte [boot_drive]
    shl     eax, 24
    mov     [MBI_ADDR + 12], eax

    ; mmap_length / mmap_addr (offsets 44, 48)
    mov     eax, [mmap_length]
    mov     [MBI_ADDR + 44], eax
    mov     dword [MBI_ADDR + 48], MMAP_ADDR

    ; -----  VBE info block pointers (offsets per include/multiboot.h) ----
    ;   +72 (4B)  vbe_control_info  - physical addr of VBE controller info
    ;   +76 (4B)  vbe_mode_info     - physical addr of VBE mode info block
    ;   +80 (2B)  vbe_mode          - current VBE mode number
    mov     dword [MBI_ADDR + 72], VBE_CTRL_ADDR
    mov     dword [MBI_ADDR + 76], VBE_MODE_ADDR
    mov     ax, [vbe_match_mode]
    mov     [MBI_ADDR + 80], ax

    ; -----  framebuffer block (offsets per include/multiboot.h) -------
    ;   +88 (8B)  framebuffer_addr   - linear FB address (PhysBasePtr)
    ;   +96 (4B)  framebuffer_pitch  - BytesPerScanLine
    ;   +100(4B)  framebuffer_width  - XResolution
    ;   +104(4B)  framebuffer_height - YResolution
    ;   +108(1B)  framebuffer_bpp    - BitsPerPixel
    ;   +109(1B)  framebuffer_type   - 1 = RGB linear
    mov     eax, [VBE_MODE_ADDR + 40]
    mov     [MBI_ADDR + 88], eax              ; framebuffer_addr lo
    mov     dword [MBI_ADDR + 92], 0          ; framebuffer_addr hi (32-bit phys)
    movzx   eax, word [VBE_MODE_ADDR + 16]
    mov     [MBI_ADDR + 96], eax              ; framebuffer_pitch
    movzx   eax, word [VBE_MODE_ADDR + 18]
    mov     [MBI_ADDR + 100], eax             ; framebuffer_width
    movzx   eax, word [VBE_MODE_ADDR + 20]
    mov     [MBI_ADDR + 104], eax             ; framebuffer_height
    movzx   eax, byte [VBE_MODE_ADDR + 25]
    mov     [MBI_ADDR + 108], al              ; framebuffer_bpp
    mov     byte [MBI_ADDR + 109], 1          ; framebuffer_type = RGB

    ; --------------------------------------------------------------
    ; Step 7: hand off to the kernel.
    ;   EAX = Multiboot magic
    ;   EBX = pointer to multiboot_info
    ;   EIP = e_entry parsed from the ELF
    ; --------------------------------------------------------------
    mov     eax, MB_MAGIC
    mov     ebx, MBI_ADDR
    mov     ecx, [kern_entry_low]       ; e_entry, saved during the load
    cmp     ecx, 0x100000               ; paranoia: reject obviously bad entry
    jb      elf_bad
    jmp     ecx

elf_bad:
    cli
.bad_hlt:
    hlt
    jmp     .bad_hlt

align 4
phdr_size:      dd 0
phdr_count:     dd 0
kernel_entry:   dd 0

; The flat binary is whatever ends up here.  The Makefile target enforces
; a 63 * 512 = 32256-byte ceiling so we always fit into the 63 sectors
; reserved for stage-2.  Pad to a sector boundary for tidy disk writes.
times (32256 - ($ - $$))        db 0

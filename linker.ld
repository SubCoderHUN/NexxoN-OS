/* ============================================================================
 * NexxoN OS - x86_64 kernel linker script (PHASE 1 of the 64-bit port)
 * ----------------------------------------------------------------------------
 * Produces an elf64-x86-64 image that GRUB (Multiboot v1) loads at the
 * conventional 1 MiB mark and enters in 32-bit protected mode at _start
 * (boot64.asm), which switches the CPU into long mode.
 *
 * .multiboot must live inside the first 8 KiB so GRUB's header scanner
 * finds the magic.
 * ============================================================================ */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

SECTIONS
{
    . = 0x00100000;
    _kernel_start = .;

    .multiboot ALIGN(4) :
    {
        KEEP(*(.multiboot))
    }

    .text ALIGN(4K) :
    {
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        _bss_start = .;
        *(COMMON)
        *(.bss*)
        _bss_end = .;
    }

    _kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

BITS 32
GLOBAL _start

SECTION .text
_start:
    mov eax, 4                  ; i386 write
    mov ebx, 1
    mov ecx, ok_msg
    mov edx, ok_len
    int 0x80

    mov eax, 1                  ; i386 exit
    xor ebx, ebx
    int 0x80

SECTION .rodata
ok_msg: db "NEXXON_LINUX_I386_OK", 10
ok_len: equ $ - ok_msg

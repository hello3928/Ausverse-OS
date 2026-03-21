bits 32

section .multiboot_header
multiboot_header_start:
    dd 0xE85250D6                                           ; magic
    dd 0                                                    ; arch: i386 protected mode
    dd multiboot_header_end - multiboot_header_start        ; header length
    dd -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header_start)) ; checksum

    ; terminating tag
    dw 0
    dw 0
    dd 8
multiboot_header_end:

section .bss
align 16
stack_bottom:
    resb 16384      ; 16 KiB kernel stack
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top  ; set up stack

    push ebx            ; multiboot info pointer (arg 2)
    push eax            ; multiboot magic number (arg 1)

    call kernel_main

    ; kernel_main returned — halt forever
    cli
.hang:
    hlt
    jmp .hang

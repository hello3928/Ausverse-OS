bits 64

extern irq_handler

; Hardware IRQ stubs — vectors 32-47 (IRQs 0-15 after PIC remapping).
; Unlike CPU exceptions, the PIC never pushes an error code, so we always
; push a dummy 0 to keep the stack frame uniform.

%macro IRQ 2
global irq%1
irq%1:
    push qword 0
    push qword %2   ; vector number (32 + IRQ line)
    jmp irq_common
%endmacro

IRQ  0, 32      ; PIT timer
IRQ  1, 33      ; PS/2 keyboard
IRQ  2, 34      ; cascade (slave PIC) — never fires directly
IRQ  3, 35      ; COM2
IRQ  4, 36      ; COM1
IRQ  5, 37      ; LPT2
IRQ  6, 38      ; floppy
IRQ  7, 39      ; LPT1 / spurious
IRQ  8, 40      ; CMOS real-time clock
IRQ  9, 41      ; free
IRQ 10, 42      ; free
IRQ 11, 43      ; free
IRQ 12, 44      ; PS/2 mouse
IRQ 13, 45      ; FPU
IRQ 14, 46      ; primary ATA
IRQ 15, 47      ; secondary ATA / spurious

irq_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp        ; first argument: pointer to int_frame
    call irq_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16         ; discard vector number and dummy error code
    iretq

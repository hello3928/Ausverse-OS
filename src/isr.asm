bits 32

extern isr_handler

; Macro for exceptions that do NOT push an error code.
; We push a dummy 0 so the stack layout is always the same.
%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

; Macro for exceptions that DO push an error code automatically.
%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

; CPU exceptions 0-31
ISR_NOERR  0   ; Division by zero
ISR_NOERR  1   ; Debug
ISR_NOERR  2   ; Non-maskable interrupt
ISR_NOERR  3   ; Breakpoint
ISR_NOERR  4   ; Overflow
ISR_NOERR  5   ; Bound range exceeded
ISR_NOERR  6   ; Invalid opcode
ISR_NOERR  7   ; Device not available
ISR_ERR    8   ; Double fault
ISR_NOERR  9   ; Coprocessor segment overrun
ISR_ERR   10   ; Invalid TSS
ISR_ERR   11   ; Segment not present
ISR_ERR   12   ; Stack segment fault
ISR_ERR   13   ; General protection fault
ISR_ERR   14   ; Page fault
ISR_NOERR 15   ; Reserved
ISR_NOERR 16   ; x87 FPU error
ISR_ERR   17   ; Alignment check
ISR_NOERR 18   ; Machine check
ISR_NOERR 19   ; SIMD floating-point exception
ISR_NOERR 20   ; Virtualisation exception
ISR_NOERR 21   ; Reserved
ISR_NOERR 22   ; Reserved
ISR_NOERR 23   ; Reserved
ISR_NOERR 24   ; Reserved
ISR_NOERR 25   ; Reserved
ISR_NOERR 26   ; Reserved
ISR_NOERR 27   ; Reserved
ISR_NOERR 28   ; Reserved
ISR_NOERR 29   ; Reserved
ISR_ERR   30   ; Security exception
ISR_NOERR 31   ; Reserved

; All ISR stubs jump here.
; Stack on entry (top to bottom):
;   int_no, err_code, eip, cs, eflags   (eip/cs/eflags pushed by CPU)
isr_common:
    pusha               ; pushes eax,ecx,edx,ebx,esp,ebp,esi,edi
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10        ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp            ; pass pointer to int_frame as argument
    call isr_handler
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8          ; discard int_no and err_code
    iret

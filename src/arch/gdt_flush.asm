bits 64

global gdt_flush

; void gdt_flush(struct gdt_ptr *gdtp)
;
; In the x86-64 SysV ABI the first argument arrives in rdi.
; In 64-bit mode lgdt expects a 10-byte descriptor (2-byte limit + 8-byte base).
; We reload CS via a far-return rather than a far-jump because NASM cannot
; encode a 64-bit far jump directly.
gdt_flush:
    lgdt [rdi]

    ; Reload CS: push the new selector and the return address onto the stack,
    ; then execute a 64-bit far return (retfq).
    push 0x08                       ; kernel code selector
    lea  rax, [rel .flush]
    push rax
    retfq

.flush:
    mov ax, 0x10                    ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

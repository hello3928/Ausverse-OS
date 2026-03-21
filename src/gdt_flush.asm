bits 32

global gdt_flush

; void gdt_flush(uint32_t gdtp_addr)
; Loads the GDT and reloads all segment registers.
gdt_flush:
    mov eax, [esp + 4]  ; gdtp_addr argument
    lgdt [eax]          ; load the GDT

    ; Reload code segment via a far jump.
    ; 0x08 = selector for entry 1 (kernel code): index 1, TI=0, RPL=0
    jmp 0x08:.flush

.flush:
    ; 0x10 = selector for entry 2 (kernel data): index 2, TI=0, RPL=0
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

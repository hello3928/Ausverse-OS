bits 32

; -----------------------------------------------------------------------
; Multiboot2 header
; -----------------------------------------------------------------------
section .multiboot_header
multiboot_header_start:
    dd 0xE85250D6
    dd 0
    dd multiboot_header_end - multiboot_header_start
    dd -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header_start))

    ; Framebuffer request tag (type 5, flags=1 = optional)
    ; GRUB will switch to 640x480x32 before handing us control.
    dw 5            ; type
    dw 1            ; flags: optional (don't fail if unavailable)
    dd 20           ; size
    dd 1280         ; preferred width
    dd 1024         ; preferred height
    dd 32           ; preferred depth (bpp)

    align 8         ; tags must be 8-byte aligned

    ; End tag
    dw 0
    dw 0
    dd 8
multiboot_header_end:

; -----------------------------------------------------------------------
; BSS: stack + page tables (zero-initialised)
; -----------------------------------------------------------------------
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

align 4096
pml4: resb 4096
pdpt: resb 4096
pd0:  resb 4096     ; identity maps 0 GB – 1 GB
pd1:  resb 4096     ; identity maps 1 GB – 2 GB
pd2:  resb 4096     ; identity maps 2 GB – 3 GB
pd3:  resb 4096     ; identity maps 3 GB – 4 GB

; -----------------------------------------------------------------------
; Temporary 64-bit GDT
; -----------------------------------------------------------------------
section .rodata
align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64

; -----------------------------------------------------------------------
; _start — 32-bit protected mode entry
; -----------------------------------------------------------------------
section .text
global _start
extern kernel_main

_start:
    mov edi, eax        ; save multiboot2 magic  (→ rdi after 64-bit switch)
    mov esi, ebx        ; save multiboot2 mbi ptr (→ rsi after 64-bit switch)

    mov esp, stack_top

    ; ---- 1. Build identity-mapped page tables for all 4 GiB ----------
    ;
    ; PML4[0] → PDPT
    ; PDPT[0] → pd0   (0   – 1 GiB)
    ; PDPT[1] → pd1   (1   – 2 GiB)
    ; PDPT[2] → pd2   (2   – 3 GiB)
    ; PDPT[3] → pd3   (3   – 4 GiB)
    ; Each PD:  512 × 2 MiB huge pages
    ;
    ; 4 GiB covers the VGA buffer (0xB8000), the kernel (~1 MiB),
    ; and typical VESA framebuffers (often at 0xFD000000+).

    mov eax, pdpt
    or  eax, 0x03
    mov [pml4], eax

    mov eax, pd0
    or  eax, 0x03
    mov [pdpt + 0*8], eax

    mov eax, pd1
    or  eax, 0x03
    mov [pdpt + 1*8], eax

    mov eax, pd2
    or  eax, 0x03
    mov [pdpt + 2*8], eax

    mov eax, pd3
    or  eax, 0x03
    mov [pdpt + 3*8], eax

    ; Fill pd0: pages 0–511  →  physical 0x00000000 – 0x3FFFFFFF
    mov esi, pd0
    xor ecx, ecx
.fill_pd0:
    mov eax, ecx
    shl eax, 21
    or  eax, 0x83
    mov [esi], eax
    mov dword [esi + 4], 0
    add esi, 8
    inc ecx
    cmp ecx, 512
    jne .fill_pd0

    ; Fill pd1: pages 512–1023  →  physical 0x40000000 – 0x7FFFFFFF
    mov esi, pd1
    mov ecx, 512
.fill_pd1:
    mov eax, ecx
    shl eax, 21
    or  eax, 0x83
    mov [esi], eax
    mov dword [esi + 4], 0
    add esi, 8
    inc ecx
    cmp ecx, 1024
    jne .fill_pd1

    ; Fill pd2: pages 1024–1535  →  physical 0x80000000 – 0xBFFFFFFF
    mov esi, pd2
    mov ecx, 1024
.fill_pd2:
    mov eax, ecx
    shl eax, 21
    or  eax, 0x83
    mov [esi], eax
    mov dword [esi + 4], 0
    add esi, 8
    inc ecx
    cmp ecx, 1536
    jne .fill_pd2

    ; Fill pd3: pages 1536–2047  →  physical 0xC0000000 – 0xFFFFFFFF
    mov esi, pd3
    mov ecx, 1536
.fill_pd3:
    mov eax, ecx
    shl eax, 21
    or  eax, 0x83
    mov [esi], eax
    mov dword [esi + 4], 0
    add esi, 8
    inc ecx
    cmp ecx, 2048
    jne .fill_pd3

    ; Restore esi — it was clobbered by the fill loops above
    mov esi, ebx        ; reload mbi pointer

    ; ---- 2. Load PML4 into CR3 ----------------------------------------
    mov eax, pml4
    mov cr3, eax

    ; ---- 3. Enable PAE ---------------------------------------------------
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; ---- 4. Set LME in EFER ----------------------------------------------
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    ; ---- 5. Enable paging -----------------------------------------------
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 0)
    mov cr0, eax

    ; ---- 6. Load 64-bit GDT and far-jump --------------------------------
    lgdt [gdt64_ptr]
    jmp  0x08:long_mode_entry

; -----------------------------------------------------------------------
; 64-bit long mode
; -----------------------------------------------------------------------
bits 64
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top

    ; edi = magic, esi = mbi (zero-extended to rdi, rsi by 64-bit mode)
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang

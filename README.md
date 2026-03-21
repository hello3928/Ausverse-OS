# AusverseOS

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-x86__64-lightgrey.svg)
![Language](https://img.shields.io/badge/language-C%20%2F%20NASM-orange.svg)
![Status](https://img.shields.io/badge/status-in%20development-yellow.svg)

A bare-metal x86-64 operating system written from scratch in C and NASM assembly. No standard library. No runtime. Just the CPU, memory, and code.

---

## Overview

AusverseOS is a personal project to build a functioning OS from the ground up — starting from a GRUB entry point and working toward memory management, hardware drivers, and a kernel shell. Every component is implemented by hand to build a complete understanding of how operating systems interact with hardware.

---

## Roadmap

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Bare metal — GRUB Multiboot2 entry, GDT, IDT, CPU exceptions | ✅ Complete |
| 2 | Memory — physical memory manager, heap allocator, 4 GB identity mapping | ✅ Complete |
| 3 | Hardware — PIC remapping, PIT timer, PS/2 keyboard driver | ✅ Complete |
| 4 | Shell — kernel-mode CLI with commands, uptime, memory stats | ✅ Complete |
| 5 | Display — VESA linear framebuffer, 1920x1080x32, bitmap font renderer | ✅ Complete |
| 6 | Userspace — privilege levels, syscalls, user programs | 🔲 Upcoming |

---

## Architecture

```
AusverseOS/
├── src/
│   ├── boot.asm          # Multiboot2 header, page tables, long mode entry
│   ├── kernel.c          # Kernel entry point, hardware init sequence
│   ├── gdt.c             # Global Descriptor Table (64-bit)
│   ├── gdt_flush.asm     # lgdt + segment register reload via retfq
│   ├── idt.c             # IDT, exception dispatcher, IRQ dispatcher
│   ├── isr.asm           # ISR stubs for CPU exceptions 0-31
│   ├── irq.asm           # IRQ stubs for hardware interrupts 0-15
│   ├── pic.c             # 8259A PIC init and remapping
│   ├── pit.c             # 8253 PIT timer at 100 Hz
│   ├── keyboard.c        # PS/2 Set 1 keyboard driver, ring buffer
│   ├── pmm.c             # Bitmap physical memory manager
│   ├── heap.c            # Free-list kernel heap (kmalloc/kfree)
│   ├── framebuffer.c     # VESA framebuffer pixel renderer
│   ├── font.c            # 8x8 bitmap font (ASCII 0x20-0x7E)
│   └── shell.c           # Kernel shell with line editor
├── include/              # Header files for all modules
├── grub/
│   └── grub.cfg          # GRUB bootloader configuration
├── linker.ld             # Kernel linker script
└── Makefile
```

---

## Boot Sequence

```
GRUB (Multiboot2)
    └── loads kernel.bin at 1 MiB, switches to 1920x1080x32 framebuffer
        └── boot.asm (_start) — 32-bit protected mode
            ├── saves Multiboot2 magic + MBI pointer
            ├── builds identity-mapped page tables (PML4 -> PDPT -> 4x PD, covers full 4 GB)
            ├── enables PAE, sets LME in EFER, enables paging
            ├── loads 64-bit GDT, far-jumps to long mode
            └── calls kernel_main()
                ├── parses Multiboot2 tags (framebuffer address, memory map)
                ├── GDT reload, IDT load
                ├── PMM init (bitmap over physical memory map)
                ├── heap init
                ├── PIC remap, PIT at 100 Hz
                ├── interrupts enabled (sti)
                ├── keyboard driver init
                └── shell_run()
```

---

## Components

### Framebuffer Renderer
Renders directly to the VESA linear framebuffer provided by GRUB. Supports a 16-color VGA palette mapped to RGB, an embedded 8x8 bitmap font covering all printable ASCII, smooth scrolling, and the full `vga_*` API used throughout the kernel.

### Physical Memory Manager
Bitmap allocator over the Multiboot2 memory map. Tracks 4 KB pages across all available RAM, guards page 0, and marks kernel pages as used at boot. Exposes `pmm_alloc_page` / `pmm_free_page`.

### Heap Allocator
Free-list allocator built on top of the PMM. Allocates contiguous physical pages at init time and manages variable-size kernel allocations with forward coalescing on free.

### PS/2 Keyboard Driver
Handles PS/2 Set 1 scancodes via IRQ1. Maintains a 256-byte ring buffer. Supports shift, caps lock, and extended scancodes (0xE0 prefix) for arrow keys. Arrow key codes (`KEY_LEFT`, `KEY_RIGHT`, etc.) are pushed as special bytes into the buffer for the shell to consume.

### PIC / PIT
Remaps the 8259A PIC so IRQs 0-15 map to vectors 32-47 (away from CPU exceptions). The PIT is configured at 100 Hz on IRQ0 to drive a monotonic tick counter used by the `uptime` command.

### GDT / IDT
64-bit GDT with null, code (`L=1`), and data segments. IDT covers 32 CPU exception vectors and 16 hardware IRQ vectors, each with a NASM stub that saves full CPU state before calling a C dispatcher.

### Shell
Kernel-mode command-line interface with a line editor supporting left/right cursor movement and in-place character insertion. Built-in commands: `help`, `clear`, `echo`, `uptime`, `mem`.

---

## Getting Started

### Prerequisites

WSL2 (Ubuntu) or any Linux environment with:

```bash
sudo apt update && sudo apt install -y \
    build-essential nasm \
    grub-pc-bin grub-common xorriso \
    qemu-system-x86
```

### Build and Run

```bash
make run
```

Compiles the kernel, packages it into a bootable ISO using GRUB, and launches it in QEMU.

### Other Targets

| Command | Description |
|---------|-------------|
| `make` | Build the ISO without launching |
| `make run` | Build and launch in QEMU |
| `make clean` | Remove all build artifacts |

### Exiting QEMU

Press `Alt+2` to open the QEMU monitor, then type `quit`.

---

## License

This project is licensed under the [MIT License](LICENSE).

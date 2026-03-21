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
| 1 | Bare Metal — GRUB entry, VGA output, GDT, IDT | ✅ Complete |
| 2 | Memory — physical allocator, paging, long mode, heap | 🔲 Upcoming |
| 3 | Hardware — PIC, PIT timer, PS/2 keyboard | 🔲 Upcoming |
| 4 | Shell — kernel-mode command-line interface | 🔲 Upcoming |
| 5 | Userspace — privilege levels, syscalls, user programs | 🔲 Upcoming |

---

## Architecture

```
AusverseOS/
├── src/
│   ├── boot.asm          # Multiboot2 header and kernel entry point
│   ├── gdt.c             # Global Descriptor Table initialisation
│   ├── gdt_flush.asm     # lgdt + segment register reload
│   ├── idt.c             # Interrupt Descriptor Table + exception dispatcher
│   ├── isr.asm           # ISR stubs for CPU exceptions 0–31
│   └── kernel.c          # Kernel main + VGA text driver
├── include/
│   ├── gdt.h
│   ├── idt.h
│   └── vga.h
├── grub/
│   └── grub.cfg          # GRUB bootloader configuration
├── linker.ld             # Kernel linker script — loads at 1 MiB
└── Makefile
```

---

## Boot Sequence

```
GRUB (Multiboot2)
    └── loads kernel.bin at 1 MiB
        └── boot.asm (_start)
            ├── sets up 16 KiB stack
            └── calls kernel_main()
                ├── VGA driver init
                ├── GDT load (lgdt)
                └── IDT load (lidt) — exceptions 0–31 active
```

> The kernel currently runs in 32-bit protected mode. The transition to x86-64 long mode will be implemented in Phase 2 alongside paging.

---

## Components

### VGA Text Driver
Writes directly to the VGA framebuffer at `0xB8000`. Supports character output, newlines, and screen scrolling. No BIOS calls — pure memory-mapped I/O.

### GDT — Global Descriptor Table
Defines memory segments for the CPU. Three entries: a mandatory null descriptor, a kernel code segment, and a kernel data segment. Both code and data use a flat model covering the full 4 GB address space at ring 0.

### IDT — Interrupt Descriptor Table
Maps all 32 CPU exception vectors (0–31) to handler stubs. Each stub saves the full CPU state to the stack and calls a C dispatcher that prints the exception name and halts. Lays the foundation for hardware interrupt handling in Phase 3.

---

## Getting Started

### Prerequisites

WSL2 (Ubuntu) or any Linux environment with the following packages:

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

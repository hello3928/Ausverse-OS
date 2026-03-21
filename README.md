# AusverseOS

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-x86__64-lightgrey.svg)
![Language](https://img.shields.io/badge/language-C%20%2F%20NASM-orange.svg)
![Status](https://img.shields.io/badge/status-in%20development-yellow.svg)

A bare-metal x86-64 operating system written from scratch in C and NASM assembly. No standard library. No runtime. Just the CPU, memory, and code.

---

## Overview

AusverseOS is a personal project to build a functioning OS from the ground up — starting from a GRUB entry point and working toward memory management, hardware drivers, a kernel shell, a graphical window manager, and userspace programs. Every component is implemented by hand to build a complete understanding of how operating systems interact with hardware.

---

## Roadmap

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Bare metal — GRUB Multiboot2 entry, GDT, IDT, CPU exceptions | ✅ Complete |
| 2 | Memory — physical memory manager, heap allocator, 4 GB identity mapping | ✅ Complete |
| 3 | Hardware — PIC remapping, PIT timer, PS/2 keyboard driver | ✅ Complete |
| 4 | Shell — kernel-mode CLI with commands, uptime, memory stats | ✅ Complete |
| 5 | Display — VESA linear framebuffer, 1920×1080×32, bitmap font, double buffering | ✅ Complete |
| 6 | Storage — ATA PIO driver, FAT32 filesystem, VFS layer | ✅ Complete |
| 7 | Input — PS/2 mouse (IRQ12), USB HID tablet (UHCI), ACPI | ✅ Complete |
| 8 | GUI — Windows 95-style window manager, taskbar, drag, ELF64 loader | ✅ Complete |
| 9 | Userspace — privilege levels, syscalls, user programs | 🔲 Upcoming |

---

## Architecture

```
AusverseOS/
├── src/
│   ├── arch/
│   │   ├── boot.asm          # Multiboot2 header, page tables, long mode entry
│   │   ├── gdt.c             # Global Descriptor Table (64-bit)
│   │   ├── gdt_flush.asm     # lgdt + segment register reload via retfq
│   │   ├── idt.c             # IDT, exception dispatcher, IRQ dispatcher
│   │   ├── isr.asm           # ISR stubs for CPU exceptions 0-31
│   │   └── irq.asm           # IRQ stubs for hardware interrupts 0-15
│   ├── core/
│   │   ├── kernel.c          # Kernel entry point, hardware init sequence
│   │   ├── pmm.c             # Bitmap physical memory manager
│   │   ├── heap.c            # Free-list kernel heap (kmalloc/kfree)
│   │   ├── shell.c           # Kernel shell with line editor and built-in commands
│   │   ├── acpi.c            # ACPI table parser (RSDP/RSDT, power off)
│   │   └── elf.c             # ELF64 loader — loads and runs userspace binaries
│   ├── drivers/
│   │   ├── pic.c             # 8259A PIC init and remapping
│   │   ├── pit.c             # 8253 PIT timer at 100 Hz
│   │   ├── keyboard.c        # PS/2 Set 1 keyboard driver, ring buffer
│   │   ├── mouse.c           # PS/2 mouse driver (IRQ12, 3-byte packets)
│   │   ├── usb_tablet.c      # UHCI USB HID tablet driver (absolute coords)
│   │   └── ata.c             # ATA PIO disk driver
│   ├── fs/
│   │   ├── vfs.c             # Virtual filesystem layer
│   │   └── fat32.c           # FAT32 filesystem driver
│   └── gui/
│       ├── framebuffer.c     # VESA framebuffer, cursor blink, double buffer
│       ├── font.c            # 8×16 bitmap font (ASCII 0x20–0x7E)
│       ├── render.c          # Compositor — Win95 chrome, desktop, surfaces
│       └── wm.c              # Window manager — z-order, drag, taskbar, clock
├── include/                  # Headers mirroring src/ subdirectory layout
│   ├── arch/
│   ├── core/
│   ├── drivers/
│   ├── fs/
│   └── gui/
├── grub/
│   └── grub.cfg              # GRUB bootloader configuration
├── scripts/
│   └── mkfat32.py            # FAT32 disk image creator (fallback for mkfs.fat)
├── linker.ld                 # Kernel linker script
└── Makefile
```

---

## Boot Sequence

```
GRUB (Multiboot2)
    └── loads kernel.bin at 1 MiB, switches to 1920×1080×32 framebuffer
        └── boot.asm (_start) — 32-bit protected mode
            ├── saves Multiboot2 magic + MBI pointer
            ├── builds identity-mapped page tables (PML4→PDPT→4×PD, covers full 4 GB)
            ├── enables PAE, sets LME in EFER, enables paging
            ├── loads 64-bit GDT, far-jumps to long mode
            └── kernel_main()
                ├── parses Multiboot2 tags (framebuffer address, memory map)
                ├── GDT reload, IDT load, PMM init, heap init
                ├── PIC remap, PIT at 100 Hz, interrupts enabled
                ├── keyboard, ACPI, VFS, PS/2 mouse, USB tablet init
                └── shell_run()
```

---

## Components

### Window Manager
A Windows 95-style windowing system. Supports multiple overlapping windows with title bars, close buttons, z-order management, and mouse drag. Draws a taskbar with a Start button and live uptime clock (HH:MM:SS). Renders at ~50 fps via a double-buffered compositor that pauses automatic PIT flushing to avoid tearing.

### ELF64 Loader
Loads and executes ELF64 binaries from the FAT32 disk. Handles ET_EXEC and ET_DYN types with load-bias calculation for any link address. Passes a `kapi_t` function table to the entry point so userspace programs can call `print`, `getchar`, `readfile`, `writefile`, and `listdir`.

### USB HID Tablet Driver
A from-scratch UHCI host controller driver targeting QEMU's `usb-tablet` device. Performs a full PCI config space scan to find the controller, resets the HC, enumerates the device via control transfers (GET_DESCRIPTOR → SET_ADDRESS → SET_CONFIGURATION → SET_IDLE), then polls an interrupt IN endpoint for 6-byte absolute position reports. Maps the 0–32767 HID coordinate range directly to screen pixels for pixel-perfect cursor tracking.

### PS/2 Mouse Driver
Handles PS/2 mouse data via IRQ12 on the 8042 slave PIC. Assembles 3-byte packets, sign-extends 9-bit deltas, and clamps the cursor within screen bounds. Uses bit 5 of the 8042 status register to distinguish mouse from keyboard data on the shared port 0x60. Falls back gracefully when a USB tablet is present.

### FAT32 / VFS
A FAT32 driver over ATA PIO supporting file read, write, create, delete, and directory listing. A thin VFS layer provides a unified `open/read/write/readdir` interface. The shell exposes this as `ls`, `cat`, `write`, `mkdir`, `rm`, `cp`, `mv`, and `echo >`.

### Framebuffer / Renderer
Renders to the VESA linear framebuffer at 32 bpp. The compositor draws Win95-style window chrome (raised/sunken borders, title bar, close button), a desktop background, and a hardware-style mouse cursor. All drawing goes to a back buffer; `render_flush()` blits it to the hardware framebuffer atomically.

### Physical Memory Manager
Bitmap allocator over the Multiboot2 memory map. Tracks 4 KB pages, guards page 0, and marks kernel pages as used at boot.

### Heap Allocator
Free-list allocator built on top of the PMM. Manages variable-size kernel allocations with forward coalescing on free.

### PS/2 Keyboard Driver
Handles PS/2 Set 1 scancodes via IRQ1. Maintains a 256-byte ring buffer. Supports shift, caps lock, and extended scancodes (0xE0 prefix) for arrow keys.

### Shell
Kernel-mode CLI with a line editor supporting cursor movement and in-place insertion. Built-in commands:

| Command | Description |
|---------|-------------|
| `help` | List commands |
| `clear` | Clear the screen |
| `echo [text] [> file]` | Print text or redirect to file |
| `uptime` | Show system uptime |
| `mem` | Show free memory |
| `ls [path]` | List directory |
| `cat <file>` | Print file contents |
| `write <file> <text>` | Write text to file |
| `mkdir <dir>` | Create directory |
| `rm <file>` | Delete file |
| `cp <src> <dst>` | Copy file |
| `mv <src> <dst>` | Move/rename file |
| `hexdump <file>` | Hex dump of file |
| `whoami` | Print current user |
| `uname` | Print OS info |
| `exec <file>` | Load and run ELF64 binary |
| `gui` | Launch the window manager |
| `shutdown` | Power off via ACPI |

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

Compiles the kernel, packages it into a bootable ISO using GRUB, creates a 64 MB FAT32 disk image, and launches everything in QEMU with USB tablet support.

### Other Targets

| Command | Description |
|---------|-------------|
| `make` | Build the ISO without launching |
| `make run` | Build and launch in QEMU |
| `make disk` | Create the FAT32 disk image |
| `make clean` | Remove all build artifacts |

### Exiting QEMU

Press `Alt+2` to open the QEMU monitor, then type `quit`.

---

## License

This project is licensed under the [MIT License](LICENSE).

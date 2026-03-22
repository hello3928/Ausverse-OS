# AusverseOS

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-x86__64-lightgrey.svg)
![Language](https://img.shields.io/badge/language-C%20%2F%20NASM-orange.svg)
![Status](https://img.shields.io/badge/status-in%20development-yellow.svg)

A bare-metal x86-64 operating system written from scratch in C and NASM assembly. No standard library. No runtime. Just the CPU, memory, and code.

---

## Overview

AusverseOS is a personal project to build a functioning OS from the ground up — starting from a GRUB entry point and working toward memory management, hardware drivers, a graphical window manager, networking, and a package manager. Every component is implemented by hand to build a complete understanding of how operating systems interact with hardware.

---

## Roadmap

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Bare metal — GRUB Multiboot2 entry, GDT, IDT, CPU exceptions | ✅ Complete |
| 2 | Memory — physical memory manager, heap allocator, 4 GB identity mapping | ✅ Complete |
| 3 | Hardware — PIC remapping, PIT timer, PS/2 keyboard driver | ✅ Complete |
| 4 | Shell — kernel-mode CLI with commands, uptime, memory stats | ✅ Complete |
| 5 | Display — VESA linear framebuffer, 1280×1024×32, bitmap font, double buffering | ✅ Complete |
| 6 | Storage — ATA PIO driver, FAT32 filesystem, VFS layer | ✅ Complete |
| 7 | Input — PS/2 mouse (IRQ12), USB HID tablet (UHCI), ACPI | ✅ Complete |
| 8 | GUI — modern flat window manager, dark/red theme, taskbar, drag, ELF64 loader | ✅ Complete |
| 9 | Networking — PCI scan, Intel E1000 NIC, Ethernet/ARP/IP/UDP/TCP/DHCP/HTTP | ✅ Complete |
| 10 | Package manager — HTTP package download, FAT32 install, repository index | ✅ Complete |
| 11 | Userspace — privilege levels, syscalls, user programs | 🔲 Upcoming |

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
│   │   ├── elf.c             # ELF64 loader — loads and runs userspace binaries
│   │   └── pkg.c             # Package manager (list, download, install)
│   ├── drivers/
│   │   ├── pic.c             # 8259A PIC init and remapping
│   │   ├── pit.c             # 8253 PIT timer at 60 Hz
│   │   ├── keyboard.c        # PS/2 Set 1 keyboard driver, ring buffer
│   │   ├── mouse.c           # PS/2 mouse driver (IRQ12, 3-byte packets)
│   │   ├── usb_tablet.c      # UHCI USB HID tablet driver (absolute coords)
│   │   ├── ata.c             # ATA PIO disk driver
│   │   ├── pci.c             # PCI bus scanner (config space, BAR, IRQ)
│   │   └── e1000.c           # Intel 82540EM NIC driver (TX/RX descriptor rings)
│   ├── fs/
│   │   ├── vfs.c             # Virtual filesystem layer
│   │   └── fat32.c           # FAT32 filesystem driver
│   ├── net/
│   │   └── net.c             # Full network stack: Ethernet, ARP, IPv4, ICMP,
│   │                         #   UDP, DHCP client, TCP, HTTP GET, DNS
│   └── gui/
│       ├── framebuffer.c     # VESA framebuffer, cursor blink, double buffer
│       ├── font.c            # 8×16 bitmap font (ASCII 0x20–0x7E)
│       ├── render.c          # Compositor — dark/red theme, surfaces, gradient
│       └── wm.c              # Window manager — z-order, drag, taskbar, clock
├── include/                  # Headers mirroring src/ subdirectory layout
│   ├── arch/
│   ├── core/
│   ├── drivers/
│   ├── fs/
│   ├── net/
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
    └── loads kernel.bin at 1 MiB, requests 1280×1024×32 framebuffer
        └── boot.asm (_start) — 32-bit protected mode
            ├── saves Multiboot2 magic + MBI pointer
            ├── builds identity-mapped page tables (PML4→PDPT→4×PD, covers full 4 GB)
            ├── enables PAE, sets LME in EFER, enables paging
            ├── loads 64-bit GDT, far-jumps to long mode
            └── kernel_main()
                ├── parses Multiboot2 tags (framebuffer address, memory map)
                ├── GDT reload, IDT load, PMM init, heap init
                ├── PIC remap, PIT at 60 Hz, interrupts enabled
                ├── keyboard, ACPI, VFS, PS/2 mouse, USB tablet init
                ├── E1000 NIC init, DHCP → IP assignment
                └── shell_run()
```

---

## Components

### Window Manager
A modern flat windowing system with a dark/red colour theme. Supports multiple overlapping windows with title bars, chrome buttons, drop shadows, z-order management, and mouse drag. Draws a taskbar with a Start button and live uptime clock (HH:MM:SS). Uses a two-dirty-flag system (`wm_dirty` / `cursor_dirty`) so mouse movement only costs a re-flush — not a full scene repaint. The cursor is stamped directly on the hardware framebuffer after each flush, never in the scene buffer.

### Networking
A complete from-scratch network stack:
- **PCI scanner** — enumerates all 256×32×8 config space entries to locate the NIC
- **Intel E1000 driver** — MMIO register access, EEPROM MAC read, 8-slot TX/RX descriptor rings, polling receive
- **Ethernet** — frame send/receive with type dispatch
- **ARP** — request/reply with an 8-entry cache
- **IPv4** — checksum, routing, TTL
- **ICMP** — echo request/reply (ping)
- **UDP** — used for DHCP and DNS
- **DHCP client** — Discover→Offer→Request→ACK, extracts IP and gateway
- **TCP** — SYN/ACK handshake, data send/receive, FIN teardown (one connection at a time)
- **HTTP** — GET-only client with header parsing and chunked body callbacks
- **DNS** — A-record query over UDP

### Package Manager
`pkg list` fetches a plain-text index from a configurable HTTP server (`10.0.2.2:8080` in VirtualBox NAT). `pkg install <name>` downloads the ELF binary and writes it to `/bin/<name>` on the FAT32 disk. Packages can then be run with `exec /bin/<name>`.

### ELF64 Loader
Loads and executes ELF64 binaries from the FAT32 disk. Handles ET_EXEC and ET_DYN types with load-bias calculation for any link address. Passes a `kapi_t` function table to the entry point so userspace programs can call `print`, `getchar`, `readfile`, `writefile`, and `listdir`.

### FAT32 / VFS
A FAT32 driver over ATA PIO supporting file read, write, create, delete, and directory listing. A thin VFS layer provides a unified `open/read/write/readdir` interface.

### Physical Memory Manager
Bitmap allocator over the Multiboot2 memory map. Tracks 4 KB pages, guards page 0, and marks kernel pages as used at boot.

### Heap Allocator
Free-list allocator built on top of the PMM. Manages variable-size kernel allocations with forward coalescing on free.

### PS/2 Keyboard / Mouse
IRQ1 keyboard with Set 1 scancodes, shift, caps lock, and arrow keys via 0xE0 prefix. IRQ12 mouse with 3-byte packet assembly, 9-bit signed delta decoding, and screen-bounds clamping.

### Shell
Kernel-mode CLI with a line editor supporting cursor movement, in-place insertion, and command history. Built-in commands:

| Command | Description |
|---------|-------------|
| `help` | List all commands |
| `clear` | Clear screen |
| `echo [text] [> file]` | Print text or redirect to file |
| `uptime` | System uptime |
| `mem` | Memory usage |
| `ls [path]` | List directory |
| `cat <file>` | Print file |
| `write <file> <text>` | Write to file |
| `append <file> <text>` | Append to file |
| `mkdir <dir>` | Create directory |
| `rm <path>` | Delete file or directory |
| `cp <src> <dst>` | Copy file |
| `mv <src> <dst>` | Move / rename |
| `hexdump <file>` | Hex dump |
| `whoami` | Current user |
| `uname` | OS info |
| `exec <file>` | Run ELF64 binary |
| `ifconfig` | Show IP and MAC address |
| `ping <ip>` | ICMP echo |
| `pkg list` | List available packages |
| `pkg install <name>` | Download and install a package |
| `gui` | Launch the window manager |
| `shutdown` | Power off via ACPI |
| `reboot` | Reboot via ACPI |

---

## Getting Started

### Prerequisites

WSL2 (Ubuntu) or any Linux environment with:

```bash
sudo apt update && sudo apt install -y \
    build-essential nasm \
    grub-pc-bin grub-common xorriso
```

### Build

```bash
make iso
```

### Run in VirtualBox (recommended)

1. Install [VirtualBox](https://www.virtualbox.org/)
2. Create a VM: **Other / Unknown (64-bit)**, 1024 MB RAM
3. **Display:** Graphics Controller = **VBoxVGA**, Video Memory = 128 MB
4. **Storage:** attach `ausverseos.iso` as a DVD on the IDE controller
5. **Network:** Adapter Type = **Intel PRO/1000 MT Desktop (82540EM)**, Attached to = **NAT**
6. Boot the VM

### Package Server (optional)

To serve packages to the OS from your host machine:

```bash
mkdir -p /path/to/packages/pkg
cd /path/to/packages
python3 -m http.server 8080
```

Create `/path/to/packages/pkg/index.txt`:
```
hello  1.0  /pkg/hello.elf
```

The OS reaches the host at `10.0.2.2:8080` via VirtualBox NAT. Then inside the OS:
```
pkg list
pkg install hello
exec /bin/hello
```

### Makefile Targets

| Command | Description |
|---------|-------------|
| `make iso` | Build the bootable ISO |
| `make disk` | Create the FAT32 disk image |
| `make clean` | Remove all build artifacts |

---

## License

This project is licensed under the [MIT License](LICENSE).

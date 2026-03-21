CC      = gcc
AS      = nasm
LD      = ld

# -mno-red-zone: prevent the compiler using the 128-byte red zone below RSP,
#                which gets clobbered by interrupt handlers in kernel mode.
CFLAGS  = -m64 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
          -nostdlib -Wall -Wextra
ASFLAGS = -f elf64
LDFLAGS = -m elf_x86_64 -T linker.ld

OBJS    = build/boot.o build/gdt_flush.o build/isr.o build/irq.o \
          build/kernel.o build/gdt.o build/idt.o build/pmm.o build/heap.o \
          build/pic.o build/pit.o build/keyboard.o build/shell.o \
          build/framebuffer.o build/font.o
KERNEL  = build/kernel.bin
ISO     = ausverseos.iso

.PHONY: all iso run clean

all: iso

build/%.o: src/%.asm | build
	$(AS) $(ASFLAGS) $< -o $@

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -I include -c $< -o $@

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

iso: $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/kernel.bin
	cp grub/grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso

run: iso
	qemu-system-x86_64 -cdrom $(ISO) -vga std -m 128 -display sdl -global VGA.vgamem_mb=16

build:
	mkdir -p build

clean:
	rm -rf build iso $(ISO)

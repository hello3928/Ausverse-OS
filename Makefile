CC      = gcc
AS      = nasm
LD      = ld

CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-pic -nostdlib -Wall -Wextra
ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T linker.ld

OBJS    = build/boot.o build/gdt_flush.o build/isr.o build/kernel.o build/gdt.o build/idt.o
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
	qemu-system-i386 -cdrom $(ISO) -display curses

build:
	mkdir -p build

clean:
	rm -rf build iso $(ISO)

CC      = gcc
AS      = nasm
LD      = ld

# -mno-red-zone: prevent the compiler using the 128-byte red zone below RSP,
#                which gets clobbered by interrupt handlers in kernel mode.
CFLAGS  = -m64 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
          -nostdlib -Wall -Wextra
ASFLAGS = -f elf64
LDFLAGS = -m elf_x86_64 -T linker.ld

OBJS    = build/arch/boot.o build/arch/gdt_flush.o build/arch/isr.o build/arch/irq.o \
          build/arch/gdt.o build/arch/idt.o \
          build/drivers/keyboard.o build/drivers/mouse.o build/drivers/usb_tablet.o \
          build/drivers/ata.o build/drivers/pit.o build/drivers/pic.o \
          build/fs/vfs.o build/fs/fat32.o \
          build/gui/framebuffer.o build/gui/render.o build/gui/wm.o build/gui/font.o \
          build/core/kernel.o build/core/heap.o build/core/pmm.o build/core/shell.o \
          build/core/elf.o build/core/acpi.o
KERNEL  = build/kernel.bin
ISO     = ausverseos.iso

.PHONY: all iso run disk clean

all: iso

build/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I include -c $< -o $@

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

iso: $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/kernel.bin
	cp grub/grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso

disk:
	@if [ ! -f disk.img ]; then \
		qemu-img create -f raw disk.img 64M; \
		if command -v mkfs.fat >/dev/null 2>&1; then \
			mkfs.fat -F 32 -n AUSVERSEOS disk.img; \
		else \
			python3 scripts/mkfat32.py disk.img; \
		fi; \
		echo "Created disk.img"; \
	fi

run: iso disk
	qemu-system-x86_64 -cdrom $(ISO) \
	    -drive file=disk.img,format=raw,if=ide,index=0 \
	    -boot d \
	    -vga std -m 128 -display sdl -global VGA.vgamem_mb=16 \
	    -usb -device usb-tablet

build:
	mkdir -p build

clean:
	rm -rf build iso $(ISO)

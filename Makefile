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
          build/drivers/pci.o build/drivers/e1000.o \
          build/fs/vfs.o build/fs/fat32.o \
          build/gui/framebuffer.o build/gui/render.o build/gui/wm.o build/gui/font.o \
          build/core/kernel.o build/core/heap.o build/core/pmm.o build/core/shell.o \
          build/core/elf.o build/core/acpi.o build/core/pkg.o \
          build/net/net.o
KERNEL  = build/kernel.bin
ISO     = ausverseos.iso

VBOXMANAGE = VBoxManage
VBOX_VM    = AusverseOS
VBOX_VDI   = disk.vdi

.PHONY: all iso run disk vbox vbox-start clean

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
	    -vga std -m 512M -display sdl -global VGA.vgamem_mb=256 \
	    -usb -device usb-tablet \
	    -accel tcg \
	    -rtc base=localtime,clock=vm \
	    -cpu qemu64

# Convert raw disk image to VDI for VirtualBox
$(VBOX_VDI): disk.img
	@rm -f $(VBOX_VDI)
	$(VBOXMANAGE) convertfromraw disk.img $(VBOX_VDI) --format VDI

# Create and configure the VirtualBox VM (safe to re-run; errors on already-exists steps are ignored)
vbox: iso disk $(VBOX_VDI)
	-$(VBOXMANAGE) createvm --name "$(VBOX_VM)" --ostype "Other_64" --register
	-$(VBOXMANAGE) modifyvm "$(VBOX_VM)" \
	    --memory 1024 --vram 128 \
	    --graphicscontroller vboxvga \
	    --boot1 dvd --boot2 disk --boot3 none --boot4 none \
	    --firmware bios \
	    --audio none \
	    --usb on
	-$(VBOXMANAGE) storagectl "$(VBOX_VM)" --name "IDE" --add ide
	-$(VBOXMANAGE) storageattach "$(VBOX_VM)" \
	    --storagectl "IDE" --port 0 --device 0 \
	    --type dvddrive --medium "$(abspath $(ISO))"
	-$(VBOXMANAGE) storageattach "$(VBOX_VM)" \
	    --storagectl "IDE" --port 1 --device 0 \
	    --type hdd --medium "$(abspath $(VBOX_VDI))"
	$(VBOXMANAGE) startvm "$(VBOX_VM)" --type gui

# Start an already-configured VM (rebuild ISO first)
vbox-start: iso
	$(VBOXMANAGE) startvm "$(VBOX_VM)" --type gui

build:
	mkdir -p build

clean:
	rm -rf build iso $(ISO)

clean-vbox:
	-$(VBOXMANAGE) unregistervm "$(VBOX_VM)" --delete
	rm -f $(VBOX_VDI)

#include <stdint.h>
#include "arch/gdt.h"
#include "arch/idt.h"
#include "drivers/vga.h"
#include "gui/framebuffer.h"
#include "gui/render.h"
#include "core/pmm.h"
#include "core/heap.h"
#include "drivers/pic.h"
#include "drivers/pit.h"
#include "drivers/keyboard.h"
#include "core/shell.h"
#include "core/acpi.h"
#include "fs/vfs.h"
#include "drivers/ata.h"
#include "fs/fat32.h"
#include "drivers/mouse.h"
#include "drivers/usb_tablet.h"
#include "core/multiboot2.h"
#include "net/net.h"

/* ------------------------------------------------------------------ */
/* Composite IRQ0 handler                                               */
/*   - PIT tick (uptime counter)                                        */
/*   - Cursor blink                                                     */
/*   - render_flush at ~30 fps (every 8 ticks at 250 Hz)               */
/* ------------------------------------------------------------------ */

static uint32_t flush_divider = 0;

static void timer_tick(void) {
    pit_tick();
    fb_cursor_tick();

    if (++flush_divider >= 2) {    /* 30 fps at 60 Hz */
        flush_divider = 0;
        render_flush();
    }
}

/* ------------------------------------------------------------------ */
/* Parse framebuffer info from Multiboot2 tags                         */
/* ------------------------------------------------------------------ */

static void parse_mbi(struct mb2_info *mbi) {
    struct mb2_tag *tag = (struct mb2_tag *)mbi->tags;

    while (tag->type != MB2_TAG_END) {
        if (tag->type == MB2_TAG_FRAMEBUFFER) {
            struct mb2_tag_framebuffer *fb =
                (struct mb2_tag_framebuffer *)tag;
            fb_init(fb->addr, fb->width, fb->height, fb->pitch, fb->bpp);
        }

        uint32_t next = (uint32_t)(uintptr_t)tag + tag->size;
        next = (next + 7) & ~7u;
        tag = (struct mb2_tag *)(uintptr_t)next;
    }

    pmm_init(mbi);
}

/* ------------------------------------------------------------------ */
/* Kernel entry point                                                   */
/* ------------------------------------------------------------------ */

void kernel_main(uint32_t magic, void *mbi) {
    (void)magic;

    parse_mbi((struct mb2_info *)mbi);

    vga_init();
    vga_print_colored("AusverseOS v0.2\n", VGA_YELLOW, VGA_BLACK);

    gdt_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("GDT loaded\n");

    idt_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("IDT loaded\n");

    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("PMM initialised - ");
    vga_print_uint(pmm_free_pages() * PAGE_SIZE / 1024 / 1024);
    vga_print(" MiB free\n");

    heap_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("Heap initialised\n");

    pic_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("PIC remapped\n");

    pit_init();
    irq_register(0, timer_tick);
    pic_unmask(0);
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("PIT configured\n");

    __asm__ volatile ("sti");
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("Interrupts enabled\n");

    keyboard_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("Keyboard driver loaded\n");

    acpi_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("ACPI initialised\n");

    vfs_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("VFS initialised\n");

    mouse_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("Mouse driver loaded\n");

    if (usb_tablet_init() == 0) {
        vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
        vga_print("USB tablet ready\n");
    } else {
        vga_print_colored("[  ] ", VGA_LGREY, VGA_BLACK);
        vga_print("USB tablet not found (using PS/2 mouse)\n");
    }

    int net_err = net_init();
    if (net_err == 0) {
        vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
        vga_print("Network ready (");
        uint8_t ip4[4]; net_get_ip(ip4);
        for (int i = 0; i < 4; i++) {
            vga_print_uint(ip4[i]);
            if (i < 3) vga_putchar('.');
        }
        vga_print(")\n");
    } else if (net_err == -1) {
        vga_print_colored("[  ] ", VGA_LGREY, VGA_BLACK);
        vga_print("E1000 NIC not found - set adapter to Intel PRO/1000 MT Desktop\n");
    } else {
        vga_print_colored("[  ] ", VGA_LGREY, VGA_BLACK);
        vga_print("DHCP timeout - NIC found but no IP assigned\n");
    }

    shell_run();
}

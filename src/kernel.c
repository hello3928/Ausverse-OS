#include <stdint.h>
#include "../include/gdt.h"
#include "../include/idt.h"
#include "../include/vga.h"
#include "../include/framebuffer.h"
#include "../include/pmm.h"
#include "../include/heap.h"
#include "../include/pic.h"
#include "../include/pit.h"
#include "../include/keyboard.h"
#include "../include/shell.h"
#include "../include/multiboot2.h"

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
    vga_print_colored("AusverseOS v0.1\n", VGA_YELLOW, VGA_BLACK);

    gdt_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("GDT loaded\n");

    idt_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("IDT loaded\n");

    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("PMM initialised - ");
    vga_print_colored("", VGA_LCYAN, VGA_BLACK);
    vga_print_uint(pmm_free_pages() * PAGE_SIZE / 1024 / 1024);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_print(" MiB free\n");

    heap_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("Heap initialised\n");

    pic_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("PIC remapped\n");

    pit_init();
    irq_register(0, pit_tick);
    pic_unmask(0);
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("PIT configured\n");

    __asm__ volatile ("sti");
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("Interrupts enabled\n");

    keyboard_init();
    vga_print_colored("[OK] ", VGA_LGREEN, VGA_BLACK);
    vga_print("Keyboard driver loaded\n");

    shell_run();
}

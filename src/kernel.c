#include <stdint.h>
#include <stddef.h>
#include "../include/gdt.h"
#include "../include/idt.h"
#include "../include/vga.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((uint16_t *)0xB8000)

/* VGA text-mode colours */
enum vga_colour {
    VGA_BLACK   = 0,
    VGA_BLUE    = 1,
    VGA_GREEN   = 2,
    VGA_CYAN    = 3,
    VGA_RED     = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN   = 6,
    VGA_LGREY   = 7,
    VGA_DGREY   = 8,
    VGA_LBLUE   = 9,
    VGA_LGREEN  = 10,
    VGA_LCYAN   = 11,
    VGA_LRED    = 12,
    VGA_LMAG    = 13,
    VGA_YELLOW  = 14,
    VGA_WHITE   = 15,
};

static uint16_t *vga_buf = VGA_MEMORY;
static size_t vga_row = 0;
static size_t vga_col = 0;
static uint8_t vga_clr;

static inline uint8_t make_colour(uint8_t fg, uint8_t bg) {
    return fg | (bg << 4);
}

static inline uint16_t make_entry(char c, uint8_t colour) {
    return (uint16_t)(uint8_t)c | ((uint16_t)colour << 8);
}

void vga_init(void) {
    vga_clr = make_colour(VGA_WHITE, VGA_BLACK);
    vga_row = vga_col = 0;
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga_buf[i] = make_entry(' ', vga_clr);
}

static void vga_scroll(void) {
    for (size_t row = 1; row < VGA_HEIGHT; row++)
        for (size_t col = 0; col < VGA_WIDTH; col++)
            vga_buf[(row - 1) * VGA_WIDTH + col] = vga_buf[row * VGA_WIDTH + col];
    for (size_t col = 0; col < VGA_WIDTH; col++)
        vga_buf[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = make_entry(' ', vga_clr);
    vga_row = VGA_HEIGHT - 1;
}

void vga_putchar(char c) {
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row == VGA_HEIGHT)
            vga_scroll();
        return;
    }
    vga_buf[vga_row * VGA_WIDTH + vga_col] = make_entry(c, vga_clr);
    if (++vga_col == VGA_WIDTH) {
        vga_col = 0;
        if (++vga_row == VGA_HEIGHT)
            vga_scroll();
    }
}

void vga_print(const char *s) {
    for (; *s; s++)
        vga_putchar(*s);
}

void kernel_main(uint32_t magic, void *mbi) {
    (void)magic;
    (void)mbi;

    vga_init();
    vga_print("AusverseOS v0.1\n");

    gdt_init();
    vga_print("[OK] GDT loaded\n");

    idt_init();
    vga_print("[OK] IDT loaded\n");

    vga_print("Kernel ready.\n");

    for (;;)
        __asm__ volatile ("hlt");
}

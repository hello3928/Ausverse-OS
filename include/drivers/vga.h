#pragma once
#include <stdint.h>

typedef enum {
    VGA_BLACK   = 0,  VGA_BLUE    = 1,  VGA_GREEN   = 2,  VGA_CYAN    = 3,
    VGA_RED     = 4,  VGA_MAGENTA = 5,  VGA_BROWN   = 6,  VGA_LGREY   = 7,
    VGA_DGREY   = 8,  VGA_LBLUE   = 9,  VGA_LGREEN  = 10, VGA_LCYAN   = 11,
    VGA_LRED    = 12, VGA_LMAG    = 13, VGA_YELLOW  = 14, VGA_WHITE   = 15,
} vga_color_t;

void vga_init(void);
void vga_set_color(vga_color_t fg, vga_color_t bg);
void vga_putchar(char c);
void vga_print(const char *s);
void vga_print_colored(const char *s, vga_color_t fg, vga_color_t bg);
void vga_print_uint(uint32_t n);

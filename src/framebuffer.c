#include <stdint.h>
#include <stddef.h>
#include "../include/framebuffer.h"
#include "../include/vga.h"
#include "../include/font.h"

/* ------------------------------------------------------------------ */
/* Framebuffer state                                                    */
/* ------------------------------------------------------------------ */

static uint8_t  *fb_addr  = 0;
static uint32_t  fb_pitch = 0;
static uint32_t  fb_w     = 0;
static uint32_t  fb_h     = 0;

/* Text grid dimensions derived from framebuffer + font size */
static uint32_t  cols = 0;
static uint32_t  rows = 0;

/* Cursor position in character cells */
static uint32_t cur_col = 0;
static uint32_t cur_row = 0;

/* Current foreground and background colours as 0x00RRGGBB */
static uint32_t fg_color = 0xFFFFFF;
static uint32_t bg_color = 0x000000;

/* ------------------------------------------------------------------ */
/* VGA colour index → RGB                                              */
/* ------------------------------------------------------------------ */

static const uint32_t palette[16] = {
    0x000000, /* VGA_BLACK   */
    0x0000AA, /* VGA_BLUE    */
    0x00AA00, /* VGA_GREEN   */
    0x00AAAA, /* VGA_CYAN    */
    0xAA0000, /* VGA_RED     */
    0xAA00AA, /* VGA_MAGENTA */
    0xAA5500, /* VGA_BROWN   */
    0xAAAAAA, /* VGA_LGREY   */
    0x555555, /* VGA_DGREY   */
    0x5555FF, /* VGA_LBLUE   */
    0x55FF55, /* VGA_LGREEN  */
    0x55FFFF, /* VGA_LCYAN   */
    0xFF5555, /* VGA_LRED    */
    0xFF55FF, /* VGA_LMAG    */
    0xFFFF55, /* VGA_YELLOW  */
    0xFFFFFF, /* VGA_WHITE   */
};

/* ------------------------------------------------------------------ */
/* Pixel-level drawing                                                  */
/* ------------------------------------------------------------------ */

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    uint32_t *p = (uint32_t *)(fb_addr + y * fb_pitch + x * 4);
    *p = color;
}

static void draw_char(uint32_t col, uint32_t row, char c,
                      uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font_glyph(c);
    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;

    for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
        for (uint32_t x = 0; x < FONT_WIDTH; x++) {
            uint32_t color = (glyph[y] & (0x80 >> x)) ? fg : bg;
            put_pixel(px + x, py + y, color);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                            */
/* ------------------------------------------------------------------ */

static void fb_scroll(void) {
    /* Move every row up by one character height */
    uint32_t row_bytes = FONT_HEIGHT * fb_pitch;
    uint8_t *dst = fb_addr;
    uint8_t *src = fb_addr + row_bytes;
    uint32_t copy_bytes = (rows - 1) * row_bytes;

    for (uint32_t i = 0; i < copy_bytes; i++)
        dst[i] = src[i];

    /* Clear the last row */
    uint8_t *last = fb_addr + (rows - 1) * row_bytes;
    for (uint32_t y = 0; y < FONT_HEIGHT; y++)
        for (uint32_t x = 0; x < fb_w; x++)
            *(uint32_t *)(last + y * fb_pitch + x * 4) = bg_color;

    cur_row = rows - 1;
}

/* ------------------------------------------------------------------ */
/* Public API (matches vga.h declarations)                             */
/* ------------------------------------------------------------------ */

void fb_init(uint64_t addr, uint32_t width, uint32_t height,
             uint32_t pitch, uint8_t bpp) {
    (void)bpp;
    fb_addr  = (uint8_t *)(uintptr_t)addr;
    fb_pitch = pitch;
    fb_w     = width;
    fb_h     = height;
    cols     = width  / FONT_WIDTH;
    rows     = height / FONT_HEIGHT;
}

void vga_init(void) {
    cur_col = cur_row = 0;
    fg_color = palette[VGA_WHITE];
    bg_color = palette[VGA_BLACK];

    /* Clear screen */
    for (uint32_t y = 0; y < fb_h; y++)
        for (uint32_t x = 0; x < fb_w; x++)
            put_pixel(x, y, bg_color);
}

void vga_set_color(vga_color_t fg, vga_color_t bg) {
    fg_color = palette[fg & 0xF];
    bg_color = palette[bg & 0xF];
}

void vga_putchar(char c) {
    if (!fb_addr) return;

    if (c == '\n') {
        cur_col = 0;
        if (++cur_row >= rows) fb_scroll();
        return;
    }

    if (c == '\b') {
        if (cur_col > 0) cur_col--;
        else if (cur_row > 0) { cur_row--; cur_col = cols - 1; }
        draw_char(cur_col, cur_row, ' ', fg_color, bg_color);
        return;
    }

    draw_char(cur_col, cur_row, c, fg_color, bg_color);

    if (++cur_col >= cols) {
        cur_col = 0;
        if (++cur_row >= rows) fb_scroll();
    }
}

void vga_print(const char *s) {
    for (; *s; s++) vga_putchar(*s);
}

void vga_print_colored(const char *s, vga_color_t fg, vga_color_t bg) {
    uint32_t sfg = fg_color, sbg = bg_color;
    fg_color = palette[fg & 0xF];
    bg_color = palette[bg & 0xF];
    for (; *s; s++) vga_putchar(*s);
    fg_color = sfg;
    bg_color = sbg;
}

void fb_get_cursor(uint32_t *col, uint32_t *row) {
    *col = cur_col;
    *row = cur_row;
}

void fb_set_cursor(uint32_t col, uint32_t row) {
    cur_col = col;
    cur_row = row;
}

uint32_t fb_cols(void) {
    return cols;
}

void vga_print_uint(uint32_t n) {
    if (n == 0) { vga_putchar('0'); return; }
    char buf[10];
    int i = 0;
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) vga_putchar(buf[i]);
}

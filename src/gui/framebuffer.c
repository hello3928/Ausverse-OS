#include <stdint.h>
#include <stddef.h>
#include "gui/framebuffer.h"
#include "gui/render.h"
#include "drivers/vga.h"
#include "gui/font.h"

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

/*
 * All pixel writes go to the render back buffer (not the hardware
 * framebuffer directly).  render_flush() pushes the back buffer to
 * hardware; it is called from the PIT handler at ~50 fps.
 */
static uint32_t *back_buf = NULL;   /* pointer into render's back buffer */
static uint32_t  fb_w     = 0;      /* screen width  (pixels) */
static uint32_t  fb_h     = 0;      /* screen height (pixels) */

/* Text grid */
static uint32_t cols = 0;
static uint32_t rows = 0;

/* Logical cursor position (character cells) */
static uint32_t cur_col = 0;
static uint32_t cur_row = 0;

/* Current colours (0x00RRGGBB) */
static uint32_t fg_color = 0xFFFFFF;
static uint32_t bg_color = 0x000000;

/* Blinking cursor state */
static int      cursor_enabled    = 0;
static int      cursor_visible    = 0;
static uint32_t cursor_tick_count = 0;
#define CURSOR_BLINK_TICKS 50   /* 500 ms at 100 Hz */

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
/* Pixel helper — writes to the render back buffer                     */
/* ------------------------------------------------------------------ */

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    back_buf[y * fb_w + x] = color;
}

/* ------------------------------------------------------------------ */
/* Glyph rendering (2× vertical scale)                                 */
/* ------------------------------------------------------------------ */

static void draw_char(uint32_t col, uint32_t row, char c,
                      uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font_glyph(c);
    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;

    for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y >> 1];   /* 2× vertical scale */
        for (uint32_t x = 0; x < FONT_WIDTH; x++) {
            uint32_t color = (bits & (0x80u >> x)) ? fg : bg;
            put_pixel(px + x, py + y, color);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Cursor underline                                                     */
/* ------------------------------------------------------------------ */

static void draw_cursor_shape(uint32_t col, uint32_t row, int show) {
    if (!back_buf) return;
    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT + FONT_HEIGHT - 3;
    uint32_t color = show ? fg_color : bg_color;
    for (uint32_t dy = 0; dy < 2; dy++)
        for (uint32_t dx = 0; dx < FONT_WIDTH; dx++)
            put_pixel(px + dx, py + dy, color);
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                            */
/* ------------------------------------------------------------------ */

static void fb_scroll(void) {
    uint32_t row_px = FONT_HEIGHT * fb_w;

    /* Shift every row up by FONT_HEIGHT pixel rows */
    for (uint32_t i = 0; i < (rows - 1) * row_px; i++)
        back_buf[i] = back_buf[i + row_px];

    /* Clear last text row */
    uint32_t last = (rows - 1) * row_px;
    for (uint32_t i = 0; i < row_px; i++)
        back_buf[last + i] = bg_color;

    cur_row = rows - 1;
}

/* ------------------------------------------------------------------ */
/* Public init                                                          */
/* ------------------------------------------------------------------ */

void fb_init(uint64_t addr, uint32_t width, uint32_t height,
             uint32_t pitch, uint8_t bpp) {
    (void)bpp;

    /* Initialise the render subsystem — this sets up the back buffer. */
    render_init(addr, width, height, pitch);

    /* Point our fast put_pixel at render's back buffer. */
    back_buf = render_pixels();
    fb_w     = render_width();
    fb_h     = render_height();
    cols     = fb_w / FONT_WIDTH;
    rows     = fb_h / FONT_HEIGHT;
}

/* ------------------------------------------------------------------ */
/* VGA-compatible text API                                              */
/* ------------------------------------------------------------------ */

void vga_init(void) {
    cur_col = cur_row = 0;
    cursor_enabled = cursor_visible = 0;
    cursor_tick_count = 0;
    fg_color = palette[VGA_WHITE];
    bg_color = palette[VGA_BLACK];

    if (!back_buf) return;
    for (uint32_t i = 0; i < fb_w * fb_h; i++)
        back_buf[i] = bg_color;
}

void vga_set_color(vga_color_t fg, vga_color_t bg) {
    fg_color = palette[fg & 0xF];
    bg_color = palette[bg & 0xF];
}

void vga_putchar(char c) {
    if (!back_buf) return;

    /* Erase cursor before touching its cell */
    if (cursor_enabled && cursor_visible)
        draw_cursor_shape(cur_col, cur_row, 0);

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

void vga_print_uint(uint32_t n) {
    if (n == 0) { vga_putchar('0'); return; }
    char buf[10]; int i = 0;
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) vga_putchar(buf[i]);
}

/* ------------------------------------------------------------------ */
/* Cursor API                                                           */
/* ------------------------------------------------------------------ */

void fb_get_cursor(uint32_t *col, uint32_t *row) {
    *col = cur_col; *row = cur_row;
}

void fb_set_cursor(uint32_t col, uint32_t row) {
    if (cursor_enabled && cursor_visible)
        draw_cursor_shape(cur_col, cur_row, 0);
    cur_col = col;
    cur_row = row;
}

uint32_t fb_cols(void) { return cols; }
uint32_t fb_rows(void) { return rows; }

void fb_cursor_enable(int on) {
    cursor_enabled = on;
    if (!on) {
        draw_cursor_shape(cur_col, cur_row, 0);
        cursor_visible    = 0;
        cursor_tick_count = 0;
    }
}

void fb_cursor_tick(void) {
    if (!cursor_enabled || !back_buf) return;
    if (++cursor_tick_count < CURSOR_BLINK_TICKS) return;
    cursor_tick_count = 0;
    cursor_visible    = !cursor_visible;
    draw_cursor_shape(cur_col, cur_row, cursor_visible);
}

/* ------------------------------------------------------------------ */
/* Legacy graphics primitives (kept for shell's header bar, etc.)      */
/* These all write to the back buffer — render_flush pushes them out.  */
/* ------------------------------------------------------------------ */

void fb_draw_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
    if (!back_buf || x >= fb_w || y >= fb_h) return;
    put_pixel(x, y, rgb);
}

void fb_draw_hline(uint32_t x, uint32_t y, uint32_t len, uint32_t rgb) {
    if (!back_buf || y >= fb_h) return;
    uint32_t end = x + len; if (end > fb_w) end = fb_w;
    for (uint32_t i = x; i < end; i++) put_pixel(i, y, rgb);
}

void fb_draw_vline(uint32_t x, uint32_t y, uint32_t len, uint32_t rgb) {
    if (!back_buf || x >= fb_w) return;
    uint32_t end = y + len; if (end > fb_h) end = fb_h;
    for (uint32_t i = y; i < end; i++) put_pixel(x, i, rgb);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
    if (!back_buf) return;
    uint32_t x2 = x + w; if (x2 > fb_w) x2 = fb_w;
    uint32_t y2 = y + h; if (y2 > fb_h) y2 = fb_h;
    for (uint32_t row = y; row < y2; row++)
        for (uint32_t col = x; col < x2; col++)
            put_pixel(col, row, rgb);
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
    fb_draw_hline(x,         y,         w, rgb);
    fb_draw_hline(x,         y + h - 1, w, rgb);
    fb_draw_vline(x,         y,         h, rgb);
    fb_draw_vline(x + w - 1, y,         h, rgb);
}

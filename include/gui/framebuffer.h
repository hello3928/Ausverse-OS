#pragma once
#include <stdint.h>

void fb_init(uint64_t addr, uint32_t width, uint32_t height,
             uint32_t pitch, uint8_t bpp);

/* Cursor position */
void     fb_get_cursor(uint32_t *col, uint32_t *row);
void     fb_set_cursor(uint32_t col, uint32_t row);
uint32_t fb_cols(void);
uint32_t fb_rows(void);

/* Blinking cursor — call fb_cursor_tick() from the PIT handler */
void fb_cursor_enable(int on);
void fb_cursor_tick(void);

/* Graphics primitives (pixel coordinates) */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void fb_draw_hline(uint32_t x, uint32_t y, uint32_t len, uint32_t rgb);
void fb_draw_vline(uint32_t x, uint32_t y, uint32_t len, uint32_t rgb);
void fb_draw_pixel(uint32_t x, uint32_t y, uint32_t rgb);

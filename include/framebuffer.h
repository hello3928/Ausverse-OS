#pragma once
#include <stdint.h>

void fb_init(uint64_t addr, uint32_t width, uint32_t height,
             uint32_t pitch, uint8_t bpp);

void     fb_get_cursor(uint32_t *col, uint32_t *row);
void     fb_set_cursor(uint32_t col, uint32_t row);
uint32_t fb_cols(void);

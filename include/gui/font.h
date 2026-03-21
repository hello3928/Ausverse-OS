#pragma once
#include <stdint.h>

#define FONT_WIDTH       8
#define FONT_GLYPH_ROWS  8   /* rows of bitmap data per glyph */
#define FONT_HEIGHT      16  /* rendered pixel height (2x vertical scale) */

/* Returns pointer to FONT_GLYPH_ROWS bytes for ASCII character c.
 * Characters outside printable ASCII are rendered as a blank. */
const uint8_t *font_glyph(char c);

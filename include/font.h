#pragma once
#include <stdint.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 8

/* Returns pointer to 8-byte glyph for ASCII character c.
 * Characters outside printable ASCII are rendered as a blank. */
const uint8_t *font_glyph(char c);

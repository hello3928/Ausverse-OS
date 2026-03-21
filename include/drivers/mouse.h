#pragma once
#include <stdint.h>

typedef struct {
    int32_t  x, y;       /* absolute pixel position, clamped to screen */
    int32_t  dx, dy;     /* delta from last packet                      */
    uint8_t  buttons;    /* bit0=left, bit1=right, bit2=middle          */
} mouse_state_t;

void          mouse_init(void);
mouse_state_t mouse_get(void);

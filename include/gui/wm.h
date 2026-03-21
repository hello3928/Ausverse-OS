#pragma once
#include <stdint.h>
#include "gui/render.h"

#define WM_MAX_WINDOWS 8

/* Paint callback: draw app content into 'client' rect on the screen surface.
   All coordinates are screen-absolute. */
typedef void (*wm_paint_fn)(struct surface *s, struct rect client, void *ud);

/* Key callback: active window receives each keypress. */
typedef void (*wm_key_fn)(char key, void *ud);

void wm_init(void);
void wm_run(void);          /* event + render loop; returns when all windows close */

/* Returns window ID (≥ 0), or -1 on failure. */
int  wm_open(const char *title,
             int32_t x, int32_t y, int32_t w, int32_t h,
             wm_paint_fn paint, wm_key_fn on_key, void *ud);

void wm_close(int id);
void wm_raise(int id);      /* bring window to front */

#include <stdint.h>
#include <stddef.h>
#include "gui/render.h"
#include "gui/font.h"

/* ------------------------------------------------------------------ */
/* Back buffer                                                          */
/*                                                                      */
/* Lives in BSS — zero-initialised, not stored in the ISO image.       */
/* At 1920×1080×4 bytes = 8 MiB.  The PMM marks these pages used      */
/* automatically because they're within kernel_end.                    */
/* ------------------------------------------------------------------ */

#define MAX_WIDTH  1920
#define MAX_HEIGHT 1080

static uint32_t back_buffer[MAX_WIDTH * MAX_HEIGHT];

/* ------------------------------------------------------------------ */
/* Hardware framebuffer metadata (set by render_init)                  */
/* ------------------------------------------------------------------ */

static int       auto_flush  = 1;    /* 0 = paused by GUI */
static uint8_t  *hw_fb       = NULL;
static uint32_t  hw_width    = 0;
static uint32_t  hw_height   = 0;
static uint32_t  hw_pitch    = 0;   /* bytes per row, hardware side */

/* ------------------------------------------------------------------ */
/* Screen surface                                                       */
/* ------------------------------------------------------------------ */

static struct surface screen_surf;

void render_init(uint64_t hw_fb_addr, uint32_t width, uint32_t height,
                 uint32_t hw_pitch_bytes) {
    hw_fb    = (uint8_t *)(uintptr_t)hw_fb_addr;
    hw_width  = width  < MAX_WIDTH  ? width  : MAX_WIDTH;
    hw_height = height < MAX_HEIGHT ? height : MAX_HEIGHT;
    hw_pitch  = hw_pitch_bytes;

    screen_surf.pixels = back_buffer;
    screen_surf.width  = hw_width;
    screen_surf.height = hw_height;
    screen_surf.stride = hw_width;   /* packed, no row padding */
}

/*
 * render_flush — blit the back buffer to the hardware framebuffer.
 *
 * The hardware pitch may differ from hw_width*4 (e.g. on some BIOSes
 * the pitch is rounded up to a 256-byte boundary).  We copy row-by-row
 * so pitch mismatches are handled correctly.
 */
static void do_flush(void) {
    for (uint32_t y = 0; y < hw_height; y++) {
        const uint32_t *src = back_buffer + y * hw_width;
        uint32_t       *dst = (uint32_t *)(hw_fb + y * hw_pitch);
        for (uint32_t x = 0; x < hw_width; x++)
            dst[x] = src[x];
    }
}

void render_flush(void) {
    if (!hw_fb || !auto_flush) return;
    do_flush();
}

void render_flush_now(void) {
    if (!hw_fb) return;
    do_flush();
}

void render_pause_auto(int pause) {
    auto_flush = !pause;
}

struct surface *render_screen(void)  { return &screen_surf; }
uint32_t       *render_pixels(void)  { return back_buffer;  }
uint32_t        render_width(void)   { return hw_width;     }
uint32_t        render_height(void)  { return hw_height;    }

/* ------------------------------------------------------------------ */
/* Internal pixel helper — no bounds check (fast path)                 */
/* ------------------------------------------------------------------ */

static inline void spx(struct surface *s, uint32_t x, uint32_t y, uint32_t c) {
    s->pixels[y * s->stride + x] = c;
}

/* ------------------------------------------------------------------ */
/* Surface primitives                                                   */
/* ------------------------------------------------------------------ */

void surf_clear(struct surface *s, color_t c) {
    uint32_t total = s->height * s->stride;
    for (uint32_t i = 0; i < total; i++)
        s->pixels[i] = c;
}

void surf_pixel(struct surface *s, int32_t x, int32_t y, color_t c) {
    if (x < 0 || y < 0 || (uint32_t)x >= s->width || (uint32_t)y >= s->height)
        return;
    spx(s, (uint32_t)x, (uint32_t)y, c);
}

void surf_fill_rect(struct surface *s, struct rect r, color_t c) {
    int32_t x2 = r.x + r.w, y2 = r.y + r.h;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (x2 > (int32_t)s->width)  x2 = (int32_t)s->width;
    if (y2 > (int32_t)s->height) y2 = (int32_t)s->height;
    for (int32_t py = r.y; py < y2; py++)
        for (int32_t px = r.x; px < x2; px++)
            spx(s, (uint32_t)px, (uint32_t)py, c);
}

void surf_hline(struct surface *s, int32_t x, int32_t y, int32_t w, color_t c) {
    if (y < 0 || (uint32_t)y >= s->height || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > (int32_t)s->width) w = (int32_t)s->width - x;
    if (w <= 0) return;
    uint32_t *row = s->pixels + (uint32_t)y * s->stride + (uint32_t)x;
    for (int32_t i = 0; i < w; i++) row[i] = c;
}

void surf_vline(struct surface *s, int32_t x, int32_t y, int32_t h, color_t c) {
    if (x < 0 || (uint32_t)x >= s->width || h <= 0) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > (int32_t)s->height) h = (int32_t)s->height - y;
    if (h <= 0) return;
    for (int32_t i = 0; i < h; i++)
        spx(s, (uint32_t)x, (uint32_t)(y + i), c);
}

void surf_outline_rect(struct surface *s, struct rect r, color_t c) {
    surf_hline(s, r.x,         r.y,         r.w, c);
    surf_hline(s, r.x,         r.y + r.h-1, r.w, c);
    surf_vline(s, r.x,         r.y,         r.h, c);
    surf_vline(s, r.x + r.w-1, r.y,         r.h, c);
}

/* Bresenham line */
void surf_line(struct surface *s, int32_t x0, int32_t y0,
                                   int32_t x1, int32_t y1, color_t c) {
    int32_t dx =  (x1 > x0 ? x1 - x0 : x0 - x1);
    int32_t dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;

    for (;;) {
        surf_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ------------------------------------------------------------------ */
/* Text rendering on a surface                                          */
/* ------------------------------------------------------------------ */

void surf_char(struct surface *s, int32_t x, int32_t y, char c,
               color_t fg, color_t bg) {
    const uint8_t *glyph = font_glyph(c);
    for (int32_t gy = 0; gy < (int32_t)FONT_HEIGHT; gy++) {
        uint8_t bits = glyph[gy >> 1];   /* 2× vertical scale */
        for (int32_t gx = 0; gx < (int32_t)FONT_WIDTH; gx++) {
            color_t col = (bits & (0x80u >> gx)) ? fg : bg;
            surf_pixel(s, x + gx, y + gy, col);
        }
    }
}

void surf_text(struct surface *s, int32_t x, int32_t y, const char *str,
               color_t fg, color_t bg) {
    for (; *str; str++, x += FONT_WIDTH) {
        if (x + FONT_WIDTH > (int32_t)s->width) break;
        surf_char(s, x, y, *str, fg, bg);
    }
}

void surf_text_transp(struct surface *s, int32_t x, int32_t y,
                      const char *str, color_t fg) {
    for (; *str; str++, x += FONT_WIDTH) {
        const uint8_t *glyph = font_glyph(*str);
        for (int32_t gy = 0; gy < (int32_t)FONT_HEIGHT; gy++) {
            uint8_t bits = glyph[gy >> 1];
            for (int32_t gx = 0; gx < (int32_t)FONT_WIDTH; gx++) {
                if (bits & (0x80u >> gx))
                    surf_pixel(s, x + gx, y + gy, fg);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Windows 95-style 3D chrome                                          */
/* ------------------------------------------------------------------ */

/*
 * 2-pixel raised border (inside r):
 *   top/left  outer: COL_WIN_HILIGHT  (white)
 *   top/left  inner: COL_WIN_LIGHT    (light gray)
 *   bot/right inner: COL_WIN_SHADOW   (mid gray)
 *   bot/right outer: COL_WIN_DKSHADOW (black)
 */
void surf_raised(struct surface *s, struct rect r) {
    /* outer */
    surf_hline(s, r.x,         r.y,         r.w,   COL_WIN_HILIGHT);
    surf_hline(s, r.x,         r.y+r.h-1,   r.w,   COL_WIN_DKSHADOW);
    surf_vline(s, r.x,         r.y,         r.h,   COL_WIN_HILIGHT);
    surf_vline(s, r.x+r.w-1,   r.y,         r.h,   COL_WIN_DKSHADOW);
    /* inner */
    surf_hline(s, r.x+1,       r.y+1,       r.w-2, COL_WIN_LIGHT);
    surf_hline(s, r.x+1,       r.y+r.h-2,   r.w-2, COL_WIN_SHADOW);
    surf_vline(s, r.x+1,       r.y+1,       r.h-2, COL_WIN_LIGHT);
    surf_vline(s, r.x+r.w-2,   r.y+1,       r.h-2, COL_WIN_SHADOW);
}

void surf_sunken(struct surface *s, struct rect r) {
    /* outer */
    surf_hline(s, r.x,         r.y,         r.w,   COL_WIN_SHADOW);
    surf_hline(s, r.x,         r.y+r.h-1,   r.w,   COL_WIN_HILIGHT);
    surf_vline(s, r.x,         r.y,         r.h,   COL_WIN_SHADOW);
    surf_vline(s, r.x+r.w-1,   r.y,         r.h,   COL_WIN_HILIGHT);
    /* inner */
    surf_hline(s, r.x+1,       r.y+1,       r.w-2, COL_WIN_DKSHADOW);
    surf_hline(s, r.x+1,       r.y+r.h-2,   r.w-2, COL_WIN_LIGHT);
    surf_vline(s, r.x+1,       r.y+1,       r.h-2, COL_WIN_DKSHADOW);
    surf_vline(s, r.x+r.w-2,   r.y+1,       r.h-2, COL_WIN_LIGHT);
}

/* ------------------------------------------------------------------ */
/* Button                                                               */
/* ------------------------------------------------------------------ */

static uint32_t slen(const char *s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

void surf_button(struct surface *s, struct rect r,
                 const char *label, int pressed) {
    /* Face */
    struct rect inner = {r.x+2, r.y+2, r.w-4, r.h-4};
    surf_fill_rect(s, inner, COL_WIN_FACE);

    /* 3D border */
    if (pressed) surf_sunken(s, r);
    else         surf_raised(s, r);

    /* Label centred */
    if (label) {
        int32_t lw = (int32_t)(slen(label) * FONT_WIDTH);
        int32_t lh = (int32_t)FONT_HEIGHT;
        int32_t lx = r.x + (r.w - lw) / 2 + (pressed ? 1 : 0);
        int32_t ly = r.y + (r.h - lh) / 2 + (pressed ? 1 : 0);
        surf_text(s, lx, ly, label, COL_WIN_TEXT, COL_WIN_FACE);
    }
}

/* ------------------------------------------------------------------ */
/* Window frame                                                         */
/* ------------------------------------------------------------------ */

#define TITLEBAR_H  (FONT_HEIGHT + 4)   /* pixels tall */
#define CHROME_BTN  (TITLEBAR_H - 2)    /* close/min/max button size */

void surf_window(struct surface *s, struct rect r,
                 const char *title, int active) {
    /* ---- Outer black border ---- */
    surf_outline_rect(s, r, COL_BLACK);

    /* ---- Outer raised border (inside the black) ---- */
    struct rect raised = {r.x+1, r.y+1, r.w-2, r.h-2};
    surf_raised(s, raised);

    /* ---- Title bar ---- */
    color_t bar_col = active ? COL_WIN_TITLEBAR : COL_WIN_TITLEBAR_I;
    struct rect bar = {r.x+2, r.y+2, r.w-4, TITLEBAR_H};
    surf_fill_rect(s, bar, bar_col);

    /* Title text */
    if (title) {
        surf_text_transp(s, bar.x + 4, bar.y + 2, title, COL_WIN_TITLETEXT);
    }

    /* Close button [ X ] */
    struct rect close_btn = {
        r.x + r.w - 2 - CHROME_BTN,
        r.y + 2,
        CHROME_BTN,
        CHROME_BTN
    };
    surf_button(s, close_btn, "X", 0);

    /* Maximise button [ □ ] */
    struct rect max_btn = {
        close_btn.x - 1 - CHROME_BTN,
        r.y + 2,
        CHROME_BTN,
        CHROME_BTN
    };
    surf_button(s, max_btn, "O", 0);

    /* Minimise button [ _ ] */
    struct rect min_btn = {
        max_btn.x - 1 - CHROME_BTN,
        r.y + 2,
        CHROME_BTN,
        CHROME_BTN
    };
    surf_button(s, min_btn, "_", 0);

    /* ---- Separator line below title bar ---- */
    surf_hline(s, r.x+2, r.y+2+TITLEBAR_H, r.w-4, COL_WIN_DKSHADOW);

    /* ---- Client area fill ---- */
    struct rect client = {
        r.x + 2,
        r.y + 2 + TITLEBAR_H + 1,
        r.w - 4,
        r.h - 4 - TITLEBAR_H - 1
    };
    surf_fill_rect(s, client, COL_WIN_FACE);

    /* ---- Inner sunken border around client area ---- */
    struct rect sunken = {
        client.x - 1,
        client.y - 1,
        client.w + 2,
        client.h + 2
    };
    surf_sunken(s, sunken);
}

/* ------------------------------------------------------------------ */
/* Desktop                                                              */
/* ------------------------------------------------------------------ */

void surf_desktop(struct surface *s) {
    surf_clear(s, COL_DESKTOP);
}

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

#define MAX_WIDTH  1280
#define MAX_HEIGHT 1024

static uint32_t back_buffer[MAX_WIDTH * MAX_HEIGHT];

/* Expanded row for 2x upscale blit (3840 pixels wide) */
static uint32_t row_buf[MAX_WIDTH * 2];

/* Pre-computed desktop gradient — filled once, blitted on every repaint.
 * Avoids re-running 1080 hline calls (expensive in QEMU TCG mode). */
static uint32_t desktop_buf[MAX_WIDTH * MAX_HEIGHT];
static int      desktop_ready = 0;

/* ------------------------------------------------------------------ */
/* Hardware framebuffer metadata (set by render_init)                  */
/* ------------------------------------------------------------------ */

static int       auto_flush  = 1;    /* 0 = paused by GUI */
static uint8_t  *hw_fb       = NULL;
static uint32_t  hw_width    = 0;   /* render resolution (1080p) */
static uint32_t  hw_height   = 0;
static uint32_t  hw_pitch    = 0;   /* bytes per row, hardware side */
static uint32_t  fb_width    = 0;   /* true framebuffer width (may be 4K) */
static uint32_t  fb_height   = 0;   /* true framebuffer height */

/* ------------------------------------------------------------------ */
/* Screen surface                                                       */
/* ------------------------------------------------------------------ */

static struct surface screen_surf;

void render_init(uint64_t hw_fb_addr, uint32_t width, uint32_t height,
                 uint32_t hw_pitch_bytes) {
    hw_fb     = (uint8_t *)(uintptr_t)hw_fb_addr;
    fb_width  = width;
    fb_height = height;
    hw_pitch  = hw_pitch_bytes;
    /* Render at 1080p regardless of actual framebuffer size */
    hw_width  = width  < MAX_WIDTH  ? width  : MAX_WIDTH;
    hw_height = height < MAX_HEIGHT ? height : MAX_HEIGHT;

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
    if (fb_width == hw_width * 2u && fb_height == hw_height * 2u) {
        /* 2x upscale: each 1080p pixel → 2×2 block in 4K framebuffer */
        for (uint32_t y = 0; y < hw_height; y++) {
            const uint32_t *src = back_buffer + y * hw_width;
            /* Expand row: pixel x → row_buf[2x], row_buf[2x+1] */
            for (uint32_t x = 0; x < hw_width; x++)
                row_buf[x * 2] = row_buf[x * 2 + 1] = src[x];
            /* Write expanded row to two consecutive 4K rows */
            for (uint32_t r = 0; r < 2; r++) {
                void    *s   = row_buf;
                void    *d   = hw_fb + (y * 2 + r) * hw_pitch;
                uint32_t n   = hw_width * 2;
                __asm__ volatile ("rep movsl"
                    : "+c"(n), "+S"(s), "+D"(d) :: "memory");
            }
        }
    } else {
        /* 1:1 blit */
        if (hw_pitch == hw_width * 4u) {
            uint32_t n   = hw_width * hw_height;
            void    *src = back_buffer;
            void    *dst = hw_fb;
            __asm__ volatile ("rep movsl"
                : "+c"(n), "+S"(src), "+D"(dst) :: "memory");
        } else {
            for (uint32_t y = 0; y < hw_height; y++) {
                uint32_t  n   = hw_width;
                void     *src = back_buffer + y * hw_width;
                void     *dst = hw_fb + y * hw_pitch;
                __asm__ volatile ("rep movsl"
                    : "+c"(n), "+S"(src), "+D"(dst) :: "memory");
            }
        }
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

/* ------------------------------------------------------------------ */
/* Cursor-composited flush                                              */
/*                                                                      */
/* The scene buffer never contains the cursor.  After blitting the     */
/* scene to hardware we stamp the cursor directly on the hw fb.        */
/* Next flush overwrites those pixels with the clean scene again.      */
/* ------------------------------------------------------------------ */

static void draw_cursor_hw(int32_t x, int32_t y) {
    /* Scale factor: 1 for 1:1, 2 when hw fb is 2× the render size   */
    uint32_t scale = (fb_width == hw_width * 2u && fb_height == hw_height * 2u)
                     ? 2u : 1u;

    for (int32_t r = 0; r < 16; r++) {
        for (int32_t c = 0; c <= 15 - r; c++) {
            int      edge  = (c == 0) || (r == 0) || (c + r == 15);
            uint32_t color = edge ? 0x000000u : 0xFFFFFFu;

            for (uint32_t sy = 0; sy < scale; sy++) {
                for (uint32_t sx = 0; sx < scale; sx++) {
                    int32_t px = x * (int32_t)scale + c * (int32_t)scale + (int32_t)sx;
                    int32_t py = y * (int32_t)scale + r * (int32_t)scale + (int32_t)sy;
                    if (px < 0 || py < 0 ||
                        (uint32_t)px >= fb_width ||
                        (uint32_t)py >= fb_height) continue;
                    *(uint32_t *)(hw_fb + (uint32_t)py * hw_pitch
                                        + (uint32_t)px * 4u) = color;
                }
            }
        }
    }
}

void render_flush_with_cursor(int32_t cx, int32_t cy) {
    if (!hw_fb) return;
    do_flush();
    draw_cursor_hw(cx, cy);
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
    uint32_t  n   = s->height * s->stride;
    void     *dst = s->pixels;
    __asm__ volatile ("rep stosl"
        : "+c"(n), "+D"(dst) : "a"(c) : "memory");
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
    uint32_t n = (uint32_t)(x2 - r.x);
    if ((int32_t)n <= 0) return;
    for (int32_t py = r.y; py < y2; py++) {
        void *dst = s->pixels + (uint32_t)py * s->stride + (uint32_t)r.x;
        uint32_t cnt = n;
        __asm__ volatile ("rep stosl"
            : "+c"(cnt), "+D"(dst) : "a"(c) : "memory");
    }
}

void surf_hline(struct surface *s, int32_t x, int32_t y, int32_t w, color_t c) {
    if (y < 0 || (uint32_t)y >= s->height || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > (int32_t)s->width) w = (int32_t)s->width - x;
    if (w <= 0) return;
    void    *dst = s->pixels + (uint32_t)y * s->stride + (uint32_t)x;
    uint32_t n   = (uint32_t)w;
    __asm__ volatile ("rep stosl"
        : "+c"(n), "+D"(dst) : "a"(c) : "memory");
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
/* Chrome geometry                                                      */
/* ------------------------------------------------------------------ */

#define TITLEBAR_H  (FONT_HEIGHT + 10)  /* 26px — modern, comfortable  */
#define CHROME_BTN  16                  /* fixed button size            */

/* ------------------------------------------------------------------ */
/* Flat stubs — kept so external code still compiles                   */
/* ------------------------------------------------------------------ */

void surf_raised(struct surface *s, struct rect r) {
    surf_outline_rect(s, r, COL_WIN_SHADOW);
}

void surf_sunken(struct surface *s, struct rect r) {
    surf_outline_rect(s, r, COL_WIN_DKSHADOW);
}

/* ------------------------------------------------------------------ */
/* Button — flat modern style                                           */
/* ------------------------------------------------------------------ */

static uint32_t slen(const char *s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

void surf_button(struct surface *s, struct rect r,
                 const char *label, int pressed) {
    color_t face = pressed ? COL_ACCENT : COL_WIN_FACE;
    color_t text = pressed ? COL_WHITE  : COL_WIN_TEXT;
    surf_fill_rect(s, r, face);
    surf_outline_rect(s, r, COL_WIN_BORDER_I);
    if (label) {
        int32_t lw = (int32_t)(slen(label) * FONT_WIDTH);
        int32_t lx = r.x + (r.w - lw) / 2 + (pressed ? 1 : 0);
        int32_t ly = r.y + (r.h - (int32_t)FONT_HEIGHT) / 2 + (pressed ? 1 : 0);
        surf_text(s, lx, ly, label, text, face);
    }
}

/* ------------------------------------------------------------------ */
/* Window frame — flat, modern, drop-shadowed                          */
/* ------------------------------------------------------------------ */

void surf_window(struct surface *s, struct rect r,
                 const char *title, int active) {
    /* Drop shadow (drawn first — windows above will overdraw it) */
    surf_fill_rect(s, make_rect(r.x + 4, r.y + 4, r.w, r.h), COL_SHADOW);

    /* Outer border — 1 px, accent when active */
    color_t border = active ? COL_WIN_BORDER_A : COL_WIN_BORDER_I;
    surf_outline_rect(s, r, border);

    /* Title bar */
    color_t bar = active ? COL_WIN_TITLEBAR : COL_WIN_TITLEBAR_I;
    struct rect title_bar = {r.x + 1, r.y + 1, r.w - 2, TITLEBAR_H};
    surf_fill_rect(s, title_bar, bar);

    /* Button vertical centre within title bar */
    int32_t by = r.y + 1 + (TITLEBAR_H - CHROME_BTN) / 2;

    /* Close button — red */
    struct rect close_btn = {r.x + r.w - 3 - CHROME_BTN, by,
                              CHROME_BTN, CHROME_BTN};
    surf_fill_rect(s, close_btn, COL_BTN_CLOSE);
    {   /* Centred "×" */
        int32_t tx = close_btn.x + (CHROME_BTN - FONT_WIDTH)  / 2;
        int32_t ty = close_btn.y + (CHROME_BTN - FONT_HEIGHT) / 2;
        surf_text(s, tx, ty, "X", COL_BTN_ICON, COL_BTN_CLOSE);
    }

    /* Maximise button */
    struct rect max_btn = {close_btn.x - 2 - CHROME_BTN, by,
                           CHROME_BTN, CHROME_BTN};
    surf_fill_rect(s, max_btn, bar);
    surf_outline_rect(s, make_rect(max_btn.x + 3, max_btn.y + 3,
                                   CHROME_BTN - 6, CHROME_BTN - 6),
                      COL_BTN_ICON);

    /* Minimise button */
    struct rect min_btn = {max_btn.x - 2 - CHROME_BTN, by,
                           CHROME_BTN, CHROME_BTN};
    surf_fill_rect(s, min_btn, bar);
    surf_hline(s, min_btn.x + 3,
                  min_btn.y + CHROME_BTN * 2 / 3,
                  CHROME_BTN - 6, COL_BTN_ICON);

    /* Title text — vertically centred, left of buttons */
    if (title) {
        int32_t tx = r.x + 8;
        int32_t ty = r.y + 1 + (TITLEBAR_H - (int32_t)FONT_HEIGHT) / 2;
        surf_text_transp(s, tx, ty, title, COL_WIN_TITLETEXT);
    }

    /* Separator line */
    surf_hline(s, r.x + 1, r.y + 1 + TITLEBAR_H, r.w - 2, COL_WIN_SEPARATOR);

    /* Client area fill */
    struct rect client = {r.x + 1, r.y + 2 + TITLEBAR_H,
                          r.w - 2, r.h - 3 - TITLEBAR_H};
    surf_fill_rect(s, client, COL_WIN_FACE);
}

/* ------------------------------------------------------------------ */
/* Desktop — pre-computed dark gradient                                 */
/*                                                                      */
/* The gradient is computed once into desktop_buf, then every repaint  */
/* is a single rep movsl (one bulk copy) rather than 1080 hline calls. */
/* ------------------------------------------------------------------ */

void surf_desktop(struct surface *s) {
    if (!desktop_ready) {
        /* Build gradient row by row into desktop_buf */
        for (uint32_t y = 0; y < s->height; y++) {
            uint32_t t   = y * 255u / (s->height > 1u ? s->height - 1u : 1u);
            uint8_t  r2  = (uint8_t)( 6u + ( 8u * t >> 8));
            uint8_t  g   = (uint8_t)( 6u + ( 2u * t >> 8));
            uint8_t  b   = (uint8_t)( 6u + ( 2u * t >> 8));
            color_t  col = RGB(r2, g, b);
            void    *dst = desktop_buf + y * s->width;
            uint32_t n   = s->width;
            __asm__ volatile ("rep stosl"
                : "+c"(n), "+D"(dst) : "a"(col) : "memory");
        }
        desktop_ready = 1;
    }

    /* Fast blit: single rep movsl, same cost as surf_clear */
    uint32_t  n   = s->height * s->width;  /* stride == width (packed) */
    void     *src = desktop_buf;
    void     *dst = s->pixels;
    __asm__ volatile ("rep movsl"
        : "+c"(n), "+S"(src), "+D"(dst) :: "memory");
}


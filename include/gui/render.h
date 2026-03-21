#pragma once
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Colour type                                                          */
/* ------------------------------------------------------------------ */

typedef uint32_t color_t;   /* 0x00RRGGBB */

#define RGB(r,g,b)  ((color_t)(((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b)))

/* ------------------------------------------------------------------ */
/* Windows 95 system colour palette                                    */
/* ------------------------------------------------------------------ */

#define COL_DESKTOP        RGB(0,   128, 128)   /* classic teal desktop    */
#define COL_WIN_FACE       RGB(192, 192, 192)   /* button / window face    */
#define COL_WIN_HILIGHT    RGB(255, 255, 255)   /* 3D highlight            */
#define COL_WIN_LIGHT      RGB(223, 223, 223)   /* 3D light                */
#define COL_WIN_SHADOW     RGB(128, 128, 128)   /* 3D shadow               */
#define COL_WIN_DKSHADOW   RGB(0,   0,   0)     /* 3D dark shadow          */
#define COL_WIN_TITLEBAR   RGB(0,   0,   128)   /* active title bar        */
#define COL_WIN_TITLEBAR_I RGB(128, 128, 128)   /* inactive title bar      */
#define COL_WIN_TITLETEXT  RGB(255, 255, 255)   /* title text              */
#define COL_WIN_TEXT       RGB(0,   0,   0)     /* client-area text        */
#define COL_BLACK          RGB(0,   0,   0)
#define COL_WHITE          RGB(255, 255, 255)
#define COL_GREY           RGB(192, 192, 192)

/* ------------------------------------------------------------------ */
/* Rectangle                                                            */
/* ------------------------------------------------------------------ */

struct rect { int32_t x, y; int32_t w, h; };

static inline struct rect make_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    struct rect r = {x, y, w, h}; return r;
}

/* ------------------------------------------------------------------ */
/* Surface — a drawable pixel buffer                                    */
/* ------------------------------------------------------------------ */

struct surface {
    uint32_t *pixels;   /* XRGB, row-major                           */
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;   /* pixels per row (>= width for alignment)   */
};

/* ------------------------------------------------------------------ */
/* Renderer lifecycle                                                   */
/* ------------------------------------------------------------------ */

/* Called once by fb_init() — takes the hardware framebuffer address.  */
void render_init(uint64_t hw_fb_addr, uint32_t width, uint32_t height,
                 uint32_t hw_pitch_bytes);

/* Blit the back buffer to hardware framebuffer.  Call from PIT.      */
void render_flush(void);

/* Pause/resume the PIT auto-flush (use in GUI mode).                 */
void render_pause_auto(int pause);

/* Always flush, regardless of pause state.                           */
void render_flush_now(void);

/* The primary screen surface (wraps the back buffer).                 */
struct surface *render_screen(void);

/* Raw pointer to the back buffer (used by framebuffer.c internally).  */
uint32_t *render_pixels(void);
uint32_t  render_width(void);
uint32_t  render_height(void);

/* ------------------------------------------------------------------ */
/* Surface primitives                                                   */
/* ------------------------------------------------------------------ */

void surf_clear(struct surface *s, color_t c);
void surf_pixel(struct surface *s, int32_t x, int32_t y, color_t c);
void surf_fill_rect(struct surface *s, struct rect r, color_t c);
void surf_outline_rect(struct surface *s, struct rect r, color_t c);
void surf_hline(struct surface *s, int32_t x, int32_t y, int32_t w, color_t c);
void surf_vline(struct surface *s, int32_t x, int32_t y, int32_t h, color_t c);
void surf_line(struct surface *s, int32_t x0, int32_t y0,
                                   int32_t x1, int32_t y1, color_t c);

/* ------------------------------------------------------------------ */
/* Text rendering on a surface                                          */
/* ------------------------------------------------------------------ */

void surf_char(struct surface *s, int32_t x, int32_t y, char c,
               color_t fg, color_t bg);
void surf_text(struct surface *s, int32_t x, int32_t y, const char *str,
               color_t fg, color_t bg);
/* Transparent background — only foreground pixels are written.        */
void surf_text_transp(struct surface *s, int32_t x, int32_t y,
                      const char *str, color_t fg);

/* ------------------------------------------------------------------ */
/* Windows 95-style 3D chrome                                          */
/* ------------------------------------------------------------------ */

/* Draw a 2-pixel raised 3D border INSIDE rect r. */
void surf_raised(struct surface *s, struct rect r);

/* Draw a 2-pixel sunken 3D border INSIDE rect r. */
void surf_sunken(struct surface *s, struct rect r);

/* Filled button with label, centred.  pressed=1 → sunken appearance. */
void surf_button(struct surface *s, struct rect r,
                 const char *label, int pressed);

/*
 * Full window frame with title bar and chrome buttons.
 * active=1 → navy title bar; active=0 → grey inactive bar.
 * The client area (inside the frame) is filled with COL_WIN_FACE.
 */
void surf_window(struct surface *s, struct rect r,
                 const char *title, int active);

/* Fill s with the classic teal desktop gradient / solid. */
void surf_desktop(struct surface *s);

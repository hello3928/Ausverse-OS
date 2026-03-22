#include <stdint.h>
#include <stddef.h>
#include "gui/wm.h"
#include "gui/render.h"
#include "drivers/mouse.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "gui/font.h"

/* ------------------------------------------------------------------ */
/* Chrome constants — must match render.c                             */
/* ------------------------------------------------------------------ */

#define TITLEBAR_H  (FONT_HEIGHT + 10)  /* must match render.c */
#define CHROME_BTN  16                  /* must match render.c */
#define TASKBAR_H   44

/* ------------------------------------------------------------------ */
/* Window table                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int         used;
    int         visible;
    struct rect rect;
    char        title[64];
    wm_paint_fn paint;
    wm_key_fn   on_key;
    void       *ud;
} wm_win_t;

static wm_win_t wins[WM_MAX_WINDOWS];
static int      zo[WM_MAX_WINDOWS]; /* zo[0]=bottom … zo[nw-1]=top, stores IDs */
static int      nw      = 0;
static int      drag_id = -1;
static int32_t  drag_ox, drag_oy;
static mouse_state_t prev_m;

/* ------------------------------------------------------------------ */
/* Geometry helpers                                                    */
/* ------------------------------------------------------------------ */

static int pt_in(int32_t x, int32_t y, struct rect r) {
    return x >= r.x && x < r.x + r.w &&
           y >= r.y && y < r.y + r.h;
}

/* Drag zone: title bar minus the chrome buttons on the right.
   Matches the title_bar rect in surf_window minus the button area. */
static struct rect tb_zone(struct rect wr) {
    int32_t btn_area = 3 * (CHROME_BTN + 2) + 4; /* 3 buttons + gaps */
    return make_rect(wr.x + 1, wr.y + 1, wr.w - 2 - btn_area, TITLEBAR_H);
}

/* Close [X] button rect — must match render.c surf_window exactly */
static struct rect close_zone(struct rect wr) {
    int32_t by = wr.y + 1 + (TITLEBAR_H - CHROME_BTN) / 2;
    return make_rect(wr.x + wr.w - 3 - CHROME_BTN, by,
                     CHROME_BTN, CHROME_BTN);
}

/* Client area — must match the 'client' rect in surf_window exactly */
static struct rect client_zone(struct rect wr) {
    return make_rect(wr.x + 1,
                     wr.y + 2 + TITLEBAR_H,
                     wr.w - 2,
                     wr.h - 3 - TITLEBAR_H);
}

static int32_t taskbar_y(void) {
    return (int32_t)render_height() - TASKBAR_H;
}

static struct rect start_rect(void) {
    return make_rect(8, taskbar_y() + 7, 72, TASKBAR_H - 14);
}

/* ------------------------------------------------------------------ */
/* String helpers                                                      */
/* ------------------------------------------------------------------ */

static size_t wm_slen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static void wm_scpy(char *d, const char *s, size_t max) {
    size_t i = 0;
    while (i + 1 < max && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void fmt2(char *buf, uint32_t n) {
    buf[0] = '0' + (char)((n / 10) % 10);
    buf[1] = '0' + (char)(n % 10);
}

/* Cursor is composited at flush time by render_flush_with_cursor().
   It is never drawn into the scene buffer. */

/* ------------------------------------------------------------------ */
/* Taskbar                                                             */
/* ------------------------------------------------------------------ */

static void draw_taskbar(struct surface *s) {
    int32_t ty = taskbar_y();
    int32_t sw = (int32_t)s->width;
    int32_t cy = ty + (TASKBAR_H - (int32_t)FONT_HEIGHT) / 2;

    /* Background */
    surf_fill_rect(s, make_rect(0, ty, sw, TASKBAR_H), COL_TASKBAR_BG);

    /* Top border */
    surf_hline(s, 0, ty, sw, COL_TASKBAR_BORDER);

    /* Start button — flat, accent blue */
    struct rect sb = start_rect();
    surf_fill_rect(s, sb, COL_START_BG);
    {
        int32_t lw = (int32_t)(5 * FONT_WIDTH); /* "Start" */
        int32_t lx = sb.x + (sb.w - lw) / 2;
        int32_t ly = sb.y + (sb.h - (int32_t)FONT_HEIGHT) / 2;
        surf_text(s, lx, ly, "Start", COL_WHITE, COL_START_BG);
    }

    /* Uptime clock — right-aligned */
    uint64_t secs = pit_ticks() / PIT_HZ;
    uint32_t hh   = (uint32_t)(secs / 3600);
    uint32_t mm   = (uint32_t)((secs / 60) % 60);
    uint32_t ss   = (uint32_t)(secs % 60);
    char clk[9];
    fmt2(clk + 0, hh); clk[2] = ':';
    fmt2(clk + 3, mm); clk[5] = ':';
    fmt2(clk + 6, ss); clk[8] = '\0';

    int32_t cw = (int32_t)(wm_slen(clk) * FONT_WIDTH);
    int32_t cx = sw - cw - 12;
    surf_text(s, cx, cy, clk, COL_TASKBAR_TEXT, COL_TASKBAR_BG);
}

/* ------------------------------------------------------------------ */
/* Render                                                              */
/* ------------------------------------------------------------------ */

static void wm_render(void) {
    struct surface *s = render_screen();

    surf_desktop(s);

    for (int i = 0; i < nw; i++) {
        wm_win_t *w = &wins[zo[i]];
        if (!w->visible) continue;
        int active = (i == nw - 1);

        /* Draw chrome (fills client area with COL_WIN_FACE) */
        surf_window(s, w->rect, w->title, active);

        /* Build a sub-surface that points directly into the screen buffer
         * at the client area origin.  stride = screen stride so every row
         * address is correct.  No heap allocation required. */
        if (w->paint) {
            struct rect cl = client_zone(w->rect);
            int32_t cx = cl.x, cy = cl.y;
            int32_t cw = cl.w, ch = cl.h;
            /* Clip to screen — bail if origin is off the left/top edge */
            if (cx < 0 || cy < 0) continue;
            if (cx >= (int32_t)s->width || cy >= (int32_t)s->height) continue;
            if (cx + cw > (int32_t)s->width)  cw = (int32_t)s->width  - cx;
            if (cy + ch > (int32_t)s->height) ch = (int32_t)s->height - cy;
            if (cw <= 0 || ch <= 0) continue;

            struct surface cs;
            cs.pixels = s->pixels + (uint32_t)cy * s->stride + (uint32_t)cx;
            cs.width  = (uint32_t)cw;
            cs.height = (uint32_t)ch;
            cs.stride = s->stride;   /* screen row width — keeps rows aligned */
            w->paint(&cs, w->ud);
        }
    }

    draw_taskbar(s);
    /* Cursor is added by render_flush_with_cursor — not part of scene. */
}

/* ------------------------------------------------------------------ */
/* Z-order management                                                  */
/* ------------------------------------------------------------------ */

void wm_raise(int id) {
    int pos = -1;
    for (int i = 0; i < nw; i++) if (zo[i] == id) { pos = i; break; }
    if (pos < 0) return;
    for (int i = pos; i < nw - 1; i++) zo[i] = zo[i + 1];
    zo[nw - 1] = id;
}

/* ------------------------------------------------------------------ */
/* Event handling                                                      */
/* ------------------------------------------------------------------ */

static int wm_dirty    = 1; /* scene changed — full repaint needed  */
static int cursor_dirty = 1; /* only cursor moved — re-flush is enough */

static void handle_mouse(void) {
    mouse_state_t m  = mouse_get();
    uint8_t pressed  =  m.buttons & ~prev_m.buttons;
    uint8_t released = ~m.buttons &  prev_m.buttons;

    /* Pure cursor movement: only need a re-flush, not a scene repaint */
    if (m.x != prev_m.x || m.y != prev_m.y)
        cursor_dirty = 1;

    /* Button state change always triggers a scene repaint */
    if (m.buttons != prev_m.buttons)
        wm_dirty = 1;

    if (drag_id >= 0) {
        wins[drag_id].rect.x = m.x - drag_ox;
        wins[drag_id].rect.y = m.y - drag_oy;
        if (wins[drag_id].rect.y < 0)               wins[drag_id].rect.y = 0;
        if (wins[drag_id].rect.y > taskbar_y() - 4) wins[drag_id].rect.y = taskbar_y() - 4;
        if (released & 1) drag_id = -1;
        wm_dirty = 1;
    } else if (pressed & 1) {
        for (int i = nw - 1; i >= 0; i--) {
            int id = zo[i];
            wm_win_t *w = &wins[id];
            if (!w->visible || !pt_in(m.x, m.y, w->rect)) continue;

            if (pt_in(m.x, m.y, close_zone(w->rect))) {
                wm_close(id);
                wm_dirty = 1;
                break;
            }

            wm_raise(id);
            wm_dirty = 1;

            if (pt_in(m.x, m.y, tb_zone(w->rect))) {
                drag_id = id;
                drag_ox = m.x - w->rect.x;
                drag_oy = m.y - w->rect.y;
            }
            break;
        }
    }

    prev_m = m;
}

static void handle_key(void) {
    char c = keyboard_getchar();
    if (!c || nw == 0) return;
    int id = zo[nw - 1];
    if (wins[id].on_key) { wins[id].on_key(c, wins[id].ud); wm_dirty = 1; }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void wm_init(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) wins[i].used = 0;
    nw = drag_id = 0;
    drag_id = -1;
    prev_m.x = (int32_t)(render_width()  / 2);
    prev_m.y = (int32_t)(render_height() / 2);
    prev_m.buttons = 0;
}

int wm_open(const char *title,
            int32_t x, int32_t y, int32_t w, int32_t h,
            wm_paint_fn paint, wm_key_fn on_key, void *ud) {
    if (nw >= WM_MAX_WINDOWS) return -1;
    int id = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!wins[i].used) { id = i; break; }
    }
    if (id < 0) return -1;

    wins[id].used    = 1;
    wins[id].visible = 1;
    wins[id].rect    = make_rect(x, y, w, h);
    wins[id].paint   = paint;
    wins[id].on_key  = on_key;
    wins[id].ud      = ud;
    wm_scpy(wins[id].title, title, 64);

    zo[nw++] = id;
    return id;
}

void wm_close(int id) {
    if (id < 0 || id >= WM_MAX_WINDOWS || !wins[id].used) return;
    wins[id].used = wins[id].visible = 0;
    for (int i = 0; i < nw; i++) {
        if (zo[i] == id) {
            for (int j = i; j < nw - 1; j++) zo[j] = zo[j + 1];
            nw--;
            break;
        }
    }
    if (drag_id == id) drag_id = -1;
}

void wm_run(void) {
    render_pause_auto(1);   /* stop PIT from flushing mid-frame */
    uint64_t last_sec = pit_ticks() / PIT_HZ;
    wm_dirty    = 1;
    cursor_dirty = 1;

    while (nw > 0) {
        __asm__ volatile ("hlt");   /* sleep until next IRQ (mouse/key/timer) */
        handle_mouse();
        handle_key();

        /* Force scene redraw once per second so the taskbar clock updates */
        uint64_t now_sec = pit_ticks() / PIT_HZ;
        if (now_sec != last_sec) { last_sec = now_sec; wm_dirty = 1; }

        mouse_state_t m = mouse_get();

        if (wm_dirty) {
            /* Scene changed: repaint everything, then flush with cursor */
            wm_dirty    = 0;
            cursor_dirty = 0;
            wm_render();
            render_flush_with_cursor(m.x, m.y);
        } else if (cursor_dirty) {
            /* Only cursor moved: re-flush the unchanged scene with cursor
             * at its new position.  No scene repaint — O(1) cost. */
            cursor_dirty = 0;
            render_flush_with_cursor(m.x, m.y);
        }
    }

    render_pause_auto(0);   /* restore PIT auto-flush for shell */
}

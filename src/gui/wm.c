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

#define TITLEBAR_H  (FONT_HEIGHT + 4)
#define CHROME_BTN  (TITLEBAR_H - 2)
#define TASKBAR_H   32

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

/* Drag zone: title bar minus the chrome buttons on the right */
static struct rect tb_zone(struct rect wr) {
    int32_t btn_w = 3 * (CHROME_BTN + 1);
    return make_rect(wr.x + 2, wr.y + 2, wr.w - 4 - btn_w, TITLEBAR_H);
}

/* Close [X] button rect — must match render.c */
static struct rect close_zone(struct rect wr) {
    return make_rect(wr.x + wr.w - 2 - CHROME_BTN, wr.y + 2,
                     CHROME_BTN, CHROME_BTN);
}

/* Content area inside the sunken inner border */
static struct rect client_zone(struct rect wr) {
    return make_rect(wr.x + 3,
                     wr.y + 4 + TITLEBAR_H,
                     wr.w - 6,
                     wr.h - 7 - TITLEBAR_H);
}

static int32_t taskbar_y(void) {
    return (int32_t)render_height() - TASKBAR_H;
}

static struct rect start_rect(void) {
    return make_rect(4, taskbar_y() + 3, 72, TASKBAR_H - 6);
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

/* ------------------------------------------------------------------ */
/* Mouse cursor                                                        */
/* ------------------------------------------------------------------ */

static void draw_cursor(struct surface *s, int32_t x, int32_t y) {
    /* Right triangle: top-left hot spot, 16px wide at top, tapering right */
    for (int32_t r = 0; r < 16; r++) {
        for (int32_t c = 0; c <= 15 - r; c++) {
            int32_t px = x + c, py = y + r;
            if (px < 0 || py < 0 ||
                (uint32_t)px >= s->width ||
                (uint32_t)py >= s->height) continue;
            /* Black border on edges, white fill inside */
            int edge = (c == 0) || (r == 0) || (c + r == 15);
            s->pixels[(uint32_t)py * s->stride + (uint32_t)px] =
                edge ? COL_BLACK : COL_WHITE;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Taskbar                                                             */
/* ------------------------------------------------------------------ */

static void draw_taskbar(struct surface *s) {
    int32_t ty = taskbar_y();
    int32_t sw = (int32_t)s->width;

    surf_fill_rect(s, make_rect(0, ty, sw, TASKBAR_H), COL_WIN_FACE);
    surf_raised(s,    make_rect(0, ty, sw, TASKBAR_H));

    surf_button(s, start_rect(), "Start", 0);

    /* Uptime clock */
    uint64_t secs = pit_ticks() / PIT_HZ;
    uint32_t hh   = (uint32_t)(secs / 3600);
    uint32_t mm   = (uint32_t)((secs / 60) % 60);
    uint32_t ss   = (uint32_t)(secs % 60);
    char clk[9]; /* "HH:MM:SS\0" */
    fmt2(clk + 0, hh); clk[2] = ':';
    fmt2(clk + 3, mm); clk[5] = ':';
    fmt2(clk + 6, ss); clk[8] = '\0';

    int32_t cw = (int32_t)(wm_slen(clk) * FONT_WIDTH);
    int32_t cx = sw - cw - 8;
    int32_t cy = ty + (TASKBAR_H - (int32_t)FONT_HEIGHT) / 2;
    surf_text(s, cx, cy, clk, COL_WIN_TEXT, COL_WIN_FACE);
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
        surf_window(s, w->rect, w->title, active);
        if (w->paint)
            w->paint(s, client_zone(w->rect), w->ud);
    }

    draw_taskbar(s);

    mouse_state_t m = mouse_get();
    draw_cursor(s, m.x, m.y);
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

static void handle_mouse(void) {
    mouse_state_t m  = mouse_get();
    uint8_t pressed  =  m.buttons & ~prev_m.buttons;
    uint8_t released = ~m.buttons &  prev_m.buttons;

    if (drag_id >= 0) {
        wins[drag_id].rect.x = m.x - drag_ox;
        wins[drag_id].rect.y = m.y - drag_oy;
        /* Keep title bar on screen */
        if (wins[drag_id].rect.y < 0)              wins[drag_id].rect.y = 0;
        if (wins[drag_id].rect.y > taskbar_y() - 4) wins[drag_id].rect.y = taskbar_y() - 4;
        if (released & 1) drag_id = -1;
    } else if (pressed & 1) {
        /* Hit test from topmost window down */
        for (int i = nw - 1; i >= 0; i--) {
            int id = zo[i];
            wm_win_t *w = &wins[id];
            if (!w->visible || !pt_in(m.x, m.y, w->rect)) continue;

            if (pt_in(m.x, m.y, close_zone(w->rect))) {
                wm_close(id);
                break;
            }

            wm_raise(id);

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
    if (wins[id].on_key) wins[id].on_key(c, wins[id].ud);
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
    uint64_t last_tick = 0;

    while (nw > 0) {
        __asm__ volatile ("hlt");   /* wait for next IRQ (mouse/keyboard/timer) */
        handle_mouse();
        handle_key();

        uint64_t now = pit_ticks();
        if (now - last_tick >= 2) {     /* ~50 fps */
            last_tick = now;
            wm_render();
            render_flush_now();         /* flush complete frame atomically */
        }
    }

    render_pause_auto(0);   /* restore PIT auto-flush for shell */
}

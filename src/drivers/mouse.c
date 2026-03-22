#include <stdint.h>
#include "drivers/mouse.h"
#include "arch/io.h"
#include "arch/idt.h"
#include "drivers/pic.h"
#include "gui/render.h"
#include "drivers/usb_tablet.h"

/* ------------------------------------------------------------------ */
/* 8042 PS/2 controller helpers                                        */
/* ------------------------------------------------------------------ */

static void ps2_wait_write(void) {
    uint32_t t = 100000;
    while (t-- && (inb(0x64) & 2));
}

static void ps2_wait_read(void) {
    uint32_t t = 100000;
    while (t-- && !(inb(0x64) & 1));
}

/* Drain any stale bytes sitting in the 8042 output buffer */
static void ps2_flush(void) {
    uint32_t t = 1000;
    while (t-- && (inb(0x64) & 1)) (void)inb(0x60);
}

static void mouse_send(uint8_t data) {
    ps2_wait_write(); outb(0x64, 0xD4);   /* route next byte to mouse */
    ps2_wait_write(); outb(0x60, data);
}

static uint8_t mouse_recv(void) {
    ps2_wait_read();
    return inb(0x60);
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static mouse_state_t state;
static uint8_t       pkt[3];
static uint8_t       pkt_idx = 0;

/* ------------------------------------------------------------------ */
/* IRQ12 handler — called once per byte received from mouse            */
/* ------------------------------------------------------------------ */

static void mouse_irq(void) {
    /* If a USB tablet is driving the cursor, drain the PS/2 byte but skip
       processing so the PS/2 relative-position state doesn't interfere. */
    uint8_t status = inb(0x64);
    if (!(status & 0x20)) return;  /* bit 5 clear = keyboard data, not mouse */
    uint8_t data = inb(0x60);
    if (usb_tablet_present()) return;  /* USB tablet takes precedence */

    if (pkt_idx == 0) {
        /* First byte must have bit 3 set, otherwise it's garbage */
        if (!(data & 0x08)) return;
    }

    pkt[pkt_idx++] = data;
    if (pkt_idx < 3) return;
    pkt_idx = 0;

    uint8_t flags = pkt[0];

    /* Ignore overflow packets */
    if (flags & 0xC0) return;

    /* Sign-extend 9-bit deltas */
    int32_t dx = (int32_t)(uint32_t)pkt[1];
    int32_t dy = (int32_t)(uint32_t)pkt[2];
    if (flags & 0x10) dx |= ~0xFF;   /* X negative */
    if (flags & 0x20) dy |= ~0xFF;   /* Y negative */

    int32_t sw = (int32_t)render_width();
    int32_t sh = (int32_t)render_height();

    state.x += dx;
    state.y -= dy;

    state.dx =  dx;
    state.dy = -dy;

    if (state.x < 0)       state.x = 0;
    if (state.y < 0)       state.y = 0;
    if (state.x >= sw)     state.x = sw - 1;
    if (state.y >= sh)     state.y = sh - 1;

    state.buttons = flags & 0x07;
}

/* ------------------------------------------------------------------ */
/* mouse_init                                                          */
/* ------------------------------------------------------------------ */

void mouse_init(void) {
    /* Centre cursor on screen */
    state.x = (int32_t)(render_width()  / 2);
    state.y = (int32_t)(render_height() / 2);
    state.dx = state.dy = 0;
    state.buttons = 0;
    pkt_idx = 0;

    /* Flush any stale output-buffer data before we start */
    ps2_flush();

    /* Enable auxiliary device */
    ps2_wait_write();
    outb(0x64, 0xA8);
    ps2_flush();   /* some controllers ACK this command */

    /* Enable IRQ12 in the 8042 command byte */
    ps2_wait_write(); outb(0x64, 0x20);   /* read command byte */
    uint8_t cmd = mouse_recv();
    cmd |=  0x02;   /* bit 1: enable auxiliary interrupt (IRQ12) */
    cmd &= ~0x20;   /* bit 5: disable mouse clock — clear it      */
    ps2_wait_write(); outb(0x64, 0x60);   /* write command byte */
    ps2_wait_write(); outb(0x60, cmd);

    /* Reset mouse to defaults */
    mouse_send(0xF6); mouse_recv();   /* set defaults, ACK */

    /* Enable streaming */
    mouse_send(0xF4); mouse_recv();   /* enable, ACK */

    /* Hook IRQ and unmask — IRQ2 is the master PIC cascade to slave */
    irq_register(12, mouse_irq);
    pic_unmask(2);    /* enable cascade line so slave IRQs reach the CPU */
    pic_unmask(12);
}

/* ------------------------------------------------------------------ */
/* mouse_get                                                           */
/* ------------------------------------------------------------------ */

mouse_state_t mouse_get(void) {
    if (usb_tablet_present()) {
        int32_t ux, uy;
        uint8_t ubtn;
        usb_tablet_get(&ux, &uy, &ubtn);
        state.dx      = ux - state.x;
        state.dy      = uy - state.y;
        state.x       = ux;
        state.y       = uy;
        state.buttons = ubtn;
    }
    return state;
}

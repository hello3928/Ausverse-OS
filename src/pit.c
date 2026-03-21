#include <stdint.h>
#include "../include/pit.h"
#include "../include/io.h"

/*
 * 8253/8254 Programmable Interval Timer
 *
 * Channel 0 is connected to IRQ0. We program it in mode 3 (square wave
 * generator) with a divisor that produces PIT_HZ interrupts per second.
 *
 *   Base frequency: 1,193,182 Hz
 *   Divisor:        1,193,182 / PIT_HZ
 */

#define PIT_BASE_FREQ   1193182
#define PIT_CHANNEL0    0x40
#define PIT_CMD         0x43

/* Command: channel 0 | lo/hi byte access | mode 3 | binary */
#define PIT_CMD_BYTE    0x36

static volatile uint64_t tick_count = 0;

void pit_init(void) {
    uint16_t divisor = PIT_BASE_FREQ / PIT_HZ;
    outb(PIT_CMD,      PIT_CMD_BYTE);
    outb(PIT_CHANNEL0, divisor & 0xFF);         /* low byte  */
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);  /* high byte */
}

/* Called from the IRQ0 handler in idt.c */
void pit_tick(void) {
    tick_count++;
}

uint64_t pit_ticks(void) {
    return tick_count;
}

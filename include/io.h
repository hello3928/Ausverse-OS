#pragma once
#include <stdint.h>

/* Write a byte to a hardware port */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Read a byte from a hardware port */
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* Short delay — needed between PIC initialisation commands on real hardware */
static inline void io_wait(void) {
    outb(0x80, 0);
}

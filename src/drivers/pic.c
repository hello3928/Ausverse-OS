#include "drivers/pic.h"
#include "arch/io.h"

/*
 * 8259A Programmable Interrupt Controller
 *
 * There are two cascaded PICs: master (IRQs 0-7) and slave (IRQs 8-15).
 *
 * By default the master maps IRQs 0-7 to interrupt vectors 8-15, which
 * clashes with CPU exception vectors. We remap:
 *   Master IRQs 0-7  -> vectors 32-39
 *   Slave  IRQs 8-15 -> vectors 40-47
 */

#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1
#define PIC_EOI     0x20    /* End-of-interrupt command */

/* Initialisation command words */
#define ICW1_INIT   0x10
#define ICW1_ICW4   0x01
#define ICW4_8086   0x01

void pic_init(void) {
    /* Start initialisation sequence (cascade mode) */
    outb(PIC1_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, 0x20); io_wait();   /* master: IRQ0 -> vector 32 */
    outb(PIC2_DATA, 0x28); io_wait();   /* slave:  IRQ8 -> vector 40 */

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04); io_wait();   /* master: slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();   /* slave: cascade identity = 2 */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask all IRQs — drivers unmask only what they need */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/*
 * Send End-of-Interrupt signal so the PIC knows the handler is done
 * and will deliver the next interrupt on that line.
 */
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);    /* notify slave first for IRQs 8-15 */
    outb(PIC1_CMD, PIC_EOI);
}

void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) & ~(1 << irq));
}

void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (1 << irq));
}

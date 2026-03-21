#include <stdint.h>
#include "../include/idt.h"
#include "../include/vga.h"

/* One IDT gate descriptor is 8 bytes */
struct idt_entry {
    uint16_t offset_low;    /* handler address bits 0-15  */
    uint16_t selector;      /* code segment selector      */
    uint8_t  zero;          /* always 0                   */
    uint8_t  type_attr;     /* gate type + DPL + present  */
    uint16_t offset_high;   /* handler address bits 16-31 */
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

#define IDT_ENTRIES 32

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;

/* ISR stubs declared in isr.asm */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

static void (*isr_stubs[IDT_ENTRIES])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

static const char *exception_names[IDT_ENTRIES] = {
    "Division by Zero",         "Debug",
    "Non-Maskable Interrupt",   "Breakpoint",
    "Overflow",                 "Bound Range Exceeded",
    "Invalid Opcode",           "Device Not Available",
    "Double Fault",             "Coprocessor Segment Overrun",
    "Invalid TSS",              "Segment Not Present",
    "Stack Segment Fault",      "General Protection Fault",
    "Page Fault",               "Reserved",
    "x87 FPU Error",            "Alignment Check",
    "Machine Check",            "SIMD FP Exception",
    "Virtualisation Exception", "Reserved",
    "Reserved",                 "Reserved",
    "Reserved",                 "Reserved",
    "Reserved",                 "Reserved",
    "Reserved",                 "Reserved",
    "Security Exception",       "Reserved",
};

static void idt_set(int i, void (*handler)(void)) {
    uint32_t addr = (uint32_t)handler;
    idt[i].offset_low  = addr & 0xFFFF;
    idt[i].offset_high = (addr >> 16) & 0xFFFF;
    idt[i].selector    = 0x08;   /* kernel code segment */
    idt[i].zero        = 0;
    idt[i].type_attr   = 0x8E;  /* present, ring 0, 32-bit interrupt gate */
}

/* Called from isr_common in isr.asm */
void isr_handler(struct int_frame *f) {
    vga_print("\n--- EXCEPTION ---\n");
    if (f->int_no < IDT_ENTRIES)
        vga_print(exception_names[f->int_no]);
    vga_print("\nSystem halted.\n");
    for (;;)
        __asm__ volatile ("cli; hlt");
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set(i, isr_stubs[i]);

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}

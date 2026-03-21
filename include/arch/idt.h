#pragma once
#include <stdint.h>

/* CPU state snapshot passed to every exception handler */
struct int_frame {
    /* pushed by isr_common (in reverse push order) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* pushed by our ISR stub */
    uint64_t int_no, err_code;
    /* pushed automatically by the CPU on exception */
    uint64_t rip, cs, rflags, rsp, ss;
};

void idt_init(void);
void irq_register(uint8_t irq, void (*handler)(void));

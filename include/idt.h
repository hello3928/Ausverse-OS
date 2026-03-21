#pragma once
#include <stdint.h>

/* CPU state snapshot passed to every exception handler */
struct int_frame {
    /* pushed by isr_common: segment regs */
    uint32_t gs, fs, es, ds;
    /* pushed by pusha */
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    /* pushed by our ISR stub */
    uint32_t int_no, err_code;
    /* pushed automatically by the CPU */
    uint32_t eip, cs, eflags;
};

void idt_init(void);

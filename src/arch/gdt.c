#include <stdint.h>
#include "arch/gdt.h"

/* One GDT entry is 8 bytes */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit;
    uint8_t  base_high;
} __attribute__((packed));

/*
 * In 64-bit mode lgdt expects a 10-byte operand: 2-byte limit + 8-byte base.
 */
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr   gdtp;

/* Defined in gdt_flush.asm */
extern void gdt_flush(struct gdt_ptr *gdtp);

static void gdt_set(int i, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t flags)
{
    gdt[i].base_low   = base & 0xFFFF;
    gdt[i].base_mid   = (base >> 16) & 0xFF;
    gdt[i].base_high  = (base >> 24) & 0xFF;
    gdt[i].limit_low  = limit & 0xFFFF;
    gdt[i].flags_limit = ((limit >> 16) & 0x0F) | (flags & 0xF0);
    gdt[i].access = access;
}

void gdt_init(void) {
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint64_t)&gdt;

    /*
     * Code segment flags (0xAF):
     *   G=1 (granularity), L=1 (64-bit), D=0 (must be 0 in 64-bit), AVL=0
     *   flags nibble = 1010 = 0xA
     *
     * Data segment flags (0xCF):
     *   G=1, B=1, L=0 — segment registers are mostly ignored in 64-bit
     *   but SS still needs a valid writable descriptor.
     */
    gdt_set(0, 0, 0x00000000, 0x00, 0x00); /* null */
    gdt_set(1, 0, 0xFFFFFFFF, 0x9A, 0xAF); /* kernel code: 64-bit, ring 0 */
    gdt_set(2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* kernel data: ring 0 */

    gdt_flush(&gdtp);
}

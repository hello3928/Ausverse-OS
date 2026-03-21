#include <stdint.h>
#include "../include/gdt.h"

/* One GDT entry is 8 bytes */
struct gdt_entry {
    uint16_t limit_low;     /* bits 0-15 of segment limit */
    uint16_t base_low;      /* bits 0-15 of base address  */
    uint8_t  base_mid;      /* bits 16-23 of base address */
    uint8_t  access;        /* present, DPL, type flags   */
    uint8_t  flags_limit;   /* high 4 bits: flags, low 4: limit bits 16-19 */
    uint8_t  base_high;     /* bits 24-31 of base address */
} __attribute__((packed));

/* Pointer passed to lgdt */
struct gdt_ptr {
    uint16_t limit;         /* size of GDT in bytes - 1 */
    uint32_t base;          /* linear address of GDT    */
} __attribute__((packed));

/* 3 entries: null, kernel code, kernel data */
static struct gdt_entry gdt[3];
static struct gdt_ptr   gdtp;

/* Defined in gdt_flush.asm — loads the GDT and reloads segment registers */
extern void gdt_flush(uint32_t gdtp_addr);

static void gdt_set(int i, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t flags)
{
    gdt[i].base_low   = base & 0xFFFF;
    gdt[i].base_mid   = (base >> 16) & 0xFF;
    gdt[i].base_high  = (base >> 24) & 0xFF;

    gdt[i].limit_low  = limit & 0xFFFF;
    /* top 4 bits of limit merged with 4-bit flags nibble */
    gdt[i].flags_limit = ((limit >> 16) & 0x0F) | (flags & 0xF0);

    gdt[i].access = access;
}

void gdt_init(void) {
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint32_t)&gdt;

    /*
     * access byte breakdown (code segment 0x9A):
     *   bit 7   present       = 1
     *   bits 6-5 DPL (ring)   = 00  (ring 0)
     *   bit 4   descriptor type = 1 (code/data, not system)
     *   bit 3   executable    = 1   (code segment)
     *   bit 2   direction/conforming = 0
     *   bit 1   readable      = 1
     *   bit 0   accessed      = 0
     *
     * access byte (data segment 0x92): same but bit 3 = 0 (not executable)
     *
     * flags nibble 0xC (upper 4 bits of flags_limit byte):
     *   bit 7 (G)  granularity = 1  (limit in 4 KiB pages)
     *   bit 6 (D/B) size       = 1  (32-bit segment)
     *   bit 5 (L)  long mode   = 0
     *   bit 4      AVL         = 0
     */

    gdt_set(0, 0, 0x00000000, 0x00, 0x00); /* null descriptor */
    gdt_set(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); /* kernel code: ring 0, 32-bit, flat */
    gdt_set(2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* kernel data: ring 0, 32-bit, flat */

    gdt_flush((uint32_t)&gdtp);
}

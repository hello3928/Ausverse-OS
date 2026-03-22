#include "drivers/pci.h"
#include "arch/io.h"

#define CFG_ADDR 0xCF8u
#define CFG_DATA 0xCFCu

static uint32_t make_addr(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off) {
    return (1u << 31) | ((uint32_t)bus  << 16) | ((uint32_t)slot << 11) |
                        ((uint32_t)fn   <<  8) | (off & 0xFC);
}

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off) {
    outl(CFG_ADDR, make_addr(bus, slot, fn, off));
    return inl(CFG_DATA);
}

void pci_write(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off, uint32_t v) {
    outl(CFG_ADDR, make_addr(bus, slot, fn, off));
    outl(CFG_DATA, v);
}

int pci_find(uint16_t vendor, uint16_t device,
             uint8_t *bus_out, uint8_t *slot_out, uint8_t *fn_out) {
    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t s = 0; s < 32; s++) {
            uint32_t id = pci_read((uint8_t)b, s, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;   /* no device */
            for (uint8_t f = 0; f < 8; f++) {
                id = pci_read((uint8_t)b, s, f, 0x00);
                if ((id & 0xFFFF) == vendor && (id >> 16) == device) {
                    *bus_out  = (uint8_t)b;
                    *slot_out = s;
                    *fn_out   = f;
                    return 0;
                }
            }
        }
    }
    return -1;
}

void pci_enable_busmaster(uint8_t bus, uint8_t slot, uint8_t fn) {
    uint32_t cmd = pci_read(bus, slot, fn, 0x04);
    cmd |= (1 << 2) | (1 << 1) | (1 << 0);  /* bus-master + mem + I/O */
    pci_write(bus, slot, fn, 0x04, cmd);
}

uint32_t pci_bar(uint8_t bus, uint8_t slot, uint8_t fn, int n) {
    return pci_read(bus, slot, fn, (uint8_t)(0x10 + n * 4));
}

uint8_t pci_irq(uint8_t bus, uint8_t slot, uint8_t fn) {
    return (uint8_t)(pci_read(bus, slot, fn, 0x3C) & 0xFF);
}

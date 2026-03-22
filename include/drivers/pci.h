#pragma once
#include <stdint.h>

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off);
void     pci_write(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off, uint32_t v);
int      pci_find(uint16_t vendor, uint16_t device,
                  uint8_t *bus_out, uint8_t *slot_out, uint8_t *fn_out);
void     pci_enable_busmaster(uint8_t bus, uint8_t slot, uint8_t fn);
uint32_t pci_bar(uint8_t bus, uint8_t slot, uint8_t fn, int n);
uint8_t  pci_irq(uint8_t bus, uint8_t slot, uint8_t fn);

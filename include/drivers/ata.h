#pragma once
#include <stdint.h>

int      ata_init(void);        /* detect drive; returns 0 on success */
int      ata_read(uint32_t lba, uint8_t count, void *buf);
int      ata_write(uint32_t lba, uint8_t count, const void *buf);
uint32_t ata_total_sectors(void);

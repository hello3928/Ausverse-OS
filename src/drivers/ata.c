#include <stdint.h>
#include "arch/io.h"
#include "drivers/ata.h"

/* ATA primary channel ports */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECT_COUNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7
#define ATA_CONTROL     0x3F6

/* Status bits */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Commands */
#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH   0xE7
#define ATA_CMD_IDENTIFY      0xEC

static uint32_t total_sects = 0;

/* Poll until BSY clears. Returns final status byte, or 0xFF on timeout. */
static uint8_t ata_wait_ready(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (!(st & ATA_SR_BSY)) return st;
    }
    return 0xFF;
}

/* Short 400ns delay by reading the alternate status register 4 times */
static void ata_delay400(void) {
    inb(ATA_CONTROL);
    inb(ATA_CONTROL);
    inb(ATA_CONTROL);
    inb(ATA_CONTROL);
}

int ata_init(void) {
    /* Select master drive */
    outb(ATA_DRIVE_HEAD, 0xE0);
    ata_delay400();

    /* Check if drive present */
    uint8_t st = inb(ATA_STATUS);
    if (st == 0xFF) return -1;  /* floating bus - no drive */

    /* Wait for BSY to clear */
    uint8_t status = ata_wait_ready();
    if (status == 0xFF) return -1;

    /* Send IDENTIFY command */
    outb(ATA_SECT_COUNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    ata_delay400();

    /* Check if drive responded */
    st = inb(ATA_STATUS);
    if (st == 0x00) return -1;  /* no drive */

    /* Wait for BSY to clear */
    status = ata_wait_ready();
    if (status == 0xFF) return -1;

    /* Check for ATAPI (LBA_MID and LBA_HI non-zero) */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) return -1;

    /* Wait for DRQ */
    for (int i = 0; i < 100000; i++) {
        st = inb(ATA_STATUS);
        if (st & ATA_SR_ERR) return -1;
        if (st & ATA_SR_DRQ) break;
    }

    /* Read 256 words of IDENTIFY data */
    uint16_t identify[256];
    for (int i = 0; i < 256; i++) {
        identify[i] = inw(ATA_DATA);
    }

    /* Words 60-61: total LBA28 sectors */
    total_sects = ((uint32_t)identify[61] << 16) | (uint32_t)identify[60];

    return 0;
}

int ata_read(uint32_t lba, uint8_t count, void *buf) {
    if (count == 0) return 0;

    uint8_t status = ata_wait_ready();
    if (status == 0xFF) return -1;

    /* LBA28 setup */
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_ERROR,      0x00);
    outb(ATA_SECT_COUNT, count);
    outb(ATA_LBA_LO,     (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID,    (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,     (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND,    ATA_CMD_READ_SECTORS);

    uint16_t *p = (uint16_t *)buf;

    for (int s = 0; s < (int)count; s++) {
        ata_delay400();

        /* Wait for DRQ */
        for (int i = 0; i < 100000; i++) {
            uint8_t st = inb(ATA_STATUS);
            if (st & ATA_SR_ERR) return -1;
            if (st & ATA_SR_DRQ) break;
            if (i == 99999) return -1;
        }

        /* Read 256 words */
        for (int i = 0; i < 256; i++) {
            *p++ = inw(ATA_DATA);
        }
    }

    return 0;
}

int ata_write(uint32_t lba, uint8_t count, const void *buf) {
    if (count == 0) return 0;

    uint8_t status = ata_wait_ready();
    if (status == 0xFF) return -1;

    /* LBA28 setup */
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_ERROR,      0x00);
    outb(ATA_SECT_COUNT, count);
    outb(ATA_LBA_LO,     (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID,    (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,     (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND,    ATA_CMD_WRITE_SECTORS);

    const uint16_t *p = (const uint16_t *)buf;

    for (int s = 0; s < (int)count; s++) {
        ata_delay400();

        /* Wait for DRQ */
        for (int i = 0; i < 100000; i++) {
            uint8_t st = inb(ATA_STATUS);
            if (st & ATA_SR_ERR) return -1;
            if (st & ATA_SR_DRQ) break;
            if (i == 99999) return -1;
        }

        /* Write 256 words */
        for (int i = 0; i < 256; i++) {
            outw(ATA_DATA, *p++);
        }
    }

    /* Flush cache */
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_ready();

    return 0;
}

uint32_t ata_total_sectors(void) {
    return total_sects;
}

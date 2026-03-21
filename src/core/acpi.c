#include <stdint.h>
#include <stddef.h>
#include "core/acpi.h"
#include "arch/io.h"

/* ------------------------------------------------------------------ */
/* ACPI table structures                                               */
/* ------------------------------------------------------------------ */

struct rsdp {
    char    sig[8];         /* "RSD PTR " */
    uint8_t checksum;
    char    oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
} __attribute__((packed));

struct acpi_header {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table[8];
    uint32_t oem_rev;
    uint32_t creator_id;
    uint32_t creator_rev;
} __attribute__((packed));

/*
 * Only the fields we need from the FADT (Fixed ACPI Description Table).
 * Offsets verified against the ACPI 1.0 specification:
 *   hdr             0–35  (36 bytes)
 *   firmware_ctrl   36    (4 bytes)
 *   dsdt            40    (4 bytes)
 *   res1            44    (1 byte)
 *   preferred_pm    45    (1 byte)
 *   sci_int         46    (2 bytes)
 *   smi_cmd         48    (4 bytes)
 *   acpi_enable     52    (1 byte)
 *   acpi_disable    53    (1 byte)
 *   s4bios_req      54    (1 byte)
 *   pstate_cnt      55    (1 byte)
 *   pm1a_evt_blk    56    (4 bytes)
 *   pm1b_evt_blk    60    (4 bytes)
 *   pm1a_cnt_blk    64    (4 bytes)   <-- we stop here
 */
struct fadt {
    struct acpi_header hdr;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  res1;
    uint8_t  preferred_pm;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static uint16_t pm1a_cnt = 0;
static uint16_t slp_typa = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint8_t acpi_sum(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s += b[i];
    return s;
}

static struct rsdp *find_rsdp(void) {
    /* "RSD PTR " packed as a little-endian uint64 */
    static const uint64_t SIG = 0x2052545020445352ULL;

    /* 1 — EBDA: first 1 KiB at the segment pointed to by 0x040E */
    uint32_t ebda = (uint32_t)(*(volatile uint16_t *)(uintptr_t)0x040E) << 4;
    for (uint32_t a = ebda; a < ebda + 1024u; a += 16) {
        if (*(volatile uint64_t *)(uintptr_t)a == SIG &&
            acpi_sum((void *)(uintptr_t)a, sizeof(struct rsdp)) == 0)
            return (struct rsdp *)(uintptr_t)a;
    }

    /* 2 — BIOS ROM 0xE0000 – 0xFFFFF */
    for (uint32_t a = 0xE0000u; a < 0x100000u; a += 16) {
        if (*(volatile uint64_t *)(uintptr_t)a == SIG &&
            acpi_sum((void *)(uintptr_t)a, sizeof(struct rsdp)) == 0)
            return (struct rsdp *)(uintptr_t)a;
    }

    return NULL;
}

/*
 * Search the DSDT AML bytecode for the _S5_ power-off object and
 * return SLP_TYPa (bits 0–2), or 0 if not found.
 */
static uint16_t find_s5_slptyp(uint32_t dsdt_phys) {
    struct acpi_header *h = (struct acpi_header *)(uintptr_t)dsdt_phys;
    if (acpi_sum(h, h->length) != 0) return 0;

    const uint8_t *aml = (const uint8_t *)((uintptr_t)dsdt_phys +
                          sizeof(struct acpi_header));
    uint32_t len = h->length - (uint32_t)sizeof(struct acpi_header);

    for (uint32_t i = 0; i + 8 < len; i++) {
        /* Look for Name opcode (0x08) then _S5_ */
        if (aml[i]   != 0x08)  continue;
        if (aml[i+1] != '_')   continue;
        if (aml[i+2] != 'S')   continue;
        if (aml[i+3] != '5')   continue;
        if (aml[i+4] != '_')   continue;

        uint32_t j = i + 5;

        /* DefPackage opcode */
        if (j >= len || aml[j] != 0x12) continue;
        j++;

        /* PkgLength: upper 2 bits encode how many extra bytes follow */
        if (j >= len) continue;
        uint8_t lead  = aml[j++];
        uint8_t extra = (lead >> 6) & 0x03;
        j += extra;

        /* NumElements */
        if (j >= len) continue;
        j++;

        /* SLP_TYPa element: optional BytePrefix (0x0A) then the value */
        if (j >= len) continue;
        if (aml[j] == 0x0A) j++;
        if (j < len) return (uint16_t)(aml[j] & 0x07);
    }

    return 0; /* default — works for most QEMU configurations */
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void acpi_init(void) {
    struct rsdp *rsdp = find_rsdp();
    if (!rsdp) return;

    struct acpi_header *rsdt_hdr =
        (struct acpi_header *)(uintptr_t)rsdp->rsdt_addr;
    if (acpi_sum(rsdt_hdr, rsdt_hdr->length) != 0) return;

    uint32_t  n       = (rsdt_hdr->length - sizeof(struct acpi_header)) / 4;
    uint32_t *entries = (uint32_t *)((uint8_t *)rsdt_hdr +
                         sizeof(struct acpi_header));

    for (uint32_t i = 0; i < n; i++) {
        struct acpi_header *t = (struct acpi_header *)(uintptr_t)entries[i];
        /* FADT signature is "FACP" */
        if (t->sig[0]=='F' && t->sig[1]=='A' &&
            t->sig[2]=='C' && t->sig[3]=='P') {
            struct fadt *fadt = (struct fadt *)t;
            pm1a_cnt = (uint16_t)fadt->pm1a_cnt_blk;
            slp_typa = find_s5_slptyp(fadt->dsdt);
            break;
        }
    }
}

void acpi_shutdown(void) {
    /* Method 1: Standard ACPI — write SLP_TYPa | SLP_EN to PM1a_CNT */
    if (pm1a_cnt)
        outw(pm1a_cnt, (uint16_t)((slp_typa << 10) | (1u << 13)));

    /* Method 2: QEMU PIIX4 ACPI power button (port 0x604) */
    outw(0x604, 0x2000);

    /* Method 3: Bochs / older QEMU */
    outw(0xB004, 0x2000);

    __asm__ volatile ("cli; hlt");
}

void acpi_reboot(void) {
    /* Method 1: PS/2 keyboard controller reset line */
    uint8_t s;
    do { s = inb(0x64); } while (s & 0x02);
    outb(0x64, 0xFE);

    /* Method 2: Triple fault via null IDT */
    __asm__ volatile ("cli");
    struct {
        uint16_t lim;
        uint64_t base;
    } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int $0" :: "m"(null_idt));

    __asm__ volatile ("hlt");
}

#include <stdint.h>
#include "drivers/e1000.h"
#include "drivers/pci.h"

/* ---- Register offsets --------------------------------------------------- */
#define CTRL    0x00000u
#define STATUS  0x00008u
#define EECD    0x00010u
#define EERD    0x00014u
#define ICR     0x000C0u
#define ITR     0x000C4u
#define IMS     0x000D0u
#define IMC     0x000D8u
#define RCTL    0x00100u
#define TCTL    0x00400u
#define TIPG    0x00410u
#define RDBAL   0x02800u
#define RDBAH   0x02804u
#define RDLEN   0x02808u
#define RDH     0x02810u
#define RDT     0x02818u
#define TDBAL   0x03800u
#define TDBAH   0x03804u
#define TDLEN   0x03808u
#define TDH     0x03810u
#define TDT     0x03818u
#define MTA     0x05200u
#define RAL0    0x05400u
#define RAH0    0x05404u

/* ---- CTRL bits ---------------------------------------------------------- */
#define CTRL_SLU    (1u << 6)   /* Set Link Up */
#define CTRL_RST    (1u << 26)  /* Reset */

/* ---- RCTL bits ---------------------------------------------------------- */
#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15)  /* Broadcast Accept */
#define RCTL_BSIZE_2048  0u     /* default buffer size */
#define RCTL_SECRC  (1u << 26)  /* Strip CRC */

/* ---- TCTL bits ---------------------------------------------------------- */
#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)   /* Pad Short Packets */
#define TCTL_CT     (0x10u << 4)
#define TCTL_COLD   (0x40u << 12)

/* ---- TX descriptor command bits ----------------------------------------- */
#define TDCMD_EOP   (1u << 0)
#define TDCMD_IFCS  (1u << 1)
#define TDCMD_RS    (1u << 3)

/* ---- TX descriptor status bits ------------------------------------------ */
#define TDSTA_DD    (1u << 0)   /* Descriptor Done */

/* ---- RX descriptor status bits ------------------------------------------ */
#define RDSTA_DD    (1u << 0)   /* Descriptor Done */
#define RDSTA_EOP   (1u << 1)   /* End of Packet */

/* ---- Descriptor ring size ----------------------------------------------- */
#define NUM_TX 8
#define NUM_RX 8
#define BUF_SIZE 2048u

/* ---- Descriptor structures ---------------------------------------------- */
struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

/* ---- Static storage ----------------------------------------------------- */
static volatile uint32_t *mmio;
static struct tx_desc tx_ring[NUM_TX] __attribute__((aligned(16)));
static struct rx_desc rx_ring[NUM_RX] __attribute__((aligned(16)));
static uint8_t tx_buf[NUM_TX][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t rx_buf[NUM_RX][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_tail;
static uint8_t rx_tail;
static uint8_t mac[6];
static int     ready = 0;

/* ---- Register helpers --------------------------------------------------- */
static inline uint32_t rd32(uint32_t off) {
    return *(volatile uint32_t *)((volatile uint8_t *)mmio + off);
}
static inline void wr32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)((volatile uint8_t *)mmio + off) = val;
}

/* ---- EEPROM read -------------------------------------------------------- */
static uint16_t eerd_read(uint8_t addr) {
    wr32(EERD, (1u) | ((uint32_t)addr << 8));
    uint32_t v;
    do { v = rd32(EERD); } while (!(v & (1u << 4)));
    return (uint16_t)(v >> 16);
}

/* ---- Delay loop --------------------------------------------------------- */
static void delay(uint32_t n) {
    volatile uint32_t i = n * 10000u;
    while (i--) __asm__ volatile ("pause");
}

/* ---- Init --------------------------------------------------------------- */
int e1000_init(void) {
    uint8_t bus, slot, fn;

    /* Try 82540EM (most common in VirtualBox) and a few other E1000 IDs */
    if (pci_find(0x8086, 0x100E, &bus, &slot, &fn) != 0 &&
        pci_find(0x8086, 0x100F, &bus, &slot, &fn) != 0 &&
        pci_find(0x8086, 0x1011, &bus, &slot, &fn) != 0 &&
        pci_find(0x8086, 0x1010, &bus, &slot, &fn) != 0) {
        return -1;
    }

    pci_enable_busmaster(bus, slot, fn);

    /* Map MMIO (BAR0 is memory-mapped, identity-mapped in our page tables) */
    uint32_t bar0 = pci_bar(bus, slot, fn, 0) & ~0xFu;
    mmio = (volatile uint32_t *)(uintptr_t)bar0;

    /* Reset */
    wr32(CTRL, rd32(CTRL) | CTRL_RST);
    delay(10);
    while (rd32(CTRL) & CTRL_RST);

    /* Set link up, clear flow control */
    wr32(CTRL, rd32(CTRL) | CTRL_SLU);

    /* Read MAC from EEPROM */
    uint16_t w0 = eerd_read(0), w1 = eerd_read(1), w2 = eerd_read(2);
    mac[0] = (uint8_t)(w0 & 0xFF); mac[1] = (uint8_t)(w0 >> 8);
    mac[2] = (uint8_t)(w1 & 0xFF); mac[3] = (uint8_t)(w1 >> 8);
    mac[4] = (uint8_t)(w2 & 0xFF); mac[5] = (uint8_t)(w2 >> 8);

    /* Set receive address */
    uint32_t ral = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                   ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    uint32_t rah = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31);
    wr32(RAL0, ral);
    wr32(RAH0, rah);

    /* Clear multicast table */
    for (int i = 0; i < 128; i++)
        wr32(MTA + (uint32_t)(i * 4), 0);

    /* ----- TX ring setup ----- */
    for (int i = 0; i < NUM_TX; i++) {
        tx_ring[i].addr   = (uint64_t)(uintptr_t)tx_buf[i];
        tx_ring[i].status = TDSTA_DD;  /* mark all as done initially */
    }
    tx_tail = 0;

    uint64_t tx_phys = (uint64_t)(uintptr_t)tx_ring;
    wr32(TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFFu));
    wr32(TDBAH, (uint32_t)(tx_phys >> 32));
    wr32(TDLEN, NUM_TX * 16u);
    wr32(TDH, 0);
    wr32(TDT, 0);

    wr32(TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr32(TIPG, 0x0060200Au);  /* standard TIPG for copper */

    /* ----- RX ring setup ----- */
    for (int i = 0; i < NUM_RX; i++) {
        rx_ring[i].addr   = (uint64_t)(uintptr_t)rx_buf[i];
        rx_ring[i].status = 0;
    }
    rx_tail = NUM_RX - 1;

    uint64_t rx_phys = (uint64_t)(uintptr_t)rx_ring;
    wr32(RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFFu));
    wr32(RDBAH, (uint32_t)(rx_phys >> 32));
    wr32(RDLEN, NUM_RX * 16u);
    wr32(RDH, 0);
    wr32(RDT, rx_tail);

    wr32(RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    /* Disable interrupts (we poll) */
    wr32(IMC, 0xFFFFFFFFu);

    ready = 1;
    return 0;
}

int e1000_send(const void *data, uint16_t len) {
    if (!ready || len > BUF_SIZE) return -1;

    /* Wait for the descriptor to be free */
    uint32_t tries = 100000;
    while (!(tx_ring[tx_tail].status & TDSTA_DD) && tries--)
        __asm__ volatile ("pause");
    if (!(tx_ring[tx_tail].status & TDSTA_DD)) return -1;

    /* Copy packet into TX buffer */
    uint8_t *dst = tx_buf[tx_tail];
    const uint8_t *src = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    tx_ring[tx_tail].length = len;
    tx_ring[tx_tail].cmd    = TDCMD_EOP | TDCMD_IFCS | TDCMD_RS;
    tx_ring[tx_tail].status = 0;

    tx_tail = (uint8_t)((tx_tail + 1) % NUM_TX);
    wr32(TDT, tx_tail);
    return 0;
}

int e1000_recv(void *buf, uint16_t max_len) {
    if (!ready) return 0;

    uint8_t head = (uint8_t)((rx_tail + 1) % NUM_RX);
    if (!(rx_ring[head].status & RDSTA_DD)) return 0;

    uint16_t len = rx_ring[head].length;
    if (len > max_len) len = max_len;

    uint8_t *src = rx_buf[head];
    uint8_t *dst = (uint8_t *)buf;
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    /* Return descriptor to hardware */
    rx_ring[head].status = 0;
    rx_tail = head;
    wr32(RDT, rx_tail);

    return (int)len;
}

void e1000_get_mac(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = mac[i];
}

int e1000_present(void) { return ready; }

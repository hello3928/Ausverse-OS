/*
 * usb_tablet.c — Minimal UHCI USB HID tablet driver for AusverseOS
 *
 * Targets QEMU's -device usb-tablet on the Intel PIIX4 UHCI controller
 * (PCI vendor 0x8086, device 0x7020, typically BDF 0:1:2).
 *
 * Identity-mapped: virtual address == physical address throughout.
 * No stdlib — all loops written by hand.
 */

#include <stdint.h>
#include "drivers/usb_tablet.h"
#include "arch/io.h"
#include "drivers/pit.h"
#include "gui/render.h"
#include "drivers/vga.h"

/* ========================================================================= */
/* PCI config space                                                           */
/* ========================================================================= */

#define PCI_ADDR  0xCF8u
#define PCI_DATA  0xCFCu

static inline uint32_t pci_cfg_addr(uint8_t bus, uint8_t dev,
                                    uint8_t fn,  uint8_t reg) {
    return (1u << 31)
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)fn   <<  8)
         | (reg & 0xFCu);
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(PCI_ADDR, pci_cfg_addr(bus, dev, fn, reg));
    return inl(PCI_DATA);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn,
                        uint8_t reg, uint32_t val) {
    outl(PCI_ADDR, pci_cfg_addr(bus, dev, fn, reg));
    outl(PCI_DATA, val);
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(PCI_ADDR, pci_cfg_addr(bus, dev, fn, reg));
    uint32_t v = inl(PCI_DATA);
    /* reg & 2: if the register is at an odd word boundary shift down */
    if (reg & 2) return (uint16_t)(v >> 16);
    return (uint16_t)(v & 0xFFFFu);
}

static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn,
                        uint8_t reg, uint16_t val) {
    outl(PCI_ADDR, pci_cfg_addr(bus, dev, fn, reg));
    uint32_t v = inl(PCI_DATA);
    if (reg & 2) {
        v = (v & 0x0000FFFFu) | ((uint32_t)val << 16);
    } else {
        v = (v & 0xFFFF0000u) | val;
    }
    outl(PCI_DATA, v);
}

/* ========================================================================= */
/* UHCI register offsets (from I/O base)                                     */
/* ========================================================================= */

#define USBCMD      0x00u   /* 16-bit */
#define USBSTS      0x02u   /* 16-bit */
#define USBINTR     0x04u   /* 16-bit */
#define FRNUM       0x06u   /* 16-bit */
#define FRBASEADD   0x08u   /* 32-bit */
#define SOFMOD      0x0Cu   /* 8-bit  */
#define PORTSC1     0x10u   /* 16-bit */
#define PORTSC2     0x12u   /* 16-bit */

/* USBCMD bits */
#define CMD_RUN         (1u << 0)
#define CMD_HCRESET     (1u << 1)
#define CMD_CONFIGURE   (1u << 6)

/* PORTSC bits */
#define PORTSC_CONN     (1u << 0)   /* current connect status */
#define PORTSC_CONN_CHG (1u << 1)   /* connect status change  */
#define PORTSC_ENABLE   (1u << 2)   /* port enabled           */
#define PORTSC_EN_CHG   (1u << 3)   /* enable/disable change  */
#define PORTSC_RESET    (1u << 9)   /* port reset             */
#define PORTSC_LSDA     (1u << 8)   /* low-speed device attached */

/* ========================================================================= */
/* Transfer Descriptor & Queue Head                                          */
/* ========================================================================= */

/* link pointer flags */
#define LP_TERM  (1u << 0)   /* terminate — no next pointer   */
#define LP_QH    (1u << 1)   /* next pointer points to a QH   */
#define LP_DEPTH (1u << 2)   /* breadth/depth select (TD only) */

/* TD status field bits */
#define TD_STATUS_ACTIVE  (1u << 23)  /* HC sets this to execute TD        */
#define TD_STATUS_STALLED (1u << 22)
#define TD_STATUS_DBUFERR (1u << 21)
#define TD_STATUS_BABBLE  (1u << 20)
#define TD_STATUS_NAK     (1u << 19)
#define TD_STATUS_CRCTO   (1u << 18)
#define TD_STATUS_BITSTUF (1u << 17)
#define TD_STATUS_RESERVED (1u << 16)
/* bits 20:16 = actual length transferred (ActLen = value + 1, or 0x7FF=0) */
#define TD_STATUS_ACTLEN_MASK  0x7FFu

/* C_ERR (retry count): bits 27:26 — set to 3 for up to 3 retries */
#define TD_STATUS_CERR(n)  (((uint32_t)(n) & 3u) << 26)

/* SPD — short packet detect: bit 29 */
#define TD_STATUS_SPD  (1u << 29)

/* LS — low speed: bit 26 in status? No — per UHCI spec:
 * bit 26 = LS (Low Speed Device), bit 27:26 = C_ERR actually occupies 27:26
 * Correction: per UHCI spec 2.1 Table 2-4:
 *   bit 0    = reserved/actual length bit 0
 *   bits 10:0 = ActLen (actual length, zero-based)
 *   bits 15:11 = reserved
 *   bit 16 = bitstuff error
 *   bit 17 = CRC/timeout
 *   bit 18 = NAK received
 *   bit 19 = Babble detected
 *   bit 20 = Data Buffer Error
 *   bit 21 = Stalled
 *   bit 22 = Active
 *   bit 23 = Interrupt on Complete (IOC)
 *   bit 24 = Isochronous Select
 *   bit 25 = Low Speed Device
 *   bits 27:26 = C_ERR
 *   bit 28 = Short Packet Detect (SPD)
 *   bits 31:29 = reserved
 *
 * Redefine correctly:
 */
#undef TD_STATUS_ACTIVE
#undef TD_STATUS_STALLED
#undef TD_STATUS_DBUFERR
#undef TD_STATUS_BABBLE
#undef TD_STATUS_NAK
#undef TD_STATUS_CRCTO
#undef TD_STATUS_BITSTUF
#undef TD_STATUS_CERR
#undef TD_STATUS_SPD

#define TD_STATUS_BITSTUF  (1u << 16)
#define TD_STATUS_CRCTO    (1u << 17)
#define TD_STATUS_NAK      (1u << 18)
#define TD_STATUS_BABBLE   (1u << 19)
#define TD_STATUS_DBUFERR  (1u << 20)
#define TD_STATUS_STALLED  (1u << 21)
#define TD_STATUS_ACTIVE   (1u << 22)  /* UHCI spec: Active is bit 22 */
#define TD_STATUS_IOC      (1u << 23)
#define TD_STATUS_LS       (1u << 25)  /* low-speed device */
#define TD_STATUS_CERR(n)  (((uint32_t)(n) & 3u) << 26)
#define TD_STATUS_SPD      (1u << 28)

/* TD token field encoding
 *  bits  7:0  = PID
 *  bits 14:8  = device address (7-bit)
 *  bits 18:15 = endpoint (4-bit)
 *  bit  19    = data toggle
 *  bits 20    = reserved
 *  bits 29:21 = (MaxLen - 1); 0x7FF means 0-byte transfer
 */
#define PID_SETUP  0x2Du
#define PID_IN     0x69u
#define PID_OUT    0xE1u

static inline uint32_t make_token(uint8_t pid, uint8_t addr, uint8_t ep,
                                  uint8_t toggle, uint16_t maxlen) {
    uint32_t ml = (maxlen == 0) ? 0x7FFu : (uint32_t)(maxlen - 1u);
    return (uint32_t)pid
         | ((uint32_t)(addr & 0x7Fu) << 8)
         | ((uint32_t)(ep   & 0x0Fu) << 15)
         | ((uint32_t)(toggle & 1u)  << 19)
         | ((ml & 0x7FFu)            << 21);
}

typedef struct {
    volatile uint32_t link;     /* next TD/QH pointer */
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;   /* physical address of data buffer */
    uint32_t _pad[4];           /* pad to 32 bytes for 16-byte alignment */
} __attribute__((packed, aligned(16))) uhci_td_t;

typedef struct {
    volatile uint32_t head;     /* next QH pointer           */
    volatile uint32_t element;  /* first TD in this QH       */
    uint32_t _pad[2];
} __attribute__((packed, aligned(16))) uhci_qh_t;

/* ========================================================================= */
/* Static storage (identity mapped — phys == virt)                          */
/* ========================================================================= */

/* Frame list: 1024 x 4-byte entries, must be 4KB-aligned.
   Allocate 8KB so we can align within. */
static uint8_t fl_buf[4096 * 2];

/* Control transfer arena: enough TDs for the longest control transfer
   (SETUP + up to 32 DATA + STATUS = 34 TDs) + a QH */
#define CTRL_TD_MAX 34
static uhci_td_t ctrl_tds[CTRL_TD_MAX] __attribute__((aligned(16)));
static uhci_qh_t ctrl_qh              __attribute__((aligned(16)));

/* Data buffer for control transfers */
static uint8_t ctrl_buf[256] __attribute__((aligned(4)));

/* Interrupt IN transfer */
static uhci_td_t intr_td  __attribute__((aligned(16)));
static uhci_qh_t intr_qh  __attribute__((aligned(16)));
static uint8_t   tablet_buf[8] __attribute__((aligned(4)));

/* ========================================================================= */
/* Driver state                                                               */
/* ========================================================================= */

static int      g_present  = 0;
static uint16_t g_iobase   = 0;
static int32_t  g_x        = 0;
static int32_t  g_y        = 0;
static uint8_t  g_buttons  = 0;

/* ========================================================================= */
/* Delay helpers                                                              */
/* ========================================================================= */

/* Busy-wait for approximately ms milliseconds using pit_ticks().
   PIT_HZ ticks per second, so 1 tick = 1000/PIT_HZ ms.
   We work in ticks to avoid division in the hot path. */
static void delay_ms(uint32_t ms) {
    /* Convert ms to ticks, rounding up.  PIT_HZ = 100 → 10ms/tick */
    uint64_t ticks_needed = ((uint64_t)ms * PIT_HZ + 999u) / 1000u;
    if (ticks_needed == 0) ticks_needed = 1;
    uint64_t start = pit_ticks();
    while ((pit_ticks() - start) < ticks_needed) {
        __asm__ volatile ("pause");
    }
}

/* Busy-wait up to timeout_ms for cond to become true, checking every 1ms.
   Returns 1 if condition met, 0 on timeout. */
#define WAIT_FOR(cond, timeout_ms) ({           \
    int _ok = 0;                                \
    uint32_t _t = (timeout_ms);                 \
    while (_t--) {                              \
        if ((cond)) { _ok = 1; break; }         \
        delay_ms(1);                            \
    }                                           \
    _ok;                                        \
})

/* ========================================================================= */
/* Frame list helpers                                                         */
/* ========================================================================= */

static uint32_t *g_fl = 0;   /* 4KB-aligned pointer into fl_buf */

static void fl_init(void) {
    /* Align fl_buf to 4KB */
    uintptr_t p = (uintptr_t)fl_buf;
    p = (p + 0xFFFu) & ~0xFFFu;
    g_fl = (uint32_t *)p;
    /* Initialise all 1024 entries to TERMINATE */
    for (int i = 0; i < 1024; i++) {
        g_fl[i] = LP_TERM;
    }
}

/* ========================================================================= */
/* HC register access                                                         */
/* ========================================================================= */

static inline void hc_write16(uint16_t reg, uint16_t val) {
    outw((uint16_t)(g_iobase + reg), val);
}
static inline uint16_t hc_read16(uint16_t reg) {
    return inw((uint16_t)(g_iobase + reg));
}
static inline void hc_write32(uint16_t reg, uint32_t val) {
    outl((uint16_t)(g_iobase + reg), val);
}

/* ========================================================================= */
/* Control transfer engine                                                    */
/* ========================================================================= */

/*
 * issue_control() — synchronous control transfer
 *
 * setup_pkt : 8-byte SETUP packet
 * data_buf  : data phase buffer (NULL if wLength=0)
 * data_len  : number of data bytes (0 if no data phase)
 * in_dir    : 1 = data IN (device→host), 0 = data OUT (host→device)
 * addr      : USB device address
 *
 * Returns 0 on success, -1 on timeout/error.
 */
static int issue_control(const uint8_t *setup_pkt,
                         uint8_t *data_buf, uint16_t data_len,
                         int in_dir, uint8_t addr) {
    /* Zero all TDs we'll use */
    uint32_t ntds = 0;

    /* ---- SETUP TD ---- */
    /* Copy setup packet into ctrl_buf[0..7] */
    for (int i = 0; i < 8; i++) ctrl_buf[i] = setup_pkt[i];

    uhci_td_t *td = &ctrl_tds[ntds++];
    td->buffer = (uint32_t)(uintptr_t)ctrl_buf;
    td->token  = make_token(PID_SETUP, addr, 0, 0, 8);
    td->status = TD_STATUS_ACTIVE | TD_STATUS_CERR(3);
    /* link will be set below */

    /* ---- DATA phase TDs (if any) ---- */
    uint8_t toggle = 1;
    uint16_t remaining = data_len;
    uint8_t *buf_ptr = data_buf;
    uint8_t  pid_data = in_dir ? PID_IN : PID_OUT;

    while (remaining > 0) {
        uint16_t chunk = remaining > 8 ? 8 : remaining;
        uhci_td_t *dtd = &ctrl_tds[ntds++];
        dtd->buffer = (uint32_t)(uintptr_t)buf_ptr;
        dtd->token  = make_token(pid_data, addr, 0, toggle, chunk);
        dtd->status = TD_STATUS_ACTIVE | TD_STATUS_CERR(3);
        toggle ^= 1;
        buf_ptr   += chunk;
        remaining -= chunk;
        if (ntds >= CTRL_TD_MAX - 1) break;  /* safety */
    }

    /* ---- STATUS TD ---- */
    /* Direction is opposite of data phase (or IN if no data) */
    uint8_t status_pid = in_dir ? PID_OUT : PID_IN;
    uhci_td_t *std = &ctrl_tds[ntds++];
    std->buffer = (uint32_t)(uintptr_t)ctrl_buf;  /* ignored for 0-len */
    std->token  = make_token(status_pid, addr, 0, 1, 0);
    std->status = TD_STATUS_ACTIVE | TD_STATUS_CERR(3);

    /* Chain TDs: each links to the next with depth-first traversal */
    for (uint32_t i = 0; i < ntds - 1; i++) {
        ctrl_tds[i].link = (uint32_t)(uintptr_t)&ctrl_tds[i + 1] | LP_DEPTH;
    }
    ctrl_tds[ntds - 1].link = LP_TERM;

    /* Point QH at first TD */
    ctrl_qh.head    = LP_TERM;
    ctrl_qh.element = (uint32_t)(uintptr_t)&ctrl_tds[0];

    /* Insert QH into ALL frame list entries so HC sees it every ms */
    uint32_t qh_ptr = (uint32_t)(uintptr_t)&ctrl_qh | LP_QH;
    for (int i = 0; i < 1024; i++) g_fl[i] = qh_ptr;

    /* Wait for last TD to complete (status TD) — timeout 500ms */
    int ok = WAIT_FOR(!(std->status & TD_STATUS_ACTIVE), 500);

    /* Remove QH from frame list */
    for (int i = 0; i < 1024; i++) g_fl[i] = LP_TERM;

    if (!ok) return -1;
    if (std->status & (TD_STATUS_STALLED | TD_STATUS_BABBLE |
                       TD_STATUS_DBUFERR | TD_STATUS_CRCTO)) return -1;
    return 0;
}

/* ========================================================================= */
/* USB SETUP packet builders                                                  */
/* ========================================================================= */

static void build_get_descriptor(uint8_t *pkt, uint8_t type,
                                 uint8_t idx, uint16_t len) {
    pkt[0] = 0x80;          /* bmRequestType: device→host, standard, device */
    pkt[1] = 0x06;          /* bRequest: GET_DESCRIPTOR */
    pkt[2] = idx;           /* wValue low  (descriptor index) */
    pkt[3] = type;          /* wValue high (descriptor type) */
    pkt[4] = 0x00;          /* wIndex low */
    pkt[5] = 0x00;          /* wIndex high */
    pkt[6] = (uint8_t)(len & 0xFF);
    pkt[7] = (uint8_t)(len >> 8);
}

static void build_set_address(uint8_t *pkt, uint8_t new_addr) {
    pkt[0] = 0x00;          /* host→device, standard, device */
    pkt[1] = 0x05;          /* SET_ADDRESS */
    pkt[2] = new_addr;
    pkt[3] = 0x00;
    pkt[4] = 0x00;
    pkt[5] = 0x00;
    pkt[6] = 0x00;
    pkt[7] = 0x00;
}

static void build_set_configuration(uint8_t *pkt, uint8_t cfg) {
    pkt[0] = 0x00;
    pkt[1] = 0x09;          /* SET_CONFIGURATION */
    pkt[2] = cfg;
    pkt[3] = 0x00;
    pkt[4] = 0x00;
    pkt[5] = 0x00;
    pkt[6] = 0x00;
    pkt[7] = 0x00;
}

static void build_set_idle(uint8_t *pkt) {
    pkt[0] = 0x21;          /* host→device, class, interface */
    pkt[1] = 0x0A;          /* SET_IDLE */
    pkt[2] = 0x00;          /* duration=0 (indefinite), report ID=0 */
    pkt[3] = 0x00;
    pkt[4] = 0x00;          /* wIndex = interface 0 */
    pkt[5] = 0x00;
    pkt[6] = 0x00;          /* wLength = 0 */
    pkt[7] = 0x00;
}

/* ========================================================================= */
/* PCI scan for UHCI controller                                               */
/* ========================================================================= */

static int find_uhci(uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_fn) {
    /*
     * Try the well-known PIIX4 UHCI location first (BDF 0:1:2).
     * Then do a full scan of bus 0 as fallback.
     */
    uint8_t candidates[4][3] = {
        {0, 1, 2},
        {0, 0, 0},
        {0, 1, 0},
        {0, 1, 1},
    };
    for (int i = 0; i < 4; i++) {
        uint8_t bus = candidates[i][0];
        uint8_t dev = candidates[i][1];
        uint8_t fn  = candidates[i][2];
        uint32_t id = pci_read32(bus, dev, fn, 0x00);
        uint16_t vendor = (uint16_t)(id & 0xFFFFu);
        uint16_t device = (uint16_t)(id >> 16);
        if (vendor == 0x8086u && device == 0x7020u) {
            *out_bus = bus; *out_dev = dev; *out_fn = fn;
            return 0;
        }
    }
    /* Full bus 0 scan */
    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint32_t id = pci_read32(0, dev, fn, 0x00);
            if ((id & 0xFFFFu) == 0xFFFFu) continue;
            /* Check class=0x0C (serial bus), subclass=0x03 (USB),
               prog IF=0x00 (UHCI) */
            uint32_t cls = pci_read32(0, dev, fn, 0x08);
            uint8_t base_class = (uint8_t)(cls >> 24);
            uint8_t sub_class  = (uint8_t)(cls >> 16);
            uint8_t prog_if    = (uint8_t)(cls >>  8);
            if (base_class == 0x0Cu && sub_class == 0x03u && prog_if == 0x00u) {
                *out_bus = 0; *out_dev = dev; *out_fn = fn;
                return 0;
            }
        }
    }
    return -1;
}

/* ========================================================================= */
/* Port reset & enable                                                        */
/* ========================================================================= */

static int reset_port(void) {
    /* Assert RESET for 50ms */
    hc_write16(PORTSC1, PORTSC_RESET);
    delay_ms(50);

    /* Deassert RESET */
    uint16_t ps = hc_read16(PORTSC1);
    ps &= ~PORTSC_RESET;
    hc_write16(PORTSC1, ps);
    delay_ms(10);

    /* Clear change bits and enable port */
    ps = hc_read16(PORTSC1);
    ps |= PORTSC_ENABLE;
    ps |= PORTSC_CONN_CHG | PORTSC_EN_CHG;   /* W1C: write 1 to clear */
    hc_write16(PORTSC1, ps);
    delay_ms(10);

    /* Check connected */
    ps = hc_read16(PORTSC1);
    if (!(ps & PORTSC_CONN)) return -1;

    /* If not enabled, force-enable */
    if (!(ps & PORTSC_ENABLE)) {
        ps |= PORTSC_ENABLE;
        hc_write16(PORTSC1, ps);
        delay_ms(10);
        ps = hc_read16(PORTSC1);
        if (!(ps & PORTSC_ENABLE)) return -1;
    }

    return 0;
}

/* ========================================================================= */
/* usb_tablet_init                                                            */
/* ========================================================================= */

int usb_tablet_init(void) {
    uint8_t bus, dev, fn;
    if (find_uhci(&bus, &dev, &fn) != 0) {
        vga_print("[USB] no UHCI controller\n");
        return -1;
    }
    vga_print("[USB] UHCI found\n");

    /* Read BAR4 (I/O base address register) */
    uint32_t bar4 = pci_read32(bus, dev, fn, 0x20);
    if (!(bar4 & 1u)) { vga_print("[USB] BAR4 not I/O\n"); return -1; }
    g_iobase = (uint16_t)(bar4 & 0xFFFC);
    if (g_iobase == 0) { vga_print("[USB] BAR4 zero\n"); return -1; }
    vga_print("[USB] iobase ok\n");

    /* Enable bus mastering (PCI command register bit 2) */
    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    cmd |= (1u << 2) | (1u << 0);          /* bus master + I/O space */
    pci_write16(bus, dev, fn, 0x04, cmd);

    /* ---- Reset the HC ---- */
    hc_write16(USBCMD, CMD_HCRESET);
    if (!WAIT_FOR(!(hc_read16(USBCMD) & CMD_HCRESET), 200)) {
        vga_print("[USB] HC reset timeout\n"); return -1;
    }
    delay_ms(5);

    /* Disable all interrupts (we poll) */
    hc_write16(USBINTR, 0);

    /* Clear status register (write 1 to clear sticky bits) */
    hc_write16(USBSTS, 0x3Fu);

    /* Set SOF modify register to default value */
    outb((uint16_t)(g_iobase + SOFMOD), 0x40);

    /* ---- Set up frame list ---- */
    fl_init();
    hc_write32(FRBASEADD, (uint32_t)(uintptr_t)g_fl);
    hc_write16(FRNUM, 0);

    /* ---- Start HC ---- */
    hc_write16(USBCMD, CMD_CONFIGURE | CMD_RUN);
    if (!WAIT_FOR(!(hc_read16(USBSTS) & (1u << 5)), 100)) {
        vga_print("[USB] HC start timeout\n"); return -1;
    }
    vga_print("[USB] HC running\n");

    /* ---- Reset port 1 ---- */
    delay_ms(100);
    if (reset_port() != 0) {
        vga_print("[USB] port reset failed\n"); return -1;
    }
    vga_print("[USB] port up\n");

    /* ---- Enumeration ---- */
    uint8_t pkt[8];
    uint8_t desc[256];

    /* Zero descriptor buffer */
    for (int i = 0; i < 256; i++) desc[i] = 0;

    /* 1. GET_DESCRIPTOR(Device, 8 bytes) at address 0 */
    build_get_descriptor(pkt, 0x01, 0, 8);
    if (issue_control(pkt, desc, 8, 1, 0) != 0) {
        vga_print("[USB] GET_DESC(8)@0 fail\n"); return -1;
    }
    vga_print("[USB] step1 ok\n");

    /* 2. SET_ADDRESS(1) */
    build_set_address(pkt, 1);
    if (issue_control(pkt, 0, 0, 0, 0) != 0) {
        vga_print("[USB] SET_ADDR fail\n"); return -1;
    }
    delay_ms(5);   /* device needs time to accept new address */
    vga_print("[USB] step2 ok\n");

    /* 3. GET_DESCRIPTOR(Device, 18 bytes) at address 1 */
    for (int i = 0; i < 256; i++) desc[i] = 0;
    build_get_descriptor(pkt, 0x01, 0, 18);
    if (issue_control(pkt, desc, 18, 1, 1) != 0) {
        vga_print("[USB] GET_DESC(18)@1 fail\n"); return -1;
    }
    vga_print("[USB] step3 ok\n");

    /* 4. GET_DESCRIPTOR(Configuration, 255 bytes) at address 1 */
    for (int i = 0; i < 256; i++) desc[i] = 0;
    build_get_descriptor(pkt, 0x02, 0, 255);
    if (issue_control(pkt, desc, 255, 1, 1) != 0) {
        vga_print("[USB] GET_DESC(cfg) fail\n"); return -1;
    }
    vga_print("[USB] step4 ok\n");

    /* 5. SET_CONFIGURATION(1) */
    build_set_configuration(pkt, 1);
    if (issue_control(pkt, 0, 0, 0, 1) != 0) {
        vga_print("[USB] SET_CFG fail\n"); return -1;
    }
    delay_ms(5);
    vga_print("[USB] step5 ok\n");

    /* 6. SET_IDLE */
    build_set_idle(pkt);
    if (issue_control(pkt, 0, 0, 0, 1) != 0) {
        vga_print("[USB] SET_IDLE fail\n"); return -1;
    }
    vga_print("[USB] step6 ok\n");

    /* ---- Set up persistent interrupt IN TD ---- */
    /* Zero tablet buffer */
    for (int i = 0; i < 8; i++) tablet_buf[i] = 0;

    intr_td.link   = LP_TERM;
    intr_td.buffer = (uint32_t)(uintptr_t)tablet_buf;
    /* QEMU tablet reports 6 bytes: buttons(1) + X(2 LE) + Y(2 LE) + wheel(1) */
    intr_td.token  = make_token(PID_IN, 1, 1, 0, 6);
    intr_td.status = TD_STATUS_ACTIVE | TD_STATUS_CERR(3);

    intr_qh.head    = LP_TERM;
    intr_qh.element = (uint32_t)(uintptr_t)&intr_td;

    /* Insert intr_qh into ALL 1024 frame list entries */
    uint32_t qh_ptr = (uint32_t)(uintptr_t)&intr_qh | LP_QH;
    for (int i = 0; i < 1024; i++) {
        g_fl[i] = qh_ptr;
    }

    g_x       = (int32_t)(render_width()  / 2);
    g_y       = (int32_t)(render_height() / 2);
    g_buttons = 0;
    g_present = 1;
    return 0;
}

/* ========================================================================= */
/* usb_tablet_present                                                         */
/* ========================================================================= */

int usb_tablet_present(void) {
    return g_present;
}

/* ========================================================================= */
/* usb_tablet_get                                                             */
/* ========================================================================= */

void usb_tablet_get(int32_t *x, int32_t *y, uint8_t *buttons) {
    if (!g_present) {
        *x = g_x; *y = g_y; *buttons = g_buttons;
        return;
    }

    /* Check if the interrupt TD has completed (ACTIVE bit clear) */
    if (!(intr_td.status & TD_STATUS_ACTIVE)) {
        /* Extract report:
         * byte 0       = buttons
         * bytes 1-2 LE = X  (0–32767 maps to 0–screen_width-1)
         * bytes 3-4 LE = Y  (0–32767 maps to 0–screen_height-1)
         * byte 5       = wheel (ignored)
         */
        uint8_t btn = tablet_buf[0];
        uint16_t raw_x = (uint16_t)tablet_buf[1] | ((uint16_t)tablet_buf[2] << 8);
        uint16_t raw_y = (uint16_t)tablet_buf[3] | ((uint16_t)tablet_buf[4] << 8);

        uint32_t sw = render_width();
        uint32_t sh = render_height();

        /* Map 0–32767 → 0–(width-1) */
        g_x = (int32_t)((uint32_t)raw_x * (sw - 1) / 32767u);
        g_y = (int32_t)((uint32_t)raw_y * (sh - 1) / 32767u);

        /* Clamp */
        if (g_x < 0)              g_x = 0;
        if (g_y < 0)              g_y = 0;
        if (g_x >= (int32_t)sw)  g_x = (int32_t)(sw - 1);
        if (g_y >= (int32_t)sh)  g_y = (int32_t)(sh - 1);

        /* bit0=left, bit1=right, bit2=middle (HID usage matches mouse.h) */
        g_buttons = btn & 0x07u;

        /* Re-arm the TD */
        intr_td.token  = make_token(PID_IN, 1, 1,
                                    (uint8_t)((intr_td.token >> 19) & 1u) ^ 1u,
                                    6);
        intr_td.status = TD_STATUS_ACTIVE | TD_STATUS_CERR(3);
        /* Reset the QH element pointer so HC picks it up again */
        intr_qh.element = (uint32_t)(uintptr_t)&intr_td;
    }

    *x       = g_x;
    *y       = g_y;
    *buttons = g_buttons;
}

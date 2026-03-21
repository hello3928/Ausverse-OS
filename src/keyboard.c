#include "../include/keyboard.h"
#include "../include/idt.h"
#include "../include/pic.h"
#include "../include/io.h"

#define KB_DATA_PORT    0x60

/* ------------------------------------------------------------------ */
/* PS/2 Set 1 scancode -> ASCII                                        */
/* Index = scancode make code (key-down). Break codes are make | 0x80 */
/* ------------------------------------------------------------------ */
static const char scancode_ascii[128] = {
/*00*/  0,   0,  '1', '2', '3', '4', '5', '6',
/*08*/ '7', '8', '9', '0', '-', '=',  '\b', '\t',
/*10*/ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
/*18*/ 'o', 'p', '[', ']', '\n',  0,  'a', 's',
/*20*/ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
/*28*/ '\'', '`',  0, '\\', 'z', 'x', 'c', 'v',
/*30*/ 'b', 'n', 'm', ',', '.', '/',  0,  '*',
/*38*/  0,  ' ',  0,   0,   0,   0,   0,   0,
/*40*/  0,   0,   0,   0,   0,   0,   0,  '7',
/*48*/ '8', '9', '-', '4', '5', '6', '+', '1',
/*50*/ '2', '3', '0', '.',  0,   0,   0,   0,
/*58*/  0,   0,   0,   0,   0,   0,   0,   0,
/*60*/  0,   0,   0,   0,   0,   0,   0,   0,
/*68*/  0,   0,   0,   0,   0,   0,   0,   0,
/*70*/  0,   0,   0,   0,   0,   0,   0,   0,
/*78*/  0,   0,   0,   0,   0,   0,   0,   0,
};

static const char scancode_shift[128] = {
/*00*/  0,   0,  '!', '@', '#', '$', '%', '^',
/*08*/ '&', '*', '(', ')', '_', '+',  '\b', '\t',
/*10*/ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
/*18*/ 'O', 'P', '{', '}', '\n',  0,  'A', 'S',
/*20*/ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
/*28*/ '"', '~',  0,  '|', 'Z', 'X', 'C', 'V',
/*30*/ 'B', 'N', 'M', '<', '>', '?',  0,  '*',
/*38*/  0,  ' ',  0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
};

/* Left shift = 0x2A, right shift = 0x36 */
#define SC_LSHIFT       0x2A
#define SC_RSHIFT       0x36
#define SC_LSHIFT_REL   (SC_LSHIFT | 0x80)
#define SC_RSHIFT_REL   (SC_RSHIFT | 0x80)
#define SC_CAPSLOCK     0x3A

/* ------------------------------------------------------------------ */
/* Ring buffer                                                          */
/* ------------------------------------------------------------------ */
#define BUF_SIZE 256

static char             buf[BUF_SIZE];
static volatile uint8_t buf_head  = 0;
static volatile uint8_t buf_tail  = 0;
static volatile int     shift     = 0;
static volatile int     caps_lock = 0;
static volatile int     extended  = 0;  /* set when 0xE0 prefix received */

static void buf_push(char c) {
    uint8_t next = (buf_tail + 1) % BUF_SIZE;
    if (next != buf_head) {     /* drop silently if full */
        buf[buf_tail] = c;
        buf_tail = next;
    }
}

/* ------------------------------------------------------------------ */
/* IRQ1 handler                                                         */
/* ------------------------------------------------------------------ */
static void keyboard_irq(void) {
    uint8_t sc = inb(KB_DATA_PORT);

    if (sc == 0xE0) { extended = 1; return; }
    if (extended) {
        extended = 0;
        if      (sc == 0x4B) buf_push(KEY_LEFT);
        else if (sc == 0x4D) buf_push(KEY_RIGHT);
        else if (sc == 0x48) buf_push(KEY_UP);
        else if (sc == 0x50) buf_push(KEY_DOWN);
        return;
    }

    if (sc == SC_LSHIFT || sc == SC_RSHIFT) { shift = 1; return; }
    if (sc == SC_LSHIFT_REL || sc == SC_RSHIFT_REL) { shift = 0; return; }
    if (sc == SC_CAPSLOCK) { caps_lock = !caps_lock; return; }

    if (sc & 0x80) return;  /* ignore all other key-release codes */

    char c = shift ? scancode_shift[sc] : scancode_ascii[sc];

    /* Apply caps lock to letters only — else if so both don't fire */
    if      (caps_lock && c >= 'a' && c <= 'z') c -= 32;
    else if (caps_lock && c >= 'A' && c <= 'Z') c += 32;

    /* Only push printable characters and recognised control codes */
    if (c == '\n' || c == '\b' || (c >= ' ' && c <= '~'))
        buf_push(c);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
void keyboard_init(void) {
    irq_register(1, keyboard_irq);
    pic_unmask(1);
}

char keyboard_getchar(void) {
    if (buf_head == buf_tail) return 0;
    char c = buf[buf_head];
    buf_head = (buf_head + 1) % BUF_SIZE;
    return c;
}

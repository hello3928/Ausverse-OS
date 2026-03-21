#include <stdint.h>
#include <stddef.h>
#include "../include/shell.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/framebuffer.h"
#include "../include/pit.h"
#include "../include/pmm.h"

/* ------------------------------------------------------------------ */
/* Minimal string utilities (no stdlib)                                */
/* ------------------------------------------------------------------ */

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Line editor                                                          */
/* ------------------------------------------------------------------ */

#define LINE_MAX 256
#define ARGS_MAX 8

static char line[LINE_MAX];
static int  line_len = 0;

static void line_reset(void) {
    line_len = 0;
    line[0]  = '\0';
}

/* Move the framebuffer cursor to (start_col + offset), handling row wrap */
static void set_cur(uint32_t start_col, uint32_t start_row, int offset) {
    uint32_t c = fb_cols();
    uint32_t abs = start_col + (uint32_t)offset;
    fb_set_cursor(abs % c, start_row + abs / c);
}

/* Redraw the input line from start, then place cursor at cursor_pos */
static void redraw_line(uint32_t start_col, uint32_t start_row, int cursor_pos) {
    set_cur(start_col, start_row, 0);
    for (int i = 0; i < line_len; i++) vga_putchar(line[i]);
    vga_putchar(' ');   /* erase any leftover char after a deletion */
    set_cur(start_col, start_row, cursor_pos);
}

static const char *readline(void) {
    line_reset();
    int cursor = 0;

    uint32_t start_col, start_row;
    fb_get_cursor(&start_col, &start_row);

    for (;;) {
        __asm__ volatile ("hlt");

        char c = keyboard_getchar();
        if (!c) continue;

        if (c == '\n') {
            set_cur(start_col, start_row, line_len);
            vga_putchar('\n');
            line[line_len] = '\0';
            return line;
        }

        if (c == KEY_LEFT) {
            if (cursor > 0) cursor--;
            set_cur(start_col, start_row, cursor);
            continue;
        }

        if (c == KEY_RIGHT) {
            if (cursor < line_len) cursor++;
            set_cur(start_col, start_row, cursor);
            continue;
        }

        /* KEY_UP / KEY_DOWN: ignore for now */
        if (c == KEY_UP || c == KEY_DOWN) continue;

        if (c == '\b') {
            if (cursor > 0) {
                for (int i = cursor - 1; i < line_len - 1; i++)
                    line[i] = line[i + 1];
                cursor--;
                line_len--;
                redraw_line(start_col, start_row, cursor);
            }
            continue;
        }

        if (c >= ' ' && c <= '~' && line_len < LINE_MAX - 1) {
            /* Insert character at cursor position */
            for (int i = line_len; i > cursor; i--)
                line[i] = line[i - 1];
            line[cursor] = c;
            cursor++;
            line_len++;
            /* Redraw from insertion point only */
            set_cur(start_col, start_row, cursor - 1);
            for (int i = cursor - 1; i < line_len; i++) vga_putchar(line[i]);
            set_cur(start_col, start_row, cursor);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Command parser                                                       */
/* ------------------------------------------------------------------ */

static char  argv_buf[LINE_MAX];
static char *argv[ARGS_MAX];
static int   argc;

static void parse(const char *input) {
    size_t len = str_len(input);
    if (len >= LINE_MAX) len = LINE_MAX - 1;
    for (size_t i = 0; i <= len; i++) argv_buf[i] = input[i];

    argc = 0;
    char *p = argv_buf;

    while (*p && argc < ARGS_MAX) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

static void cmd_help(void) {
    vga_print_colored("Available commands:\n", VGA_YELLOW, VGA_BLACK);
    vga_print_colored("  help   ", VGA_LCYAN, VGA_BLACK);
    vga_print("- show this message\n");
    vga_print_colored("  clear  ", VGA_LCYAN, VGA_BLACK);
    vga_print("- clear the screen\n");
    vga_print_colored("  echo   ", VGA_LCYAN, VGA_BLACK);
    vga_print("- print text\n");
    vga_print_colored("  uptime ", VGA_LCYAN, VGA_BLACK);
    vga_print("- seconds since boot\n");
    vga_print_colored("  mem    ", VGA_LCYAN, VGA_BLACK);
    vga_print("- memory usage\n");
}

static void cmd_clear(void) {
    vga_init();
}

static void cmd_echo(void) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) vga_putchar(' ');
        vga_print(argv[i]);
    }
    vga_putchar('\n');
}

static void cmd_uptime(void) {
    uint64_t secs = pit_ticks() / PIT_HZ;
    vga_print("Uptime: ");
    vga_print_colored("", VGA_YELLOW, VGA_BLACK);
    vga_print_uint((uint32_t)secs);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_print(" seconds\n");
}

static void cmd_mem(void) {
    uint32_t free_mb  = pmm_free_pages()  * PAGE_SIZE / 1024 / 1024;
    uint32_t total_mb = pmm_total_pages() * PAGE_SIZE / 1024 / 1024;
    vga_print("Memory: ");
    vga_print_colored("", VGA_LGREEN, VGA_BLACK);
    vga_print_uint(free_mb);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_print(" MiB free / ");
    vga_print_uint(total_mb);
    vga_print(" MiB total\n");
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                       */
/* ------------------------------------------------------------------ */

struct command {
    const char *name;
    void (*fn)(void);
};

static const struct command commands[] = {
    { "help",   cmd_help   },
    { "clear",  cmd_clear  },
    { "echo",   cmd_echo   },
    { "uptime", cmd_uptime },
    { "mem",    cmd_mem    },
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static void dispatch(void) {
    if (argc == 0) return;

    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (str_eq(argv[0], commands[i].name)) {
            commands[i].fn();
            return;
        }
    }

    vga_print_colored("Unknown command: ", VGA_LRED, VGA_BLACK);
    vga_print(argv[0]);
    vga_putchar('\n');
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* ------------------------------------------------------------------ */
/* Shell entry point                                                    */
/* ------------------------------------------------------------------ */

void shell_run(void) {
    vga_print_colored("\nAusverseOS shell", VGA_YELLOW, VGA_BLACK);
    vga_print(" - type ");
    vga_print_colored("help", VGA_LCYAN, VGA_BLACK);
    vga_print(" for commands\n\n");

    for (;;) {
        vga_print_colored("> ", VGA_LGREEN, VGA_BLACK);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        const char *input = readline();
        parse(input);
        dispatch();
    }
}

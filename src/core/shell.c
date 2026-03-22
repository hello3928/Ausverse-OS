#include <stdint.h>
#include <stddef.h>
#include "core/shell.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "gui/framebuffer.h"
#include "gui/font.h"
#include "drivers/pit.h"
#include "core/pmm.h"
#include "core/acpi.h"
#include "fs/vfs.h"
#include "core/elf.h"
#include "gui/wm.h"
#include "gui/render.h"
#include "gui/font.h"
#include "net/net.h"
#include "core/pkg.h"

/* ------------------------------------------------------------------ */
/* Minimal string utilities (no stdlib)                                */
/* ------------------------------------------------------------------ */

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static size_t str_len(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static void str_cpy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void print_hex_byte(uint8_t v) {
    const char *h = "0123456789abcdef";
    vga_putchar(h[v >> 4]);
    vga_putchar(h[v & 0xF]);
}

static void print_hex32(uint32_t v) {
    for (int s = 28; s >= 0; s -= 4)
        vga_putchar("0123456789abcdef"[(v >> s) & 0xF]);
}

/* ------------------------------------------------------------------ */
/* Command history                                                     */
/* ------------------------------------------------------------------ */

#define HISTORY_MAX  16
#define LINE_MAX    256
#define ARGS_MAX     16

static char history[HISTORY_MAX][LINE_MAX];
static int  history_count = 0;
static int  history_head  = 0;

static void history_push(const char *ln) {
    if (!ln[0]) return;
    history_head = (history_head + 1) % HISTORY_MAX;
    str_cpy(history[history_head], ln, LINE_MAX);
    if (history_count < HISTORY_MAX) history_count++;
}

static const char *history_get(int idx) {
    if (idx < 0 || idx >= history_count) return NULL;
    int pos = ((history_head - idx) % HISTORY_MAX + HISTORY_MAX) % HISTORY_MAX;
    return history[pos];
}

/* ------------------------------------------------------------------ */
/* Line editor                                                          */
/* ------------------------------------------------------------------ */

static char buf_line[LINE_MAX];
static int  line_len = 0;

static void line_reset(void) { line_len = 0; buf_line[0] = '\0'; }

static void set_cur(uint32_t sc, uint32_t sr, int offset) {
    uint32_t c = fb_cols();
    uint32_t abs = sc + (uint32_t)offset;
    fb_set_cursor(abs % c, sr + abs / c);
}

static void redraw_line(uint32_t sc, uint32_t sr, int cpos) {
    set_cur(sc, sr, 0);
    for (int i = 0; i < line_len; i++) vga_putchar(buf_line[i]);
    vga_putchar(' ');
    set_cur(sc, sr, cpos);
}

static const char *readline(void) {
    line_reset();
    int cursor = 0, hist_idx = -1;
    uint32_t sc, sr;
    fb_get_cursor(&sc, &sr);
    fb_cursor_enable(1);

    for (;;) {
        __asm__ volatile ("hlt");
        char c = keyboard_getchar();
        if (!c) continue;

        if (c == '\n') {
            fb_cursor_enable(0);
            set_cur(sc, sr, line_len);
            vga_putchar('\n');
            buf_line[line_len] = '\0';
            return buf_line;
        }

        if (c == KEY_UP) {
            int ni = hist_idx + 1;
            const char *e = history_get(ni);
            if (e) {
                hist_idx = ni;
                set_cur(sc, sr, 0);
                for (int i = 0; i < line_len; i++) vga_putchar(' ');
                str_cpy(buf_line, e, LINE_MAX);
                line_len = (int)str_len(buf_line);
                cursor   = line_len;
                redraw_line(sc, sr, cursor);
            }
            continue;
        }

        if (c == KEY_DOWN) {
            if (hist_idx > 0) {
                hist_idx--;
                const char *e = history_get(hist_idx);
                set_cur(sc, sr, 0);
                for (int i = 0; i < line_len; i++) vga_putchar(' ');
                str_cpy(buf_line, e, LINE_MAX);
                line_len = (int)str_len(buf_line);
                cursor   = line_len;
                redraw_line(sc, sr, cursor);
            } else if (hist_idx == 0) {
                hist_idx = -1;
                set_cur(sc, sr, 0);
                for (int i = 0; i < line_len; i++) vga_putchar(' ');
                line_reset();
                cursor = 0;
                redraw_line(sc, sr, cursor);
            }
            continue;
        }

        if (c == KEY_LEFT)  { if (cursor > 0)        cursor--; set_cur(sc, sr, cursor); continue; }
        if (c == KEY_RIGHT) { if (cursor < line_len) cursor++; set_cur(sc, sr, cursor); continue; }

        if (c == '\b') {
            if (cursor > 0) {
                for (int i = cursor - 1; i < line_len - 1; i++)
                    buf_line[i] = buf_line[i + 1];
                cursor--; line_len--;
                redraw_line(sc, sr, cursor);
            }
            hist_idx = -1;
            continue;
        }

        if (c >= ' ' && c <= '~' && line_len < LINE_MAX - 1) {
            for (int i = line_len; i > cursor; i--)
                buf_line[i] = buf_line[i - 1];
            buf_line[cursor++] = c;
            line_len++;
            set_cur(sc, sr, cursor - 1);
            for (int i = cursor - 1; i < line_len; i++) vga_putchar(buf_line[i]);
            set_cur(sc, sr, cursor);
            hist_idx = -1;
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
/* Shell header                                                         */
/* Rows 0-2 are reserved for the static header; the interactive prompt */
/* always starts at row SHELL_PROMPT_ROW so readline can never move    */
/* the cursor into the header area.                                    */
/* ------------------------------------------------------------------ */

#define SHELL_PROMPT_ROW 3u

static void shell_draw_header(void) {
    vga_init();   /* clear screen, reset cursor to (0,0) */

    /* Row 0 — coloured title bar */
    fb_fill_rect(0, 0, fb_cols() * FONT_WIDTH, FONT_HEIGHT, 0x1A1A2E);
    fb_draw_hline(0, FONT_HEIGHT - 1, fb_cols() * FONT_WIDTH, 0x4444AA);
    fb_set_cursor(1, 0);
    vga_print_colored("AusverseOS", VGA_YELLOW, VGA_BLACK);
    vga_set_color(VGA_WHITE, VGA_BLACK);

    /* Row 1 — description */
    fb_set_cursor(0, 1);
    vga_print_colored("AusverseOS shell", VGA_YELLOW, VGA_BLACK);
    vga_print(" - type ");
    vga_print_colored("help", VGA_LCYAN, VGA_BLACK);
    vga_print(" for commands");

    /* Row 2 — separator line */
    fb_set_cursor(0, 2);
    fb_draw_hline(0, 2 * FONT_HEIGHT, fb_cols() * FONT_WIDTH, 0x444444);

    /* Lock cursor below the header — readline saves this as its floor */
    fb_set_cursor(0, SHELL_PROMPT_ROW);
}

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

static void cmd_help(void) {
    vga_print_colored("System:\n", VGA_YELLOW, VGA_BLACK);
    vga_print_colored("  help     ", VGA_LCYAN, VGA_BLACK); vga_print("show this message\n");
    vga_print_colored("  clear    ", VGA_LCYAN, VGA_BLACK); vga_print("clear the screen\n");
    vga_print_colored("  echo     ", VGA_LCYAN, VGA_BLACK); vga_print("print text\n");
    vga_print_colored("  uptime   ", VGA_LCYAN, VGA_BLACK); vga_print("seconds since boot\n");
    vga_print_colored("  mem      ", VGA_LCYAN, VGA_BLACK); vga_print("memory usage\n");
    vga_print_colored("  reboot   ", VGA_LCYAN, VGA_BLACK); vga_print("reboot the system\n");
    vga_print_colored("  shutdown ", VGA_LCYAN, VGA_BLACK); vga_print("power off\n");
    vga_print_colored("Filesystem:\n", VGA_YELLOW, VGA_BLACK);
    vga_print_colored("  pwd      ", VGA_LCYAN, VGA_BLACK); vga_print("print working directory\n");
    vga_print_colored("  ls       ", VGA_LCYAN, VGA_BLACK); vga_print("list directory\n");
    vga_print_colored("  cd       ", VGA_LCYAN, VGA_BLACK); vga_print("change directory\n");
    vga_print_colored("  mkdir    ", VGA_LCYAN, VGA_BLACK); vga_print("create directory\n");
    vga_print_colored("  touch    ", VGA_LCYAN, VGA_BLACK); vga_print("create empty file\n");
    vga_print_colored("  write    ", VGA_LCYAN, VGA_BLACK); vga_print("write text to file\n");
    vga_print_colored("  append   ", VGA_LCYAN, VGA_BLACK); vga_print("append text to file\n");
    vga_print_colored("  cat      ", VGA_LCYAN, VGA_BLACK); vga_print("print file contents\n");
    vga_print_colored("  rm       ", VGA_LCYAN, VGA_BLACK); vga_print("delete file or empty dir\n");
    vga_print_colored("  cp       ", VGA_LCYAN, VGA_BLACK); vga_print("copy file\n");
    vga_print_colored("  mv       ", VGA_LCYAN, VGA_BLACK); vga_print("move/rename file\n");
    vga_print_colored("  hexdump  ", VGA_LCYAN, VGA_BLACK); vga_print("hex dump file contents\n");
    vga_print_colored("Network:\n", VGA_YELLOW, VGA_BLACK);
    vga_print_colored("  ifconfig ", VGA_LCYAN, VGA_BLACK); vga_print("show IP and MAC address\n");
    vga_print_colored("  ping     ", VGA_LCYAN, VGA_BLACK); vga_print("ping an IP address\n");
    vga_print_colored("  pkg      ", VGA_LCYAN, VGA_BLACK); vga_print("package manager (list, install)\n");
}

static void cmd_clear(void) { shell_draw_header(); }

static void cmd_echo(void) {
    /* Find '>' redirect operator */
    int redir = -1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '>' && argv[i][1] == '\0') { redir = i; break; }
    }
    static char b[LINE_MAX];
    size_t pos = 0;
    int end = (redir >= 0) ? redir : argc;
    for (int i = 1; i < end; i++) {
        if (i > 1 && pos + 1 < LINE_MAX) b[pos++] = ' ';
        size_t l = str_len(argv[i]);
        for (size_t j = 0; j < l && pos + 1 < LINE_MAX; j++) b[pos++] = argv[i][j];
    }
    b[pos++] = '\n'; b[pos] = '\0';
    if (redir >= 0 && redir + 1 < argc) {
        if (vfs_write(argv[redir + 1], b, (uint32_t)pos) != 0)
            vga_print_colored("echo: write failed\n", VGA_LRED, VGA_BLACK);
    } else {
        vga_print(b);
    }
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
    uint32_t fm = pmm_free_pages()  * PAGE_SIZE / 1024 / 1024;
    uint32_t tm = pmm_total_pages() * PAGE_SIZE / 1024 / 1024;
    vga_print("Memory: ");
    vga_print_colored("", VGA_LGREEN, VGA_BLACK);
    vga_print_uint(fm);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_print(" MiB free / "); vga_print_uint(tm); vga_print(" MiB total\n");
}

static void cmd_reboot(void)   { vga_print_colored("Rebooting...\n",    VGA_YELLOW, VGA_BLACK); acpi_reboot();   }
static void cmd_shutdown(void) { vga_print_colored("Shutting down...\n", VGA_YELLOW, VGA_BLACK); acpi_shutdown(); }

static void cmd_pwd(void) {
    vga_print_colored(vfs_pwd(), VGA_LCYAN, VGA_BLACK); vga_putchar('\n');
}

static void ls_cb(const char *name, int is_dir, uint32_t size, void *ud) {
    (void)size; (void)ud;
    if (is_dir) { vga_print_colored(name, VGA_LBLUE, VGA_BLACK); vga_putchar('/'); }
    else          vga_print(name);
    vga_putchar('\n');
}

static void cmd_ls(void) {
    const char *path = (argc > 1) ? argv[1] : ".";
    int r = vfs_readdir(path, ls_cb, NULL);
    if (r < 0) { vga_print_colored("ls: failed\n", VGA_LRED, VGA_BLACK); }
}

static void cmd_cd(void) {
    const char *path = (argc > 1) ? argv[1] : "/";
    if (vfs_chdir(path) != 0) {
        vga_print_colored("cd: not found: ", VGA_LRED, VGA_BLACK);
        vga_print(path); vga_putchar('\n');
    }
}

static void cmd_mkdir(void) {
    if (argc < 2) { vga_print("usage: mkdir <name>\n"); return; }
    if (vfs_mkdir(argv[1]) != 0) {
        vga_print_colored("mkdir: failed: ", VGA_LRED, VGA_BLACK);
        vga_print(argv[1]); vga_putchar('\n');
    }
}

static void cmd_touch(void) {
    if (argc < 2) { vga_print("usage: touch <name>\n"); return; }
    if (vfs_create(argv[1]) != 0) {
        vga_print_colored("touch: already exists: ", VGA_LRED, VGA_BLACK);
        vga_print(argv[1]); vga_putchar('\n');
    }
}

static void cmd_write(void) {
    if (argc < 3) { vga_print("usage: write <file> <text...>\n"); return; }
    static char b[LINE_MAX];
    size_t pos = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2 && pos + 1 < LINE_MAX) b[pos++] = ' ';
        size_t l = str_len(argv[i]);
        for (size_t j = 0; j < l && pos + 1 < LINE_MAX; j++) b[pos++] = argv[i][j];
    }
    b[pos++] = '\n'; b[pos] = '\0';
    if (vfs_write(argv[1], b, (uint32_t)pos) != 0)
        vga_print_colored("write: failed\n", VGA_LRED, VGA_BLACK);
}

static void cmd_append(void) {
    if (argc < 3) { vga_print("usage: append <file> <text...>\n"); return; }
    static char b[LINE_MAX];
    size_t pos = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2 && pos + 1 < LINE_MAX) b[pos++] = ' ';
        size_t l = str_len(argv[i]);
        for (size_t j = 0; j < l && pos + 1 < LINE_MAX; j++) b[pos++] = argv[i][j];
    }
    b[pos++] = '\n'; b[pos] = '\0';
    if (vfs_append(argv[1], b, (uint32_t)pos) != 0)
        vga_print_colored("append: failed\n", VGA_LRED, VGA_BLACK);
}

static void cmd_cat(void) {
    if (argc < 2) { vga_print("usage: cat <file>\n"); return; }
    static char b[4096];
    int n = vfs_read(argv[1], b, sizeof(b) - 1);
    if (n < 0) {
        vga_print_colored("cat: not found\n", VGA_LRED, VGA_BLACK);
        return;
    }
    b[n] = '\0';
    vga_print(b);
    if (n > 0 && b[n-1] != '\n') vga_putchar('\n');
}

static void cmd_rm(void) {
    if (argc < 2) { vga_print("usage: rm <file>\n"); return; }
    if (vfs_delete(argv[1]) != 0) {
        vga_print_colored("rm: failed: ", VGA_LRED, VGA_BLACK);
        vga_print(argv[1]); vga_putchar('\n');
    }
}

static void cmd_cp(void) {
    if (argc < 3) { vga_print("usage: cp <src> <dst>\n"); return; }
    static char b[8192];
    int n = vfs_read(argv[1], b, sizeof(b));
    if (n < 0) { vga_print_colored("cp: cannot read source\n", VGA_LRED, VGA_BLACK); return; }
    if (vfs_write(argv[2], b, (uint32_t)n) != 0)
        vga_print_colored("cp: write failed\n", VGA_LRED, VGA_BLACK);
}

static void cmd_mv(void) {
    if (argc < 3) { vga_print("usage: mv <src> <dst>\n"); return; }
    static char b[8192];
    int n = vfs_read(argv[1], b, sizeof(b));
    if (n < 0) { vga_print_colored("mv: cannot read source\n", VGA_LRED, VGA_BLACK); return; }
    if (vfs_write(argv[2], b, (uint32_t)n) != 0) {
        vga_print_colored("mv: write failed\n", VGA_LRED, VGA_BLACK); return;
    }
    vfs_delete(argv[1]);
}

static void cmd_whoami(void) { vga_print("root\n"); }
static void cmd_uname(void)  { vga_print("AusverseOS x86_64 v0.2\n"); }

static void cmd_ifconfig(void) {
    if (!e1000_present()) {
        vga_print_colored("No network adapter\n", VGA_LRED, VGA_BLACK);
        return;
    }
    uint8_t mac[6], ip[4];
    net_get_mac(mac);
    net_get_ip(ip);
    vga_print("MAC: ");
    for (int i = 0; i < 6; i++) {
        vga_print_colored("", VGA_LCYAN, VGA_BLACK);
        /* print hex byte */
        const char *h = "0123456789abcdef";
        vga_putchar(h[mac[i] >> 4]);
        vga_putchar(h[mac[i] & 0xF]);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        if (i < 5) vga_putchar(':');
    }
    vga_putchar('\n');
    vga_print("IP:  ");
    vga_print_colored("", VGA_LGREEN, VGA_BLACK);
    for (int i = 0; i < 4; i++) {
        vga_print_uint(ip[i]);
        if (i < 3) vga_putchar('.');
    }
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_putchar('\n');
}

static void cmd_ping(void) {
    if (argc < 2) { vga_print("usage: ping <ip>\n"); return; }
    /* Parse dotted-decimal */
    const char *s = argv[1];
    uint32_t ip = 0;
    for (int oct = 0; oct < 4; oct++) {
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
        if (*s == '.') s++;
        ip = (ip << 8) | (v & 0xFF);
    }
    ip4_t target = htonl(ip);
    vga_print("PING "); vga_print(argv[1]); vga_print(" ...\n");
    int ms = net_ping(target, 3000);
    if (ms < 0) {
        vga_print_colored("Request timeout\n", VGA_LRED, VGA_BLACK);
    } else {
        vga_print("Reply from ");
        vga_print(argv[1]);
        vga_print(": time=");
        vga_print_uint((uint32_t)ms);
        vga_print(" ms\n");
    }
}

static void cmd_pkg_sh(void) { cmd_pkg(argc, argv); }

/* Convert n to decimal string; returns number of chars written. */
static int32_t fmt_u32(char *buf, int32_t cap, uint32_t n) {
    char tmp[12]; int32_t i = 0;
    if (!n) { tmp[i++] = '0'; }
    else { while (n && i < 12) { tmp[i++] = '0' + (char)(n % 10); n /= 10; } }
    int32_t j = 0;
    while (i > 0 && j < cap - 1) buf[j++] = tmp[--i];
    if (j < cap) buf[j] = '\0';
    return j;
}

/* Append a string literal into buf[pos..cap]; returns new pos. */
static int32_t fmt_str(char *buf, int32_t cap, int32_t pos, const char *s) {
    while (*s && pos < cap - 1) buf[pos++] = *s++;
    buf[pos] = '\0';
    return pos;
}

static void welcome_paint(struct surface *s, void *ud) {
    (void)ud;

    /* Layout constants */
    const int32_t lx = 20;           /* label column x */
    const int32_t vx = 160;          /* value column x */
    const int32_t ls = FONT_HEIGHT + 8; /* line spacing */
    int32_t y = 18;

    /* ---- Title ---- */
    surf_text(s, lx, y, "AUSVERSEOS v0.2", COL_ACCENT, COL_WIN_FACE);
    y += ls;
    surf_hline(s, lx, y - 4, (int32_t)s->width - lx * 2, COL_WIN_BORDER_A);

    /* ---- Uptime ---- */
    y += 4;
    uint64_t secs = pit_ticks() / PIT_HZ;
    uint32_t hh = (uint32_t)(secs / 3600);
    uint32_t mm = (uint32_t)((secs / 60) % 60);
    uint32_t ss = (uint32_t)(secs % 60);
    char uptime[9];
    uptime[0] = '0' + (char)((hh / 10) % 10); uptime[1] = '0' + (char)(hh % 10); uptime[2] = ':';
    uptime[3] = '0' + (char)((mm / 10) % 10); uptime[4] = '0' + (char)(mm % 10); uptime[5] = ':';
    uptime[6] = '0' + (char)((ss / 10) % 10); uptime[7] = '0' + (char)(ss % 10); uptime[8] = '\0';
    surf_text(s, lx, y, "UPTIME",        COL_LABEL,  COL_WIN_FACE);
    surf_text(s, vx, y, uptime,          COL_WIN_TEXT, COL_WIN_FACE);
    y += ls;

    /* ---- Memory ---- */
    uint32_t total_mb = pmm_total_pages() * PAGE_SIZE / 1024 / 1024;
    uint32_t free_mb  = pmm_free_pages()  * PAGE_SIZE / 1024 / 1024;
    uint32_t used_mb  = total_mb - free_mb;
    char mem[48]; int32_t p = 0;
    p += fmt_u32(mem + p, 48 - p, used_mb);
    p  = fmt_str(mem, 48, p, " MB / ");
    p += fmt_u32(mem + p, 48 - p, total_mb);
         fmt_str(mem, 48, p, " MB");
    surf_text(s, lx, y, "MEMORY",        COL_LABEL,  COL_WIN_FACE);
    surf_text(s, vx, y, mem,             COL_WIN_TEXT, COL_WIN_FACE);
    y += ls;

    /* ---- Free pages ---- */
    char pages[32]; int32_t pp = 0;
    pp += fmt_u32(pages + pp, 32 - pp, pmm_free_pages());
    fmt_str(pages, 32, pp, " pages");
    surf_text(s, lx, y, "FREE MEM",      COL_LABEL,  COL_WIN_FACE);
    surf_text(s, vx, y, pages,           COL_WIN_TEXT, COL_WIN_FACE);
    y += ls;

    /* ---- Static info ---- */
    surf_text(s, lx, y, "ARCH",          COL_LABEL,  COL_WIN_FACE);
    surf_text(s, vx, y, "x86-64",        COL_WIN_TEXT, COL_WIN_FACE);
    y += ls;

    surf_text(s, lx, y, "STORAGE",       COL_LABEL,  COL_WIN_FACE);
    surf_text(s, vx, y, "FAT32",         COL_WIN_TEXT, COL_WIN_FACE);
    y += ls;

    surf_text(s, lx, y, "INPUT",         COL_LABEL,  COL_WIN_FACE);
    surf_text(s, vx, y, "PS/2 KB + MOUSE", COL_WIN_TEXT, COL_WIN_FACE);
    y += ls + 8;

    /* ---- Footer ---- */
    surf_hline(s, lx, y - 4, (int32_t)s->width - lx * 2, COL_WIN_BORDER_A);
    surf_text(s, lx, y, "CLOSE WINDOW TO RETURN TO SHELL",
              COL_LABEL, COL_WIN_FACE);
}

static void cmd_gui(void) {
    int32_t sw = (int32_t)render_width();
    int32_t sh = (int32_t)render_height();
    wm_init();
    wm_open("Welcome to AusverseOS",
            sw / 2 - 260, sh / 2 - 160, 520, 320,
            welcome_paint, NULL, NULL);
    wm_run();
    shell_draw_header();
}

static void cmd_exec(void) {
    if (argc < 2) { vga_print("usage: exec <file>\n"); return; }
    int ret = elf_exec(argv[1]);
    if (ret < 0) return;
    vga_print("exit: ");
    vga_print_uint((uint32_t)ret);
    vga_putchar('\n');
}

static void cmd_hexdump(void) {
    if (argc < 2) { vga_print("usage: hexdump <file>\n"); return; }
    static char b[4096];
    int n = vfs_read(argv[1], b, sizeof(b));
    if (n < 0) { vga_print_colored("hexdump: not found\n", VGA_LRED, VGA_BLACK); return; }
    for (int i = 0; i < n; i += 16) {
        print_hex32((uint32_t)i); vga_print(": ");
        for (int j = 0; j < 16; j++) {
            if (i + j < n) { print_hex_byte((uint8_t)b[i + j]); vga_putchar(' '); }
            else             vga_print("   ");
            if (j == 7) vga_putchar(' ');
        }
        vga_print(" |");
        for (int j = 0; j < 16 && i + j < n; j++) {
            char ch = b[i + j];
            vga_putchar((ch >= ' ' && ch <= '~') ? ch : '.');
        }
        vga_print("|\n");
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                       */
/* ------------------------------------------------------------------ */

struct command { const char *name; void (*fn)(void); };

static const struct command commands[] = {
    {"help",cmd_help},{"clear",cmd_clear},{"echo",cmd_echo},
    {"uptime",cmd_uptime},{"mem",cmd_mem},
    {"reboot",cmd_reboot},{"shutdown",cmd_shutdown},
    {"pwd",cmd_pwd},{"ls",cmd_ls},{"cd",cmd_cd},
    {"mkdir",cmd_mkdir},{"touch",cmd_touch},
    {"write",cmd_write},{"append",cmd_append},
    {"cat",cmd_cat},{"rm",cmd_rm},
    {"cp",cmd_cp},{"mv",cmd_mv},
    {"whoami",cmd_whoami},{"uname",cmd_uname},
    {"hexdump",cmd_hexdump},{"exec",cmd_exec},
    {"ifconfig",cmd_ifconfig},{"ping",cmd_ping},
    {"pkg",cmd_pkg_sh},
    {"gui",cmd_gui},
};
#define NUM_COMMANDS (sizeof(commands)/sizeof(commands[0]))

static void dispatch(void) {
    if (argc == 0) return;
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (str_eq(argv[0], commands[i].name)) { commands[i].fn(); return; }
    }
    vga_print_colored("Unknown command: ", VGA_LRED, VGA_BLACK);
    vga_print(argv[0]);
    vga_print_colored("  (type 'help')\n", VGA_DGREY, VGA_BLACK);
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* ------------------------------------------------------------------ */
/* Shell entry point                                                    */
/* ------------------------------------------------------------------ */

void shell_run(void) {
    shell_draw_header();

    for (;;) {
        vga_print_colored(vfs_pwd(), VGA_LCYAN, VGA_BLACK);
        vga_print_colored(" > ", VGA_LGREEN, VGA_BLACK);
        vga_set_color(VGA_WHITE, VGA_BLACK);

        const char *input = readline();
        history_push(input);
        parse(input);
        dispatch();
    }
}

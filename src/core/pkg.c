#include <stdint.h>
#include "core/pkg.h"
#include "net/net.h"
#include "fs/vfs.h"
#include "drivers/vga.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
static uint32_t str_len(const char *s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}
static void str_cpy(char *d, const char *s, uint32_t max) {
    uint32_t i = 0;
    while (i + 1 < max && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Download buffer (static — avoids heap pressure)                    */
/* ------------------------------------------------------------------ */

#define DL_BUF_SIZE (256 * 1024)   /* 256 KB max package size */
static uint8_t dl_buf[DL_BUF_SIZE];

/* ------------------------------------------------------------------ */
/* Index parsing                                                       */
/* ------------------------------------------------------------------ */

/* Parse one line of the index: "name version path\n"
 * Returns pointer to next line, or NULL at end. */
static const char *parse_line(const char *p,
                               char *name, char *ver, char *path) {
    name[0] = ver[0] = path[0] = '\0';
    /* skip whitespace / blank lines */
    while (*p == ' ' || *p == '\n' || *p == '\r') p++;
    if (!*p) return NULL;

    /* name */
    int i = 0;
    while (*p && *p != ' ' && *p != '\n' && i < 63) name[i++] = *p++;
    name[i] = '\0';
    while (*p == ' ') p++;

    /* version */
    i = 0;
    while (*p && *p != ' ' && *p != '\n' && i < 31) ver[i++] = *p++;
    ver[i] = '\0';
    while (*p == ' ') p++;

    /* path */
    i = 0;
    while (*p && *p != '\n' && *p != '\r' && i < 127) path[i++] = *p++;
    path[i] = '\0';

    while (*p == '\n' || *p == '\r') p++;
    return p;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void pkg_list(void) {
    static uint8_t idx[4096];
    vga_print("Fetching package index...\n");
    int n = http_get_buf(PKG_SERVER, PKG_PORT, PKG_INDEX_PATH, idx, sizeof(idx) - 1);
    if (n < 0) {
        vga_print_colored("pkg: failed to fetch index\n", VGA_LRED, VGA_BLACK);
        return;
    }
    idx[n] = '\0';

    vga_print_colored("Available packages:\n", VGA_YELLOW, VGA_BLACK);
    const char *p = (const char *)idx;
    char name[64], ver[32], path[128];
    while ((p = parse_line(p, name, ver, path)) != NULL) {
        if (!name[0]) continue;
        vga_print_colored("  ", VGA_WHITE, VGA_BLACK);
        vga_print_colored(name, VGA_LCYAN, VGA_BLACK);
        vga_print("  ");
        vga_print(ver);
        vga_putchar('\n');
    }
}

static void pkg_install(const char *pkg_name) {
    static uint8_t idx[4096];
    vga_print("Fetching package index...\n");
    int n = http_get_buf(PKG_SERVER, PKG_PORT, PKG_INDEX_PATH, idx, sizeof(idx) - 1);
    if (n < 0) {
        vga_print_colored("pkg: failed to fetch index\n", VGA_LRED, VGA_BLACK);
        return;
    }
    idx[n] = '\0';

    /* Find the package */
    const char *p = (const char *)idx;
    char name[64], ver[32], path[128];
    path[0] = '\0';
    while ((p = parse_line(p, name, ver, path)) != NULL) {
        if (str_eq(name, pkg_name)) break;
        path[0] = '\0';
    }
    if (!path[0]) {
        vga_print_colored("pkg: package not found: ", VGA_LRED, VGA_BLACK);
        vga_print(pkg_name); vga_putchar('\n');
        return;
    }

    /* Download */
    vga_print("Downloading ");
    vga_print(pkg_name); vga_print(" ");
    vga_print(ver); vga_print("...\n");

    int bytes = http_get_buf(PKG_SERVER, PKG_PORT, path, dl_buf, DL_BUF_SIZE - 1);
    if (bytes < 0) {
        vga_print_colored("pkg: download failed\n", VGA_LRED, VGA_BLACK);
        return;
    }

    /* Build install path: /bin/<name> */
    static char install_path[128];
    str_cpy(install_path, PKG_INSTALL_DIR "/", 128);
    uint32_t prefix_len = str_len(install_path);
    str_cpy(install_path + prefix_len, pkg_name, 128 - prefix_len);

    /* Ensure /bin exists */
    vfs_mkdir(PKG_INSTALL_DIR);

    /* Write to filesystem */
    if (vfs_write(install_path, (char *)dl_buf, (uint32_t)bytes) != 0) {
        vga_print_colored("pkg: install failed (filesystem error)\n",
                          VGA_LRED, VGA_BLACK);
        return;
    }

    vga_print_colored("[OK] Installed: ", VGA_LGREEN, VGA_BLACK);
    vga_print(install_path); vga_putchar('\n');
}

/* ------------------------------------------------------------------ */
/* Entry point called from shell                                       */
/* ------------------------------------------------------------------ */

void cmd_pkg(int argc, char **argv) {
    if (!e1000_present()) {
        vga_print_colored("pkg: no network adapter found\n", VGA_LRED, VGA_BLACK);
        return;
    }
    if (argc < 2) {
        vga_print("usage: pkg <list|install> [name]\n");
        return;
    }
    if (str_eq(argv[1], "list")) {
        pkg_list();
    } else if (str_eq(argv[1], "install") && argc >= 3) {
        pkg_install(argv[2]);
    } else {
        vga_print("usage: pkg <list|install> [name]\n");
    }
}

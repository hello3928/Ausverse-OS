#include <stdint.h>
#include <stddef.h>
#include "core/elf.h"
#include "fs/vfs.h"
#include "core/heap.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"

/* ------------------------------------------------------------------ */
/* Kernel API implementation                                           */
/* ------------------------------------------------------------------ */

static int kapi_getchar(void) {
    char c;
    do { __asm__ volatile ("hlt"); c = keyboard_getchar(); } while (!c);
    return (int)(unsigned char)c;
}

static kapi_t g_kapi = {
    .print    = vga_print,
    .putchar  = vga_putchar,
    .getchar  = kapi_getchar,
    .readfile = vfs_read,
    .writefile= (int (*)(const char *, const void *, uint32_t))vfs_write,
    .listdir  = vfs_readdir,
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void elf_memcpy(uint8_t *dst, const uint8_t *src, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
}

static void elf_memzero(uint8_t *dst, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) dst[i] = 0;
}

/* ------------------------------------------------------------------ */
/* elf_exec                                                            */
/* ------------------------------------------------------------------ */

int elf_exec(const char *path) {
    /* --- 1. Read file into a temporary heap buffer ---- */
    uint32_t max_size = 1024 * 1024;   /* 1 MB max ELF */
    uint8_t *fbuf = (uint8_t *)kmalloc(max_size);
    if (!fbuf) {
        vga_print_colored("exec: out of memory\n", 4, 0);
        return -1;
    }

    int fsz = vfs_read(path, fbuf, max_size);
    if (fsz < (int)sizeof(Elf64_Ehdr)) {
        vga_print_colored("exec: cannot read file\n", 4, 0);
        kfree(fbuf);
        return -1;
    }

    /* --- 2. Validate ELF header --- */
    Elf64_Ehdr *eh = (Elf64_Ehdr *)fbuf;

    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        vga_print_colored("exec: not an ELF file\n", 4, 0);
        kfree(fbuf);
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64) {
        vga_print_colored("exec: not a 64-bit ELF\n", 4, 0);
        kfree(fbuf);
        return -1;
    }
    if (eh->e_machine != EM_X86_64) {
        vga_print_colored("exec: not x86-64\n", 4, 0);
        kfree(fbuf);
        return -1;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        vga_print_colored("exec: not an executable ELF\n", 4, 0);
        kfree(fbuf);
        return -1;
    }
    if (eh->e_phnum == 0) {
        vga_print_colored("exec: no program headers\n", 4, 0);
        kfree(fbuf);
        return -1;
    }

    /* --- 3. Find VMA range across all PT_LOAD segments --- */
    uint64_t min_vaddr = (uint64_t)-1;
    uint64_t max_vaddr = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(fbuf + eh->e_phoff +
                                         (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
        uint64_t end = ph->p_vaddr + ph->p_memsz;
        if (end > max_vaddr) max_vaddr = end;
    }

    if (min_vaddr == (uint64_t)-1) {
        vga_print_colored("exec: no loadable segments\n", 4, 0);
        kfree(fbuf);
        return -1;
    }

    uint64_t load_size = max_vaddr - min_vaddr;

    /* --- 4. Allocate load buffer --- */
    uint8_t *load_base = (uint8_t *)kmalloc((size_t)load_size);
    if (!load_base) {
        vga_print_colored("exec: out of memory for segments\n", 4, 0);
        kfree(fbuf);
        return -1;
    }
    elf_memzero(load_base, load_size);

    /* load bias: maps vaddr → physical address in load_base */
    int64_t bias = (int64_t)(uintptr_t)load_base - (int64_t)min_vaddr;

    /* --- 5. Copy PT_LOAD segments --- */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(fbuf + eh->e_phoff +
                                         (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        uint8_t *dst = (uint8_t *)(uintptr_t)((int64_t)ph->p_vaddr + bias);
        elf_memcpy(dst, fbuf + ph->p_offset, ph->p_filesz);
        /* memsz > filesz means BSS — already zeroed above */
    }

    /* --- 6. Call entry point --- */
    uint64_t entry_vaddr = eh->e_entry + (uint64_t)bias;
    typedef int (*entry_fn_t)(kapi_t *);
    entry_fn_t entry = (entry_fn_t)(uintptr_t)entry_vaddr;

    int ret = entry(&g_kapi);

    /* --- 7. Cleanup --- */
    kfree(load_base);
    kfree(fbuf);

    return ret;
}

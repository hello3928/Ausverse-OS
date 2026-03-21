#pragma once
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* ELF64 structures                                                    */
/* ------------------------------------------------------------------ */

#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define ET_DYN      3
#define EM_X86_64   62
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

/* ------------------------------------------------------------------ */
/* Kernel API struct passed to every ELF program at entry             */
/* ------------------------------------------------------------------ */

typedef struct {
    void (*print)(const char *s);
    void (*putchar)(char c);
    int  (*getchar)(void);
    int  (*readfile)(const char *path, void *buf, uint32_t max);
    int  (*writefile)(const char *path, const void *buf, uint32_t len);
    int  (*listdir)(const char *path,
                    void (*cb)(const char *name, int is_dir,
                               uint32_t size, void *ud),
                    void *ud);
} kapi_t;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int elf_exec(const char *path);

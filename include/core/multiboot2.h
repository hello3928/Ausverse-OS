#pragma once
#include <stdint.h>

#define MB2_MAGIC              0x36D76289
#define MB2_TAG_END            0
#define MB2_TAG_MMAP           6
#define MB2_TAG_FRAMEBUFFER    8
#define MB2_MMAP_AVAILABLE     1
#define MB2_FB_TYPE_RGB        1

/* Generic tag header — every tag starts with these two fields */
struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

/* A single entry in the memory map */
struct mb2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;      /* 1 = usable RAM, anything else = reserved */
    uint32_t reserved;
} __attribute__((packed));

/* Tag type 6 — memory map */
struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
} __attribute__((packed));

/* Tag type 8 — framebuffer info provided by GRUB */
struct mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;     /* bytes per scan line */
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;       /* bits per pixel */
    uint8_t  fb_type;   /* 1 = RGB */
    uint16_t reserved;
} __attribute__((packed));

/* Multiboot2 info header (at the address GRUB passes in ebx) */
struct mb2_info {
    uint32_t total_size;
    uint32_t reserved;
    struct mb2_tag tags[];
} __attribute__((packed));

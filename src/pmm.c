#include <stdint.h>
#include "../include/pmm.h"
#include "../include/multiboot2.h"

/*
 * Bitmap physical memory manager.
 *
 * One bit per 4 KiB page. 0 = free, 1 = used.
 * Supports up to 4 GiB of addressable RAM (1M pages = 128 KiB bitmap).
 */

#define MAX_PAGES    (0x100000)         /* 4 GiB / 4 KiB = 1,048,576 pages */
#define BITMAP_BYTES (MAX_PAGES / 8)    /* 128 KiB                          */

static uint8_t bitmap[BITMAP_BYTES];
static uint32_t total_pages = 0;
static uint32_t free_pages  = 0;

/* Symbols exported by the linker script */
extern uint8_t kernel_end;

/* ------------------------------------------------------------------ */
/* Bitmap helpers                                                       */
/* ------------------------------------------------------------------ */

static inline void bitmap_set(uint32_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bitmap_clear(uint32_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int bitmap_test(uint32_t page) {
    return bitmap[page / 8] & (1 << (page % 8));
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/*
 * pmm_init — walk the Multiboot2 memory map and set up the bitmap.
 *
 * Strategy:
 *   1. Mark every page as used (safe default).
 *   2. For each usable region in the memory map, mark those pages free.
 *   3. Re-mark page 0 as used (guards against null-pointer dereferences).
 *   4. Re-mark all kernel pages as used.
 */
void pmm_init(struct mb2_info *mbi) {
    /* Start with everything marked used */
    for (uint32_t i = 0; i < BITMAP_BYTES; i++)
        bitmap[i] = 0xFF;

    /* Walk Multiboot2 tags to find the memory map */
    struct mb2_tag *tag = (struct mb2_tag *)mbi->tags;

    while (tag->type != MB2_TAG_END) {
        if (tag->type == MB2_TAG_MMAP) {
            struct mb2_tag_mmap *mmap = (struct mb2_tag_mmap *)tag;
            uint32_t num_entries = (mmap->size - sizeof(struct mb2_tag_mmap))
                                   / mmap->entry_size;

            for (uint32_t i = 0; i < num_entries; i++) {
                struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)
                    ((uint8_t *)mmap->entries + i * mmap->entry_size);

                if (e->type != MB2_MMAP_AVAILABLE)
                    continue;

                /* Only handle memory within our 4 GiB window */
                uint64_t base = e->base_addr;
                uint64_t end  = base + e->length;
                if (base >= (uint64_t)MAX_PAGES * PAGE_SIZE)
                    continue;
                if (end > (uint64_t)MAX_PAGES * PAGE_SIZE)
                    end = (uint64_t)MAX_PAGES * PAGE_SIZE;

                /* Align base up and end down to page boundaries */
                uint32_t first = (uint32_t)((base + PAGE_SIZE - 1) / PAGE_SIZE);
                uint32_t last  = (uint32_t)(end / PAGE_SIZE);

                for (uint32_t p = first; p < last; p++) {
                    if (bitmap_test(p)) {
                        bitmap_clear(p);
                        free_pages++;
                        total_pages++;
                    }
                }
            }
        }

        /* Advance to next tag (each tag is 8-byte aligned) */
        uint32_t next = (uint32_t)tag + tag->size;
        next = (next + 7) & ~7u;
        tag = (struct mb2_tag *)next;
    }

    /* Guard page 0 — catches null pointer dereferences */
    if (!bitmap_test(0)) {
        bitmap_set(0);
        free_pages--;
    }

    /* Mark all kernel pages as used */
    uint32_t kernel_end_page = ((uint32_t)&kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t p = 0; p < kernel_end_page; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            free_pages--;
        }
    }
}

/*
 * pmm_alloc_page — find the first free page, mark it used, return its
 * physical address. Returns 0 if out of memory.
 */
uint32_t pmm_alloc_page(void) {
    for (uint32_t i = 0; i < BITMAP_BYTES; i++) {
        if (bitmap[i] == 0xFF)
            continue;   /* all 8 pages in this byte are used */

        for (int bit = 0; bit < 8; bit++) {
            uint32_t page = i * 8 + bit;
            if (!bitmap_test(page)) {
                bitmap_set(page);
                free_pages--;
                return page * PAGE_SIZE;
            }
        }
    }
    return 0; /* out of memory */
}

/*
 * pmm_free_page — release a previously allocated page.
 */
void pmm_free_page(uint32_t addr) {
    uint32_t page = addr / PAGE_SIZE;
    if (bitmap_test(page)) {
        bitmap_clear(page);
        free_pages++;
    }
}

uint32_t pmm_free_pages(void)  { return free_pages;  }
uint32_t pmm_total_pages(void) { return total_pages;  }

#include <stddef.h>
#include <stdint.h>
#include "../include/heap.h"
#include "../include/pmm.h"

/*
 * Simple free-list heap allocator.
 *
 * Layout in memory:
 *
 *   [ block_header | .... data .... ] [ block_header | .... data .... ] ...
 *
 * Every allocation is preceded by a block_header. The header stores the
 * size of the data region, a free flag, and a pointer to the next block.
 *
 * Operations:
 *   kmalloc — first-fit: walk the list, return the first free block that
 *             is large enough. Split the block if there is enough leftover
 *             space for a new header + at least MIN_SPLIT bytes of data.
 *
 *   kfree   — mark the block free, then coalesce all adjacent free blocks
 *             to prevent fragmentation.
 */

#define HEAP_INITIAL_PAGES  16              /* 64 KiB initial heap */
#define MIN_SPLIT           16              /* minimum useful leftover size */

struct block_header {
    size_t             size;    /* bytes of data (not including this header) */
    int                free;    /* 1 = available, 0 = in use */
    struct block_header *next;  /* next block in the list, NULL = end */
};

#define HEADER_SIZE sizeof(struct block_header)

static struct block_header *heap_head = NULL;

/* ------------------------------------------------------------------ */

static struct block_header *alloc_pages(size_t n, size_t *actual) {
    uint32_t first = pmm_alloc_page();
    if (!first) { *actual = 0; return NULL; }

    size_t got = 1;
    for (size_t i = 1; i < n; i++) {
        uint32_t next = pmm_alloc_page();
        if (!next) break;
        if (next != first + i * PAGE_SIZE) {
            pmm_free_page(next); /* return non-contiguous page to PMM */
            break;
        }
        got++;
    }

    *actual = got;
    return (struct block_header *)(uintptr_t)first;
}

void heap_init(void) {
    size_t got;
    struct block_header *start = alloc_pages(HEAP_INITIAL_PAGES, &got);
    if (!start || got == 0) return;

    start->size = (got * PAGE_SIZE) - HEADER_SIZE;
    start->free = 1;
    start->next = NULL;
    heap_head   = start;
}

void *kmalloc(size_t size) {
    if (!size || !heap_head) return NULL;

    /* Align size to 8 bytes so all allocations are naturally aligned */
    size = (size + 7) & ~(size_t)7;

    struct block_header *cur = heap_head;

    while (cur) {
        if (cur->free && cur->size >= size) {
            /* Can we split this block? */
            if (cur->size >= size + HEADER_SIZE + MIN_SPLIT) {
                struct block_header *split =
                    (struct block_header *)((uint8_t *)cur + HEADER_SIZE + size);
                split->size = cur->size - size - HEADER_SIZE;
                split->free = 1;
                split->next = cur->next;
                cur->next   = split;
                cur->size   = size;
            }
            cur->free = 0;
            return (void *)((uint8_t *)cur + HEADER_SIZE);
        }
        cur = cur->next;
    }

    return NULL; /* out of heap space */
}

void kfree(void *ptr) {
    if (!ptr) return;

    struct block_header *blk =
        (struct block_header *)((uint8_t *)ptr - HEADER_SIZE);
    blk->free = 1;

    /* Coalesce: merge this block with all following free blocks */
    while (blk->next && blk->next->free) {
        blk->size += HEADER_SIZE + blk->next->size;
        blk->next  = blk->next->next;
    }
}

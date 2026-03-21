#pragma once
#include <stdint.h>
#include "multiboot2.h"

#define PAGE_SIZE 4096

void     pmm_init(struct mb2_info *mbi);
uint32_t pmm_alloc_page(void);   /* returns physical address, 0 on failure */
void     pmm_free_page(uint32_t addr);
uint32_t pmm_free_pages(void);
uint32_t pmm_total_pages(void);

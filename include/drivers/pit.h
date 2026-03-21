#pragma once
#include <stdint.h>

#define PIT_HZ 100      /* timer frequency: 100 ticks per second */

void     pit_init(void);
void     pit_tick(void);
uint64_t pit_ticks(void);

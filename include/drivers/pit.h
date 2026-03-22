#pragma once
#include <stdint.h>

#define PIT_HZ 60       /* 60 ticks/sec — minimum needed for 30fps + uptime.       */
                        /* TCG has no hardware IRQ path; every tick is fully        */
                        /* software-emulated. 500Hz was 8x more overhead than this. */

void     pit_init(void);
void     pit_tick(void);
uint64_t pit_ticks(void);

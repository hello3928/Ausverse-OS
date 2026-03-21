#pragma once
#include <stdint.h>

int  usb_tablet_init(void);   /* returns 0 on success, -1 if no tablet found */
int  usb_tablet_present(void);
void usb_tablet_get(int32_t *x, int32_t *y, uint8_t *buttons);

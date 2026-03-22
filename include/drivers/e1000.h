#pragma once
#include <stdint.h>

/* Intel 82540EM (E1000) NIC driver.
 * Vendor 0x8086, Device 0x100E.
 * VirtualBox: Settings → Network → Intel PRO/1000 MT Desktop */

int     e1000_init(void);            /* 0 on success, -1 if not found */
int     e1000_send(const void *data, uint16_t len);
int     e1000_recv(void *buf, uint16_t max_len); /* returns bytes, 0 if none */
void    e1000_get_mac(uint8_t mac[6]);
int     e1000_present(void);

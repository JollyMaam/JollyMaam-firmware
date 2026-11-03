#ifndef FLASH_YC3121_H
#define FLASH_YC3121_H

#include <stdint.h>

/* What this reads, erases and writes, raw (not decrypted; flash_yc3121.c):
 * the settings store's two banks at the top of the app slot, and the data
 * pages above the slot (stock v1.01 keeps its own at 0x0107d000, 0x0107e000
 * and 0x0107f000; 0x0107c000 it leaves blank).  Nothing below: the app. */
#define YC_STORE_A    0x01074000u
#define YC_STORE_B    0x01078000u
#define YC_STORE_BANK 0x4000u
#define YC_DATA_START YC_STORE_A
#define YC_DATA_END   0x01080000u
#define YC_SECTOR     0x1000u
#define YC_PAGE       0x100u

/* 1, or 0 outside that range (or a failed erase or write).  A write may
 * cross pages: it goes a page program per page. */
int plat_flash_read(uint32_t at, void *buf, uint32_t n);
int plat_flash_erase(uint32_t at);
int plat_flash_write(uint32_t at, const void *buf, uint32_t n);

#endif

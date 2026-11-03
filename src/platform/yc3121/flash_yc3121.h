#ifndef FLASH_YC3121_H
#define FLASH_YC3121_H

#include <stdint.h>

/* The flash's data pages above the app slot (stock v1.01 keeps its own at
 * 0x0107d000, 0x0107e000 and 0x0107f000; 0x0107c000 it leaves blank).  Only
 * these are read, erased or written here: raw, not decrypted
 * (flash_yc3121.c). */
#define YC_DATA_START 0x0107c000u
#define YC_DATA_END   0x01080000u
#define YC_SECTOR     0x1000u

/* 1, or 0 outside the data pages (or a failed erase or write).  A write is
 * page programs of up to 256 bytes from the address: it keeps within a
 * page when the address is page aligned. */
int plat_flash_read(uint32_t at, void *buf, uint32_t n);
int plat_flash_erase(uint32_t at);
int plat_flash_write(uint32_t at, const void *buf, uint32_t n);

#endif

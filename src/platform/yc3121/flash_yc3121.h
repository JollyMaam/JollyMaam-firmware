#ifndef FLASH_YC3121_H
#define FLASH_YC3121_H

#include "pad.h"

/* What this reads, erases and writes, raw (not decrypted; flash_yc3121.c):
 * the settings store's two 16 KiB banks above the app slot (make: STORE_A,
 * STORE_B), and the data pages above them (stock v1.01 keeps its own at
 * 0x0107d000, 0x0107e000 and 0x0107f000; 0x0107c000 it leaves blank).
 * Raw addresses are the flash's offset + 0x01000000, as the ROM takes them. */
#define YC_DATA_START 0x01074000u
#define YC_DATA_END   0x01080000u
#define YC_PAGE       0x100u

/* plat_flash_* (pad.h) refuse what is outside that range. */

/* The app slot (CPU addresses: the code window), ciphered as the boot
 * loader writes it.  It ends where its flash would reach the raw banks:
 * window 12 (0x01060000) is flash 0x6c010..0x75010.  The updater's: */
#define YC_SLOT_START 0x01018000u
#define YC_SLOT_END   0x01060000u
#define YC_BLOCK      32u

int yc_slot_erase(u32 at);       /* the 32 KiB window from at */
/* Whole blocks from at; data in RAM (the flash is off the bus meanwhile). */
int yc_slot_write(u32 at, const u8 *data, u32 n);
void yc_slot_read(u32 at, u8 *out, u32 n);   /* blocks not written read 0xff */

#endif

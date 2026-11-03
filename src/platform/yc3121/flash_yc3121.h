#ifndef FLASH_YC3121_H
#define FLASH_YC3121_H

#include "pad.h"

/* What this reads, erases and writes, raw (not decrypted; flash_yc3121.c):
 * the settings store's two 16 KiB banks at the top of the app slot
 * (make: STORE_A, STORE_B), and the data pages above the slot (stock v1.01
 * keeps its own at 0x0107d000, 0x0107e000 and 0x0107f000; 0x0107c000 it
 * leaves blank).  Nothing below: the app (app.ld ends it here too). */
#define YC_DATA_START 0x01074000u
#define YC_DATA_END   0x01080000u
#define YC_PAGE       0x100u

/* plat_flash_* (pad.h) refuse what is outside that range. */

#endif

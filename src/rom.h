#ifndef CYCLONE2_ROM_H
#define CYCLONE2_ROM_H
#include "typedef.h"
void rom_mdelay(u32 milliseconds);
void rom_flush_dcache(const void *address, u32 size);
void rom_nvram_set_boot_state(u32 state);
void rom_chip_reset(void);
#endif

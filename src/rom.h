#ifndef CYCLONE2_ROM_H
#define CYCLONE2_ROM_H
#include "typedef.h"
void rom_mdelay(u32 milliseconds);
void rom_flush_dcache(const void *address, u32 size);
#endif

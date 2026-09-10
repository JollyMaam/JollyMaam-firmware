#ifndef CYCLONE2_ROM_H
#define CYCLONE2_ROM_H

#include "typedef.h"

/* Exported by the SDK's cpu/br23/maskrom_stubs.ld, which sdk_ld.c includes. */
extern void flush_dcache(const void *address, u32 size);
extern void nvram_set_boot_state(u32 state);

/* The SDK's accompanying cpu/br23/tools/rom.lst names these two entries, but
 * maskrom_stubs.ld omits them. Keep the exceptions explicit and centralized. */
#define CYCLONE2_ROM_MDELAY_ADDRESS          0x00111524U
#define CYCLONE2_ROM_CHIP_RESET_STUB_ADDRESS 0x001127f4U

#endif

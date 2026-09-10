#include "rom.h"

/* Exported by the SDK's cpu/br23/maskrom_stubs.ld, which sdk_ld.c includes. */
extern void flush_dcache(const void *address, u32 size);
extern void nvram_set_boot_state(u32 state);

/* The SDK's accompanying cpu/br23/tools/rom.lst names these two entries, but
 * maskrom_stubs.ld omits them. Keep the exceptions explicit and centralized. */
#define CYCLONE2_ROM_MDELAY_ADDRESS          0x00111524U
#define CYCLONE2_ROM_CHIP_RESET_STUB_ADDRESS 0x001127f4U

/* Calls from flash cannot reach these MaskROM stubs with PI32's limited-range
 * direct CALL instruction. Volatile pointers force full-address calls. */
void rom_mdelay(u32 milliseconds)
{
    void (* volatile function)(u32) =
        (void (*)(u32))CYCLONE2_ROM_MDELAY_ADDRESS;
    function(milliseconds);
}

void rom_flush_dcache(const void *address, u32 size)
{
    void (* volatile function)(const void *, u32) = flush_dcache;
    function(address, size);
}

void rom_nvram_set_boot_state(u32 state)
{
    void (* volatile function)(u32) = nvram_set_boot_state;
    function(state);
}

void rom_chip_reset(void)
{
    void (* volatile function)(void) =
        (void (*)(void))CYCLONE2_ROM_CHIP_RESET_STUB_ADDRESS;
    function();
}


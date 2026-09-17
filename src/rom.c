#include "rom.h"

/* Exported by the SDK's cpu/br23/maskrom_stubs.ld, which sdk_ld.c includes. */
extern void flush_dcache(const void *address, u32 size);

/* The SDK's cpu/br23/tools/rom.lst names this entry, but maskrom_stubs.ld
 * omits it. Keep the exception explicit and centralized. */
#define CYCLONE2_ROM_MDELAY_ADDRESS          0x00111524U

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

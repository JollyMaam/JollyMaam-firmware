#include "rom.h"

extern void flush_dcache(const void *address, u32 size);

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

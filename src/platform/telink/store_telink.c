#include "sdk.h"
#include "board.h"

/* core/store.c's flash, by the SDK's: its page write splits at pages. */

int plat_flash_read(u32 at, void *buf, u32 n)
{
    flash_read_page(at, n, buf);
    return 1;
}

int plat_flash_write(u32 at, const void *buf, u32 n)
{
    flash_write_page(at, n, (u8 *)buf);
    return 1;
}

int plat_flash_erase(u32 at)
{
    flash_erase_sector(at);
    return 1;
}

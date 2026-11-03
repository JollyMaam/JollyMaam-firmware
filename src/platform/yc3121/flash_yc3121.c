#include "pad.h"
#include "yc3121.h"
#include "flash_yc3121.h"
#include "store.h"

/* The data pages, by the ROM's QSPI routines (Yichip's rom_api.h) as stock
 * v1.01 calls them (its 0x010286e8 read, 0x01028724 erase, 0x0102874c
 * write): interrupts off, bit 7 of the QSPI control word 0xf881c clear for
 * the duration, then back.  The ROM routines take (address, length, buffer)
 * and send plain flash commands (page program 0x02, sector erase 0x20, the
 * configured read opcode), so what they write and read is the raw flash:
 * not through the decryption that code fetches and the CPU's own reads of
 * 0x01000000.. go through.  Their wrapper runs from RAM, as the flash it
 * sits in is busy meanwhile (stock instead loads its helper into the cache
 * first: ROM 0x4404). */

#define QSPI_CTL        (*(volatile uint32_t *)0xf881c)
#define ROM_ERASE_4K    ((int (*)(uint32_t))0x48b5)
#define ROM_WRITE       ((int (*)(uint32_t, uint32_t, const uint8_t *))0x47f5)
#define ROM_READ        ((int (*)(uint32_t, uint32_t, uint8_t *))0x48f7)

enum { OP_READ, OP_ERASE, OP_WRITE };

__attribute__((section(".data.flash_op"), noinline, long_call))
static int flash_op(int op, uint32_t at, uint32_t n, uint8_t *buf)
{
    uint32_t ctl = QSPI_CTL;
    int r;

    QSPI_CTL = ctl & ~0x80u;
    r = op == OP_READ ? ROM_READ(at, n, buf) : op == OP_ERASE ? ROM_ERASE_4K(at) : ROM_WRITE(at, n, buf);
    QSPI_CTL = ctl;
    return r;
}

static int run(int op, uint32_t at, uint32_t n, uint8_t *buf)
{
    int r;

    __asm__ volatile ("cpsid i" ::: "memory");
    r = flash_op(op, at, n, buf);
    __asm__ volatile ("cpsie i" ::: "memory");
    return r;
}

static int in_data(u32 at, u32 n)
{
    return at >= YC_DATA_START && n <= YC_DATA_END - YC_DATA_START && at + n <= YC_DATA_END;
}

int plat_flash_read(u32 at, void *buf, u32 n)
{
    if (!in_data(at, n)) return 0;
    run(OP_READ, at, n, buf);
    return 1;
}

int plat_flash_erase(u32 at)
{
    if (at % STORE_SECTOR || !in_data(at, STORE_SECTOR)) return 0;
    return run(OP_ERASE, at, 0, 0) == 1;
}

/* The ROM's write programs 256 bytes at a time from the address, with no
 * regard for page boundaries (a program past one wraps within the page),
 * so a page at a time here. */
int plat_flash_write(u32 at, const void *buf, u32 n)
{
    const uint8_t *p = buf;

    if (!in_data(at, n)) return 0;
    while (n) {
        uint32_t k = YC_PAGE - at % YC_PAGE;

        if (k > n) k = n;
        if (run(OP_WRITE, at, k, (uint8_t *)p) != 1) return 0;
        at += k, p += k, n -= k;
    }
    return 1;
}

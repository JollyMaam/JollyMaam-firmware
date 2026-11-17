#include <string.h>
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
/* The ciphered flash (below): Yichip's enc_write_flash (addr, buf, len,
 * 1 to write, 0), and its check of the blocks from addr to end (0: each
 * block's check word matches), both with the OTP on for the key; the
 * cache's lines from addr to end dropped. */
#define ROM_ENC_WRITE   ((int (*)(uint32_t, uint8_t *, uint32_t, uint32_t, uint32_t))0x51f1)
#define ROM_ENC_CHECK   ((int (*)(uint32_t, uint32_t))0x513d)
#define ROM_PREFETCH    ((void (*)(uint32_t, uint32_t))0x4405)
#define ROM_INIT_OTP    ((void (*)(void))0x442d)
#define ROM_DEINIT_OTP  ((void (*)(void))0x4481)

enum { OP_READ, OP_ERASE, OP_WRITE, OP_ENC_WRITE };

__attribute__((section(".data.flash_op"), noinline, long_call))
static int flash_op(int op, uint32_t at, uint32_t n, uint8_t *buf)
{
    uint32_t ctl = QSPI_CTL;
    int r;

    QSPI_CTL = ctl & ~0x80u;
    if (op == OP_ENC_WRITE) {
        ROM_INIT_OTP();
        r = ROM_ENC_WRITE(at, buf, n, 1, 0);
        ROM_DEINIT_OTP();
    } else {
        r = op == OP_READ ? ROM_READ(at, n, buf) : op == OP_ERASE ? ROM_ERASE_4K(at) : ROM_WRITE(at, n, buf);
    }
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
static int write_raw(u32 at, const uint8_t *p, u32 n)
{
    while (n) {
        uint32_t k = YC_PAGE - at % YC_PAGE;

        if (k > n) k = n;
        if (run(OP_WRITE, at, k, (uint8_t *)p) != 1) return 0;
        at += k, p += k, n -= k;
    }
    return 1;
}

int plat_flash_write(u32 at, const void *buf, u32 n)
{
    return in_data(at, n) && write_raw(at, buf, n);
}

/* ---- the app slot, ciphered (the updater's) ---------------------------------
 * As the boot loader writes and reads it (its 0x0100561c, 0x0100559c,
 * 0x01005518) and code fetches see it.  The ROM maps an address (its
 * 0x503c): each 32-byte block is 36 bytes of flash, data and a check word;
 * the 1024 blocks of a 32 KiB window (CPU 0x01000000 + w * 0x8000) are the
 * 0x9000 bytes of flash from w * 0x9000 + 16, in a chip-keyed order.  So a
 * window's first and last flash sectors hold 16 bytes of its neighbours',
 * which an erase writes back.  A CPU read of a block whose check word does
 * not match (erased flash among them) resets the chip: reads check first,
 * and such a block reads as 0xff. */
#define WINDOW      0x8000u
#define WINDOW_RAW  0x9000u

static uint8_t sector_copy[STORE_SECTOR];

int yc_slot_erase(u32 at)
{
    uint8_t below[16];
    u32 raw, s;

    if (at % WINDOW || at < YC_SLOT_START || at >= YC_SLOT_END) return 0;
    raw = 0x01000000u + (at - 0x01000000u) / WINDOW * WINDOW_RAW;
    run(OP_READ, raw, sizeof(below), below);
    run(OP_ERASE, raw, 0, 0);
    if (!write_raw(raw, below, sizeof(below))) return 0;
    for (s = raw + STORE_SECTOR; s < raw + WINDOW_RAW; s += STORE_SECTOR) run(OP_ERASE, s, 0, 0);
    run(OP_READ, s, sizeof(sector_copy), sector_copy);
    memset(sector_copy, 0xff, 16);
    run(OP_ERASE, s, 0, 0);
    for (raw = 0; raw < sizeof(sector_copy); raw += YC_PAGE) {   /* pages left blank: skipped */
        uint32_t i;

        for (i = 0; i < YC_PAGE && sector_copy[raw + i] == 0xff; i++) {
        }
        if (i < YC_PAGE && !write_raw(s + raw, sector_copy + raw, YC_PAGE)) return 0;
    }
    return 1;
}

int yc_slot_write(u32 at, const u8 *data, u32 n)
{
    if (at % YC_BLOCK || n % YC_BLOCK || at < YC_SLOT_START || n > YC_SLOT_END - at) return 0;
    return run(OP_ENC_WRITE, at, n, (uint8_t *)data) == 1;
}

void yc_slot_read(u32 at, u8 *out, u32 n)
{
    while (n) {
        u32 block = at & ~(YC_BLOCK - 1), k = YC_BLOCK - (at - block);
        int bad;

        if (k > n) k = n;
        plat_lock();
        ROM_INIT_OTP();
        bad = at < YC_SLOT_START || at >= YC_SLOT_END || ROM_ENC_CHECK(block, block + YC_BLOCK);
        ROM_DEINIT_OTP();
        if (!bad) ROM_PREFETCH(block, block + YC_BLOCK);
        if (bad) memset(out, 0xff, k);
        else memcpy(out, (const void *)at, k);
        plat_unlock();
        at += k, out += k, n -= k;
    }
}

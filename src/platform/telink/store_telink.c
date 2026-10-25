#include "sdk.h"
#include "board.h"

/* Settings in flash (plat_store_*): a log of records in one of two 4 KiB
 * sectors, the newest record of a key wins.  A sector starts with
 * {u32 MAGIC, u32 generation}; the one with the higher generation is in
 * use.  A record is {u16 key, u16 len, data, u16 CRC-16 of key, len and
 * data}, padded to 4 bytes: a torn one fails its CRC and ends the log.
 * When the sector is full, the newest record of every key moves to the
 * other one, whose header is written last.  Records go through a small
 * buffer, a piece at a time (the stack is short); no RAM is kept but where
 * the log ends. */

#define STORE_A   0x6f000        /* two sectors below the bonds (bt_telink.c) */
#define STORE_B   0x70000
#define SECTOR    0x1000
#define MAGIC     0x52545343u    /* "CSTR" */
#define HEADER    8
#define KEY_MAX   64             /* keys are 1..KEY_MAX-1 */
#define DATA_MAX  248            /* the most bytes a record holds */
#define EMPTY     0xffff
#define PIECE     32

static u32 sector, end;          /* the sector in use, where its log ends; 0: not read yet */

static u32 read32(u32 at)
{
    u32 v;

    flash_read_page(at, 4, (u8 *)&v);
    return v;
}

static u16 store_crc(u16 crc, const u8 *d, u32 n)
{
    u32 i;

    while (n--) {
        crc ^= *d++;
        for (i = 0; i < 8; i++) crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
    }
    return crc;
}

static u32 padded(u32 len)
{
    return (4 + len + 2 + 3) & ~3u;
}

/* The record at: its key and length; returns its size in flash, 0: none
 * (it ends the log). */
static u32 record(u32 at, u16 *key, u32 *len)
{
    u8 b[PIECE];
    u32 n, o, k;
    u16 crc;

    if (at + 4 > sector + SECTOR) return 0;
    flash_read_page(at, 4, b);
    *key = (u16)(b[0] | b[1] << 8);
    n = (u32)(b[2] | b[3] << 8);
    if (*key == EMPTY || n > DATA_MAX || at + padded(n) > sector + SECTOR) return 0;
    crc = store_crc(0xffff, b, 4);
    for (o = 0; o < n; o += k) {
        k = n - o < PIECE ? n - o : PIECE;
        flash_read_page(at + 4 + o, k, b);
        crc = store_crc(crc, b, k);
    }
    flash_read_page(at + 4 + n, 2, b);
    if (crc != (u16)(b[0] | b[1] << 8)) return 0;
    *len = n;
    return padded(n);
}

/* Which sector is in use, and where its log ends. */
static void find_log(void)
{
    u32 a = read32(STORE_A) == MAGIC ? read32(STORE_A + 4) : 0;
    u32 b = read32(STORE_B) == MAGIC ? read32(STORE_B + 4) : 0;
    u16 key;
    u32 len, n;

    if (!a && !b) {                      /* first use */
        u32 head[2] = { MAGIC, 1 };

        flash_erase_sector(STORE_A);
        flash_write_page(STORE_A, sizeof(head), (u8 *)head);
        a = 1;
    }
    sector = a >= b ? STORE_A : STORE_B;
    for (end = sector + HEADER; (n = record(end, &key, &len)); end += n) {}
    /* A torn record: nothing may be written over it, so the next write
     * compacts first. */
    if (end + 4 <= sector + SECTOR && read32(end) != 0xffffffffu) end = sector + SECTOR;
}

/* The newest record of key in the sector in use: its address, 0 none. */
static u32 newest(u16 key, u32 *len)
{
    u32 at, n, found = 0, l;
    u16 k;

    for (at = sector + HEADER; (n = record(at, &k, &l)); at += n)
        if (k == key) {
            found = at;
            *len = l;
        }
    return found;
}

/* A record at the log's end, its data from flash (from) or RAM (data). */
static void append(u32 at, u16 key, u32 len, u32 from, const u8 *data)
{
    u8 b[PIECE];
    u32 o, k;
    u16 crc;

    b[0] = (u8)key;
    b[1] = (u8)(key >> 8);
    b[2] = (u8)len;
    b[3] = (u8)(len >> 8);
    crc = store_crc(0xffff, b, 4);
    flash_write_page(at, 4, b);
    for (o = 0; o < len; o += k) {
        k = len - o < PIECE ? len - o : PIECE;
        if (data) memcpy(b, data + o, k);
        else flash_read_page(from + o, k, b);
        crc = store_crc(crc, b, k);
        flash_write_page(at + 4 + o, k, b);
    }
    b[0] = (u8)crc;
    b[1] = (u8)(crc >> 8);
    flash_write_page(at + 4 + len, 2, b);
}

/* The newest record of every key into the other sector, then it is in use. */
static void compact(void)
{
    u32 to = sector == STORE_A ? STORE_B : STORE_A, at = to + HEADER, from, len;
    u32 head[2] = { MAGIC, read32(sector + 4) + 1 };
    u16 key;

    flash_erase_sector(to);
    for (key = 1; key < KEY_MAX; key++) {
        from = newest(key, &len);
        if (!from) continue;
        append(at, key, len, from + 4, NULL);
        at += padded(len);
    }
    flash_write_page(to, sizeof(head), (u8 *)head);
    sector = to;
    end = at;
}

int plat_store_read(u16 key, void *buf, u16 len)
{
    u32 at, n;

    if (!sector) find_log();
    at = newest(key, &n);
    if (!at) return 0;
    if (n > len) n = len;
    flash_read_page(at + 4, n, buf);
    return (int)n;
}

/* The record at holds len bytes the same as data. */
static int same(u32 at, const u8 *data, u32 len)
{
    u8 b[PIECE];
    u32 o, k;

    for (o = 0; o < len; o += k) {
        k = len - o < PIECE ? len - o : PIECE;
        flash_read_page(at + 4 + o, k, b);
        if (memcmp(b, data + o, k)) return 0;
    }
    return 1;
}

int plat_store_write(u16 key, const void *buf, u16 len)
{
    u32 at, n;

    if (!key || key >= KEY_MAX || len > DATA_MAX) return 0;
    if (!sector) find_log();
    at = newest(key, &n);
    if (at && n == len && same(at, buf, len)) return len;
    if (end + padded(len) > sector + SECTOR) compact();
    if (end + padded(len) > sector + SECTOR) return 0;
    append(end, key, len, 0, buf);
    end += padded(len);
    return len;
}

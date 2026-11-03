#include <string.h>
#include "pad.h"
#include "store.h"

/* Settings in flash (plat_store_*), for the boards whose SDK has no store
 * of its own: a log of records in one of two banks of STORE_BANK bytes (a
 * whole number of 4 KiB sectors) at STORE_A and STORE_B, through the
 * platform's plat_flash_*; the newest record of a key wins.  A bank starts
 * with {u32 MAGIC, u32 generation}; the one with the higher generation is in
 * use.  A record is {u16 key, u16 len, data, u16 CRC-16 of key, len and
 * data}, padded to 4 bytes: a torn one fails its CRC and ends the log.
 * When the bank is full, the newest record of every key moves to the
 * other one, whose header is written last.  Records go through a small
 * buffer, a piece at a time (the stack is short); no RAM is kept but where
 * the log ends.  The CRCs are checked once, finding the end: a read walks
 * only the headers of the records before it. */

#if defined(UNITTEST) && !defined(STORE_A)  /* a board without the store: its geometry */
#define STORE_A    0x6f000
#define STORE_B    0x70000
#define STORE_BANK 0x1000
#endif
#if !defined(STORE_A) || !defined(STORE_B) || !defined(STORE_BANK)
#error "STORE_A, STORE_B and STORE_BANK come from make/<product>.mk"
#endif
#if STORE_BANK % STORE_SECTOR
#error "STORE_BANK is whole sectors"
#endif
_Static_assert(STORE_B >= STORE_A + STORE_BANK || STORE_A >= STORE_B + STORE_BANK,
               "the banks do not overlap");
#define MAGIC     0x52545343u    /* "CSTR" */
#define HEADER    STORE_HEADER_SIZE
#define KEY_MAX   STORE_KEY_MAX
#define DATA_MAX  STORE_DATA_MAX
#define EMPTY     0xffff
#define PIECE     16             /* on the stack: the settings write is the deepest path */

static u32 bank, end;            /* the bank in use, where its log ends; 0: not read yet */
static u32 valid;                /* the end of its whole records (end, unless one is torn) */

static u32 read32(u32 at)
{
    u32 v;

    plat_flash_read(at, &v, 4);
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
    return STORE_RECORD_SIZE(len);
}

/* The record at: its key and length; returns its size in flash, 0: none
 * (it ends the log).  check: its CRC too. */
static u32 record(u32 at, u16 *key, u32 *len, int check)
{
    u8 b[PIECE];
    u32 n, o, k;
    u16 crc;

    if (at + 4 > bank + STORE_BANK) return 0;
    plat_flash_read(at, b, 4);
    *key = (u16)(b[0] | b[1] << 8);
    n = (u32)(b[2] | b[3] << 8);
    if (*key == EMPTY || n > DATA_MAX || at + padded(n) > bank + STORE_BANK) return 0;
    *len = n;
    if (!check) return padded(n);
    crc = store_crc(0xffff, b, 4);
    for (o = 0; o < n; o += k) {
        k = n - o < PIECE ? n - o : PIECE;
        plat_flash_read(at + 4 + o, b, k);
        crc = store_crc(crc, b, k);
    }
    plat_flash_read(at + 4 + n, b, 2);
    if (crc != (u16)(b[0] | b[1] << 8)) return 0;
    return padded(n);
}

static void erase(u32 at)
{
    u32 o;

    for (o = 0; o < STORE_BANK; o += STORE_SECTOR) plat_flash_erase(at + o);
}

/* Which bank is in use, and where its log ends. */
static void find_log(void)
{
    u32 a = read32(STORE_A) == MAGIC ? read32(STORE_A + 4) : 0;
    u32 b = read32(STORE_B) == MAGIC ? read32(STORE_B + 4) : 0;
    u16 key;
    u32 len, n;

    if (!a && !b) {                      /* first use */
        u32 head[2] = { MAGIC, 1 };

        erase(STORE_A);
        plat_flash_write(STORE_A, head, sizeof(head));
        a = 1;
    }
    bank = a >= b ? STORE_A : STORE_B;
    for (end = bank + HEADER; (n = record(end, &key, &len, 1)); end += n) {}
    valid = end;
    /* A torn record: nothing may be written over it, so the next write
     * compacts first. */
    if (end + 4 <= bank + STORE_BANK && read32(end) != 0xffffffffu) end = bank + STORE_BANK;
}

/* The newest record of key in the bank in use: its address, 0 none. */
static u32 newest(u16 key, u32 *len)
{
    u32 at, n, found = 0, l;
    u16 k;

    for (at = bank + HEADER; at < valid && (n = record(at, &k, &l, 0)); at += n)
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
    plat_flash_write(at, b, 4);
    for (o = 0; o < len; o += k) {
        k = len - o < PIECE ? len - o : PIECE;
        if (data) memcpy(b, data + o, k);
        else plat_flash_read(from + o, b, k);
        crc = store_crc(crc, b, k);
        plat_flash_write(at + 4 + o, b, k);
    }
    b[0] = (u8)crc;
    b[1] = (u8)(crc >> 8);
    plat_flash_write(at + 4 + len, b, 2);
}

/* The newest record of every key into the other bank, then it is in use. */
static void compact(void)
{
    u32 to = bank == STORE_A ? STORE_B : STORE_A, at = to + HEADER, from, len;
    u32 head[2] = { MAGIC, read32(bank + 4) + 1 };
    u16 key;

    erase(to);
    for (key = 1; key < KEY_MAX; key++) {
        from = newest(key, &len);
        if (!from) continue;
        append(at, key, len, from + 4, NULL);
        at += padded(len);
    }
    plat_flash_write(to, head, sizeof(head));
    bank = to;
    end = valid = at;
}

int plat_store_read(u16 key, void *buf, u16 len)
{
    u32 at, n;

    if (!bank) find_log();
    at = newest(key, &n);
    if (!at) return 0;
    if (n > len) n = len;
    plat_flash_read(at + 4, buf, n);
    return (int)n;
}

/* The record at holds len bytes the same as data. */
static int same(u32 at, const u8 *data, u32 len)
{
    u8 b[PIECE];
    u32 o, k;

    for (o = 0; o < len; o += k) {
        k = len - o < PIECE ? len - o : PIECE;
        plat_flash_read(at + 4 + o, b, k);
        if (memcmp(b, data + o, k)) return 0;
    }
    return 1;
}

int plat_store_write(u16 key, const void *buf, u16 len)
{
    u32 at, n;

    if (!key || key >= KEY_MAX || len > DATA_MAX) return 0;
    if (!bank) find_log();
    at = newest(key, &n);
    if (at && n == len && same(at, buf, len)) return len;
    if (end + padded(len) > bank + STORE_BANK) compact();
    if (end + padded(len) > bank + STORE_BANK) return 0;
    append(end, key, len, 0, buf);
    end += padded(len);
    valid = end;
    return len;
}

#ifdef UNITTEST
#include "test.h"

/* NOR flash: a write only clears bits, an erase sets a sector to 0xff. */
static u8 flash[0x80000];
static u32 erases, writes_left = ~0u;

int plat_flash_read(u32 at, void *buf, u32 n)
{
    memcpy(buf, flash + at % sizeof(flash), n);
    return 1;
}

int plat_flash_write(u32 at, const void *buf, u32 n)
{
    const u8 *p = buf;

    for (u32 i = 0; i < n; i++) {
        if (!writes_left) return 0;          /* power lost from here on */
        writes_left--;
        flash[(at + i) % sizeof(flash)] &= p[i];
    }
    return 1;
}

int plat_flash_erase(u32 at)
{
    CHECK(at % STORE_SECTOR == 0);
    memset(flash + at % sizeof(flash), 0xff, STORE_SECTOR);
    erases++;
    return 1;
}

static void reopen(void)
{
    bank = 0;                                /* as at boot: found again from flash */
}

int main(void)
{
    u8 v[STORE_DATA_MAX], big[STORE_DATA_MAX];
    u32 i, k, round;

    memset(flash, 0xff, sizeof(flash));
    CHECK(plat_store_read(1, v, sizeof(v)) == 0);
    CHECK(erases == STORE_BANK / STORE_SECTOR);  /* first use: bank A, all of it */
    CHECK(plat_store_write(0, "x", 1) == 0);     /* key 0 and too long are refused */
    CHECK(plat_store_write(KEY_MAX, "x", 1) == 0);
    CHECK(plat_store_write(1, big, STORE_DATA_MAX + 1) == 0);

    /* The newest of a key wins, and survives a restart. */
    CHECK(plat_store_write(1, "one", 3) == 3);
    CHECK(plat_store_write(2, "two", 3) == 3);
    CHECK(plat_store_write(1, "uno", 3) == 3);
    reopen();
    CHECK(plat_store_read(1, v, sizeof(v)) == 3 && !memcmp(v, "uno", 3));
    CHECK(plat_store_read(2, v, 2) == 2 && !memcmp(v, "tw", 2));
    CHECK(plat_store_read(3, v, sizeof(v)) == 0);

    /* The same value again writes nothing. */
    k = end;
    CHECK(plat_store_write(1, "uno", 3) == 3 && end == k);

    /* Many rounds of every key at its largest: compactions back and forth,
     * nothing lost, each compaction erasing one bank. */
    for (round = 0; round < 40; round++)
        for (k = 1; k <= 12; k++) {
            memset(big, (int)(round * 16 + k), sizeof(big));
            CHECK(plat_store_write((u16)k, big, sizeof(big)) == sizeof(big));
        }
    CHECK(erases > STORE_BANK / STORE_SECTOR);
    CHECK(erases % (STORE_BANK / STORE_SECTOR) == 0);
    reopen();
    for (k = 1; k <= 12; k++) {
        CHECK(plat_store_read((u16)k, v, sizeof(v)) == sizeof(v));
        CHECK(v[0] == (u8)(39 * 16 + k) && v[sizeof(v) - 1] == (u8)(39 * 16 + k));
    }

    /* Power lost in the middle of a record: the old value stays, and the
     * next write still lands (compacting past the torn one). */
    writes_left = 7;
    memset(big, 0x5a, sizeof(big));
    plat_store_write(3, big, 100);
    writes_left = ~0u;
    reopen();
    CHECK(plat_store_read(3, v, sizeof(v)) == sizeof(v) && v[0] == (u8)(39 * 16 + 3));
    CHECK(plat_store_write(3, "new", 3) == 3);
    reopen();
    CHECK(plat_store_read(3, v, sizeof(v)) == 3 && !memcmp(v, "new", 3));
    for (i = 1; i <= 12; i++)
        if (i != 3) CHECK(plat_store_read((u16)i, v, sizeof(v)) == sizeof(v));
    return TEST_DONE("store");
}
#endif

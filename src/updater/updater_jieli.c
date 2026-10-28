#include <system/includes.h>
#include <boot.h>
#include <string.h>
#include "board.h"
#include "updater.h"

/* The updater on the BR23 (updater.c): the image uboot.boot starts, in the
 * app area.  The app is linked MAIN_AT above it in the same code window.
 * At reset, before the SDK's start-up, updater_entry() starts the app as
 * uboot.boot would have (its boot information in r0, no hardware touched),
 * unless the app asked for the updater (the reset mailbox) or its header
 * is not there.  The flash holds the app area enciphered for the chip: the
 * updater enciphers what it programs and reads it back through the code
 * window. */

#define CODE_WINDOW     0x1e00000u      /* the app area's start, as the CPU sees it */
#define MAIN_HEAD       ((const volatile u32 *)(CODE_WINDOW + MAIN_AT))
#define MAIN_ENTRY      (CODE_WINDOW + MAIN_AT + MAIN_HEAD_SIZE)

extern void _start(void *boot_info);    /* the SDK's start-up */
extern u32 sfc_write(const u8 *buf, u32 addr, u32 len);
extern int sfc_erase(int cmd, u32 addr);   /* 2: the 4 KiB sector at addr */
/* The SDK's status-register write: SR1 <- sr1, and SR2's CMP bit from
 * sr2 & 0x40 in the same command (SRP1 cleared, the rest kept). */
extern int update_norflash_write_protect(u32 sr2_apart, u32 volatile_enable, u32 sr1, u32 sr2);

/* Before the SDK's start-up: no .data, no .bss, uboot's stack, so only
 * the magic, the entry and the format are checked before the app starts. */
__attribute__((section(".updater_entry"), noreturn, used))
void updater_entry(void *info)
{
    volatile u32 *mailbox = plat_updater_mailbox;

    if (mailbox[0] == UPDATER_STAY && mailbox[1] == ~UPDATER_STAY) {
        mailbox[0] = 0;
    } else if (MAIN_HEAD[0] == MAIN_MAGIC && MAIN_HEAD[3] == MAIN_ENTRY &&
               MAIN_HEAD[4] == MAIN_VERSION) {
        u32 tries = (mailbox[2] & ~0xffu) == UPDATER_TRIES ? mailbox[2] & 0xff : 0;

        if (tries < UPDATER_TRIES_MAX) {
            mailbox[2] = UPDATER_TRIES | (tries + 1);
            ((void (*)(void *))MAIN_ENTRY)(info);
        }
    }
    _start(info);
}

u32 upd_region(void) { return boot_info.sfc.sfc_base_addr + MAIN_AT; }
u32 upd_room(void) { return CODE_LIMIT - upd_region(); }
u32 upd_entry(void) { return MAIN_ENTRY; }
u32 upd_read32(u32 offset) { return *(const volatile u32 *)(CODE_WINDOW + MAIN_AT + offset); }
void upd_read(u32 offset, u8 *out, u32 n) { memcpy(out, (const void *)(CODE_WINDOW + MAIN_AT + offset), n); }
u32 upd_data_max(void) { return 512; }
/* The app's start-ups that never got its control thread going: why this
 * updater runs instead (updater_entry()). */
void upd_started(void)
{
    if (plat_updater_mailbox[2] == (UPDATER_TRIES | UPDATER_TRIES_MAX))
        util_log("updater: the app started %u times without coming up", UPDATER_TRIES_MAX);
    plat_updater_mailbox[2] = 0;
}
void upd_committed(void) {}

/* The app area's cipher (jlnewfw's jl_sfc): a 32-byte block n bytes from
 * the area's start is XORed with the ENC stream keyed chip_key ^ (n >> 2).
 * The chip key does not read back from JL_SFCENC (0 on a 0xffff chip). */
static u16 chip_key;
static u8 key_found, unprotected;
static u8 cipher[512] __attribute__((aligned(4)));

static u16 enc_next(u16 key)
{
    return (u16)((key << 1) ^ ((key & 0x8000) ? 0x1021 : 0));
}

u32 upd_extra(void) { return key_found ? chip_key : 0xffffffffu; }

/* An erased block shows its keystream through the code window (raw 0xff,
 * deciphered): the one chip key whose stream it is. */
static int find_key(u32 area_offset)
{
    const volatile u8 *v = (const volatile u8 *)(CODE_WINDOW + area_offset);
    u8 stream[32];
    u32 k, i;

    for (i = 0; i < 32; i++) stream[i] = v[i] ^ 0xff;
    for (k = 0; k < 0x10000; k++) {
        u16 key = (u16)(k ^ (area_offset >> 2));

        for (i = 0; i < 32 && (u8)key == stream[i]; i++) key = enc_next(key);
        if (i == 32) {
            chip_key = (u16)k;
            key_found = 1;
            return 1;
        }
    }
    return 0;
}

static void encrypt(u8 *p, u32 n, u32 area_offset)
{
    u16 key = 0;
    u32 i, k;

    for (i = 0; i < n; i++) {
        u32 at = area_offset + i;

        if (!(at & 31)) key = chip_key ^ (u16)(at >> 2);
        else if (!i) {           /* into a block: its stream up to here */
            key = chip_key ^ (u16)((at & ~31u) >> 2);
            for (k = 0; k < (at & 31); k++) key = enc_next(key);
        }
        p[i] ^= (u8)key;
        key = enc_next(key);
    }
}

int upd_program(u32 offset, const u8 *data, u32 n, u32 skip)
{
    u32 area_offset = MAIN_AT + offset;

    if (n > sizeof(cipher)) return 0;
    memcpy(cipher, data, n);
    encrypt(cipher, n, area_offset);
    memset(cipher, 0xff, skip);
    sfc_write(cipher, upd_region() + offset, n);
    return memcmp((const void *)(CODE_WINDOW + area_offset + skip), data + skip, n - skip) == 0;
}

u8 upd_prepare(void)
{
    if (!unprotected) {
        unprotected = 1;
        update_norflash_write_protect(0, 0, 0, 0);
        util_log("updater: block protection off");
    }
    return ERR_NONE;
}

void upd_erase(u32 offset)
{
    sfc_erase(2, upd_region() + offset);
}

u8 upd_erased(void)
{
    return key_found || find_key(MAIN_AT) ? ERR_NONE : ERR_KEY;
}

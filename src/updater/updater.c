#include <system/includes.h>
#include <boot.h>
#include <string.h>
#include "board.h"

/* The updater (OTA milestone 2, /work/ota-loader-plan.md): the image
 * uboot.boot starts.  The app is linked MAIN_AT above it in the same code
 * window, behind jlnewfw's header ("MAIN", size, CRC16 of the app, entry).
 * At reset, before the SDK's start-up, updater_entry() starts the app as
 * uboot.boot would have (its boot information in r0, no hardware touched),
 * unless the app asked for the updater (the reset mailbox) or its header
 * is not there.  Otherwise this is the SDK and our platform without the
 * radio: the tool device, whose update commands rewrite the app in place.
 *
 * The header's magic word is programmed last, after the app's CRC checked
 * out through the code window: until then a reset comes back here.
 *
 * Tool-device commands (the main app's are in core/usb.c):
 *   `0x00` IDENTIFY, `0x01` STATUS, `0x02` LOG, `0x10` REBOOT, `0x11`
 *   REBOOT_LOADER as there; IDENTIFY's byte 7 is 1, the updater
 *   `0x41` UPDATE_INFO   in (32): u8 version (1), u8 state (0 idle, 1
 *                        writing, 2 committed), u8 error (below), u8 app
 *                        valid, u32 the app's flash offset, u32 room (header
 *                        and app), u32 bytes written, u32 size begun, u32
 *                        chip key (found at BEGIN; ~0 before), u32 –, u32 –
 *   `0x42` UPDATE_BEGIN  CDB bytes 2-5: size (header and app, as jlnewfw
 *                        writes them): erases the header's sector (the app
 *                        is gone from here on)
 *   `0x43` UPDATE_DATA   CDB bytes 2-5: offset, the bytes written so far;
 *                        out: up to 512 bytes of the image in the clear
 *   `0x44` UPDATE_COMMIT – : checks the header and the app's CRC through the
 *                        code window, then programs the magic word
 * Errors: 1 bad size, 2 out of order, 3 not begun, 4 verify (the code window
 * does not read back what was written), 5 incomplete, 6 bad header, 7 CRC,
 * 8 no chip key fits the erased header sector.  An error stays until the
 * next BEGIN. */

#define CODE_WINDOW     0x1e00000u      /* the app area's start, as the CPU sees it */
#define MAIN_HEAD       ((const volatile u32 *)(CODE_WINDOW + MAIN_AT))
#define MAIN_ENTRY      (CODE_WINDOW + MAIN_AT + MAIN_HEAD_SIZE)
#define MAIN_MAGIC      0x4e49414du     /* "MAIN" */
#define MAIN_HEAD_SIZE  0x120u
#define SECTOR          0x1000u
#define IDLE_OFF_MS     60000           /* without a USB host */

enum { OP = 0xf5, CMD_IDENTIFY = 0x00, CMD_STATUS = 0x01, CMD_LOG = 0x02, CMD_REBOOT = 0x10,
       CMD_REBOOT_LOADER = 0x11, CMD_INFO = 0x41, CMD_BEGIN = 0x42, CMD_DATA = 0x43,
       CMD_COMMIT = 0x44 };
enum { IDLE, WRITING, COMMITTED };
enum { ERR_NONE, ERR_SIZE, ERR_ORDER, ERR_NOT_BEGUN, ERR_VERIFY, ERR_INCOMPLETE, ERR_HEADER,
       ERR_CRC, ERR_KEY };

extern void _start(void *boot_info);    /* the SDK's start-up */
extern u32 sfc_write(const u8 *buf, u32 addr, u32 len);
extern int sfc_erase(int cmd, u32 addr);   /* 2: the 4 KiB sector at addr */
/* The SDK's status-register write: SR1 <- sr1, and SR2's CMP bit from
 * sr2 & 0x40 in the same command (SRP1 cleared, the rest kept). */
extern int update_norflash_write_protect(u32 sr2_apart, u32 volatile_enable, u32 sr1, u32 sr2);

static int app_valid(void)
{
    return MAIN_HEAD[0] == MAIN_MAGIC && MAIN_HEAD[3] == MAIN_ENTRY &&
           MAIN_HEAD[1] <= CODE_LIMIT - (boot_info.sfc.sfc_base_addr + MAIN_AT + MAIN_HEAD_SIZE);
}

/* Before the SDK's start-up: no .data, no .bss, uboot's stack.  The size
 * check above reads boot_info, which is still .bss here: only the magic
 * and the entry are checked before the app starts. */
__attribute__((section(".updater_entry"), noreturn, used))
void updater_entry(void *info)
{
    volatile u32 *mailbox = plat_updater_mailbox;

    if (mailbox[0] == UPDATER_STAY && mailbox[1] == ~UPDATER_STAY) {
        mailbox[0] = 0;
    } else if (MAIN_HEAD[0] == MAIN_MAGIC && MAIN_HEAD[3] == MAIN_ENTRY) {
        ((void (*)(void *))MAIN_ENTRY)(info);
    }
    _start(info);
}

/* ---- the app in flash ----------------------------------------------------- */

static u8 state, error, unprotected;
static u32 begun, written;
static u8 held[4];               /* the magic word, programmed by COMMIT */
static u8 cipher[512] __attribute__((aligned(4)));

static u32 app_flash(void) { return boot_info.sfc.sfc_base_addr + MAIN_AT; }
static u32 room(void) { return CODE_LIMIT - app_flash(); }

/* The app area's cipher (jlnewfw's jl_sfc): a 32-byte block n bytes from
 * the area's start is XORed with the ENC stream keyed chip_key ^ (n >> 2).
 * The chip key does not read back from JL_SFCENC (0 on a 0xffff chip). */
static u16 chip_key;
static u8 key_found;

static u16 enc_next(u16 key)
{
    return (u16)((key << 1) ^ ((key & 0x8000) ? 0x1021 : 0));
}

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
    u32 i;

    for (i = 0; i < n; i++) {
        u32 at = area_offset + i;

        if (!i || !(at & 31)) key = chip_key ^ (u16)((at & ~31u) >> 2);
        p[i] ^= (u8)key;
        key = enc_next(key);
    }
}

/* Programs data (in the clear) at offset within the app's header and
 * body, then reads it back through the code window.  skip: leading bytes
 * left erased (the magic word). */
static int program(u32 offset, const u8 *data, u32 n, u32 skip)
{
    u32 area_offset = MAIN_AT + offset;

    memcpy(cipher, data, n);
    encrypt(cipher, n, area_offset);
    memset(cipher, 0xff, skip);
    sfc_write(cipher, app_flash() + offset, n);
    return memcmp((const void *)(CODE_WINDOW + area_offset + skip), data + skip, n - skip) == 0;
}

static void unprotect(void)
{
    if (unprotected) return;
    unprotected = 1;
    update_norflash_write_protect(0, 0, 0, 0);
    util_log("updater: block protection off");
}

static void erase(u32 offset)
{
    sfc_erase(2, app_flash() + offset);
}

static u8 begin(u32 size)
{
    state = IDLE;
    if (size <= MAIN_HEAD_SIZE || size > room()) return ERR_SIZE;
    unprotect();
    erase(0);
    if (!key_found && !find_key(MAIN_AT)) return ERR_KEY;
    begun = size;
    written = 0;
    state = WRITING;
    util_log("updater: begin %u bytes at %x", size, app_flash());
    return ERR_NONE;
}

static u8 data(u32 offset, const u8 *d, u32 n)
{
    u32 skip = 0;

    if (state != WRITING) return ERR_NOT_BEGUN;
    if (offset != written || !n || n > begun - written) return ERR_ORDER;
    if (offset / SECTOR != (offset + n - 1) / SECTOR || (offset && !(offset % SECTOR)))
        erase((offset + n - 1) & ~(SECTOR - 1));
    if (!offset) {
        memcpy(held, d, 4);
        skip = 4;
    }
    if (!program(offset, d, n, skip)) {
        state = IDLE;
        util_log("updater: verify failed at %x", offset);
        return ERR_VERIFY;
    }
    written += n;
    return ERR_NONE;
}

static u16 crc16(const u8 *p, u32 n)
{
    u16 crc = 0;

    while (n--) {
        u32 b;

        crc ^= (u16)(*p++ << 8);
        for (b = 0; b < 8; b++) crc = (u16)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static u8 commit(void)
{
    u32 size, b;

    if (state != WRITING) return ERR_NOT_BEGUN;
    if (written != begun) return ERR_INCOMPLETE;
    size = MAIN_HEAD[1];
    if (memcmp(held, "MAIN", 4) || MAIN_HEAD[3] != MAIN_ENTRY ||
        size != begun - MAIN_HEAD_SIZE)
        return ERR_HEADER;
    b = crc16((const u8 *)MAIN_ENTRY, size);
    if (b != MAIN_HEAD[2]) {
        util_log("updater: crc %04x, header %04x", b, MAIN_HEAD[2]);
        return ERR_CRC;
    }
    if (!program(0, held, 4, 0)) return ERR_VERIFY;
    state = COMMITTED;
    util_log("updater: committed %u bytes, crc %04x", size, b);
    return ERR_NONE;
}

/* ---- the tool device ------------------------------------------------------ */

static u8 reboot, to_rom;

u32 usb_command(const u8 cdb[16], u8 *d, u32 max, int from_host)
{
    u32 arg = cdb[2] | (u32)cdb[3] << 8 | (u32)cdb[4] << 16 | (u32)cdb[5] << 24;
    u32 n = 0;

    if (cdb[0] != OP) {
        reboot = to_rom = 1;
        return 0;
    }
    switch (cdb[1]) {
    case CMD_IDENTIFY: {
        const u8 *uid = plat_unique_id();

        if (max < 56) return 0;
        memcpy(d, "C2PD", 4);
        util_put16(d + 4, 2);
        d[6] = 0;                /* the pad */
        d[7] = 1;                /* its updater */
        strncpy((char *)d + 8, BUILD_ID, 16);
        if (uid) memcpy(d + 24, uid, 16);
        strncpy((char *)d + 40, BOARD_PID, 16);
        n = 56;
        if (max >= 88) {
            strncpy((char *)d + 56, board_name, 32);
            n = 88;
        }
        break;
    }
    case CMD_STATUS:
        if (max < 16) return 0;
        memset(d, 0, 16);
        d[2] = 1;
        util_put16(d + 4, plat_battery_mv());
        util_put32(d + 8, plat_ms());
        n = 16;
        break;
    case CMD_LOG: {
        if (max < 8) return 0;
        n = util_log_read(&arg, (char *)d + 8, max - 8);
        util_put32(d, arg);
        util_put32(d + 4, n);
        n += 8;
        break;
    }
    case CMD_REBOOT:
        reboot = 1;
        break;
    case CMD_REBOOT_LOADER:
        reboot = to_rom = 1;
        break;
    case CMD_INFO:
        if (max < 32) return 0;
        memset(d, 0, 32);
        d[0] = 1;
        d[1] = state;
        d[2] = error;
        d[3] = (u8)app_valid();
        util_put32(d + 4, app_flash());
        util_put32(d + 8, room());
        util_put32(d + 12, written);
        util_put32(d + 16, begun);
        util_put32(d + 20, key_found ? chip_key : 0xffffffffu);
        n = 32;
        break;
    case CMD_BEGIN:
        error = begin(arg);
        break;
    case CMD_DATA:
        if (from_host && !error) error = data(arg, d, max);
        break;
    case CMD_COMMIT:
        if (!error) error = commit();
        break;
    }
    return n;
}

/* ---- the loop ------------------------------------------------------------- */

static void lights(u8 r, u8 g, u8 b)
{
    struct led_frame f;
    u32 i;

    for (i = 0; i < LIGHT_COUNT; i++) f.light[i] = (struct rgb){ r, g, b };
    plat_leds(&f);
}

void ctl_main(void)
{
    u32 host_seen = plat_ms(), reboot_at = 0;

    util_log("updater: app at %x, %s", app_flash(), app_valid() ? "valid" : "none");
    lights(0, 0, 2);
    for (;;) {
        u32 now;

        plat_wait(plat_ms() + 100, 0);
        plat_usb_service();
        now = plat_ms();
        if (state == COMMITTED) lights(0, 2, 0);
        if (reboot && !reboot_at) reboot_at = now + 100;   /* the command's status goes first */
        if (reboot_at && (s32)(now - reboot_at) >= 0) {
            if (to_rom) plat_reboot_loader();
            plat_reboot();
        }
        if (plat_usb_present()) host_seen = now;
        else if (now - host_seen > IDLE_OFF_MS) plat_power_off();
    }
}

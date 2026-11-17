#include <string.h>
#include "pad.h"
#include "yc3121.h"
#include "board.h"
#include "flash_yc3121.h"
#include "updater.h"

/* The updater on the YC3121 (updater.c): the program at the app slot's
 * base, where the 8BitDo boot loader jumps.  The app's region is MAIN_AT
 * above it: its header, then the app (linked to run there) at
 * JM_MAIN_HEAD_SIZE.  First in main(), updater_boot() jumps to the app's
 * entry unless the app asked for the updater or its header is not there;
 * while the app runs, start.S passes the entries on to it.  USB, and LE
 * through the radio core (updater_ble_yc3121.c).
 *
 * The slot is ciphered flash, 32-byte blocks with a check word each
 * (flash_yc3121.c), so a block takes one program: the commit word cannot
 * follow the rest of its block later.  The header's first block is held
 * here until the commit and programmed then; until then reads of it answer
 * from the held copy, its commit word erased.  The erase unit is a 32 KiB
 * window: the first sector a write reaches in each erases its window.  The
 * window below the region stays empty (the updater has MAIN_AT - 0x8000),
 * as its last 16 bytes of flash share a sector with the region's first. */

#define REGION   (YC_SLOT_START + MAIN_AT)
#define WINDOW   0x8000u

static u32 boot_stay;            /* yc_handover.stay as it came */
static u8 first[YC_BLOCK];       /* the header's first block, until the commit */
static u8 first_held;
static u8 block[YC_BLOCK];

static u32 get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

void updater_boot(void)
{
    u32 tries, entry = REGION + JM_MAIN_HEAD_SIZE;
    u8 h[JM_MAIN_AT_OFF];

    yc_handover.app = 0;
    boot_stay = yc_handover.stay;
    tries = (boot_stay & ~0xffu) == UPDATER_TRIES ? boot_stay & 0xff : 0;
    yc_handover.stay = 0;
    if (boot_stay == UPDATER_STAY || tries >= UPDATER_TRIES_MAX) return;
    yc_slot_read(REGION, h, sizeof(h));
    if (get32(h + JM_MAIN_MAGIC_OFF) != JM_MAIN_MAGIC || get32(h + JM_MAIN_ENTRY_OFF) != entry ||
        get32(h + JM_MAIN_FORMAT_OFF) != JM_MAIN_VERSION || get32(h + JM_MAIN_SIZE_OFF) > upd_room() - JM_MAIN_HEAD_SIZE)
        return;
    yc_handover.stay = UPDATER_TRIES + tries + 1;
    yc_handover.app = entry;
    ((void (*)(void))(entry | 1))();
}

void upd_started(void)
{
    if (boot_stay == UPDATER_TRIES + UPDATER_TRIES_MAX)
        util_log("updater: the app started %u times without coming up", UPDATER_TRIES_MAX);
    else
        util_log("updater: asked %08x", (unsigned)boot_stay);
}

u32 upd_region(void) { return REGION; }
u32 upd_room(void) { return YC_SLOT_END - REGION; }
u32 upd_entry(void) { return REGION + JM_MAIN_HEAD_SIZE; }
u32 upd_extra(void) { return 0xffffffffu; }
/* The tool device gathers a command's data from 64-byte reports. */
u32 upd_data_max(void) { return 512; }

void upd_read(u32 offset, u8 *out, u32 n)
{
    yc_slot_read(REGION + offset, out, n);
    if (first_held && offset < YC_BLOCK) memcpy(out, first + offset, n < YC_BLOCK - offset ? n : YC_BLOCK - offset);
}

u32 upd_read32(u32 offset)
{
    u8 b[4];

    upd_read(offset, b, 4);
    return get32(b);
}

u8 upd_prepare(void)
{
    first_held = 0;
    return JM_UPDATE_ERR_NONE;
}

u8 upd_erased(void) { return JM_UPDATE_ERR_NONE; }
void upd_committed(void) {}

void upd_erase(u32 offset)
{
    if (!((REGION + offset) % WINDOW)) yc_slot_erase(REGION + offset);
}

/* n bytes at offset (a block's start), a short last block padded with 0xff;
 * then read back. */
static int program(u32 offset, const u8 *data, u32 n)
{
    u32 whole = n & ~(YC_BLOCK - 1), at;

    if (whole && !yc_slot_write(REGION + offset, data, whole)) return 0;
    if (n > whole) {
        memset(block, 0xff, YC_BLOCK);
        memcpy(block, data + whole, n - whole);
        if (!yc_slot_write(REGION + offset + whole, block, YC_BLOCK)) return 0;
    }
    for (at = 0; at < n; at += YC_BLOCK) {
        u32 k = n - at < YC_BLOCK ? n - at : YC_BLOCK;

        yc_slot_read(REGION + offset + at, block, k);
        if (memcmp(block, data + at, k)) return 0;
    }
    return 1;
}

/* The first data (skip: the commit word) holds the first block; the commit
 * (the word at 0) programs it whole. */
int upd_program(u32 offset, const u8 *data, u32 n, u32 skip)
{
    if (!offset && skip) {
        u32 k = n < YC_BLOCK ? n : YC_BLOCK;

        memset(first, 0xff, YC_BLOCK);
        memcpy(first + skip, data + skip, k - skip);
        first_held = 1;
        return n <= YC_BLOCK || program(YC_BLOCK, data + YC_BLOCK, n - YC_BLOCK);
    }
    if (!offset && first_held) {
        memcpy(first, data, n);
        first_held = 0;
        return program(0, first, YC_BLOCK);
    }
    return program(offset, data, n);
}

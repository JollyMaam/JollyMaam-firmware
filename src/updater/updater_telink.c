#include "sdk.h"
#include "board.h"
#include "updater.h"

/* The updater on the B80 (updater.c): the image the 8BitDo boot loader
 * starts, at APP_START.  The app's region is MAIN_AT above it: its header,
 * then the app (its image, linked to run there) at MAIN_HEAD_SIZE.  At the
 * very start of main(), updater_boot() starts the app as the boot loader
 * would (its vectors and RAM code into SRAM, then the CPU restarts from
 * SRAM), unless the app asked for the updater or its header is not there.
 *
 * The app's request is analog register 0x3c = 0x55: it survives a reset
 * (the SDK's DEEP_ANA_REG2, cleared only by a power cycle), and the 8BitDo
 * boot loader acts on 0x5a (stay in it) and 0x5b, not on this.  Flash is
 * not enciphered and reads at its address. */

#define REGION          (APP_START + MAIN_AT)
#define SRAM            0x840000
#define RAM_COPY_MAX    0x3200          /* the boot loader's own limit */

static u8 boot_flag;                     /* 0x3c as it came */
static u16 lock_was;                     /* the flash's protection before an update */
static u8 unlocked;
static u8 page[256];

/* The 8BitDo boot loader's launch (its code at 0x3000): the image's first
 * (header word 0x0c, low half) x 16 bytes into SRAM, then register 0x602
 * restarts the CPU there.  From flash, with interrupts off: the copy
 * overwrites this updater's own RAM code and vectors. */
static void __attribute__((noreturn, noinline)) start_app(u32 image, u32 bytes)
{
    const volatile u32 *from = (const volatile u32 *)image;
    volatile u32 *to = (volatile u32 *)SRAM;
    u32 n = bytes >> 2;

    irq_disable();
    write_reg8(0x643, 0);
    while (n--) *to++ = *from++;
    write_reg8(0x602, 0x88);
    for (;;) {
    }
}

void updater_boot(void)
{
    const volatile u32 *head = (const volatile u32 *)REGION;
    u32 entry = REGION + MAIN_HEAD_SIZE, bytes;

    boot_flag = analog_read(UPDATER_STAY_REG);
    if (boot_flag == UPDATER_STAY) {
        analog_write(UPDATER_STAY_REG, 0x0f);    /* its power-on value */
        return;
    }
    if (head[0] != MAIN_MAGIC || head[3] != entry || head[4] != MAIN_VERSION) return;
    if (*(const volatile u32 *)(entry + 8) != 0x544c4e4b) return;      /* "KNLT" */
    bytes = (*(const volatile u32 *)(entry + 0x0c) & 0xffff) << 4;
    if (!bytes || bytes > RAM_COPY_MAX) return;
    start_app(entry, bytes);
}

void upd_started(void)
{
    util_log("updater: boot flag %02x", boot_flag);
}

u32 upd_region(void) { return REGION; }
u32 upd_room(void) { return CODE_LIMIT - REGION; }
u32 upd_entry(void) { return REGION + MAIN_HEAD_SIZE; }
u32 upd_extra(void) { return 0xffffffffu; }
/* A frame of 152 data bytes is a 160-byte ATT write: the most the LE side
 * takes in fragments (rx_telink.c). */
u32 upd_data_max(void) { return 152; }

/* Through the SPI, not the cache: what was just programmed. */
void upd_read(u32 offset, u8 *out, u32 n)
{
    flash_read_page(REGION + offset, n, out);
}

u32 upd_read32(u32 offset)
{
    u8 b[4];

    upd_read(offset, b, 4);
    return b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
}

/* The stock status register protects the low 256 KiB, the region among it:
 * off for the update, back as it was after the commit (upd_committed). */
u8 upd_prepare(void)
{
    if (!unlocked) {
        lock_was = flash_get_lock_block_mid136085();
        if (lock_was) flash_unlock_mid136085();
        unlocked = 1;
        util_log("updater: flash protection %04x off", lock_was);
    }
    return ERR_NONE;
}

void upd_committed(void)
{
    if (unlocked && lock_was) flash_lock_mid136085(lock_was);
    unlocked = 0;
}

u8 upd_erased(void) { return ERR_NONE; }

void upd_erase(u32 offset)
{
    flash_erase_sector(REGION + offset);
}

int upd_program(u32 offset, const u8 *data, u32 n, u32 skip)
{
    u32 at;

    if (n <= skip) return 1;
    flash_write_page(REGION + offset + skip, n - skip, (u8 *)data + skip);
    for (at = skip; at < n; at += sizeof(page)) {
        u32 k = n - at < sizeof(page) ? n - at : sizeof(page);

        upd_read(offset + at, page, k);
        if (memcmp(page, data + at, k)) return 0;
    }
    return 1;
}

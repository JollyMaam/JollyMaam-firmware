#include <string.h>
#include "updater.h"

/* The updater (/work/ota-loader-plan.md, docs/firmware/updater.md): the
 * image the chip's boot starts.  The app is linked above it, behind a header
 * ("MAIN", size, CRC16 of the app, entry, format version, its offset, the
 * image's PID, CRC16 of those fields; jollymaam jieli pack --main, 8bitdoboot.py
 * main).  At reset, before anything else, the chip's side (updater_jieli.c,
 * updater_telink.c) starts the app unless the app asked for the updater or
 * its header is not there.  Otherwise this runs: the tool device and LE,
 * whose update commands rewrite the app in place.
 *
 * The header's magic word is programmed last, after the app's CRC checked
 * out as written: until then a reset comes back here.
 *
 * Home pressed and released goes back to the app, when it is valid; 60 s
 * without a host (USB or LE) and outside a write powers off.  A dongle's
 * updater is USB only: its button goes back, and it stays on.
 *
 * Tool-device commands (the app's are in core/usb.c); over LE the same
 * (updater_frame()):
 *   `0x00` IDENTIFY, `0x01` STATUS, `0x02` LOG, `0x10` REBOOT, `0x11`
 *   REBOOT_LOADER as there; IDENTIFY's byte 7 is 1, the updater
 *   `0x41` UPDATE_INFO   in (32): u8 version (1), u8 state (0 idle, 1
 *                        writing, 2 committed), u8 error (below), u8 app
 *                        valid, u32 the app's flash offset, u32 room (header
 *                        and app), u32 bytes written, u32 size begun, u32
 *                        chip key on the BR23 (found at BEGIN; ~0 before),
 *                        u32 the most UPDATE_DATA bytes per command, u32 –
 *   `0x42` UPDATE_BEGIN  CDB bytes 2-5: size (header and app); out: the
 *                        header's first 0x2c bytes.  Checked first (this
 *                        board's PID, this updater's format and offset, the
 *                        header's CRC); then erases the header's sector
 *                        (the app is gone from here on)
 *   `0x43` UPDATE_DATA   CDB bytes 2-5: offset, the bytes written so far;
 *                        out: the image in the clear; at another offset it
 *                        is ignored (the host resends from UPDATE_INFO's count)
 *   `0x44` UPDATE_COMMIT – : checks the header and the app's CRC as written,
 *                        then programs the magic word
 * Errors: 1 bad size, 2 past the end, 3 not begun, 4 verify (the flash does
 * not read back what was written), 5 incomplete, 6 bad header, 7 CRC, 8 no
 * chip key fits the erased header sector, 9 the image is for another board,
 * 10 for another updater layout.  An error from BEGIN's check comes before
 * anything is erased.  An error stays until the next BEGIN. */

#define HEAD_CHECKED    0x2c            /* BEGIN's copy: up to the header's CRC */
#define IDLE_OFF_MS     60000           /* without a host */

enum { OP = 0xf5, CMD_IDENTIFY = 0x00, CMD_STATUS = 0x01, CMD_LOG = 0x02, CMD_REBOOT = 0x10,
       CMD_REBOOT_LOADER = 0x11, CMD_INFO = 0x41, CMD_BEGIN = 0x42, CMD_DATA = 0x43,
       CMD_COMMIT = 0x44 };
enum { IDLE, WRITING, COMMITTED };

static u8 state, error;
static u32 begun, written;
static u8 held[4];               /* the magic word, programmed by COMMIT */
static u8 head[HEAD_CHECKED];    /* BEGIN's: the first data must match it */

static u32 get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static u16 crc16_add(u16 crc, const u8 *p, u32 n)
{
    while (n--) {
        u32 b;

        crc ^= (u16)(*p++ << 8);
        for (b = 0; b < 8; b++) crc = (u16)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static int app_valid(void)
{
    return upd_read32(0) == MAIN_MAGIC && upd_read32(12) == upd_entry() &&
           upd_read32(16) == MAIN_VERSION && upd_read32(4) <= upd_room() - MAIN_HEAD_SIZE;
}

/* The image's header, before anything is erased: for this board, this
 * updater's layout, and whole. */
static u8 check_head(const u8 *h, u32 n, u32 size)
{
    char pid[17] = { 0 };

    if (n < HEAD_CHECKED || memcmp(h, "MAIN", 4) || get32(h + 40) != crc16_add(0, h + 4, 36))
        return ERR_HEADER;
    memcpy(pid, h + 24, 16);
    if (strcmp(pid, BOARD_PID)) {
        util_log("updater: image for %.16s, not %s", pid, BOARD_PID);
        return ERR_BOARD;
    }
    if (get32(h + 16) != MAIN_VERSION || get32(h + 20) != MAIN_AT || get32(h + 12) != upd_entry()) {
        util_log("updater: image format %u at %x", get32(h + 16), get32(h + 20));
        return ERR_LAYOUT;
    }
    if (get32(h + 4) != size - MAIN_HEAD_SIZE) return ERR_SIZE;
    return ERR_NONE;
}

static u8 begin(u32 size, const u8 *h, u32 n)
{
    u8 e;

    state = IDLE;
    if (size <= MAIN_HEAD_SIZE || size > upd_room()) return ERR_SIZE;
    if ((e = check_head(h, n, size))) return e;
    memcpy(head, h, HEAD_CHECKED);
    if ((e = upd_prepare())) return e;
    upd_erase(0);
    if ((e = upd_erased())) return e;
    begun = size;
    written = 0;
    state = WRITING;
    util_log("updater: begin %u bytes at %x", size, upd_region());
    return ERR_NONE;
}

static u8 data(u32 offset, const u8 *d, u32 n)
{
    u32 skip = 0;

    if (state != WRITING) return ERR_NOT_BEGUN;
    /* Not the next bytes: dropped, a write lost or repeated on the way (LE
     * writes without response); the host resends from UPDATE_INFO's count. */
    if (offset != written) return ERR_NONE;
    if (!n || n > begun - written) return ERR_ORDER;
    if (offset / SECTOR != (offset + n - 1) / SECTOR || (offset && !(offset % SECTOR)))
        upd_erase((offset + n - 1) & ~(SECTOR - 1));
    if (!offset) {
        if (n < HEAD_CHECKED || memcmp(d, head, HEAD_CHECKED)) return ERR_HEADER;
        memcpy(held, d, 4);
        skip = 4;
    }
    if (!upd_program(offset, d, n, skip)) {
        state = IDLE;
        util_log("updater: verify failed at %x", offset);
        return ERR_VERIFY;
    }
    written += n;
    return ERR_NONE;
}

static u8 commit(void)
{
    u8 buf[64];
    u32 size, at, n;
    u16 crc = 0;

    if (state != WRITING) return ERR_NOT_BEGUN;
    if (written != begun) return ERR_INCOMPLETE;
    size = upd_read32(4);
    if (memcmp(held, "MAIN", 4) || upd_read32(12) != upd_entry() || size != begun - MAIN_HEAD_SIZE)
        return ERR_HEADER;
    for (at = 0; at < size; at += n) {
        n = size - at < sizeof(buf) ? size - at : sizeof(buf);
        upd_read(MAIN_HEAD_SIZE + at, buf, n);
        crc = crc16_add(crc, buf, n);
    }
    if (crc != upd_read32(8)) {
        util_log("updater: crc %04x, header %04x", crc, upd_read32(8));
        return ERR_CRC;
    }
    if (!upd_program(0, held, 4, 0)) return ERR_VERIFY;
    upd_committed();
    state = COMMITTED;
    util_log("updater: committed %u bytes, crc %04x", size, crc);
    return ERR_NONE;
}

/* ---- the tool device ------------------------------------------------------ */

static u8 reboot, to_rom, to_config;

u32 usb_command(const u8 cdb[16], u8 *d, u32 max, int from_host)
{
    u32 arg = get32(cdb + 2);
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
#ifdef BOARD_DONGLE
        d[6] = 1;                /* the dongle */
#else
        d[6] = 0;                /* the pad */
#endif
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
        {
            struct battery_status b;
            plat_battery(&b);
            d[3] = (b.flags & (JM_BATTERY_LOW_VALID | JM_BATTERY_LOW)) ==
                   (JM_BATTERY_LOW_VALID | JM_BATTERY_LOW);
            util_put16(d + 4, b.mv);
            d[6] = b.flags;
            d[7] = b.percent;
        }
        util_put32(d + 8, plat_ms());
        n = 16;
        break;
    case CMD_LOG:
        if (max < 8) return 0;
        n = util_log_read(&arg, (char *)d + 8, max - 8);
        util_put32(d, arg);
        util_put32(d + 4, n);
        n += 8;
        break;
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
        util_put32(d + 4, upd_region());
        util_put32(d + 8, upd_room());
        util_put32(d + 12, written);
        util_put32(d + 16, begun);
        util_put32(d + 20, upd_extra());
        util_put32(d + 24, upd_data_max());
        n = 32;
        break;
    case CMD_BEGIN:
        error = begin(arg, d, from_host ? max : 0);
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

/* An LE frame: data with the frame goes in as a host-to-pad command's;
 * without, the answer (if any) comes back after the sub-command. */
u32 updater_frame(const u8 *frame, u32 len, u8 *reply, u32 max)
{
    u8 cdb[16] = { OP };
    u32 n;

    if (len < 5 || max < 2) return 0;
    /* The REBOOT after an update over LE: the app starts in config mode,
     * where the host that updated it can find it again. */
    if (frame[0] == CMD_REBOOT && state == COMMITTED) to_config = 1;
    cdb[1] = frame[0];
    memcpy(cdb + 2, frame + 1, 4);
    if (len > 5) {
        usb_command(cdb, (u8 *)frame + 5, len - 5, 1);
        return 0;
    }
    memset(reply, 0, max);
    n = usb_command(cdb, reply + 1, max - 1, 0);
    if (!n) return 0;
    reply[0] = frame[0];
    return n + 1;
}

/* ---- the loop ------------------------------------------------------------- */

#ifdef NO_BT                     /* a dongle's: USB only */
void updater_ble_start(void) {}
int updater_ble_connected(void) { return 0; }
#endif

static void lights(u8 r, u8 g, u8 b)
{
#ifdef BOARD_DONGLE              /* its LED not driven */
    (void)r; (void)g; (void)b;
#else
    struct led_frame f;
    u32 i;

    for (i = 0; i < LIGHT_COUNT; i++) f.light[i] = (struct rgb){ r, g, b };
    plat_leds(&f);
#endif
}

/* Home pressed and released here goes back to a valid app: offline
 * Home+M in the app is how a pad gets here by hand.  On a dongle, its
 * button.  A press counts only after Home was seen up here, so letting go
 * of a chord that brought the pad here does not go back. */
static int home_back(void)
{
    static u8 down, armed;
    int held;
#ifdef BOARD_DONGLE
    u32 changed_us;

    held = plat_lines(&changed_us) & 1;
#else
    struct in_raw raw;

    plat_input(&raw);
    held = (raw.buttons & PAD_HOME) != 0;
#endif
    if (held) {
        down = armed;
        return 0;
    }
    armed = 1;
    if (!down) return 0;
    down = 0;
    return app_valid() && state != WRITING;
}

void ctl_main(void)
{
#ifndef BOARD_DONGLE
    u32 host_seen = plat_ms();
#endif
    u32 reboot_at = 0;

    util_log("updater: app at %x, %s", upd_region(), app_valid() ? "valid" : "none");
    upd_started();
    updater_ble_start();
    lights(0, 0, 2);
    for (;;) {
        u32 now;

        plat_wait(plat_ms() + 100, 0);
        plat_usb_service();
        now = plat_ms();
        if (state == COMMITTED) lights(0, 2, 0);
        if (home_back()) {
            util_log("updater: Home, back to the app");
            reboot = 1;
        }
        if (reboot && !reboot_at) reboot_at = now + 100;   /* the command's status goes first */
        if (reboot_at && (s32)(now - reboot_at) >= 0) {
            if (to_rom) plat_reboot_loader();
            if (to_config) plat_reboot_config();
            plat_reboot();
        }
#ifndef BOARD_DONGLE             /* a dongle is off with its USB */
        if (plat_usb_present() || updater_ble_connected() || state == WRITING) host_seen = now;
        else if (now - host_seen > IDLE_OFF_MS) plat_power_off();
#endif
    }
}

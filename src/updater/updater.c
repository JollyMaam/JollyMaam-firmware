#include <string.h>
#if defined(UNITTEST) && !defined(MAIN_AT)
#define MAIN_AT 0x20000
#endif
#include "updater.h"
#ifdef HAVE_BUILD_ID_H
#include "build_id.h"
#endif
#if defined(UNITTEST) && !defined(BUILD_ID)
#define BUILD_ID "host"
#endif

/* The updater (/work/ota-loader-plan.md, docs/firmware/updater.md): the
 * image the chip's boot starts.  The app is linked above it, behind a header
 * ("MAIN", size, CRC16 of the app, entry, format version, its offset, the
 * image's PID, CRC16 of those fields; jollymaam pack jieli --main or pack join
 * main).  At reset, before anything else, the chip's side (updater_jieli.c,
 * updater_telink.c, updater_yc3121.c) starts the app unless the app asked for the updater or
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

#define IDLE_OFF_MS     60000           /* without a host */


static u8 state, error;
static u32 begun, written;
static u8 held[4];               /* the magic word, programmed by COMMIT */
static u8 head[JM_MAIN_CHECKED_SIZE];    /* BEGIN's: the first data must match it */

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
    return upd_read32(JM_MAIN_MAGIC_OFF) == JM_MAIN_MAGIC && upd_read32(JM_MAIN_ENTRY_OFF) == upd_entry() &&
           upd_read32(JM_MAIN_FORMAT_OFF) == JM_MAIN_VERSION && upd_read32(JM_MAIN_SIZE_OFF) <= upd_room() - JM_MAIN_HEAD_SIZE;
}

/* The image's header, before anything is erased: for this board, this
 * updater's layout, and whole. */
static u8 check_head(const u8 *h, u32 n, u32 size)
{
    char pid[17] = { 0 };

    if (n < JM_MAIN_CHECKED_SIZE || memcmp(h, "MAIN", 4) || get32(h + JM_MAIN_HEADER_CRC_OFF) != crc16_add(0, h + JM_MAIN_CRC_START, JM_MAIN_CRC_SIZE))
        return JM_UPDATE_ERR_HEADER;
    memcpy(pid, h + JM_MAIN_PRODUCT_OFF, 16);
    if (strcmp(pid, BOARD_PID)) {
        util_log("updater: image for %.16s, not %s", pid, BOARD_PID);
        return JM_UPDATE_ERR_BOARD;
    }
    if (get32(h + JM_MAIN_FORMAT_OFF) != JM_MAIN_VERSION || get32(h + JM_MAIN_AT_OFF) != MAIN_AT || get32(h + JM_MAIN_ENTRY_OFF) != upd_entry()) {
        util_log("updater: image format %u at %x", get32(h + JM_MAIN_FORMAT_OFF), get32(h + JM_MAIN_AT_OFF));
        return JM_UPDATE_ERR_LAYOUT;
    }
    if (get32(h + JM_MAIN_SIZE_OFF) != size - JM_MAIN_HEAD_SIZE) return JM_UPDATE_ERR_SIZE;
    return JM_UPDATE_ERR_NONE;
}

static u8 begin(u32 size, const u8 *h, u32 n)
{
    u8 e;

    state = JM_UPDATE_IDLE;
    if (size <= JM_MAIN_HEAD_SIZE || size > upd_room()) return JM_UPDATE_ERR_SIZE;
    if ((e = check_head(h, n, size))) return e;
    memcpy(head, h, JM_MAIN_CHECKED_SIZE);
    if ((e = upd_prepare())) return e;
    upd_erase(0);
    if ((e = upd_erased())) return e;
    begun = size;
    written = 0;
    state = JM_UPDATE_WRITING;
    util_log("updater: begin %u bytes at %x", size, upd_region());
    return JM_UPDATE_ERR_NONE;
}

static u8 data(u32 offset, const u8 *d, u32 n)
{
    u32 skip = 0;

    if (state != JM_UPDATE_WRITING) return JM_UPDATE_ERR_NOT_BEGUN;
    /* Not the next bytes: dropped, a write lost or repeated on the way (LE
     * writes without response); the host resends from UPDATE_INFO's count. */
    if (offset != written) return JM_UPDATE_ERR_NONE;
    if (!n || n > begun - written) return JM_UPDATE_ERR_ORDER;
    if (offset / SECTOR != (offset + n - 1) / SECTOR || (offset && !(offset % SECTOR)))
        upd_erase((offset + n - 1) & ~(SECTOR - 1));
    if (!offset) {
        if (n < JM_MAIN_CHECKED_SIZE || memcmp(d, head, JM_MAIN_CHECKED_SIZE)) return JM_UPDATE_ERR_HEADER;
        memcpy(held, d, 4);
        skip = 4;
    }
    if (!upd_program(offset, d, n, skip)) {
        state = JM_UPDATE_IDLE;
        util_log("updater: verify failed at %x", offset);
        return JM_UPDATE_ERR_VERIFY;
    }
    written += n;
    return JM_UPDATE_ERR_NONE;
}

static u8 commit(void)
{
    u8 buf[64];
    u32 size, at, n;
    u16 crc = 0;

    if (state != JM_UPDATE_WRITING) return JM_UPDATE_ERR_NOT_BEGUN;
    if (written != begun) return JM_UPDATE_ERR_INCOMPLETE;
    size = upd_read32(JM_MAIN_SIZE_OFF);
    if (memcmp(held, "MAIN", 4) || upd_read32(JM_MAIN_ENTRY_OFF) != upd_entry() || size != begun - JM_MAIN_HEAD_SIZE)
        return JM_UPDATE_ERR_HEADER;
    for (at = 0; at < size; at += n) {
        n = size - at < sizeof(buf) ? size - at : sizeof(buf);
        upd_read(JM_MAIN_HEAD_SIZE + at, buf, n);
        crc = crc16_add(crc, buf, n);
    }
    if (crc != upd_read32(JM_MAIN_CRC_OFF)) {
        util_log("updater: crc %04x, header %04x", crc, upd_read32(JM_MAIN_CRC_OFF));
        return JM_UPDATE_ERR_CRC;
    }
    if (!upd_program(0, held, 4, 0)) return JM_UPDATE_ERR_VERIFY;
    upd_committed();
    state = JM_UPDATE_COMMITTED;
    util_log("updater: committed %u bytes, crc %04x", size, crc);
    return JM_UPDATE_ERR_NONE;
}

/* ---- the tool device ------------------------------------------------------ */

static u8 reboot, to_rom, to_config;

u32 usb_command(const u8 cdb[16], u8 *d, u32 max, int from_host)
{
    u32 arg = get32(cdb + 2);
    u32 n = 0;

    if (cdb[0] != JM_OP) {
        reboot = to_rom = 1;
        return 0;
    }
    switch (cdb[1]) {
    case JM_CMD_IDENTIFY: {
        const u8 *uid = plat_unique_id();

        if (max < JM_ID_SIZE) return 0;
        memcpy(d, "C2PD", 4);
        util_put16(d + JM_ID_PROTOCOL_OFF, JM_PROTOCOL);
#ifdef BOARD_DONGLE
        d[JM_ID_BOARD_OFF] = 1;                /* the dongle */
#else
        d[JM_ID_BOARD_OFF] = 0;                /* the pad */
#endif
        d[JM_ID_UPDATER_OFF] = 1;                /* its updater */
        strncpy((char *)d + JM_ID_BUILD_OFF, BUILD_ID, JM_ID_BUILD_SIZE);
        if (uid) memcpy(d + JM_ID_UID_OFF, uid, JM_ID_UID_SIZE);
        strncpy((char *)d + JM_ID_PRODUCT_OFF, BOARD_PID, JM_PRODUCT_SIZE);
        n = JM_ID_SIZE;
        if (max >= JM_ID_EXT_SIZE) {
            strncpy((char *)d + JM_ID_BOARD_NAME_OFF, board_name, JM_ID_BOARD_NAME_SIZE);
            n = JM_ID_EXT_SIZE;
        }
        break;
    }
    case JM_CMD_STATUS:
        if (max < JM_STATUS_SIZE) return 0;
        memset(d, 0, JM_STATUS_SIZE);
        d[JM_STATUS_USB_POWERED_OFF] = 1;
        {
            struct battery_status b;
            plat_battery(&b);
            d[JM_STATUS_LOW_BATTERY_OFF] = (b.flags & (JM_BATTERY_LOW_VALID | JM_BATTERY_LOW)) ==
                   (JM_BATTERY_LOW_VALID | JM_BATTERY_LOW);
            util_put16(d + JM_STATUS_BATTERY_MV_OFF, b.mv);
            d[JM_STATUS_BATTERY_FLAGS_OFF] = b.flags;
            d[JM_STATUS_BATTERY_PERCENT_OFF] = b.percent;
        }
        util_put32(d + JM_STATUS_UPTIME_OFF, plat_ms());
        n = JM_STATUS_SIZE;
        break;
    case JM_CMD_LOG:
        if (max < 8) return 0;
        n = util_log_read(&arg, (char *)d + 8, max - 8);
        util_put32(d, arg);
        util_put32(d + 4, n);
        n += 8;
        break;
    case JM_CMD_REBOOT:
        reboot = 1;
        break;
    case JM_CMD_REBOOT_LOADER:
        reboot = to_rom = 1;
        break;
    case JM_CMD_UPDATE_INFO:
        if (max < JM_UPDATE_INFO_SIZE) return 0;
        memset(d, 0, JM_UPDATE_INFO_SIZE);
        d[JM_UPDATE_INFO_VERSION_OFF] = JM_UPDATE_INFO_VERSION;
        d[JM_UPDATE_INFO_STATE_OFF] = state;
        d[JM_UPDATE_INFO_ERROR_OFF] = error;
        d[JM_UPDATE_INFO_VALID_OFF] = (u8)app_valid();
        util_put32(d + JM_UPDATE_INFO_AT_OFF, upd_region());
        util_put32(d + JM_UPDATE_INFO_ROOM_OFF, upd_room());
        util_put32(d + JM_UPDATE_INFO_WRITTEN_OFF, written);
        util_put32(d + JM_UPDATE_INFO_BEGUN_OFF, begun);
        util_put32(d + JM_UPDATE_INFO_EXTRA_OFF, upd_extra());
        util_put32(d + JM_UPDATE_INFO_DATA_MAX_OFF, upd_data_max());
        n = JM_UPDATE_INFO_SIZE;
        break;
    case JM_CMD_UPDATE_BEGIN:
        error = begin(arg, d, from_host ? max : 0);
        break;
    case JM_CMD_UPDATE_DATA:
        if (from_host && !error) error = data(arg, d, max);
        break;
    case JM_CMD_UPDATE_COMMIT:
        if (!error) error = commit();
        break;
    }
    return n;
}

/* An LE frame: data with the frame goes in as a host-to-pad command's;
 * without, the answer (if any) comes back after the sub-command. */
u32 updater_frame(const u8 *frame, u32 len, u8 *reply, u32 max)
{
    u8 cdb[16] = { JM_OP };
    u32 n;

    if (len < 5 || max < 2) return 0;
    /* The REBOOT after an update over LE: the app starts in config mode,
     * where the host that updated it can find it again. */
    if (frame[0] == JM_CMD_REBOOT && state == JM_UPDATE_COMMITTED) to_config = 1;
    cdb[1] = frame[0];
    memcpy(cdb + 2, frame + 1, 4);
    if (len > 5) {
        usb_command(cdb, (u8 *)frame + 5, len - 5, 1);
        return 0;
    }
    memset(reply, 0, max);
    n = usb_command(cdb, reply + 1, max - 1, 0);
    if (!n) return 0;
#ifdef UPD_LE_DATA_MAX           /* the LE side takes shorter writes than USB */
    if (frame[0] == JM_CMD_UPDATE_INFO && upd_data_max() > UPD_LE_DATA_MAX)
        util_put32(reply + 1 + JM_UPDATE_INFO_DATA_MAX_OFF, UPD_LE_DATA_MAX);
#endif
    reply[0] = frame[0];
    return n + 1;
}

#ifndef UNITTEST
/* ---- the loop ------------------------------------------------------------- */

#ifdef NO_BT                     /* USB only: the dongles, the U2CB */
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
    return app_valid() && state != JM_UPDATE_WRITING;
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
        int ble;

        plat_wait(plat_ms() + 100, 0);
        plat_usb_service();
        now = plat_ms();
        if (state == JM_UPDATE_COMMITTED) lights(0, 2, 0);
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
        ble = updater_ble_connected();   /* every pass: on the YC3121 it takes the host's writes */
#ifndef BOARD_DONGLE             /* a dongle is off with its USB */
        if (plat_usb_present() || ble || state == JM_UPDATE_WRITING) host_seen = now;
        else if (now - host_seen > IDLE_OFF_MS) plat_power_off();
#endif
    }
}

#else
#include "../lib/upgrade_jollymaam.h"
#include "../lib/jieli_image/crypto.h"
#include <assert.h>
#include <stdio.h>

static u8 flash[8192];
static unsigned erases, commits;
static const u8 test_uid[16] = { 1, 2, 3 };
const char board_name[] = "Test updater";
const u8 *plat_unique_id(void) { return test_uid; }
u32 plat_ms(void) { return 1234; }
void plat_battery(struct battery_status *b) {
    *b = (struct battery_status){3900, 66, JM_BATTERY_MV_VALID | JM_BATTERY_PERCENT_VALID};
}
void util_put16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
void util_put32(u8 *p, u32 v) { util_put16(p, v); util_put16(p + 2, v >> 16); }
void util_log(const char *fmt, ...) { (void)fmt; }
u32 util_log_read(u32 *seq, char *out, u32 max) { (void)seq; (void)out; (void)max; return 0; }
u32 upd_region(void) { return MAIN_AT; }
u32 upd_room(void) { return sizeof flash; }
u32 upd_entry(void) { return MAIN_AT + JM_MAIN_HEAD_SIZE; }
u32 upd_read32(u32 at) { assert(at + 4 <= sizeof flash); return get32(flash + at); }
void upd_read(u32 at, u8 *out, u32 n) { assert(at + n <= sizeof flash); memcpy(out, flash + at, n); }
u8 upd_prepare(void) { return JM_UPDATE_ERR_NONE; }
u8 upd_erased(void) { return JM_UPDATE_ERR_NONE; }
void upd_erase(u32 at) { assert(at + SECTOR <= sizeof flash); memset(flash + at, 0xff, SECTOR); erases++; }
int upd_program(u32 at, const u8 *p, u32 n, u32 skip) {
    assert(at + n <= sizeof flash && skip <= n);
    memcpy(flash + at + skip, p + skip, n - skip);
    return 1;
}
void upd_committed(void) { commits++; }
u32 upd_extra(void) { return 0x1234; }
u32 upd_data_max(void) { return 128; }

/* Literal image offsets deliberately pin the external format independently
 * of the shared definitions. The real updater handles every host request. */
static void test_update(int wrong_board, int corrupt_app)
{
    u8 image[600] = {0}, reply[88];
    struct jm_update op;
    struct jm_update_request request;
    enum jm_update_step step;
    unsigned requests = 0;

    memset(flash, 0xff, sizeof flash);
    state = error = reboot = 0;
    begun = written = erases = commits = 0;
    memcpy(image, "MAIN", 4);
    util_put32(image + 4, sizeof image - 0x120);
    util_put32(image + 8, jm_crc16(image + 0x120, sizeof image - 0x120));
    util_put32(image + 12, upd_entry());
    util_put32(image + 16, 1);
    util_put32(image + 20, MAIN_AT);
    memcpy(image + 24, wrong_board ? "wrong" : BOARD_PID,
           strlen(wrong_board ? "wrong" : BOARD_PID));
    util_put32(image + 40, jm_crc16(image + 4, 36));
    if (wrong_board) {
        u8 cdb[16];
        struct jm_update_info info;

        /* Also check firmware preflight directly: the host normally refuses
         * this image at IDENTIFY before BEGIN is sent. */
        jm_command(cdb, JM_CMD_UPDATE_BEGIN, sizeof image);
        usb_command(cdb, image, 44, 1);
        jm_command(cdb, JM_CMD_UPDATE_INFO, 0);
        assert(usb_command(cdb, reply, sizeof reply, 0) == 32);
        assert(jm_update_info_decode(reply, 32, &info));
        assert(info.error == JM_UPDATE_ERR_BOARD && !erases);
        error = 0;
    }
    assert(jm_update_init(&op, image, sizeof image, 128));
    while ((step = jm_update_next(&op, &request)) == JM_UPDATE_TRANSFER) {
        u32 n;
        assert(++requests < 100);
        memset(reply, 0, sizeof reply);
        if (request.out_size) {
            usb_command(request.cdb, (u8 *)request.data, request.out_size, 1);
            n = 0;
        } else {
            if (corrupt_app && request.cdb[1] == JM_CMD_UPDATE_COMMIT)
                flash[0x120] ^= 1;
            n = usb_command(request.cdb, reply, request.in_size, 0);
        }
        jm_update_complete(&op, 1, reply, n);
    }
    if (wrong_board) {
        assert(step == JM_UPDATE_FAILED && !erases && !commits && !reboot);
    } else if (corrupt_app) {
        assert(step == JM_UPDATE_FAILED && op.error == JM_UPDATE_ERR_CRC && !commits && !reboot);
        assert(!app_valid());
    } else {
        assert(step == JM_UPDATE_DONE && commits == 1 && reboot && app_valid());
        assert(!memcmp(flash, image, sizeof image));
    }
}

int main(void)
{
    u8 cdb[16], reply[88];
    struct jm_identity id;
    struct jm_battery_status battery;
    struct jm_update_info info;

    memset(flash, 0xff, sizeof flash);
    memset(reply, 0, sizeof reply);
    jm_command(cdb, JM_CMD_IDENTIFY, 0);
    assert(usb_command(cdb, reply, 55, 0) == 0);
    assert(usb_command(cdb, reply, 56, 0) == 56);
    assert(jm_identify_decode(reply, 56, &id) && !id.board_name[0]);
    assert(usb_command(cdb, reply, sizeof reply, 0) == 88);
    assert(jm_identify_decode(reply, 88, &id) && id.updater == 1);
    assert(id.protocol == JM_PROTOCOL && !strcmp(id.product, BOARD_PID));
    assert(!strcmp(id.board_name, board_name) && !memcmp(id.uid, test_uid, 16));
    jm_command(cdb, JM_CMD_STATUS, 0);
    assert(usb_command(cdb, reply, sizeof reply, 0) == 16);
    assert(jm_battery_decode(reply, 16, &battery) && battery.mv == 3900 && battery.percent == 66);
    jm_command(cdb, JM_CMD_UPDATE_INFO, 0);
    assert(usb_command(cdb, reply, 31, 0) == 0);
    assert(usb_command(cdb, reply, sizeof reply, 0) == 32);
    assert(jm_update_info_decode(reply, 32, &info));
    assert(info.at == MAIN_AT && info.room == sizeof flash && info.key == 0x1234 && info.data_max == 128);
    test_update(0, 0);
    test_update(1, 0);
    test_update(0, 1);
    return TEST_DONE("updater firmware/libjollymaam compatibility");
}
#endif

#include <string.h>
#include "pad.h"

/* Vendor SCSI commands on the mass-storage interface.
 *
 * CDB: [0] 0xF5, [1] sub-command, [2..5] argument (little-endian).  The data
 * phase carries what the host sent for a host-to-pad command (WRITE), or the
 * answer for the others.  Status is the CSW.  Everything is little-endian.
 *
 * The Jieli loader's own opcodes (0xFB 0xFC 0xFD) reboot into the loader, as
 * the SDK does, so its tools find the loader behind a running pad.
 *
 * Sub-commands (data is in the data phase; "in" is pad to host, "out" host to pad):
 *   `0x00` IDENTIFY       in (40): magic `C2PD`, u16 protocol, board (6: 0 pad,
 *                         1 dongle), build ID (16 chars at 8), flash UID (16 at
 *                         24)
 *   `0x01` STATUS         in (16): state, slot, USB powered, low battery, u16
 *                         battery mV, u32 uptime ms, ephemeral (12), settings
 *                         items collected (13)
 *   `0x02` LOG            CDB bytes 2–5: from sequence number; in: u32 first
 *                         sequence returned, u32 length, text
 *   `0x03` STATS          CDB byte 2 bit 0: reset after reading; in: u16
 *                         version, u16 size, `struct ctl_stats` as in memory
 *   `0x10` REBOOT         –
 *   `0x11` REBOOT_LOADER  – (any loader opcode does the same)
 *   `0x17` PAIR           CDB byte 2: slot: start pairing; its host is replaced once one pairs,
 *                         keeping its address; dongle: forget its single bond
 *                         (slot ignored)
 *   `0x30` TARGET         dongle only: CDB bytes 2–7: Bluetooth address in
 *                         written order (AA:BB:CC:DD:EE:FF); page that device
 *   `0x13` CONNECT        CDB byte 2: slot (`0xfe` the last): start its
 *                         session, as a Home connect does
 *   `0x14` PROFILE        CDB byte 2: 1 start at the rate in bytes 3-4
 *                         (samples/s), 2 stop, 0 read from the byte offset in
 *                         bytes 3-6; in: header (version, rate, bucket shifts,
 *                         the flash/RAM/ROM ranges and bucket counts, samples,
 *                         outside, saturated, the last 8 outside addresses),
 *                         then u16 counts per bucket, flash, RAM, ROM
 *   `0x16` TRACE          CDB byte 2: 1 start (clears), 2 stop, 0 read from the
 *                         byte offset in bytes 3–6; in: u32 markers written,
 *                         then 1024 entries `{u32 µs, u16 argument, u8 marker,
 *                         u8 –}` (`util_trace()`, pad.h)
 *   `0x15` PEEK           CDB bytes 2–5: address; in: 32-bit words read from it
 *                         (registers, RAM)
 *   `0x12` RELINK         – : while connected, take the session down and
 *                         connect the same slot again (the host is awake;
 *                         nothing is woken)
 *   `0x20` FIELD          argument: field number; in: u8 version, target kind
 *                         (0 slot, 1 palette, 2 calibration), id, type, size,
 *                         –, u16 max, then name, help, enum names (NUL-
 *                         terminated), defaults (one per slot for slot fields,
 *                         fields up to 4 bytes); all zero past the last
 *   `0x21` READ           CDB byte 2: target (0-7 slot, 8 palette, 9
 *                         calibration); in: u8 version, target, count, then per
 *                         field id, set, len, value
 *   `0x22` WRITE          out: items `{target, id, len, value}`, collected;
 *                         `len` 0: default; id 0: the whole target
 *   `0x26` RESULT         in: the last WRITE: u8 count, then a code per item (0
 *                         ok, 1 unknown, 2 bad length, 3 out of range, 4 full)
 *   `0x23` COMMIT         – : apply what was collected, in order; save unless
 *                         ephemeral
 *   `0x24` DROP           – : throw away what was collected
 *   `0x25` EPHEMERAL      – : app settings/bond changes stay in RAM until the
 *                         next boot; SDK radio trim writes are unaffected
 *
 * The other SCSI commands answer as a device with no medium: INQUIRY answers,
 * TEST UNIT READY says "no medium", READ CAPACITY fails.  Nothing is mounted.
 *
 * usb_poll() runs on every pass of the loop and lets the platform service a
 * parked command; usb_command() runs on the control thread.  The platform's
 * private_scsi_cmd() hook parks each vendor command and raises EV_USB.
 * padtool.py (Linux, SG_IO) is the main client. */

#define OP          0xf5
#define PROTOCOL    2

#define STATS_VERSION 6

enum { CMD_IDENTIFY = 0x00, CMD_STATUS = 0x01, CMD_LOG = 0x02, CMD_STATS = 0x03,
       CMD_REBOOT = 0x10, CMD_REBOOT_LOADER = 0x11, CMD_RELINK = 0x12, CMD_CONNECT = 0x13, CMD_PROFILE = 0x14, CMD_PEEK = 0x15, CMD_TRACE = 0x16, CMD_PAIR = 0x17,
       CMD_FIELD = 0x20, CMD_READ = 0x21, CMD_WRITE = 0x22, CMD_COMMIT = 0x23, CMD_DROP = 0x24,
       CMD_EPHEMERAL = 0x25, CMD_RESULT = 0x26, CMD_TARGET = 0x30 };

#ifdef BOARD_DONGLE
#include "dongle.h"
#endif

#ifndef BUILD_ID
#define BUILD_ID "host"
#endif

#ifdef BOARD_DONGLE
#define BOARD_KIND 1
#else
#define BOARD_KIND 0             /* the pad */
#endif

static struct ctl_request pending;
static const struct usb_status *status;
static u8 result[256];           /* the last WRITE: a code per item */
static u32 result_len;

u32 usb_command(const u8 cdb[16], u8 *data, u32 max, int from_host)
{
    u32 n = 0;

    if (cdb[0] != OP) {
        pending.kind = REQ_REBOOT_LOADER;
        return 0;
    }
    switch (cdb[1]) {
    case CMD_IDENTIFY: {
        const u8 *uid = plat_unique_id();

        if (max < 40) return 0;
        memcpy(data, "C2PD", 4);
        util_put16(data + 4, PROTOCOL);
        data[6] = BOARD_KIND;
        data[7] = 0;
        strncpy((char *)data + 8, BUILD_ID, 16);
        if (uid) memcpy(data + 24, uid, 16);
        n = 40;
        break;
    }
    case CMD_STATUS:
        if (max < 16 || !status) return 0;
        data[0] = status->state;
        data[1] = status->slot;
        data[2] = status->usb_powered;
        data[3] = status->low_battery;
        util_put16(data + 4, status->battery_mv);
        util_put32(data + 8, status->uptime_ms);
        data[12] = status->ephemeral;
        data[13] = status->staged;
        n = 16;
        break;
    case CMD_LOG: {
        u32 seq = util_get32(cdb + 2);

        if (max < 8) return 0;
        n = util_log_read(&seq, (char *)data + 8, max - 8);
        util_put32(data, seq);
        util_put32(data + 4, n);
        n += 8;
        break;
    }
    case CMD_STATS:              /* argument bit 0: reset after reading */
        if (max < 4 + sizeof(struct ctl_stats) || !status || !status->stats) return 0;
        util_put16(data, STATS_VERSION);
        util_put16(data + 2, sizeof(struct ctl_stats));
        memcpy(data + 4, status->stats, sizeof(struct ctl_stats));
        n = 4 + sizeof(struct ctl_stats);
        if (cdb[2] & 1) pending.kind = REQ_STATS_RESET;
        break;
    case CMD_REBOOT:
        pending.kind = REQ_REBOOT;
        break;
    case CMD_REBOOT_LOADER:
        pending.kind = REQ_REBOOT_LOADER;
        break;
    case CMD_RELINK:
        pending.kind = REQ_RELINK;
        break;
    case CMD_PROFILE:            /* byte 2: 0 read from the offset in bytes 3-6; 1 start
                                    at the rate in bytes 3-4, flash window from bytes
                                    5-8 (0: all) in buckets of 1 << byte 9; 2 stop */
        if (cdb[2] == 1) plat_prof_start((u16)(cdb[3] | cdb[4] << 8), util_get32(cdb + 5), cdb[9]);
        else if (cdb[2] == 2) plat_prof_start(0, 0, 0);
        else n = plat_prof_read(util_get32(cdb + 3), data, max);
        break;
    case CMD_TRACE:              /* byte 2: 0 read from the offset in bytes 3-6, 1 start, 2 stop */
        if (cdb[2] == 1) util_trace_enable(1);
        else if (cdb[2] == 2) util_trace_enable(0);
        else n = util_trace_read(util_get32(cdb + 3), data, max);
        break;
    case CMD_PEEK:               /* argument: address; in: 32-bit words from it */
        n = plat_peek(util_get32(cdb + 2), data, max);
        break;
    case CMD_PAIR:               /* argument: slot; keeps its address */
        pending.kind = REQ_PAIR;
        pending.slot = cdb[2];
        pending.generate_new_address = 0;
        break;
    case CMD_CONNECT:            /* argument: slot, 0xfe the last */
        pending.kind = REQ_CONNECT;
        pending.slot = cdb[2];
        break;
    case CMD_FIELD:              /* argument: field number */
        n = set_describe(util_get32(cdb + 2), data, max);
        break;
    case CMD_READ:               /* argument: target */
        n = set_read(cdb[2], data, max);
        break;
    case CMD_WRITE:
        if (!from_host) max = 0;
        result_len = set_write(data, max, result + 1, sizeof(result) - 1);
        result[0] = (u8)result_len;
        break;
    case CMD_RESULT:
        n = result_len + 1 < max ? result_len + 1 : max;
        memcpy(data, result, n);
        break;
    case CMD_COMMIT:
        set_apply();
        break;
    case CMD_DROP:
        set_drop();
        break;
    case CMD_EPHEMERAL:
        set_ephemeral();
        break;
#ifdef BOARD_DONGLE
    case CMD_TARGET:             /* CDB bytes 2-7: the device to page, as written */
        dongle_bt_target(cdb + 2);
        break;
#endif
    }
    return n;
}

struct ctl_request usb_poll(const struct usb_status *s)
{
    struct ctl_request r;

    status = s;
    plat_usb_service();
    r = pending;
    pending.kind = REQ_NONE;
    pending.slot = SLOT_NONE;
    return r;
}

#ifdef UNITTEST
#ifdef BOARD_DONGLE
static u8 target_seen[6];
void dongle_bt_target(const u8 addr[6]) { memcpy(target_seen, addr, 6); }
#endif
static const u8 uid[16] = { 1, 2, 3 };
const u8 *plat_unique_id(void) { return uid; }
void plat_usb_service(void) {}
void plat_prof_start(u16 hz, u32 zoom, u8 shift) { (void)hz; (void)zoom; (void)shift; }
u32 plat_prof_read(u32 offset, u8 *out, u32 max) { (void)offset; (void)out; (void)max; return 0; }
u32 plat_peek(u32 addr, u8 *out, u32 max) { (void)addr; (void)out; (void)max; return 0; }
void util_trace_enable(int on) { (void)on; }
u32 util_trace_read(u32 offset, u8 *out, u32 max) { (void)offset; (void)out; (void)max; return 0; }
u32 util_log_read(u32 *seq, char *out, u32 max)
{
    (void)max;
    *seq = 7;
    memcpy(out, "hi\n", 3);
    return 3;
}
void util_put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u32 applied;
u32 set_describe(u32 n, u8 *out, u32 max) { (void)max; out[0] = (u8)n; return n < 3 ? 8 : 0; }
u32 set_read(u8 t, u8 *out, u32 max) { (void)max; out[1] = t; return 3; }
u32 set_write(const u8 *items, u32 len, u8 *results, u32 max)
{
    (void)max;
    results[0] = items[0];
    results[1] = (u8)len;
    return 2;
}
void set_apply(void) { applied++; }
void set_drop(void) {}
void set_ephemeral(void) {}
void util_put32(u8 *p, u32 v) { util_put16(p, (u16)v); util_put16(p + 2, (u16)(v >> 16)); }
u32 util_get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

int main(void)
{
    u8 cdb[16] = { OP, CMD_IDENTIFY }, data[512];
    static struct usb_status st = { CTL_IDLE, SLOT_NONE, 1, 0, 3900, 1234 };
    struct ctl_request r;

    CHECK(usb_command(cdb, data, sizeof(data), 0) == 40);
    CHECK(!memcmp(data, "C2PD", 4) && data[4] == PROTOCOL && data[25] == 2);
    CHECK(data[6] == BOARD_KIND && data[7] == 0);
#ifdef BOARD_DONGLE
    cdb[1] = CMD_TARGET;
    memcpy(cdb + 2, "ABCDEF", 6);
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 0);
    CHECK(!memcmp(target_seen, "ABCDEF", 6));
    memset(cdb + 2, 0, 6);
#endif
    status = &st;
    cdb[1] = CMD_STATUS;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 16);
    CHECK(data[0] == CTL_IDLE && data[4] == (3900 & 0xff) && data[8] == (1234 & 0xff));
    cdb[1] = CMD_LOG;
    cdb[2] = 5;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 11 && data[0] == 7 && data[4] == 3);
    r = usb_poll(&st);
    CHECK(r.kind == REQ_NONE);
    {
        static struct ctl_stats stats;

        stats.passes = 0x01020304;
        st.stats = &stats;
        cdb[1] = CMD_STATS;
        cdb[2] = 1;
        CHECK(usb_command(cdb, data, sizeof(data), 0) == 4 + sizeof(stats));
        CHECK(data[0] == STATS_VERSION && data[2] == (u8)sizeof(stats) && data[4 + 8] == 4);
        CHECK(usb_poll(&st).kind == REQ_STATS_RESET);
    }
    cdb[1] = CMD_FIELD;
    cdb[2] = 1;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 8 && data[0] == 1);
    cdb[1] = CMD_WRITE;
    data[0] = 0x42;
    CHECK(usb_command(cdb, data, 9, 1) == 0);
    cdb[1] = CMD_RESULT;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 3 && data[0] == 2 && data[1] == 0x42 && data[2] == 9);
    cdb[1] = CMD_COMMIT;
    usb_command(cdb, data, sizeof(data), 0);
    CHECK(applied == 1);
    cdb[2] = 0;
    cdb[1] = CMD_REBOOT_LOADER;
    usb_command(cdb, data, sizeof(data), 0);
    r = usb_poll(&st);
    CHECK(r.kind == REQ_REBOOT_LOADER);
    cdb[0] = 0xfc;
    usb_command(cdb, data, sizeof(data), 0);
    CHECK(usb_poll(&st).kind == REQ_REBOOT_LOADER);
    CHECK(usb_poll(&st).kind == REQ_NONE);
    return TEST_DONE("usb");
}
#endif

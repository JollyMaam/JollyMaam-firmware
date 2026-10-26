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
 *   `0x00` IDENTIFY       in (56): magic `C2PD`, u16 protocol, board (6: 0
 *                         pad, 1 dongle), build ID (16 chars at 8), flash UID
 *                         (16 at 24), the image's PID (16 chars at 40,
 *                         NUL-padded); asked for 88 or more:
 *                         board_name (32 chars at 56, NUL-padded)
 *   `0x01` STATUS         in (16): state, slot, USB powered, low battery, u16
 *                         battery mV, u32 uptime ms, ephemeral (12), settings
 *                         items collected (13)
 *   `0x02` LOG            CDB bytes 2–5: from sequence number; in: u32 first
 *                         sequence returned, u32 length, text
 *   `0x27` DEFAULTS       pad only: same arguments and format as READ, no state changes
 *   `0x04` INPUT          pad only: in (36): u8 version (1), flags
 *                         (bit 0: sticks read), u16 –, u32 buttons (PAD_*), s16
 *                         lx ly rx ry and u16 l2 r2 calibrated, before Home
 *                         gestures and the remap; u16 raw sticks [4] and
 *                         triggers [2] as read, u32 uptime ms
 *   `0x03` STATS          CDB byte 2 bit 0: reset after reading; in: u16
 *                         version, u16 size, `struct ctl_stats` as in memory
 *   `0x10` REBOOT         –
 *   `0x11` REBOOT_LOADER  – (any loader opcode does the same)
 *   `0x17` PAIR           CDB byte 2: slot: start pairing; its host is replaced once one pairs,
 *                         keeping its address; dongle: forget its single bond
 *                         (slot ignored)
 *   `0x18` CALIBRATE      pad only: start shared sensor calibration; no slot argument
 *   `0x19` LED_DEMO       pad only: CDB byte 2: LED demo number (0xff:
 *                         stop), byte 3 1: show it (for UX_LED_DEMO_MS); in: its name,
 *                         NUL-terminated, empty past the last
 *   `0x1a` RUMBLE         pad only: CDB bytes 2, 3: left, right motor
 *                         duty (0-255, the slot's rumble percent not applied),
 *                         bytes 4-5: for how many ms (at most RUMBLE_TEST_MAX_MS;
 *                         0 stops), over whatever the host asks for
 *   `0x1b` DISCONNECT     pad only: end the current session, keeping its bond
 *   `0x30` TARGET         dongle only: CDB bytes 2–7: Bluetooth address in
 *                         written order (AA:BB:CC:DD:EE:FF); page that device
 *   `0x31` TOOL_ONLY      dongle only: stay the tool device until the next boot,
 *                         also while a pad is linked (its type is not put on USB)
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
 *                         (0 slot, 1 palette, 2 calibration, 3 remap slot), id,
 *                         type, size, flags, u16 max, then name, help, enum names (NUL-
 *                         terminated), defaults (one per slot for slot fields,
 *                         fields up to 4 bytes); all zero past the last
 *   `0x21` READ           CDB byte 2: target (0-7 slot, 8 palette, 9
 *                         calibration, 10-17 remap slot, 18 the base
 *                         remap); in: u8 version, target, count, then per
 *                         field id, set, len, value
 *   `0x22` WRITE          out: items `{target, id, len, value}`, collected;
 *                         `len` 0: default; id 0: the whole target
 *                         id 0xfe, len 0: unpair slot, generate and save a new
 *                         address on COMMIT; DROP cancels
 *   `0x26` RESULT         in: the last WRITE: u8 count, then a code per item (0
 *                         ok, 1 unknown, 2 bad length, 3 out of range, 4 full)
 *   `0x23` COMMIT         – : apply what was collected, in order; save unless
 *                         ephemeral
 *   `0x24` DROP           – : throw away what was collected
 *   `0x25` EPHEMERAL      – : app settings/bond changes stay in RAM until the
 *                         next boot; SDK radio trim writes are unaffected
 *
 * A board built without the settings store (NO_SETTINGS) answers FIELD to
 * EPHEMERAL, DEFAULTS and LED_DEMO with no data and changes nothing.
 *
 * The other SCSI commands answer as a device with no medium: INQUIRY answers,
 * TEST UNIT READY says "no medium", READ CAPACITY fails.  Nothing is mounted.
 *
 * usb_poll() runs on EV_USB and with the loop's housekeeping, and lets the
 * platform service a parked command; usb_command() runs on the control thread.  The platform's
 * private_scsi_cmd() hook parks each vendor command and raises EV_USB.
 * padtool/padtool.py and padtool/padgui.pyw (Linux/Windows SCSI) are the clients. */

#define OP          0xf5
#define PROTOCOL    2

#define STATS_VERSION 1

enum { CMD_IDENTIFY = 0x00, CMD_STATUS = 0x01, CMD_LOG = 0x02, CMD_STATS = 0x03, CMD_INPUT = 0x04, CMD_CLOCK = 0x05,
       CMD_REBOOT = 0x10, CMD_REBOOT_LOADER = 0x11, CMD_RELINK = 0x12, CMD_CONNECT = 0x13, CMD_PROFILE = 0x14, CMD_PEEK = 0x15, CMD_TRACE = 0x16, CMD_PAIR = 0x17, CMD_CALIBRATE = 0x18, CMD_LED_DEMO = 0x19, CMD_RUMBLE = 0x1a,
       CMD_DISCONNECT = 0x1b, CMD_PROBE = 0x1c, CMD_FIELD = 0x20, CMD_READ = 0x21, CMD_WRITE = 0x22, CMD_COMMIT = 0x23, CMD_DROP = 0x24,
       CMD_EPHEMERAL = 0x25, CMD_RESULT = 0x26, CMD_DEFAULTS = 0x27, CMD_TOOL_ONLY = 0x31, CMD_USB_PROBE = 0x3f };

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
#ifndef NO_SETTINGS
/* The last WRITE: a code per item; a board short of RAM keeps fewer. */
#ifndef USB_RESULTS
#define USB_RESULTS 256
#endif
static u8 result[USB_RESULTS];
static u32 result_len;
#endif

u32 usb_command(const u8 cdb[16], u8 *data, u32 max, int from_host)
{
    u32 n = 0;

#ifdef NO_SETTINGS
    (void)from_host;             /* only WRITE takes data from the host */
#endif
    if (cdb[0] != OP) {
        pending.kind = REQ_REBOOT_LOADER;
        return 0;
    }
    switch (cdb[1]) {
    case CMD_IDENTIFY: {
        const u8 *uid = plat_unique_id();

        if (max < 56) return 0;
        memcpy(data, "C2PD", 4);
        util_put16(data + 4, PROTOCOL);
        data[6] = BOARD_KIND;
        data[7] = 0;
        strncpy((char *)data + 8, BUILD_ID, 16);
        if (uid) memcpy(data + 24, uid, 16);
        strncpy((char *)data + 40, BOARD_PID, 16);
        n = 56;
        if (max >= 88) {
            strncpy((char *)data + 56, board_name, 32);
            n = 88;
        }
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
    case CMD_INPUT: {
        const struct pad *p = status ? status->input : NULL;
        const struct in_raw *raw = status ? status->raw : NULL;
        u32 i;
        if (!p || !raw || max < 36) return 0;
        memset(data, 0, 36);
        data[0] = 1;
        data[1] = raw->sticks_valid;
        util_put32(data + 4, p->buttons);
        util_put16(data + 8, (u16)p->lx);
        util_put16(data + 10, (u16)p->ly);
        util_put16(data + 12, (u16)p->rx);
        util_put16(data + 14, (u16)p->ry);
        util_put16(data + 16, p->l2);
        util_put16(data + 18, p->r2);
        for (i = 0; i < 4; i++) util_put16(data + 20 + 2 * i, raw->stick[i]);
        util_put16(data + 28, raw->trigger[0]);
        util_put16(data + 30, raw->trigger[1]);
        util_put32(data + 32, status->uptime_ms);
        n = 36;
        break;
    }
    case CMD_CLOCK:              /* the microsecond clock, for mapping it onto the PC's */
        if (max < 4) return 0;
        util_put32(data, plat_us());
        n = 4;
        break;
    case CMD_PROBE: {            /* byte 2: 1 start, edges in bytes 3-6, period ms in 7-8;
                                    2 stop; 0 take: u32 count, u32 lost, u8 active, 3 spare,
                                    then count records of edge us, sent us, level, 3 spare */
        struct util_probe_edge e[16];
        u32 lost, k, i;
        u8 active;

        if (cdb[2] == 1) util_probe_start(util_get32(cdb + 3), (u32)(cdb[7] | cdb[8] << 8));
        else if (cdb[2] == 2) util_probe_stop();
        else {
            if (max < 12) return 0;
            k = util_probe_take(e, (max - 12) / 12 < 16 ? (max - 12) / 12 : 16, &lost, &active);
            util_put32(data, k);
            util_put32(data + 4, lost);
            data[8] = active;
            data[9] = data[10] = data[11] = 0;
            for (i = 0; i < k; i++) {
                util_put32(data + 12 + 12 * i, e[i].edge_us);
                util_put32(data + 16 + 12 * i, e[i].sent_us);
                data[20 + 12 * i] = e[i].level;
                data[21 + 12 * i] = data[22 + 12 * i] = data[23 + 12 * i] = 0;
            }
            n = 12 + 12 * k;
        }
        break;
    }
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
    case CMD_CALIBRATE:
#ifndef BOARD_DONGLE
        pending.kind = REQ_CALIBRATE;
        pending.slot = SLOT_NONE;
#endif
        break;
    case CMD_LED_DEMO:
#if !defined(BOARD_DONGLE) && !defined(NO_SETTINGS)
        if (cdb[2] < ux_led_demo_count && max) {
            n = (u32)strlen(ux_led_demos[cdb[2]].name) + 1;
            if (n > max) n = max;
            memcpy(data, ux_led_demos[cdb[2]].name, n);
            data[n - 1] = 0;
        }
        if (cdb[3] || cdb[2] == 0xff) {
            pending.kind = REQ_LED_DEMO;
            pending.slot = cdb[2] < ux_led_demo_count ? cdb[2] : SLOT_NONE;
        }
#endif
        break;
    case CMD_RUMBLE:
#ifndef BOARD_DONGLE
        pending.kind = REQ_RUMBLE;
        pending.motor[0] = cdb[2];
        pending.motor[1] = cdb[3];
        pending.ms = (u16)(cdb[4] | cdb[5] << 8);
#endif
        break;
    case CMD_CONNECT:            /* argument: slot, 0xfe the last */
        pending.kind = REQ_CONNECT;
        pending.slot = cdb[2];
        break;
    case CMD_DISCONNECT:
#ifndef BOARD_DONGLE
        pending.kind = REQ_DISCONNECT;
        pending.slot = SLOT_NONE;
#endif
        break;
#ifndef NO_SETTINGS
    case CMD_FIELD:              /* argument: field number */
        n = set_describe(util_get32(cdb + 2), data, max);
        break;
    case CMD_DEFAULTS:           /* same packet as READ, all default values */
#ifndef BOARD_DONGLE
        n = set_read_defaults(cdb[2], data, max);
#endif
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
#endif
#ifdef BOARD_DONGLE
    case CMD_TOOL_ONLY:
        dongle_tool_only();
        break;
#endif
#ifdef USB_PROBE
    case CMD_USB_PROBE:          /* test builds: platform_jieli.c usb_probe() */
        plat_usb_probe_request(util_get32(cdb + 2));   /* argument: USB_IO_CON0 bits to add */
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
static int tool_only_seen;
void dongle_tool_only(void) { tool_only_seen++; }
#endif
static const u8 uid[16] = { 1, 2, 3 };
const char board_name[] = "Test pad";
#ifndef BOARD_DONGLE
const struct ux_led_demo ux_led_demos[] = { { "one" }, { "two" } };
const u8 ux_led_demo_count = 2;
#endif
const u8 *plat_unique_id(void) { return uid; }
void plat_usb_service(void) {}
void plat_prof_start(u16 hz, u32 zoom, u8 shift) { (void)hz; (void)zoom; (void)shift; }
u32 plat_prof_read(u32 offset, u8 *out, u32 max) { (void)offset; (void)out; (void)max; return 0; }
u32 plat_peek(u32 addr, u8 *out, u32 max) { (void)addr; (void)out; (void)max; return 0; }
void util_trace_enable(int on) { (void)on; }
u32 util_trace_read(u32 offset, u8 *out, u32 max) { (void)offset; (void)out; (void)max; return 0; }
u32 plat_us(void) { return 0; }
void util_probe_start(u32 edges, u32 period_ms) { (void)edges; (void)period_ms; }
void util_probe_stop(void) {}
u32 util_probe_take(struct util_probe_edge *out, u32 max, u32 *lost, u8 *active)
{
    (void)out; (void)max;
    *lost = 0;
    *active = 0;
    return 0;
}
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
u32 set_read_defaults(u8 t, u8 *out, u32 max) { return set_read(t, out, max); }
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

    CHECK(usb_command(cdb, data, 40, 0) == 0);
    CHECK(usb_command(cdb, data, 87, 0) == 56);
    CHECK(!memcmp(data, "C2PD", 4) && data[4] == PROTOCOL && data[25] == 2);
    CHECK(!strncmp((char *)data + 40, BOARD_PID, 16));
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 88 && !strcmp((char *)data + 56, board_name));
    CHECK(data[6] == BOARD_KIND && data[7] == 0);
#ifdef BOARD_DONGLE
    cdb[1] = CMD_TOOL_ONLY;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 0 && tool_only_seen == 1);
    memset(cdb + 2, 0, 6);
#endif
    status = &st;
    cdb[1] = CMD_STATUS;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 16);
    CHECK(data[0] == CTL_IDLE && data[4] == (3900 & 0xff) && data[8] == (1234 & 0xff));
    cdb[1] = CMD_INPUT;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 0);    /* none */
    {
        static struct pad in = { PAD_A | PAD_LEFT, -2, 3, 0, 0, 0, 65535 };
        static struct in_raw raw = { 0, 1, { 100, 200, 300, 400 }, { 5, 6 } };

        st.input = &in;
        st.raw = &raw;
        CHECK(usb_command(cdb, data, sizeof(data), 0) == 36);
        CHECK(data[0] == 1 && data[1] == 1 && data[4] == PAD_A && data[6] == (PAD_LEFT >> 16));
        CHECK(data[8] == 0xfe && data[9] == 0xff && data[10] == 3 && data[18] == 0xff);
        CHECK(data[20] == 100 && data[26] == (400 & 0xff) && data[28] == 5 && data[32] == (1234 & 0xff));
        st.input = NULL;
    }
    cdb[1] = CMD_LOG;
    cdb[2] = 5;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 11 && data[0] == 7 && data[4] == 3);
    r = usb_poll(&st);
    CHECK(r.kind == REQ_NONE);
    cdb[1] = CMD_DISCONNECT;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 0);
    r = usb_poll(&st);
#ifndef BOARD_DONGLE
    CHECK(r.kind == REQ_DISCONNECT && r.slot == SLOT_NONE);
#else
    CHECK(r.kind == REQ_NONE);
#endif
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
    cdb[1] = CMD_DEFAULTS;
    cdb[2] = 2;
#if defined(BOARD_DONGLE) || defined(NO_SETTINGS)
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 0);
#else
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 3 && data[1] == 2);
#endif
    cdb[1] = CMD_CALIBRATE;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 0);
    r = usb_poll(&st);
#ifdef BOARD_DONGLE
    CHECK(r.kind == REQ_NONE);
#else
    CHECK(r.kind == REQ_CALIBRATE && r.slot == SLOT_NONE);
#endif
#if !defined(BOARD_DONGLE) && defined(NO_SETTINGS)
    cdb[1] = CMD_LED_DEMO;
    cdb[2] = 1;
    cdb[3] = 1;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 0 && usb_poll(&st).kind == REQ_NONE);
    memset(cdb + 2, 0, 4);
#elif !defined(BOARD_DONGLE)
    cdb[1] = CMD_LED_DEMO;
    cdb[2] = 1;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 4 && !strcmp((char *)data, "two"));
    CHECK(usb_poll(&st).kind == REQ_NONE);                  /* the name only */
    cdb[3] = 1;
    usb_command(cdb, data, sizeof(data), 0);
    r = usb_poll(&st);
    CHECK(r.kind == REQ_LED_DEMO && r.slot == 1);
    cdb[2] = 2;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 0);    /* past the last */
    cdb[2] = 0xff; cdb[3] = 0;
    usb_command(cdb, data, sizeof(data), 0);
    r = usb_poll(&st);
    CHECK(r.kind == REQ_LED_DEMO && r.slot == SLOT_NONE);
    cdb[3] = 0;
    cdb[1] = CMD_RUMBLE;
    cdb[2] = 64; cdb[3] = 255; cdb[4] = 0xe8; cdb[5] = 0x03;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 0);
    r = usb_poll(&st);
    CHECK(r.kind == REQ_RUMBLE && r.motor[0] == 64 && r.motor[1] == 255 && r.ms == 1000);
    memset(cdb + 2, 0, 4);
#endif
#ifdef NO_SETTINGS
    cdb[1] = CMD_FIELD;
    cdb[2] = 1;
    data[0] = 0x55;
    CHECK(usb_command(cdb, data, sizeof(data), 0) == 0 && data[0] == 0x55);
    cdb[1] = CMD_WRITE;
    CHECK(usb_command(cdb, data, 9, 1) == 0);
    cdb[1] = CMD_COMMIT;
    usb_command(cdb, data, sizeof(data), 0);
    CHECK(applied == 0);
#else
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
#endif
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

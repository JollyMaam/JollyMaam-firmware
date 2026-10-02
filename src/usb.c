#include <string.h>
#include "pad.h"

/* Vendor SCSI commands on the mass-storage interface; architecture/usb.md.
 * CDB: [0] 0xF5, [1] command, [2..5] argument (little-endian).  The Jieli
 * loader's own opcodes (0xFB 0xFC 0xFD) reboot into the loader, as the SDK
 * does, so its tools find the loader behind a running pad. */

#define OP          0xf5
#define PROTOCOL    1

#define STATS_VERSION 2

enum { CMD_IDENTIFY = 0x00, CMD_STATUS = 0x01, CMD_LOG = 0x02, CMD_STATS = 0x03,
       CMD_REBOOT = 0x10, CMD_REBOOT_LOADER = 0x11 };

#ifndef BUILD_ID
#define BUILD_ID "host"
#endif

static struct ctl_request pending;
static const struct usb_status *status;

u32 usb_command(const u8 cdb[16], u8 *data, u32 max)
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
static const u8 uid[16] = { 1, 2, 3 };
const u8 *plat_unique_id(void) { return uid; }
void plat_usb_service(void) {}
u32 util_log_read(u32 *seq, char *out, u32 max)
{
    (void)max;
    *seq = 7;
    memcpy(out, "hi\n", 3);
    return 3;
}
void util_put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
void util_put32(u8 *p, u32 v) { util_put16(p, (u16)v); util_put16(p + 2, (u16)(v >> 16)); }
u32 util_get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

int main(void)
{
    u8 cdb[16] = { OP, CMD_IDENTIFY }, data[512];
    static struct usb_status st = { CTL_IDLE, SLOT_NONE, 1, 0, 3900, 1234 };
    struct ctl_request r;

    CHECK(usb_command(cdb, data, sizeof(data)) == 40);
    CHECK(!memcmp(data, "C2PD", 4) && data[4] == PROTOCOL && data[25] == 2);
    status = &st;
    cdb[1] = CMD_STATUS;
    CHECK(usb_command(cdb, data, sizeof(data)) == 16);
    CHECK(data[0] == CTL_IDLE && data[4] == (3900 & 0xff) && data[8] == (1234 & 0xff));
    cdb[1] = CMD_LOG;
    cdb[2] = 5;
    CHECK(usb_command(cdb, data, sizeof(data)) == 11 && data[0] == 7 && data[4] == 3);
    r = usb_poll(&st);
    CHECK(r.kind == REQ_NONE);
    {
        static struct ctl_stats stats;

        stats.passes = 0x01020304;
        st.stats = &stats;
        cdb[1] = CMD_STATS;
        cdb[2] = 1;
        CHECK(usb_command(cdb, data, sizeof(data)) == 4 + sizeof(stats));
        CHECK(data[0] == STATS_VERSION && data[2] == (u8)sizeof(stats) && data[4 + 8] == 4);
        CHECK(usb_poll(&st).kind == REQ_STATS_RESET);
    }
    cdb[1] = CMD_REBOOT_LOADER;
    usb_command(cdb, data, sizeof(data));
    r = usb_poll(&st);
    CHECK(r.kind == REQ_REBOOT_LOADER);
    cdb[0] = 0xfc;
    usb_command(cdb, data, sizeof(data));
    CHECK(usb_poll(&st).kind == REQ_REBOOT_LOADER);
    CHECK(usb_poll(&st).kind == REQ_NONE);
    return TEST_DONE("usb");
}
#endif

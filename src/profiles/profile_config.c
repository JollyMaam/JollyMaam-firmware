#include <string.h>
#include "pad.h"
#include "../updater/updater_gatt.h"

/* Config mode (Home+M held): the tool device's commands over Bluetooth LE,
 * in the updater's GATT service (updater_gatt.h) on the updater's address,
 * advertised as "<board> Config" to anyone, with no pairing and no bond.  A
 * host writes a frame to REQUEST: u8 sub-command, u32 argument
 * (little-endian), then the data a host-to-pad command carries; a command
 * with an answer answers in a notification of REPLY: u8 sub-command, then
 * the answer.  The same frames as the updater's (updater_frame()), so one
 * host program talks to both: IDENTIFY's byte 7 tells them apart, and
 * UPDATER restarts into the updater on the same address.  A write carries
 * at most BT_WRITE_MAX bytes, an answer CONFIG_REPLY_MAX. */

#define CONFIG_REPLY_MAX 158     /* bt_pad.c's largest message */

static u8 scan_response[31];

static u8 advertising(const struct bt_bond *bond, int pairing, int wake, u8 out[31])
{
    (void)bond; (void)pairing; (void)wake;
    memcpy(out, updater_adv, sizeof(updater_adv));
    return sizeof(updater_adv);
}

static const struct bt_device config_device = {
    .transport = BT_LE,
    .name = "Config",
    .att_db = updater_att_db,
    .advertising = advertising,
    .scan_response = scan_response, .scan_response_len = sizeof(scan_response),   /* zeros after the name */
    .interval_min = 6, .interval_max = 12,
};

void profile_config_address(u8 out[6])
{
    updater_address(out);
}

static u8 begin(const struct profile_session *s)
{
    u32 n = strlen(board_name);

    (void)s;
    if (n > 22) n = 22;
    scan_response[0] = (u8)(n + 8);
    scan_response[1] = 0x09;     /* complete local name */
    memcpy(scan_response + 2, board_name, n);
    memcpy(scan_response + 2 + n, " Config", 7);
    return 0;
}

/* Nothing for the host but answers. */
static int report(const struct pad *pad, u32 now, u8 *out, u8 *id)
{
    (void)pad; (void)now; (void)out; (void)id;
    return 0;
}

static int write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    static u8 reply[CONFIG_REPLY_MAX];
    u8 cdb[16] = { 0xf5 };
    u32 n;

    (void)now; (void)bond;
    if (w->kind != BT_WRITE_ATT || w->id != UPD_REQUEST || w->len < 5) return 0;
    cdb[1] = w->data[0];
    memcpy(cdb + 2, w->data + 1, 4);
    if (w->len > 5) {
        usb_command(cdb, (u8 *)w->data + 5, w->len - 5u, 1);
        return 0;
    }
    memset(reply, 0, sizeof(reply));
    n = usb_command(cdb, reply + 1, sizeof(reply) - 1, 0);
    if (!n) return 0;
    reply[0] = w->data[0];
    if (!host_send(UPD_REPLY, reply, (u16)(n + 1))) util_log("config: reply %02x dropped", reply[0]);
    return 0;
}

const struct profile profile_config = {
    .name = "config", .device = &config_device,
    .begin = begin, .report = report, .write = write,
};

#include <string.h>
#include "pad.h"
#include "../updater/updater_gatt.h"

/* Config mode (Home+M held): the tool device's commands over Bluetooth LE,
 * in the updater's GATT service (updater_gatt.h) on the updater's address,
 * advertised as "<board> Config" to anyone, with no pairing and no bond.  A
 * host writes a frame to REQUEST: u8 sub-command, u32 argument
 * (little-endian), then the data a host-to-pad command carries, up to
 * CONFIG_DATA_MAX bytes: a frame carries at most BT_WRITE_MAX, so with more
 * the host sends frames with bit 7 of the sub-command set, whose data the pad
 * collects, then the last without it, which runs the command; a command
 * with an answer answers in notifications of REPLY: u8 sub-command, then
 * the answer, up to CONFIG_ANSWER_MAX bytes in pieces of CONFIG_PIECE (an
 * empty answer is one piece of nothing, so every command answers); every
 * piece but the last has bit 7 of the sub-command set (sub-commands are
 * below 0x80).  The same frames as the updater's (updater_frame()), whose
 * answers are one piece, so one host program talks to both: IDENTIFY's
 * byte 7 tells them apart, and UPDATER restarts into the updater on the
 * same address.  The host waits for each answer before the next write, so
 * the pieces fit bt_pad.c's message queue. */

#define CONFIG_REPLY_MAX  158    /* bt_pad.c's largest message */
#define CONFIG_PIECE      (CONFIG_REPLY_MAX - 1)
#define CONFIG_ANSWER_MAX 512    /* as on the tool device */
#define CONFIG_DATA_MAX   488
#define CONFIG_MORE       0x80

static u8 scan_response[31];
static u8 buf[CONFIG_ANSWER_MAX];        /* the data collected, then the answer */
static u32 held;                         /* bytes of data collected; ~0: too many */

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
    .interval_min = 6, .interval_max = 6, .unencrypted = 1,   /* 7.5 ms: two per command */
};

void profile_config_address(u8 out[6])
{
    updater_address(out);
}

static u8 begin(const struct profile_session *s)
{
    u32 n = strlen(board_name);

    (void)s;
    held = 0;
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
    static u8 piece[CONFIG_REPLY_MAX];
    u8 cdb[16] = { 0xf5 };
    u8 sub;
    u32 n, off, k;

    (void)now; (void)bond;
    if (w->kind != BT_WRITE_ATT || w->id != UPD_REQUEST || w->len < 5) return 0;
    sub = w->data[0] & (u8)~CONFIG_MORE;
    n = w->len - 5u;
    if (held != ~0u && held + n <= CONFIG_DATA_MAX) {
        memcpy(buf + held, w->data + 5, n);
        held += n;
    } else {
        held = ~0u;
    }
    if (w->data[0] & CONFIG_MORE) return 0;
    cdb[1] = sub;
    memcpy(cdb + 2, w->data + 1, 4);
    n = held;
    held = 0;
    if (n == ~0u) {
        util_log("config: command %02x over %u bytes, dropped", sub, (unsigned)CONFIG_DATA_MAX);
        return 0;
    }
    if (n) {
        usb_command(cdb, buf, n, 1);
        return 0;
    }
    memset(buf, 0, sizeof(buf));
    n = usb_command(cdb, buf, sizeof(buf), 0);
    off = 0;
    do {                         /* an empty answer too: past the last FIELD, say */
        k = n - off < CONFIG_PIECE ? n - off : CONFIG_PIECE;
        piece[0] = (u8)(sub | (off + k < n ? CONFIG_MORE : 0));
        memcpy(piece + 1, buf + off, k);
        if (!host_send(UPD_REPLY, piece, (u16)(k + 1))) {
            util_log("config: reply %02x dropped at %u", sub, (unsigned)off);
            break;
        }
        off += k;
    } while (off < n);
    return 0;
}

const struct profile profile_config = {
    .name = "config", .device = &config_device,
    .begin = begin, .report = report, .write = write,
};

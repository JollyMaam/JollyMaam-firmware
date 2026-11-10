#include <string.h>
#include "pad.h"
#include "../updater/updater_gatt.h"

/* Config mode (Home+M held): the tool device's commands over Bluetooth LE,
 * in the updater's GATT service (updater_gatt.h) on the updater's address,
 * advertised as "<board> Config" to anyone, with no pairing and no bond.
 *
 * Every program on the host that subscribes to REPLY gets every
 * notification (BlueZ: and one copy more for each other program
 * subscribed, back to back), so a command and its answer carry a tag the
 * host picks, as the tool device's carry a client and tag:
 *
 *   request frame (REQUEST): u8 sub-command (bit 7: more frames follow),
 *     u8 tag (6 bits), u32 argument (little-endian), then data;
 *   answer piece (REPLY): u8 tag << 2 | parity << 1 | more, then a piece
 *     of the answer; parity alternates from 0, so a host drops a piece with
 *     the parity of the one before (a copy); "more" with no data: refused.
 *
 * A host-to-pad command's data (up to CONFIG_DATA_MAX, less on a pad with
 * a smaller buffer: IDENTIFY says how many, below) goes in frames of at
 * most BT_WRITE_MAX; the pad collects them and the last runs the command.
 * Its answer, up to CONFIG_ANSWER_MAX bytes, goes in pieces of CONFIG_PIECE;
 * every command answers, one with data or none with one piece of nothing,
 * so a host always learns whether its command ran or was refused.
 * One command at a time: a frame with another tag while one is collected
 * or answered is refused (the host sends its command again).  A command
 * left half sent for CONFIG_HALF_MS is given up.  Here IDENTIFY answers 90
 * bytes: u16 at 88, the most data a command takes.
 *
 * The updater takes the same frames (updater_frame()) and answers each in
 * one piece, so one host program talks to both: IDENTIFY's byte 7 tells
 * them apart, and UPDATER restarts into the updater on the same address.
 *
 * The command runs in profile_config_service(), from the control loop, as
 * the tool device's do; the answer goes out from there, a piece whenever
 * the link takes one.  One buffer holds the CDB, then the data collected
 * or the answer, each piece's header written over the byte before it (the
 * CDB's last, unused, or answer already sent).  With CONFIG_LENT_BUFFER it
 * is the tool device's (plat_tool_lend), borrowed for a command: a pad
 * short of RAM keeps no second one.  The scan response is Generic BLE's
 * (profile_scan_name()): one session at a time. */

#ifndef CONFIG_REPLY_MAX
#define CONFIG_REPLY_MAX  158    /* bt_pad.c's largest message */
#endif
#define CONFIG_PIECE      (CONFIG_REPLY_MAX - 1)
#define CONFIG_ANSWER_MAX 512    /* as on the tool device */
#define CONFIG_DATA_MAX   488
#define CONFIG_MORE       0x80
#define CONFIG_HEAD       6      /* a frame's sub-command, tag, argument */
#define CONFIG_OVER       0xffff /* held: too much data, or no buffer */
#define CONFIG_HALF_MS    1000   /* a command half sent this long: given up */
#define CONFIG_REFUSALS   4
#define CDB               16     /* buf: the CDB, then the data or the answer */

static u8 *buf;
static struct {
    u16 room;                    /* of buf, after the CDB */
    u16 held;                    /* bytes of data collected; CONFIG_OVER: dropped */
    u16 answer, off;             /* the answer's length; how much of it is out */
    u8 ready, sending;
    u8 collecting;               /* frames of a command came, its last not yet */
    u8 tag;                      /* the command's, while collecting, ready or sending */
    u8 parity;                   /* the next piece's */
    u32 last_ms;                 /* its last frame */
    u8 refused[CONFIG_REFUSALS], refusals;   /* tags to tell "send it again" */
} cf;

#ifdef CONFIG_LENT_BUFFER
static int take(void)
{
    u32 n;

    if (!buf && (buf = plat_tool_lend(&n)) != NULL) cf.room = (u16)(n - CDB);
    return buf != NULL;
}

static void give_back(void)
{
    if (buf) plat_tool_return();
    buf = NULL;
}
#else
static u8 own[CDB + CONFIG_ANSWER_MAX];

static int take(void)
{
    buf = own;
    cf.room = CONFIG_ANSWER_MAX;
    return 1;
}

static void give_back(void) {}
#endif

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
    .scan_response = profile_scan_response, .scan_response_len = 31,   /* zeros after the name */
    .interval_min = 6, .interval_max = 6, .unencrypted = 1,   /* 7.5 ms: two per command */
    .write_paced = 1,            /* a frame may come while a command runs */
};

void profile_config_address(u8 out[6])
{
    updater_address(out);
}

/* A command done: its buffer goes back, refusals not yet sent stay. */
static void done(void)
{
    u8 refused[CONFIG_REFUSALS], refusals = cf.refusals;

    memcpy(refused, cf.refused, sizeof(refused));
    memset(&cf, 0, sizeof(cf));
    memcpy(cf.refused, refused, sizeof(refused));
    cf.refusals = refusals;
    give_back();
}

/* A new link, a new session: nothing of the last command stays. */
static void end(void)
{
    memset(&cf, 0, sizeof(cf));
    give_back();
}

static void refuse(u8 tag)
{
    if (cf.refusals < CONFIG_REFUSALS) cf.refused[cf.refusals++] = tag;
}

static u8 begin(const struct profile_session *s)
{
    (void)s;
    end();
    profile_scan_name(" Config");
    return 0;
}

/* Nothing for the host but answers. */
static int report(const struct pad *pad, u32 now, u8 *out, u8 *id)
{
    (void)pad; (void)now; (void)out; (void)id;
    return 0;
}

/* A frame: its data collected; the last one leaves the command for
 * profile_config_service().  While a command is collected or answered, a
 * frame with another tag is refused. */
static int write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    u32 n, max;
    u8 sub, tag;

    (void)bond;
    if (w->kind != BT_WRITE_ATT || w->id != UPD_REQUEST || w->len < CONFIG_HEAD) return 0;
    sub = w->data[0];
    tag = w->data[1] & 0x3f;
    if (cf.collecting && !cf.ready && !cf.sending && now - cf.last_ms > CONFIG_HALF_MS) {
        util_log("config: command %02x half sent, given up", buf[1]);
        done();
    }
    if (cf.ready || cf.sending || (cf.collecting && tag != cf.tag)) {
        refuse(tag);
        return 0;
    }
    if (!cf.collecting) {
        cf.collecting = 1;
        cf.tag = tag;
        cf.held = 0;
    }
    cf.last_ms = now;
    if (!cf.held && !take()) cf.held = CONFIG_OVER;  /* the tool device has a command */
    n = w->len - (u32)CONFIG_HEAD;
    max = cf.room < CONFIG_DATA_MAX ? cf.room : CONFIG_DATA_MAX;
    if (cf.held != CONFIG_OVER && cf.held + n <= max) {
        memcpy(buf + CDB + cf.held, w->data + CONFIG_HEAD, n);
        cf.held = (u16)(cf.held + n);
    } else {
        cf.held = CONFIG_OVER;
    }
    if (sub & CONFIG_MORE) return 0;
    if (cf.held == CONFIG_OVER) {
        util_log("config: command %02x over %u bytes or busy, refused", sub, (unsigned)max);
        done();
        refuse(tag);
        return 0;
    }
    memset(buf, 0, CDB);
    buf[0] = 0xf5;
    buf[1] = sub;
    memcpy(buf + 2, w->data + 2, 4);
    cf.ready = 1;
    return 0;
}

/* The answer's next piece, if the link takes it; returns whether more
 * is to go. */
static int piece(void)
{
    u16 k = cf.answer - cf.off < CONFIG_PIECE ? cf.answer - cf.off : CONFIG_PIECE;
    u8 *at = buf + CDB - 1 + cf.off;
    int more = cf.off + k < cf.answer;

    *at = (u8)(cf.tag << 2 | cf.parity << 1 | more);
    if (!host_send(UPD_REPLY, at, (u16)(k + 1))) return 0;
    cf.off = (u16)(cf.off + k);
    cf.parity ^= 1;
    if (more) return 1;
    done();
    return 0;
}

void profile_config_service(void)
{
    u32 n;

    while (cf.refusals) {        /* "send it again": "more", no data */
        u8 r = (u8)(cf.refused[0] << 2 | 1);

        if (!host_send(UPD_REPLY, &r, 1)) break;
        memmove(cf.refused, cf.refused + 1, --cf.refusals);
    }
    if (cf.ready) {
        cf.ready = 0;
        if (cf.held) {           /* data to the pad: an empty answer says it ran */
            usb_command(buf, buf + CDB, cf.held, 1);
            n = 0;
        } else {
            memset(buf + CDB, 0, cf.room);
            n = usb_command(buf, buf + CDB, cf.room, 0);
        }
        if (buf[1] == 0x00 && n >= JM_ID_EXT_SIZE && cf.room >= JM_ID_EXT_SIZE + 2) {  /* IDENTIFY: the data a command takes */
            util_put16(buf + CDB + JM_ID_EXT_SIZE, cf.room < CONFIG_DATA_MAX ? cf.room : CONFIG_DATA_MAX);
            n = JM_ID_EXT_SIZE + 2;
        }
        cf.answer = (u16)n;
        cf.off = 0;
        cf.parity = 0;
        cf.sending = 1;          /* an empty answer too: past the last FIELD, say */
    }
    while (cf.sending && piece()) {}
}

const struct profile profile_config = {
    .name = "config", .device = &config_device,
    .begin = begin, .end = end, .report = report, .write = write,
};

#include <string.h>
#include "pad.h"
#include "../updater/updater_gatt.h"

/* Config mode (Home+M held): the tool device's commands over Bluetooth LE,
 * in the updater's GATT service (updater_gatt.h) on the updater's address,
 * advertised as "<board> Config" to anyone, with no pairing and no bond.  A
 * host writes a frame to REQUEST: u8 sub-command, u32 argument
 * (little-endian), then the data a host-to-pad command carries, up to
 * CONFIG_DATA_MAX bytes (less on a pad with a smaller buffer: IDENTIFY says
 * how many, below): a frame carries at most BT_WRITE_MAX, so with more
 * the host sends frames with bit 7 of the sub-command set, whose data the pad
 * collects, then the last without it, which runs the command; a command
 * with an answer answers in notifications of REPLY: u8 sub-command, then
 * the answer, up to CONFIG_ANSWER_MAX bytes in pieces of CONFIG_PIECE (an
 * empty answer is one piece of nothing, so every command answers); every
 * piece but the last has bit 7 of the sub-command set (sub-commands are
 * below 0x80).  The same frames as the updater's (updater_frame()), whose
 * answers are one piece, so one host program talks to both: IDENTIFY's
 * byte 7 tells them apart, and UPDATER restarts into the updater on the
 * same address.  Here IDENTIFY answers 90 bytes: u16 at 88, the most data
 * a command takes.  The host waits for each answer before the next write.
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
#define CONFIG_OVER       0xffff /* held: too much data, or no buffer */
#define CDB               16     /* buf: the CDB, then the data or the answer */

static u8 *buf;
static struct {
    u16 room;                    /* of buf, after the CDB */
    u16 held;                    /* bytes of data collected; CONFIG_OVER: dropped */
    u16 answer, off;             /* the answer's length; how much of it is out */
    u8 ready, sending;
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

/* A new link, a new session: nothing of the last command stays. */
static void end(void)
{
    memset(&cf, 0, sizeof(cf));
    give_back();
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
 * profile_config_service().  Its first frame drops what is left of the
 * last command's answer. */
static int write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    u32 n, max;
    u8 sub = w->data[0];

    (void)now; (void)bond;
    if (w->kind != BT_WRITE_ATT || w->id != UPD_REQUEST || w->len < 5) return 0;
    if (cf.sending || cf.ready) {
        util_log("config: command %02x dropped, %02x not done", buf[1], sub & (u8)~CONFIG_MORE);
        end();
    }
    if (!cf.held && !take()) cf.held = CONFIG_OVER;  /* the tool device has a command */
    n = w->len - 5u;
    max = cf.room < CONFIG_DATA_MAX ? cf.room : CONFIG_DATA_MAX;
    if (cf.held != CONFIG_OVER && cf.held + n <= max) {
        memcpy(buf + CDB + cf.held, w->data + 5, n);
        cf.held = (u16)(cf.held + n);
    } else {
        cf.held = CONFIG_OVER;
    }
    if (sub & CONFIG_MORE) return 0;
    if (cf.held == CONFIG_OVER) {
        util_log("config: command %02x over %u bytes or busy, dropped", sub, (unsigned)max);
        end();
        return 0;
    }
    memset(buf, 0, CDB);
    buf[0] = 0xf5;
    buf[1] = sub;
    memcpy(buf + 2, w->data + 1, 4);
    cf.ready = 1;
    return 0;
}

/* The answer's next piece, if the link takes it; returns whether more
 * is to go. */
static int piece(void)
{
    u16 k = cf.answer - cf.off < CONFIG_PIECE ? cf.answer - cf.off : CONFIG_PIECE;
    u8 *at = buf + CDB - 1 + cf.off;

    *at = (u8)(buf[1] | (cf.off + k < cf.answer ? CONFIG_MORE : 0));
    if (!host_send(UPD_REPLY, at, (u16)(k + 1))) return 0;
    cf.off = (u16)(cf.off + k);
    if (cf.off < cf.answer) return 1;
    end();
    return 0;
}

void profile_config_service(void)
{
    u32 n;

    if (cf.ready) {
        cf.ready = 0;
        if (cf.held) {
            usb_command(buf, buf + CDB, cf.held, 1);
            end();
            return;
        }
        memset(buf + CDB, 0, cf.room);
        n = usb_command(buf, buf + CDB, cf.room, 0);
        if (buf[1] == 0x00 && n >= JM_ID_EXT_SIZE && cf.room >= JM_ID_EXT_SIZE + 2) {  /* IDENTIFY: the data a command takes */
            util_put16(buf + CDB + JM_ID_EXT_SIZE, cf.room < CONFIG_DATA_MAX ? cf.room : CONFIG_DATA_MAX);
            n = JM_ID_EXT_SIZE + 2;
        }
        cf.answer = (u16)n;
        cf.off = 0;
        cf.sending = 1;          /* an empty answer too: past the last FIELD, say */
    }
    while (cf.sending && piece()) {}
}

const struct profile profile_config = {
    .name = "config", .device = &config_device,
    .begin = begin, .end = end, .report = report, .write = write,
};

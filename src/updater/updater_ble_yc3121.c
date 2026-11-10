#include <string.h>
#include "pad.h"
#include "board.h"
#include "updater.h"
#include "updater_gatt.h"

/* The updater over LE on the YC3121: the GATT service (updater_gatt.h) as a
 * profile on bt_yc3121.c, which the radio core serves, no pairing.  Writes
 * come as bt_poll() events (the core's ring, bt_yc3121.c's queue); each
 * frame's answer goes out as a REPLY notification.  A write the queue had
 * no room for is lost: the host sees it in UPDATE_INFO's count and sends
 * it again.  The core's MTU is 23: an answer goes in notifications of 20
 * bytes, {command | MORE, 19 bytes} but the last, as config mode's. */

#define PIECE   19               /* an answer's bytes in a notification */
#define MORE    0x80
#define SEND_MS 50               /* a notification the core's ring has no room for: retried this long */

static u8 scan[31];
static u8 connected;

static u8 advertising(const struct bt_bond *bond, int pairing, int wake, u8 out[31])
{
    (void)bond;
    (void)pairing;
    (void)wake;
    memcpy(out, updater_adv, sizeof(updater_adv));
    return sizeof(updater_adv);
}

static struct bt_device device = {
    .transport = BT_LE,
    .name = "Update",
    .att_db = updater_att_db,
    .advertising = advertising,
    .scan_response = scan,
};

static const struct profile profile = { .name = "updater", .device = &device };

void updater_ble_start(void)
{
    struct bt_bond none;
    struct bt_link link;
    u8 a[6];

    memset(&none, 0, sizeof(none));
    memset(&link, 0, sizeof(link));
    updater_address(a);
    device.scan_response_len = updater_scan_response(scan);
    bt_start(&profile, a, &none, 0, &link);
}

static void send(const u8 *reply, u32 n)
{
    u8 piece[1 + PIECE];
    u32 off, k, t;

    for (off = 1; off < n; off += k) {
        k = n - off < PIECE ? n - off : PIECE;
        piece[0] = (u8)(reply[0] | (off + k < n ? MORE : 0));
        memcpy(piece + 1, reply + off, k);
        for (t = plat_ms(); !bt_send_message(UPD_REPLY, piece, (u16)(k + 1)); plat_bt_poll())
            if (plat_ms() - t > SEND_MS) {
                util_log("updater: answer %02x not sent at %u of %u", reply[0], off, n);
                return;
            }
    }
}

/* From the loop, every pass: the stack's news, the host's frames. */
int updater_ble_connected(void)
{
    static u8 reply[1 + 128];
    struct bt_ev ev;
    u32 n;

    while (bt_poll(&ev)) {
        if (ev.kind == BT_EV_LINKED) connected = 1;
        if (ev.kind == BT_EV_UNLINKED) connected = 0;
        if (ev.kind != BT_EV_WRITE || ev.write.kind != BT_WRITE_ATT || ev.write.id != UPD_REQUEST) continue;
        n = updater_frame(ev.write.data, ev.write.len, reply, sizeof(reply));
        if (n) send(reply, n);
    }
    return connected;
}

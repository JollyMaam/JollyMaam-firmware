#include <string.h>
#include "pad.h"
#include "att_telink.h"
#include "updater.h"
#include "updater_gatt.h"

/* The updater over LE on the B80: the GATT service (updater_gatt.h) as a
 * profile on bt_telink.c, no pairing.  Each write runs in the stack's
 * callback (write_now) and its response goes after: one write in flight,
 * held while a sector erases.  An answer goes out as a REPLY notification. */

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

static int write_now(u16 handle, const u8 *data, u16 len)
{
    u8 reply[TELINK_ATT_MESSAGE_MAX];
    u32 n;

    if (handle != UPD_REQUEST) return 0;
    n = updater_frame(data, len, reply, sizeof(reply));
    if (n) plat_att_message(UPD_REPLY, reply, (u16)n);
    return 0;
}

static struct bt_device device = {
    .transport = BT_LE,
    .name = "updater",
    .att_db = updater_att_db,
    .advertising = advertising,
    .scan_response = scan,
    .write_now = write_now,
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

/* From the loop, every pass: the stack's news. */
int updater_ble_connected(void)
{
    struct bt_ev ev;

    while (bt_poll(&ev)) {
        if (ev.kind == BT_EV_LINKED) connected = 1;
        if (ev.kind == BT_EV_UNLINKED) connected = 0;
    }
    return connected;
}

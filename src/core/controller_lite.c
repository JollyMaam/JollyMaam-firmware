#include <string.h>
#include "pad.h"

/* The control loop for a pad whose RAM controller.c and settings.c do not
 * fit (the Telink B80's 16 KiB): ctl_main() as controller.c has it, built
 * from the same modules, with what the board can afford.  So far: one
 * session of one LE profile (genericble), started at boot, whose host
 * gets a report whenever the input changes; the input read and calibrated
 * as the Cyclone 2's (by default, nothing stored); the tools' commands.
 * No settings store, slots, remap, Home gestures or lights yet; the
 * requests for them are logged and dropped. */

#define POLL_US 4000             /* connected: the input is looked at this often */

static const struct profile *const profile = &profile_generic_ble;

static struct {
    struct in_raw raw;
    struct pad pad;
    struct usb_status status;
    struct profile_session session;
    u8 linked;
} c;

static const struct set_cal no_calibration;     /* zero: input_calibration.c's defaults */

static void read_input(void)
{
    plat_input(&c.raw);
    in_map(&c.raw, &c.pad);
    cal_apply(&no_calibration, NULL, &c.raw, &c.pad);
}

/* A static random address from the chip's unique ID. */
static void address(u8 out[6])
{
    memcpy(out, plat_unique_id(), 6);
    out[5] |= 0xc0;
}

static void start(void)
{
    memset(&c.session, 0, sizeof(c.session));
    c.session.pairing = 1;
    address(c.session.address);
    profile->begin(&c.session);
    bt_start(profile, c.session.address, NULL, 1, NULL);
}

static void bluetooth(void)
{
    struct bt_ev ev;

    while (bt_poll(&ev)) {
        if (ev.kind == BT_EV_LINKED) {
            c.linked = 1;
            profile->begin(&c.session);          /* the first report goes out whatever it is */
        } else if (ev.kind == BT_EV_UNLINKED) {
            c.linked = 0;
        }
    }
}

static void report(u32 now)
{
    u8 out[64], id = 0;
    int n;

    if (!c.linked || !bt_input_wanted()) return;
    n = profile->report(&c.pad, now, out, &id);
    if (n > 0) bt_send_input(id, out, (u16)n);
}

static void request(const struct ctl_request *r)
{
    switch (r->kind) {
    case REQ_NONE:
        break;
    case REQ_REBOOT:
        util_log("ctl: reboot");
        plat_reboot();
        break;
    case REQ_REBOOT_LOADER:
        util_log("ctl: to the boot loader");
        plat_reboot_loader();
        break;
    default:
        util_log("ctl: request %u is not on this board", r->kind);
        break;
    }
}

void ctl_main(void)
{
    struct ctl_request r;

    c.status.input = &c.pad;
    c.status.raw = &c.raw;
    start();
    for (;;) {
        plat_wait(0, c.linked ? plat_us() + POLL_US : 0);
        bluetooth();
        read_input();
        report(plat_ms());
        c.status.state = c.linked ? CTL_CONNECTED : CTL_PAIRING;
        c.status.slot = c.linked ? 0 : SLOT_NONE;
        c.status.usb_powered = (u8)plat_usb_present();
        c.status.battery_mv = plat_battery_mv();
        c.status.uptime_ms = plat_ms();
        r = usb_poll(&c.status);
        request(&r);
    }
}

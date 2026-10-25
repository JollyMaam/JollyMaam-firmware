#include "pad.h"

/* The control loop for a pad whose RAM controller.c and settings.c do not
 * fit (the Telink B80's 16 KiB): ctl_main() as controller.c has it, built
 * from the same modules, with what the board can afford.  So far: the
 * input, read and calibrated as the Cyclone 2's (by default, nothing
 * stored), and the tools' commands.  No settings store, slots, remap, Home
 * gestures or lights yet; the requests for them are logged and dropped. */

static struct {
    struct in_raw raw;
    struct pad pad;
    struct usb_status status;
} c;

static const struct set_cal no_calibration;     /* zero: input_calibration.c's defaults */

static void read_input(void)
{
    plat_input(&c.raw);
    in_map(&c.raw, &c.pad);
    cal_apply(&no_calibration, NULL, &c.raw, &c.pad);
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

    c.status.state = CTL_IDLE;
    c.status.slot = SLOT_NONE;
    c.status.input = &c.pad;
    c.status.raw = &c.raw;
    for (;;) {
        plat_wait(0, 0);
        read_input();
        c.status.usb_powered = (u8)plat_usb_present();
        c.status.battery_mv = plat_battery_mv();
        c.status.uptime_ms = plat_ms();
        r = usb_poll(&c.status);
        request(&r);
    }
}

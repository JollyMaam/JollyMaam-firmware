#include <string.h>
#include "pad.h"

/* The control loop for a pad whose RAM controller.c and settings.c do not
 * fit (the Telink B80's 16 KiB): ctl_main() as controller.c has it, built
 * from the same modules, with what the board can afford.  So far: one
 * session of one LE profile (genericble), started at boot, whose host
 * gets a report whenever the input changes; the input read and calibrated
 * as the Cyclone 2's (by default, nothing stored); the Home light (blinking
 * while no host is linked, lit when one is); the tools' commands, the motor
 * test among them.  No settings store, slots, remap or Home gestures yet;
 * the requests for them are logged and dropped. */

#define POLL_US 4000             /* connected: the input is looked at this often */

/* The Home light: how bright (of 255) and, unlinked, its blink. */
static const struct {
    u8 level;
    u16 on_ms, off_ms;
} home_light = { 24, 250, 250 };

static const struct profile *const profile = &profile_generic_ble;

static struct {
    struct in_raw raw;
    struct pad pad;
    struct usb_status status;
    struct profile_session session;
    u8 linked;
    u8 lit;                      /* the Home light as last set */
    u32 rumble_until;            /* plat_ms; 0: motors off */
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

/* Returns the next change, plat_ms. */
static u32 lights(u32 now)
{
    struct led_frame f;
    u32 period = home_light.on_ms + home_light.off_ms, at = now % period;
    u8 lit = c.linked || at < home_light.on_ms;

    if (lit != c.lit) {
        memset(&f, 0, sizeof(f));
        f.light[LIGHT_HOME].r = f.light[LIGHT_HOME].g = f.light[LIGHT_HOME].b = lit ? home_light.level : 0;
        plat_leds(&f);
        c.lit = lit;
    }
    if (c.linked) return 0;
    return now - at + (at < home_light.on_ms ? home_light.on_ms : period);
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
    case REQ_RUMBLE:
        plat_motors(r->motor[0], r->motor[1]);
        c.rumble_until = r->ms ? plat_ms() + (r->ms < RUMBLE_TEST_MAX_MS ? r->ms : RUMBLE_TEST_MAX_MS) : 0;
        if (!c.rumble_until) plat_motors(0, 0);
        break;
    default:
        util_log("ctl: request %u is not on this board", r->kind);
        break;
    }
}

void ctl_main(void)
{
    struct ctl_request r;
    u32 next;

    c.status.input = &c.pad;
    c.status.raw = &c.raw;
    c.lit = 0xff;
    start();
    for (;;) {
        next = lights(plat_ms());
        if (c.rumble_until && (!next || (s32)(c.rumble_until - next) < 0)) next = c.rumble_until;
        plat_wait(next, c.linked ? plat_us() + POLL_US : 0);
        if (c.rumble_until && (s32)(plat_ms() - c.rumble_until) >= 0) {
            plat_motors(0, 0);
            c.rumble_until = 0;
        }
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

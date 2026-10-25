#include <string.h>
#include "pad.h"

/* The control loop for a pad whose RAM controller.c and settings.c do not
 * fit (the Telink B80's 16 KiB): ctl_main() as controller.c has it, built
 * from the same modules, with what the board can afford.  So far: one
 * session of one LE profile (genericble), started at boot, whose host
 * gets a report whenever the input changes; the settings (what the board
 * keeps of them), the sensor calibration and its routine; the Home light
 * (blinking while no host is linked, lit when one is); the tools'
 * commands, the motor test among them.  No slots, remap or Home gestures
 * yet; the requests for them are logged and dropped. */

#define POLL_US 4000             /* connected: the input is looked at this often */
#define CAL_SAMPLE_MS 2          /* calibrating: the input is read this often */

/* The Home light: how bright (of 255) and, unlinked, its blink. */
static const struct {
    u8 level;
    u16 on_ms, off_ms;
} home_light = { 24, 250, 250 };

static const struct profile *const profile = &profile_generic_ble;

static struct {
    struct in_raw raw;
    struct pad pad;
    struct pad input;            /* calibrated, as padtool input shows it */
    struct usb_status status;
    struct profile_session session;
    u8 linked;
    u8 lit;                      /* the Home light as last set */
    u32 rumble_until;            /* plat_ms; 0: motors off */
    u8 calibrating;
    struct set_cal cal;          /* what the routine has measured */
} c;

static void read_input(u32 now)
{
    plat_input(&c.raw);
    in_map(&c.raw, &c.pad);
    cal_apply(set_cal(), NULL, &c.raw, &c.pad);
    c.input = c.pad;
    if (!c.calibrating) return;
    switch (cal_step(&c.raw, now, &c.cal)) {
    case CAL_SAVE:
        set_cal_store(&c.cal);
        break;
    case CAL_IDLE:
        c.calibrating = 0;
        break;
    }
    c.pad.buttons = 0;
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

static __attribute__((noinline)) void bluetooth(void)
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

static __attribute__((noinline)) void report(u32 now)
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
    struct rgb cal;
    u32 period = home_light.on_ms + home_light.off_ms, at = now % period;
    u8 lit = c.linked || at < home_light.on_ms;

    memset(&f, 0, sizeof(f));
    if (cal_led(now, &cal)) {    /* the calibration routine's own light */
        f.light[LIGHT_HOME] = cal;
        plat_leds(&f);
        c.lit = 0xff;
        return now + 20;
    }
    if (lit != c.lit) {
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
        set_flush();
        plat_reboot();
        break;
    case REQ_REBOOT_LOADER:
        util_log("ctl: to the boot loader");
        set_flush();
        plat_reboot_loader();
        break;
    case REQ_CALIBRATE:
        c.calibrating = 1;
        c.cal = *set_cal();
        cal_start(plat_ms());
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

    c.status.input = &c.input;
    c.status.raw = &c.raw;
    c.lit = 0xff;
    set_init();
    start();
    for (;;) {
        next = lights(plat_ms());
        if (c.rumble_until && (!next || (s32)(c.rumble_until - next) < 0)) next = c.rumble_until;
        if (c.calibrating) next = plat_ms() + CAL_SAMPLE_MS;
        plat_wait(next, c.linked ? plat_us() + POLL_US : 0);
        if (c.rumble_until && (s32)(plat_ms() - c.rumble_until) >= 0) {
            plat_motors(0, 0);
            c.rumble_until = 0;
        }
        bluetooth();
        read_input(plat_ms());
        report(plat_ms());
        c.status.state = c.linked ? CTL_CONNECTED : CTL_PAIRING;
        c.status.slot = c.linked ? 0 : SLOT_NONE;
        c.status.usb_powered = (u8)plat_usb_present();
        c.status.battery_mv = plat_battery_mv();
        c.status.uptime_ms = plat_ms();
        c.status.ephemeral = (u8)set_is_ephemeral();
        c.status.staged = (u8)(set_staged() < 255 ? set_staged() : 255);
        r = usb_poll(&c.status);
        request(&r);
        set_poll(plat_ms(), bt_radio_quiet());
    }
}

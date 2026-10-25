#include <string.h>
#include "pad.h"

/* The control loop for a pad whose RAM controller.c and settings.c do not
 * fit (the Telink B80's 16 KiB): ctl_main() as controller.c has it, built
 * from the same modules, with what the board can afford.  So far: one
 * session of one LE profile (genericble), started at boot, whose host
 * gets a report whenever the input changes; the settings (what the board
 * keeps of them), the sensor calibration and its routine; the Home light
 * (blinking while no host is linked, lit when one is); the tools'
 * commands, the motor test among them.  The single session keeps its bond
 * in slot A. Shared Home gestures and Sync control that session; other
 * slots and remap configuration are not supported yet. */

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
    u8 activity_ref[offsetof(struct pad, imu_count)]; /* no unused IMU data */
    u32 activity_ms, sleep_ms, since_ms, usb_seen_ms;
    u8 sleeping, idle, stopping, pending, renew, pair_down;
} c;

static void start(void);
static void request(const struct ctl_request *r);

static u8 state(void)
{
    return c.sleeping ? CTL_SLEEPING : c.stopping ? CTL_STOPPING : c.idle ? CTL_IDLE :
           c.linked ? CTL_CONNECTED : c.session.pairing ? CTL_PAIRING : CTL_CONNECTING;
}

static void read_input(u32 now)
{
    struct ctl_request r;
    u8 pair;

    plat_input(&c.raw);
    pair = c.raw.pair_button && !c.pair_down;
    c.pair_down = c.raw.pair_button;
    in_map(&c.raw, &c.pad);
    cal_apply(set_cal(), NULL, &c.raw, &c.pad);
    /* USB reads this snapshot only later on this same thread. Reuse it
     * for the reference before publishing the new calibrated input. */
    memcpy(&c.input, c.activity_ref, sizeof(c.activity_ref));
    if (in_moved(&c.pad, &c.input)) {
        memcpy(c.activity_ref, &c.pad, sizeof(c.activity_ref));
        c.activity_ms = now;
    }
    c.input = c.pad;
    if (!c.calibrating) {
        r = ux_step(&c.pad, now, state());
        if (pair && !c.sleeping) {
            r.kind = REQ_PAIR;
            r.slot = 0;
            r.generate_new_address = 1;
            ux_ignore_held();
        }
        request(&r);
        return;
    }
    switch (cal_step(&c.raw, now, &c.cal)) {
    case CAL_SAVE:
        set_cal_store(&c.cal);
        break;
    case CAL_IDLE:
        c.calibrating = 0;
        ux_ignore_held();
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

static void start_session(int pairing, int renew)
{
    static const u8 zero[16];

    c.sleeping = 0;
    c.idle = 0;
    c.activity_ms = c.since_ms = plat_ms();
    memset(&c.session, 0, sizeof(c.session));
    address(c.session.address);
    c.session.bond = set_bond(0);
    c.session.pairing = !memcmp(c.session.bond->ltk, zero, sizeof(zero));
    if (!c.session.pairing) set_slot_address(0, c.session.address);
    if (pairing >= 0) c.session.pairing = (u8)pairing;
    if (c.session.pairing && renew) set_pairing_address(0, 1, c.session.address);
    c.session.address[5] |= 0xc0;       /* LE static random */
    profile->begin(&c.session);
    bt_start(profile, c.session.address, c.session.bond, c.session.pairing, NULL);
    util_log("ctl: %s slot A", c.session.pairing ? "pairing" : "connecting");
}

static void start(void) { start_session(-1, 0); }

/* Restart only after BT_EV_STOPPED, including a link still doing SMP when
 * it has not emitted LINKED yet. The latest request replaces the pending one. */
static void transition(u8 kind, u8 renew)
{
    c.pending = kind;
    c.renew = renew;
    c.idle = 0;
    c.rumble_until = 0;
    plat_motors(0, 0);
    if (c.stopping) return;
    c.stopping = 1;
    c.since_ms = plat_ms();
    bt_stop();
}

static void stopped(u32 now)
{
    u8 pending = c.pending;

    c.stopping = c.linked = 0;
    c.pending = REQ_NONE;
    if (c.sleeping) return;
    if (pending == REQ_PAIR || pending == REQ_CONNECT) {
        start_session(pending == REQ_PAIR ? 1 : -1, c.renew);
    } else {
        c.idle = 1;
        c.since_ms = now;
        util_log("ctl: idle");
    }
}

static __attribute__((noinline)) void bluetooth(void)
{
    struct bt_ev ev;

    while (bt_poll(&ev)) {
        if (ev.kind == BT_EV_LINKED) {
            if (c.stopping || c.sleeping) continue;
            c.linked = 1;
            c.activity_ms = plat_ms();
            profile->begin(&c.session);          /* the first report goes out whatever it is */
        } else if (ev.kind == BT_EV_UNLINKED) {
            c.linked = 0;
            c.since_ms = plat_ms();
        } else if (ev.kind == BT_EV_PAIRED) {
            set_pairing_store(0, c.session.address, &ev.bond);
            c.session.pairing = 0;
            bt_update_bond(&ev.bond);
            util_log("ctl: slot A bond saved");
        } else if (ev.kind == BT_EV_STOPPED) {
            stopped(plat_ms());
        }
    }
}

static __attribute__((noinline)) void report(u32 now)
{
    u8 out[64], id = 0;
    int n;

    if (c.sleeping || c.stopping || !c.linked || !bt_input_wanted()) return;
    n = profile->report(&c.pad, now, out, &id);
    if (n > 0 && bt_send_input(id, out, (u16)n) && (c.pad.buttons & PAD_HOME)) ux_reported(now);
}

/* Returns the next change, plat_ms. */
static u32 lights(u32 now)
{
    struct led_frame f;
    struct rgb cal;
    u32 period = home_light.on_ms + home_light.off_ms, at = now % period;
    u8 lit = !c.idle && (c.linked || at < home_light.on_ms);

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
    if (c.linked || c.idle) return 0;
    return now - at + (at < home_light.on_ms ? home_light.on_ms : period);
}

static void request(const struct ctl_request *r)
{
    switch (r->kind) {
    case REQ_NONE:
        break;
    case REQ_CONNECT:
    case REQ_PAIR:
        if (c.sleeping || (r->kind == REQ_CONNECT && c.linked && !c.stopping)) break;
        if (r->slot != 0 && r->slot != SLOT_LAST) {
            util_log("ctl: only slot A is supported");
            break;
        }
        transition(r->kind, r->generate_new_address);
        break;
    case REQ_DISCONNECT:
        if (!c.sleeping) transition(REQ_NONE, 0);
        break;
    case REQ_SLEEP:
        if (!plat_usb_present()) {
            c.sleeping = 1;
            c.sleep_ms = plat_ms();
        }
        transition(REQ_NONE, 0);
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
        ux_ignore_held();
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

/* Battery inactivity ends the radio session before cutting the supply. USB
 * power and calibration keep the pad awake.  A cable arriving during the
 * disconnect leaves the radio off once that disconnect has finished. */
static u32 power_step(u32 now)
{
    int powered = plat_usb_present();
    u32 deadline;

    if (powered) c.usb_seen_ms = now;
    if (powered || c.calibrating) c.activity_ms = now;
    if (c.sleeping) {
        if (powered) {
            if (!c.stopping) {
                c.sleeping = 0;
                c.idle = 1;
                c.since_ms = now;
            }
            return c.stopping ? c.sleep_ms + ux_timeouts.stop_max_ms : 0;
        }
        if ((!c.stopping && now - c.sleep_ms >= 50) || now - c.sleep_ms >= ux_timeouts.stop_max_ms) {
            set_flush();
            plat_power_off();
        }
        return c.sleep_ms + (c.stopping ? ux_timeouts.stop_max_ms : 50);
    }
    if (c.stopping) {
        deadline = c.since_ms + ux_timeouts.stop_max_ms;
        if ((s32)(now - deadline) >= 0) {
            set_flush();
            if (powered) plat_reboot();
            else plat_power_off();
        }
        return deadline;
    }
    if (!c.linked && !c.idle) {
        deadline = c.since_ms + (c.session.pairing ? ux_timeouts.pair_ms : ux_timeouts.connect_ms);
        if ((s32)(now - deadline) < 0) return deadline;
        transition(REQ_NONE, 0);
        return now + ux_timeouts.stop_max_ms;
    }
    if (powered || c.calibrating) return 0;
    deadline = c.idle ? ux_idle_deadline(c.since_ms, c.activity_ms, c.usb_seen_ms,
                                        powered, set_is_ephemeral()) :
                        c.activity_ms + ux_timeouts.idle_sleep_ms;
    if ((s32)(now - deadline) >= 0) {
        util_log("ctl: sleeping");
        c.sleeping = 1;
        c.sleep_ms = now;
        transition(REQ_NONE, 0);
        return now + 50;
    }
    return deadline;
}

#ifndef UNITTEST
void ctl_main(void)
{
    struct ctl_request r;
    u32 next, power, gesture;

    c.status.input = &c.input;
    c.status.raw = &c.raw;
    c.lit = 0xff;
    set_init();
    ux_ignore_held();
    start();
    for (;;) {
        power = power_step(plat_ms());
        next = lights(plat_ms());
        gesture = ux_deadline();
        if (gesture && (!next || (s32)(gesture - next) < 0)) next = gesture;
        if (power && (!next || (s32)(power - next) < 0)) next = power;
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
        c.status.state = state();
        c.status.slot = c.idle || c.sleeping ? SLOT_NONE : 0;
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
#else
/* Supply loss must never skip the radio stop or pending settings write.
 * The platform's physical power hold and USB enumeration need pad tests. */
static u32 test_ms;
static int test_usb, stops, flushes, offs, starts, motors, renewals, reboots;
static struct bt_bond test_bond;

u32 plat_ms(void) { return test_ms; }
int plat_usb_present(void) { return test_usb; }
const u8 *plat_unique_id(void) { static const u8 id[16]; return id; }
const struct bt_bond *set_bond(u8 slot) { (void)slot; return &test_bond; }
void set_slot_address(u8 slot, u8 out[6]) { (void)slot; memset(out, 0x11, 6); }
void set_pairing_address(u8 slot, int renew, u8 out[6])
{ (void)slot; renewals += renew; memset(out, 0x22, 6); }
void bt_start(const struct profile *p, const u8 a[6], const struct bt_bond *k,
              int pairing, const struct bt_link *link)
{ (void)p; (void)a; (void)k; (void)pairing; (void)link; starts++; }
void bt_stop(void) { stops++; }
void set_flush(void) { flushes++; }
int set_is_ephemeral(void) { return 0; }
u32 ux_idle_deadline(u32 since, u32 activity, u32 usb_seen, int powered, int ephemeral)
{
    u32 offline = since + 15000, unplug = usb_seen + 5000;
    (void)activity; (void)powered; (void)ephemeral;
    return (s32)(offline - unplug) > 0 ? offline : unplug;
}
void plat_power_off(void) { CHECK(flushes > offs); offs++; }
void plat_reboot(void) { reboots++; }
void plat_motors(u8 left, u8 right) { CHECK(!left && !right); motors++; }
void util_log(const char *fmt, ...) { (void)fmt; }
static u8 test_begin(const struct profile_session *s) { (void)s; return 0; }
const struct profile profile_generic_ble = { .begin = test_begin };
const struct ux_timeouts ux_timeouts = {
    .idle_sleep_ms = 300000, .stop_max_ms = 12000, .connect_ms = 30000, .pair_ms = 180000,
};

static void test_reset(void)
{
    memset(&c, 0, sizeof(c));
    test_ms = 100;
    test_usb = stops = flushes = offs = starts = motors = renewals = reboots = 0;
    memset(&test_bond, 0, sizeof(test_bond));
    c.activity_ms = test_ms;
    c.linked = 1;
}

int main(void)
{
    u32 due;

    test_reset();
    due = c.activity_ms + ux_timeouts.idle_sleep_ms;
    CHECK(power_step(due - 1) == due && !stops && !offs);
    CHECK(power_step(due) == due + 50 && stops == 1 && motors == 1 && !offs);
    CHECK(c.sleeping && !c.rumble_until);
    CHECK(power_step(due + 50) == due + ux_timeouts.stop_max_ms && !offs);
    c.linked = 0;
    stopped(due + 51);
    power_step(due + 51);
    CHECK(flushes == 1 && offs == 1);

    /* A stopped radio still receives a grace period before the supply cut. */
    test_reset(); c.linked = 0; c.idle = 1; c.since_ms = due - 15000;
    power_step(due);
    stopped(due);
    power_step(due + 49); CHECK(!offs);
    power_step(due + 50); CHECK(offs == 1);

    /* USB and calibration refresh the inactivity reference. */
    test_reset(); test_usb = 1;
    CHECK(!power_step(due) && c.activity_ms == due && !stops);
    test_usb = 0;
    CHECK(power_step(due + 1) == due + ux_timeouts.idle_sleep_ms && !stops);
    test_reset(); c.calibrating = 1;
    CHECK(!power_step(due) && !stops);

    /* A cable arriving during disconnect leaves the radio idle and powered. */
    test_reset(); power_step(due); test_usb = 1;
    power_step(due + 1); CHECK(c.sleeping && !starts && !offs);
    c.linked = 0; test_ms = due + 2;
    stopped(test_ms);
    power_step(test_ms);
    CHECK(!c.sleeping && c.idle && !starts && !offs && c.activity_ms == test_ms);

    /* Reconnect and pairing have their own timeout, then the offline wait. */
    test_reset(); c.linked = 0; c.since_ms = 100;
    CHECK(power_step(30099) == 30100 && !stops);
    CHECK(power_step(30100) == 42100 && c.stopping && stops == 1 && !offs);
    stopped(30100);
    CHECK(power_step(30100) == 45100 && c.idle);
    power_step(45100); CHECK(c.sleeping && !offs);
    stopped(45100);
    power_step(45150); CHECK(offs == 1);
    test_reset(); c.linked = 0; c.since_ms = 100; c.session.pairing = 1;
    CHECK(power_step(180099) == 180100 && !stops);
    CHECK(power_step(180100) == 192100 && c.stopping && !offs);
    stopped(180100);
    CHECK(power_step(180100) == 195100 && c.idle);
    power_step(195100); CHECK(c.sleeping);

    /* USB remains available after an unsuccessful reconnect times out. */
    test_reset(); c.linked = 0; test_usb = 1;
    power_step(30000); CHECK(c.stopping && !c.sleeping && !offs);
    stopped(30000); CHECK(c.idle);
    CHECK(!power_step(900000) && !offs);

    /* Recently unplugged USB grants the full unplug grace in idle. */
    test_reset(); c.linked = 0; c.idle = 1; c.usb_seen_ms = 30000;
    CHECK(power_step(30001) == 35000 && !c.sleeping);

    /* Even before LINKED, pairing/reconnect waits for the SDK stop ack. */
    test_reset(); c.linked = 0;
    transition(REQ_PAIR, 1);
    CHECK(!starts && c.stopping && stops == 1);
    stopped(test_ms);
    CHECK(starts == 1 && c.session.pairing && renewals == 1 && !c.stopping);
    CHECK(c.session.address[0] == 0x22 && (c.session.address[5] & 0xc0) == 0xc0);
    /* Newest request wins while the previous session is stopping. */
    test_reset(); test_bond.ltk[0] = 1;
    transition(REQ_PAIR, 1); transition(REQ_CONNECT, 0);
    CHECK(stops == 1 && !starts);
    stopped(test_ms);
    CHECK(starts == 1 && !c.session.pairing && !renewals);
    /* Disconnect retains the bond and leaves advertising off in idle. */
    test_reset(); test_bond.ltk[0] = 7;
    transition(REQ_NONE, 0); stopped(test_ms);
    CHECK(c.idle && !starts && test_bond.ltk[0] == 7);

    /* An unresponsive radio cannot prevent battery shutdown indefinitely. */
    test_reset(); power_step(due);
    power_step(due + ux_timeouts.stop_max_ms - 1); CHECK(!offs);
    power_step(due + ux_timeouts.stop_max_ms); CHECK(offs == 1);

    test_reset(); c.activity_ms = 0xfffffff0u;
    due = c.activity_ms + ux_timeouts.idle_sleep_ms;
    CHECK(power_step(due - 1) == due && !stops);
    power_step(due); CHECK(stops == 1 && !offs);
    return TEST_DONE("controller_lite power");
}
#endif

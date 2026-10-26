#include <string.h>
#include "pad.h"

/* The control loop for a pad whose RAM controller.c and settings.c do not
 * fit (the Telink B80's 16 KiB). Four slots select Generic BLE, Xbox (LE) or Switch 2,
 * sharing the input, calibration, settings, Home gestures and one radio
 * session at a time. Host commands run here, outside the SDK callbacks. */

#define POLL_US 4000             /* connected: the input is looked at this often */
#define CAL_SAMPLE_MS 2          /* calibrating: the input is read this often */
#define LITE_SLOTS 4
static const char slot_names[] = "ABXY";

/* The lights (of 255): the Home ring blinks while connecting, faster while
 * pairing, with the red one by the USB port; the one under Home (on/off
 * only) shows a cable arriving. A single light shows no player number. */
static const struct {
    u8 home, red;
    u16 connect_ms, pair_ms;     /* on, then off as long */
} lite_lights = { 24, 32, 250, 120 };

static const struct profile *profile = &profile_generic_ble;

/* The LE profiles a slot can run here; NULL for the others. */
static const struct profile *slot_profile(u8 type)
{
    return type == PROFILE_GENERIC_BLE ? &profile_generic_ble : type == PROFILE_XBOX_BLE ? &profile_xbox_ble :
           type == PROFILE_SWITCH2 ? &profile_switch2 : NULL;
}

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
    u8 activity_ref[offsetof(struct pad, imu_count)]; /* no unused IMU data */
    u32 activity_ms, sleep_ms, since_ms, usb_seen_ms;
    u8 sleeping, idle, stopping, pending, renew, pair_down;
    u8 slot, last_slot, pending_slot;
    u8 peer[6];
    u8 usb_was, plug_lit;        /* plug_lit: the light under Home for a new cable */
    u32 plugged_ms;
    u8 remap;                    /* the session's remap slot, REMAP_BASE the base */
    u8 remap_live;               /* remap.c state belongs to remap_live - 1 at remap_edits */
    u8 turbo_up;                 /* the last report left turbo_max up */
    u32 remap_edits;
    u32 turbo_max;               /* keys flipped every report, down first */
} c;

static void start(void);
static void request(const struct ctl_request *r);

static u8 state(void)
{
    return c.sleeping ? CTL_SLEEPING : c.stopping ? CTL_STOPPING : c.idle ? CTL_IDLE :
           c.linked ? CTL_CONNECTED : c.session.pairing ? CTL_PAIRING : CTL_CONNECTING;
}

/* The routine finishes on top of the calibration in use, keeping what did
 * not calibrate; built here, it needs no RAM between steps. */
static __attribute__((noinline)) void calibrate(u32 now)
{
    struct set_cal cal = *set_cal();

    switch (cal_step(&c.raw, now, &cal)) {
    case CAL_SAVE:
        set_cal_store(&cal);
        break;
    case CAL_IDLE:
        c.calibrating = 0;
        ux_ignore_held();
        break;
    }
}

/* The session's remap: its sticks connected only (NULL: as calibrated). */
static const struct set_remap *sticks(void)
{
    return c.linked && !c.stopping ? set_remap(c.remap) : NULL;
}

/* Connected, outside calibration: the remap slot in use.  Its state starts
 * over with another remap slot or new settings. */
static void remap(u32 now)
{
    u32 edits = set_edits();

    c.turbo_max = 0;
    if (!c.linked || c.stopping || c.calibrating) {
        if (c.remap_live) remap_reset();
        c.remap_live = 0;
        return;
    }
    if (c.remap_live != c.remap + 1 || c.remap_edits != edits) {
        remap_reset();
        c.remap_live = (u8)(c.remap + 1);
        c.remap_edits = edits;
    }
    c.pad.buttons = remap_step(set_remap(c.remap), c.pad.buttons, now, &c.turbo_max);
}

/* The loop's steps stay out of line (noinline): their locals are then not
 * on the stack under bluetooth(), the deepest path (a Switch 2 command). */
static __attribute__((noinline)) void read_input(u32 now)
{
    struct ctl_request r;
    u8 pair;

    plat_input(&c.raw);
    pair = c.raw.pair_button && !c.pair_down;
    c.pair_down = c.raw.pair_button;
    in_map(&c.raw, &c.pad);
    cal_apply(set_cal(), sticks() ? sticks()->deadzone : NULL, &c.raw, &c.pad);
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
            r.slot = c.last_slot;
            r.generate_new_address = 1;
            ux_ignore_held();
        }
        request(&r);
    } else {
        calibrate(now);
        c.pad.buttons = 0;
    }
    remap(now);
    in_response(sticks(), &c.pad);
}

static int slot_ready(u8 slot)
{
    const struct set_slot *s;

    if (slot >= LITE_SLOTS) return 0;
    s = set_slot(slot);
    return slot_profile(s->type) && s->transport == TRANSPORT_BT;
}

static void start_session(u8 slot, int pairing, int renew)
{
    if (!slot_ready(slot)) {
        c.idle = 1;
        c.since_ms = plat_ms();
        util_log("ctl: slot %u type/transport unavailable", slot);
        return;
    }
    c.sleeping = 0;
    c.idle = 0;
    c.slot = c.last_slot = slot;
    c.activity_ms = c.since_ms = plat_ms();
    memset(&c.session, 0, sizeof(c.session));
    c.session.slot = slot;
    c.session.settings = set_slot(slot);
    c.session.bond = set_bond(slot);
    set_slot_address(slot, c.session.address);
    c.session.pairing = util_is_zero(c.session.bond->ltk, sizeof(c.session.bond->ltk));
    c.remap = c.session.settings->remap ? (u8)(c.session.settings->remap - 1) : REMAP_BASE;
    if (pairing >= 0) c.session.pairing = (u8)pairing;
    if (c.session.pairing && renew) set_pairing_address(slot, 1, c.session.address);
    if (c.session.settings->type != PROFILE_SWITCH2) c.session.address[5] |= 0xc0;
    set_flush();                     /* previous session is stopped */
    if (!set_is_ephemeral()) plat_retain_slot(slot);
    profile = slot_profile(c.session.settings->type);
    profile->begin(&c.session);
    bt_start(profile, c.session.address, c.session.bond, c.session.pairing, NULL);
    util_log("ctl: %s slot %c", c.session.pairing ? "pairing" : "connecting", slot_names[slot]);
}

static void start(void)
{
    u8 slot = plat_retained_slot();

    if (!slot_ready(slot)) for (slot = 0; slot < LITE_SLOTS && !slot_ready(slot); slot++) {}
    start_session(slot, -1, 0);
}

/* Restart only after BT_EV_STOPPED, including a link still doing SMP when
 * it has not emitted LINKED yet. The latest request replaces the pending one. */
static void transition(u8 kind, u8 slot, u8 renew)
{
    c.pending = kind;
    c.pending_slot = slot;
    c.renew = renew;
    c.idle = 0;
    c.rumble_until = 0;
    plat_motors(0, 0);
    if (c.stopping) return;
    c.stopping = 1;
    c.since_ms = plat_ms();
    bt_stop();
}

static void select_session(u8 kind, u8 slot, u8 renew)
{
    if (slot == SLOT_LAST) slot = c.last_slot;
    if (!slot_ready(slot)) {
        util_log("ctl: slot %u type/transport unavailable", slot);
        return;
    }
    if (kind == REQ_CONNECT && c.linked && !c.stopping && slot == c.slot) return;
    transition(kind, slot, renew);
}

static void stopped(u32 now)
{
    u8 pending = c.pending, slot = c.pending_slot;

    c.stopping = c.linked = 0;
    c.pending = REQ_NONE;
    c.pending_slot = SLOT_NONE;
    if (c.sleeping) return;
    if (pending == REQ_PAIR || pending == REQ_CONNECT) {
        start_session(slot, pending == REQ_PAIR ? 1 : -1, c.renew);
        if (pending == REQ_CONNECT && profile == &profile_switch2 && !c.session.pairing)
            bt_connect();
    } else {
        c.idle = 1;
        c.since_ms = now;
        util_log("ctl: idle");
    }
}

/* Returns whether the session has stopped; the caller acts on that once
 * the event is off the stack. */
static __attribute__((noinline)) int bt_events(void)
{
    struct bt_ev ev;
    int stopped_seen = 0;

    while (bt_poll(&ev)) {
        if (ev.kind == BT_EV_LINKED) {
            if (c.stopping || c.sleeping) continue;
            c.linked = 1;
            memcpy(c.peer, ev.bond.le_peer, 6);
            c.activity_ms = plat_ms();
            profile->begin(&c.session);          /* the first report goes out whatever it is */
        } else if (ev.kind == BT_EV_UNLINKED) {
            c.linked = 0;
            c.rumble_until = 0;
            plat_motors(0, 0);
            c.since_ms = plat_ms();
            /* Match controller.c: a deliberate disconnect ends this session;
             * only a lost link stays in reconnect mode. Our own stop's
             * disconnect keeps the request it is stopping for. */
            if (!ev.lost && !c.stopping) {
                util_log("ctl: host ended the session");
                transition(REQ_NONE, SLOT_NONE, 0);
            }
        } else if (ev.kind == BT_EV_PAIRED) {
            set_pairing_store(c.slot, c.session.address, &ev.bond);
            c.session.pairing = 0;
            bt_update_bond(&ev.bond);
            util_log("ctl: slot %c bond saved", slot_names[c.slot]);
        } else if (ev.kind == BT_EV_WRITE && !c.stopping && !c.sleeping) {
            if (profile->write && profile->write(&ev.write, plat_ms(), &ev.bond)) {
                memcpy(ev.bond.le_peer, c.peer, 6);
                set_pairing_store(c.slot, c.session.address, &ev.bond);
                c.session.pairing = 0;
                bt_update_bond(&ev.bond);
            }
        } else if (ev.kind == BT_EV_STOPPED) {
            stopped_seen = 1;
        }
    }
    return stopped_seen;
}

static void bluetooth(void)
{
    if (bt_events()) stopped(plat_ms());
}

int host_send(u16 id, const u8 *data, u16 len)
{
    return !c.stopping && !c.sleeping && c.linked && bt_send_message(id, data, len);
}

/* Host rumble spreads over the motor's range for the remap's step: 1 is
 * its rumble_min, 255 its rumble_max. */
static u8 motor_duty(const struct set_remap *m, u8 host, int right)
{
    u32 step = m->rumble, lo, hi;

    if (!host || !step) return 0;
    lo = 255u * m->rumble_min[right][step - 1] / 100;
    hi = 255u * m->rumble_max[right][step - 1] / 100;
    if (hi < lo) hi = lo;
    return (u8)(lo + (hi - lo) * (host - 1u) / 254);
}

static __attribute__((noinline)) void host(u32 now)
{
    struct profile_host h;
    const struct set_remap *m;

    if (!profile->host || !c.linked || c.stopping || c.sleeping || c.rumble_until) return;
    memset(&h, 0, sizeof(h));
    profile->host(now, &h);
    m = set_remap(c.remap);
    plat_motors(motor_duty(m, h.rumble[0], 0), motor_duty(m, h.rumble[1], 1));
    if (h.ended) transition(REQ_NONE, SLOT_NONE, 0);
}

static __attribute__((noinline)) void report(u32 now)
{
    u8 out[64], id = 0;
    u32 buttons = c.pad.buttons;
    int n;

    if (c.sleeping || c.stopping || !c.linked || !bt_input_wanted()) return;
    if (!c.turbo_up) c.pad.buttons |= c.turbo_max;
    n = profile->report(&c.pad, now, out, &id);
    c.pad.buttons = buttons;     /* a profile may change them while building */
    if (n > 0 && bt_send_input(id, out, (u16)n)) {
        c.turbo_up = c.turbo_max && !c.turbo_up;
        if (buttons & PAD_HOME) ux_reported(now);
    }
}

static u32 earliest(u32 a, u32 b)
{
    return !a || (b && (s32)(b - a) < 0) ? b : a;
}

/* Returns the next change, plat_ms. */
static __attribute__((noinline)) u32 lights(u32 now)
{
    struct led_frame f;
    struct rgb cal;
    int pairing = state() == CTL_PAIRING, blinking = !c.linked && !c.idle;
    u32 on = pairing ? lite_lights.pair_ms : lite_lights.connect_ms, at = now % (2 * on);
    u32 next = 0;
    u8 lit;

    if (c.plug_lit && now - c.plugged_ms >= ux_timeouts.usb_plug_ms) c.plug_lit = 0;
    lit = (u8)((!c.idle && (c.linked || at < on)) | c.plug_lit << 1 | (pairing && at < on) << 2);

    memset(&f, 0, sizeof(f));
    if (cal_led(now, &cal)) {    /* the calibration routine's own light */
        f.light[LIGHT_HOME] = cal;
        plat_leds(&f);
        c.lit = 0xff;
        return now + 20;
    }
    if (lit != c.lit) {
        f.light[LIGHT_HOME].r = f.light[LIGHT_HOME].g = f.light[LIGHT_HOME].b = (lit & 1) ? lite_lights.home : 0;
        f.light[LIGHT_P0].r = f.light[LIGHT_P0].g = f.light[LIGHT_P0].b = (lit & 2) ? 255 : 0;
        f.light[LIGHT_P1].r = f.light[LIGHT_P1].g = f.light[LIGHT_P1].b = (lit & 4) ? lite_lights.red : 0;
        plat_leds(&f);
        c.lit = lit;
    }
    if (c.plug_lit) next = c.plugged_ms + ux_timeouts.usb_plug_ms;
    if (blinking) next = earliest(next, now - at + (at < on ? on : 2 * on));
    return next;
}

static void request(const struct ctl_request *r)
{
    switch (r->kind) {
    case REQ_NONE:
        break;
    case REQ_CONNECT:
    case REQ_PAIR:
        if (!c.sleeping) select_session(r->kind, r->slot, r->generate_new_address);
        break;
    case REQ_DISCONNECT:
        if (!c.sleeping) transition(REQ_NONE, SLOT_NONE, 0);
        break;
    case REQ_SLEEP:
        if (!plat_usb_present()) {
            c.sleeping = 1;
            c.sleep_ms = plat_ms();
        }
        transition(REQ_NONE, SLOT_NONE, 0);
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
static __attribute__((noinline)) u32 power_step(u32 now)
{
    int powered = plat_usb_present();
    u32 deadline;

    if (powered) c.usb_seen_ms = now;
    if (powered && !c.usb_was) {     /* a cable, or one there at power-up */
        c.plugged_ms = now;
        c.plug_lit = 1;
    }
    c.usb_was = (u8)powered;
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
        transition(REQ_NONE, SLOT_NONE, 0);
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
        transition(REQ_NONE, SLOT_NONE, 0);
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
        next = earliest(next, remap_deadline());
        if (c.calibrating) next = plat_ms() + CAL_SAMPLE_MS;
        plat_wait(next, c.linked ? plat_us() + POLL_US : 0);
        if (c.rumble_until && (s32)(plat_ms() - c.rumble_until) >= 0) {
            plat_motors(0, 0);
            c.rumble_until = 0;
        }
        bluetooth();
        read_input(plat_ms());
        host(plat_ms());
        report(plat_ms());
        c.status.state = state();
        c.status.slot = c.idle || c.sleeping ? SLOT_NONE : c.slot;
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
static int test_usb, stops, flushes, offs, starts, motors, renewals, reboots, wakes;
static struct bt_bond test_bonds[LITE_SLOTS];
#define test_bond test_bonds[0]
static struct set_slot test_slots[LITE_SLOTS];
static u8 test_addresses[LITE_SLOTS][6], retained;
static int ephemeral, retained_writes;
static struct bt_ev test_events[2];
static u8 event_at, event_count;

u32 plat_ms(void) { return test_ms; }
int plat_usb_present(void) { return test_usb; }
const u8 *plat_unique_id(void) { static const u8 id[16]; return id; }
const struct bt_bond *set_bond(u8 slot) { return &test_bonds[slot]; }
const struct set_slot *set_slot(u8 slot) { return &test_slots[slot]; }
void set_slot_address(u8 slot, u8 out[6]) { memcpy(out, test_addresses[slot], 6); }
void set_pairing_address(u8 slot, int renew, u8 out[6])
{ (void)slot; renewals += renew; memset(out, 0x22, 6); }
u8 plat_retained_slot(void) { return retained; }
void plat_retain_slot(u8 slot) { retained = slot; retained_writes++; }
int bt_poll(struct bt_ev *ev)
{ if (event_at == event_count) return 0; *ev = test_events[event_at++]; return 1; }
void set_pairing_store(u8 slot, const u8 address[6], const struct bt_bond *bond)
{ test_bonds[slot] = *bond; memcpy(test_addresses[slot], address, 6); }
void bt_update_bond(const struct bt_bond *bond) { (void)bond; }
void bt_start(const struct profile *p, const u8 a[6], const struct bt_bond *k,
              int pairing, const struct bt_link *link)
{ (void)p; (void)a; (void)k; (void)pairing; (void)link; starts++; }
void bt_stop(void) { stops++; }
void bt_connect(void) { wakes++; }
void set_flush(void) { flushes++; }
int set_is_ephemeral(void) { return ephemeral; }
u32 ux_idle_deadline(u32 since, u32 activity, u32 usb_seen, int powered, int ephemeral)
{
    u32 offline = since + 15000, unplug = usb_seen + 5000;
    (void)activity; (void)powered; (void)ephemeral;
    return (s32)(offline - unplug) > 0 ? offline : unplug;
}
void plat_power_off(void) { CHECK(flushes > offs); offs++; }
void plat_reboot(void) { reboots++; }
void plat_motors(u8 left, u8 right) { CHECK(!left && !right); motors++; }
static struct led_frame test_frame;
void plat_leds(const struct led_frame *f) { test_frame = *f; }
int cal_led(u32 now, struct rgb *out) { (void)now; (void)out; return 0; }
#define LIT(l) (test_frame.light[l].r)
void util_log(const char *fmt, ...) { (void)fmt; }
static u8 test_begin(const struct profile_session *s) { (void)s; return 0; }
const struct profile profile_generic_ble = { .begin = test_begin };
static int test_write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    (void)now; CHECK(w->kind == BT_WRITE_ATT && w->id == 0x14);
    memset(bond, 0, sizeof(*bond)); bond->ltk[0] = w->data[0]; return 1;
}
const struct profile profile_switch2 = { .begin = test_begin, .write = test_write };
const struct profile profile_xbox_ble = { .begin = test_begin };
int bt_send_message(u16 id, const u8 *data, u16 len) { (void)id; (void)data; (void)len; return 1; }
static struct set_remap test_remaps[REMAP_BASE + 1];
static u32 test_edits, remap_resets, remap_steps;
const struct set_remap *set_remap(u8 remap) { return &test_remaps[remap]; }
u32 set_edits(void) { return test_edits; }
void remap_reset(void) { remap_resets++; }
u32 remap_step(const struct set_remap *m, u32 in, u32 now, u32 *every_report)
{ (void)now; remap_steps++; *every_report = PAD_X; return in | m->map[0]; }
const struct ux_timeouts ux_timeouts = {
    .idle_sleep_ms = 300000, .stop_max_ms = 12000, .connect_ms = 30000, .pair_ms = 180000,
    .usb_plug_ms = 1000,
};

static void test_reset(void)
{
    u8 i;

    memset(&c, 0, sizeof(c));
    profile = &profile_generic_ble;
    test_ms = 100;
    test_usb = stops = flushes = offs = starts = motors = renewals = reboots = wakes = 0;
    memset(test_bonds, 0, sizeof(test_bonds));
    memset(test_slots, 0, sizeof(test_slots));
    for (i = 0; i < LITE_SLOTS; i++) {
        test_slots[i].type = PROFILE_GENERIC_BLE;
        test_slots[i].transport = TRANSPORT_BT;
        memset(test_addresses[i], 0x11 + i, 6);
        test_addresses[i][5] |= 0xc0;
    }
    retained = SLOT_NONE;
    ephemeral = retained_writes = event_at = event_count = 0;
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
    transition(REQ_PAIR, 0, 1);
    CHECK(!starts && c.stopping && stops == 1);
    stopped(test_ms);
    CHECK(starts == 1 && c.session.pairing && renewals == 1 && !c.stopping);
    CHECK(c.session.address[0] == 0x22 && (c.session.address[5] & 0xc0) == 0xc0);
    /* Newest request wins while the previous session is stopping. */
    test_reset(); test_bond.ltk[0] = 1;
    transition(REQ_PAIR, 0, 1); transition(REQ_CONNECT, 0, 0);
    CHECK(stops == 1 && !starts);
    stopped(test_ms);
    CHECK(starts == 1 && !c.session.pairing && !renewals);
    /* Disconnect retains the bond and leaves advertising off in idle. */
    test_reset(); test_bond.ltk[0] = 7;
    transition(REQ_NONE, SLOT_NONE, 0); stopped(test_ms);
    CHECK(c.idle && !starts && test_bond.ltk[0] == 7);

    /* An unresponsive radio cannot prevent battery shutdown indefinitely. */
    test_reset(); power_step(due);
    power_step(due + ux_timeouts.stop_max_ms - 1); CHECK(!offs);
    power_step(due + ux_timeouts.stop_max_ms); CHECK(offs == 1);

    test_reset(); c.activity_ms = 0xfffffff0u;
    due = c.activity_ms + ux_timeouts.idle_sleep_ms;
    CHECK(power_step(due - 1) == due && !stops);
    power_step(due); CHECK(stops == 1 && !offs);

    /* All four slots use their own identity, bond and profile session. */
    test_reset(); c.linked = 0;
    {
        u8 i;
        for (i = 0; i < LITE_SLOTS; i++) {
            test_bonds[i].ltk[0] = 0x30 + i;
            select_session(REQ_CONNECT, i, 0);
            stopped(test_ms);
            CHECK(c.slot == i && c.last_slot == i && retained == i);
            CHECK(c.session.slot == i && c.session.bond == &test_bonds[i]);
            CHECK(c.session.settings == &test_slots[i] && !c.session.pairing);
            CHECK(!memcmp(c.session.address, test_addresses[i], 6));
        }
        CHECK(starts == 4);
        select_session(REQ_PAIR, SLOT_LAST, 1); stopped(test_ms);
        CHECK(c.slot == 3 && c.session.pairing && renewals == 1);
    }
    /* Xbox runs its own profile on a static random address. */
    test_reset(); test_slots[1].type = PROFILE_XBOX_BLE;
    start_session(1, 1, 0); CHECK(profile == &profile_xbox_ble && (c.session.address[5] & 0xc0) == 0xc0);
    /* Switch 2 writes save their proprietary key with the connected peer,
     * without changing another slot's bond. */
    test_reset(); test_slots[2].type = PROFILE_SWITCH2;
    test_bonds[0].ltk[0] = 0x17;
    start_session(2, 1, 0); CHECK(profile == &profile_switch2 && c.slot == 2);
    test_events[0].kind = BT_EV_LINKED;
    memset(test_events[0].bond.le_peer, 0x23, 6);
    test_events[1].kind = BT_EV_WRITE;
    test_events[1].write.kind = BT_WRITE_ATT; test_events[1].write.id = 0x14;
    test_events[1].write.data[0] = 0x42;
    event_count = 2; bluetooth();
    CHECK(test_bonds[2].ltk[0] == 0x42 && test_bonds[2].le_peer[0] == 0x23);
    CHECK(!c.session.pairing && test_bonds[0].ltk[0] == 0x17);
    test_events[0].kind = BT_EV_UNLINKED; event_at = 0; event_count = 1;
    c.rumble_until = 1000; bluetooth();
    CHECK(!c.linked && !c.rumble_until);

    /* Console wake is explicit and bonded, never an automatic boot or pairing. */
    test_reset(); test_slots[2].type = PROFILE_SWITCH2; test_bonds[2].ltk[0] = 1;
    retained = 2; start(); CHECK(!wakes);
    c.linked = 0;                /* offline: connecting the same slot again */
    select_session(REQ_CONNECT, 2, 0); stopped(test_ms); CHECK(wakes == 1);
    select_session(REQ_PAIR, 2, 0); stopped(test_ms); CHECK(wakes == 1);
    test_reset(); test_slots[2].type = PROFILE_SWITCH2;
    select_session(REQ_CONNECT, 2, 0); stopped(test_ms); CHECK(!wakes);

    /* Disabled/unsupported slots cannot tear down the running session. */
    test_reset();
    test_slots[1].type = PROFILE_NONE;
    test_slots[2].type = PROFILE_XBOX;
    test_slots[3].transport = TRANSPORT_USB;
    select_session(REQ_CONNECT, 0, 0); CHECK(!stops);
    select_session(REQ_CONNECT, 1, 0); select_session(REQ_PAIR, 2, 1);
    select_session(REQ_CONNECT, 3, 0); select_session(REQ_CONNECT, 4, 0);
    CHECK(!stops && !starts);
    /* A different slot can replace a connected session; latest slot wins. */
    test_reset();
    select_session(REQ_PAIR, 1, 1); select_session(REQ_CONNECT, 2, 0);
    CHECK(stops == 1 && !starts && c.slot == 0);
    stopped(test_ms); CHECK(c.slot == 2 && !renewals);
    /* Pairing completed during stop still belongs to the OLD slot. */
    test_reset(); select_session(REQ_CONNECT, 1, 0);
    memset(test_events, 0, sizeof(test_events));
    test_events[0].kind = BT_EV_PAIRED; test_events[0].bond.ltk[0] = 0x5a;
    test_events[1].kind = BT_EV_STOPPED; event_count = 2;
    bluetooth();
    CHECK(test_bonds[0].ltk[0] == 0x5a && !test_bonds[1].ltk[0] && c.slot == 1);
    /* The disconnect of our own stop keeps the pending slot change. */
    test_reset(); c.linked = 1; select_session(REQ_CONNECT, 1, 0);
    memset(test_events, 0, sizeof(test_events));
    test_events[0].kind = BT_EV_UNLINKED;
    test_events[1].kind = BT_EV_STOPPED; event_at = 0; event_count = 2;
    bluetooth();
    CHECK(starts == 1 && c.slot == 1 && !c.idle);
    /* The host's own disconnect still ends the session. */
    test_reset(); c.linked = 1;
    memset(test_events, 0, sizeof(test_events));
    test_events[0].kind = BT_EV_UNLINKED; event_at = 0; event_count = 1;
    bluetooth();
    CHECK(c.stopping && stops == 1);
    test_events[0].kind = BT_EV_STOPPED; event_at = 0; event_count = 1;
    bluetooth();
    CHECK(c.idle && !starts);
    /* Boot resumes the retained slot, falling back if it is disabled. */
    test_reset(); retained = 3; start(); CHECK(c.slot == 3);
    test_reset(); retained = 3; test_slots[3].type = PROFILE_NONE;
    start(); CHECK(c.slot == 0);
    test_reset(); ephemeral = 1; start(); CHECK(!retained_writes);

    /* Lights: no player light; red and a faster Home blink while pairing;
     * the light under Home for a second when a cable arrives. */
    test_reset(); c.lit = 0xff; c.linked = 0; c.session.pairing = 1;
    CHECK(lights(960) == 1080 && LIT(LIGHT_HOME) == lite_lights.home && LIT(LIGHT_P1) == lite_lights.red);
    CHECK(lights(1080) == 1200 && !LIT(LIGHT_HOME) && !LIT(LIGHT_P1) && !LIT(LIGHT_P0));
    c.session.pairing = 0;
    CHECK(lights(1000) == 1250 && LIT(LIGHT_HOME) && !LIT(LIGHT_P1));
    c.linked = 1;
    CHECK(!lights(1000) && LIT(LIGHT_HOME) && !LIT(LIGHT_P0) && !LIT(LIGHT_P1));
    test_usb = 1; power_step(5000);
    CHECK(lights(5000) == 5000u + ux_timeouts.usb_plug_ms && LIT(LIGHT_P0) == 255);
    power_step(5500);            /* still plugged: no new light */
    CHECK(!lights(5000 + ux_timeouts.usb_plug_ms) && !LIT(LIGHT_P0));
    test_usb = 0; power_step(7000); test_usb = 1; power_step(7100);
    CHECK(lights(7100) == 7100u + ux_timeouts.usb_plug_ms && LIT(LIGHT_P0));
    c.idle = 1; c.linked = 0; test_usb = 0; power_step(9000);
    CHECK(!lights(9000) && !LIT(LIGHT_HOME) && !LIT(LIGHT_P0));
    /* The remap: connected only, outside calibration; its state starts
     * over with another remap slot or new settings. */
    test_reset();
    test_remaps[2].map[0] = 1; test_remaps[REMAP_BASE].map[0] = 2;
    c.remap = 2; c.pad.buttons = PAD_A;
    remap(1000);
    CHECK(c.pad.buttons == (PAD_A | 1) && c.turbo_max == PAD_X && remap_resets == 1 && c.remap_live == 3);
    remap(1004); CHECK(remap_resets == 1 && remap_steps == 2);
    test_edits++; remap(1008); CHECK(remap_resets == 2);
    c.remap = REMAP_BASE; c.pad.buttons = 0; remap(1012);
    CHECK(remap_resets == 3 && c.pad.buttons == 2);
    c.calibrating = 1; remap(1016);
    CHECK(remap_resets == 4 && !c.remap_live && !c.turbo_max && remap_steps == 4);
    c.calibrating = 0; c.linked = 0; remap(1020); CHECK(remap_resets == 4 && remap_steps == 4);
    CHECK(sticks() == NULL);
    c.linked = 1; CHECK(sticks() == &test_remaps[REMAP_BASE]);
    /* Rumble over the step's range; step 0 off. */
    test_remaps[0].rumble = 2;
    test_remaps[0].rumble_min[0][1] = 20; test_remaps[0].rumble_max[0][1] = 60;
    CHECK(motor_duty(&test_remaps[0], 0, 0) == 0 && motor_duty(&test_remaps[0], 1, 0) == 51);
    CHECK(motor_duty(&test_remaps[0], 255, 0) == 153);
    test_remaps[0].rumble = 0;
    CHECK(motor_duty(&test_remaps[0], 255, 0) == 0);
    return TEST_DONE("controller_lite power");
}
#endif

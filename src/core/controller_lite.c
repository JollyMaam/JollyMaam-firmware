#include <string.h>
#include "pad.h"

/* The control loop for a pad whose RAM controller.c and settings.c do not
 * fit (the Telink B80's 16 KiB). Four slots select Generic BLE, Xbox (LE) or Switch 2,
 * sharing the input, calibration, settings, Home gestures and one radio
 * session at a time. Host commands run here, outside the SDK callbacks.
 *
 * The board's 2.4G/Bluetooth switch (in_raw.radio_24g) is above the slots'
 * radio setting: on 2.4G the pad is the stock 8BitDo receiver's (RECEIVER,
 * RADIO_RECEIVER, profile_8bitdo), whatever slot is asked for, and Sync pairs
 * the receiver; on Bluetooth, the slots, each with radio bluetooth (the only
 * radio this board's settings take).  Moving it ends the session: Home then
 * connects the other kind.  USB is the tool device only: no slot's usb_type
 * runs here.
 *
 * Config mode (Home+M held, padtool config): the tool's commands over LE
 * (profile_config.c) instead of a session, no slot's, as controller.c's. */

#define POLL_US 4000             /* connected: the input is looked at this often */
#define CAL_SAMPLE_MS 2          /* calibrating: the input is read this often */
#define LITE_SLOTS 4
#define RECEIVER   LITE_SLOTS    /* the receiver's session, after the slots */
static const char slot_names[] = "ABXYR";

/* The receiver's session runs as an Xbox pad through it, with the base
 * remap; its pairing is the platform's (plat_receiver_pair). */
static const struct set_slot receiver_settings = {
    .radio = RADIO_RECEIVER, .radio_type = PROFILE_XBOX, .usb_type = PROFILE_NONE,
};

static const struct profile *profile = &profile_generic_ble;

/* The profile a slot runs here: an LE type over Bluetooth; NULL for the
 * others (radio off, an unknown radio or type). */
static const struct profile *slot_profile(const struct set_slot *s)
{
    u8 type = s->radio_type;

    if (s->radio != RADIO_BLUETOOTH || !set_radio_ok(s->radio, type)) return NULL;
    return type == PROFILE_GENERIC_BLE ? &profile_generic_ble : type == PROFILE_XBOX_BLE ? &profile_xbox_ble :
           type == PROFILE_SWITCH2 ? &profile_switch2 : type == PROFILE_DS5EDGE_BLE ? &profile_ds5edge_ble : NULL;
}

static struct {
    struct in_raw raw;
    struct pad pad;
    struct pad input;            /* calibrated, as padtool input shows it */
    struct usb_status status;
    struct profile_session session;
    u8 linked;
    struct led_frame shown;      /* what the lights show */
    u8 led_demo;                 /* the LED demo shown (ux_led_demos), SLOT_NONE: none */
    u32 led_demo_ms, led_demo_since;    /* last asked for; started */
    u32 rumble_until;            /* plat_ms; 0: motors off */
    u8 calibrating;
    u8 activity_ref[offsetof(struct pad, imu_count)]; /* no unused IMU data */
    u32 activity_ms, sleep_ms, since_ms, usb_seen_ms;
    u8 sleeping, idle, stopping, pending, renew, pair_down;
    u8 slot, last_slot, pending_slot;   /* last_slot: a Bluetooth one, never RECEIVER */
#ifdef U2CW_REQUIRE_SLOT_SELECTION
    u8 slot_chosen;              /* a Bluetooth slot explicitly chosen this boot */
#endif
    u8 radio_24g;                /* the board's switch, as the session has it */
    u8 config;                   /* the session is config mode */
    u8 peer[6];
    u8 usb_was, plug_lit;        /* plug_lit: the light under Home for a new cable */
    u32 plugged_ms;
    u8 remap;                    /* the session's remap slot, REMAP_BASE the base */
    u8 remap_chosen, remap_slot; /* picked in quick settings: kept when remap_slot reconnects */
    u8 remap_live;               /* remap.c state belongs to remap_live - 1 at remap_edits */
    u8 turbo_up;                 /* the last report left turbo_max up */
    u8 rumble_step;              /* picked in quick settings, step + 1; 0: the remap's */
    u8 feedback;                 /* a step picked: this many pulses from feedback_ms */
    u16 feedback_ms;             /* plat_ms, low bits: the pulses take under a second */
    u32 remap_edits;
    u32 turbo_max;               /* keys flipped every report, down first */
#if PROBE_EDGES
    u32 probe_at_us;             /* the latency probe's next edge, 0: none */
#endif
    struct in_trigger triggers[2];
#if TOOL_PRESS
    u32 press, press_until;      /* the tool's PRESS: keys held until then */
#endif
} c;

static void start(void);
static void request(const struct ctl_request *r);
static void transition(u8 kind, u8 slot, u8 renew);

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

/* Connected, outside quick settings and calibration: the remap slot in
 * use.  Its state starts over with another remap slot or new settings. */
static void remap(u32 now)
{
    u32 edits = set_edits(), out;

#if TOOL_PRESS
    if (c.press && (s32)(now - c.press_until) >= 0) c.press = 0;
    if (c.linked && !c.stopping) c.pad.buttons |= c.press;
#endif
    c.turbo_max = 0;
    if (!c.linked || c.stopping || c.calibrating || ux_quick(NULL)) {
        if (c.remap_live) remap_reset();
        c.remap_live = 0;
        macro_stop();
        quick_stop(now);
        return;
    }
    if (quick_recording()) {     /* the remap suspended: the host sees what will play */
        c.pad.buttons = quick_record_step(c.pad.buttons, now);
        c.remap_live = 0;        /* its state starts over after */
        return;
    }
    if (c.remap_live != c.remap + 1 || c.remap_edits != edits) {
        remap_reset();
        macro_stop();
        c.remap_live = (u8)(c.remap + 1);
        c.remap_edits = edits;
    }
    out = remap_step(set_remap(c.remap), c.pad.buttons, now, &c.turbo_max, quick_keys());
    out = macro_play(c.remap, remap_macros(), out, now, &c.turbo_max);
    c.pad.buttons = quick_play(c.pad.buttons, out, now, &c.turbo_max);
}

/* The switch moved: the session ends, and Home connects the other kind
 * (a Home gesture, so a console may wake then, never at the switch). */
static void radio_switch(void)
{
    if (c.raw.radio_24g == c.radio_24g) return;
    c.radio_24g = c.raw.radio_24g;
    util_log("ctl: switch on %s", c.radio_24g ? "2.4G" : "Bluetooth");
    if (!c.idle && !c.sleeping) transition(REQ_NONE, SLOT_NONE, 0);
}

/* The loop's steps stay out of line (noinline): their locals are then not
 * on the stack under bluetooth(), the deepest path (a Switch 2 command). */
static __attribute__((noinline)) void read_input(u32 now)
{
    struct ctl_request r;
    u32 trig;
    u8 pair;

    plat_input(&c.raw);
    radio_switch();
#if PROBE_EDGES
    c.probe_at_us = util_probe_apply(&c.raw.buttons, &c.raw.changed_us);
#endif
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
    if (c.plug_lit && !c.linked && ux_updater_chord(c.raw.buttons)) {
        r = (struct ctl_request){ REQ_REBOOT_UPDATER, SLOT_NONE, 0 };   /* held as USB arrived */
        request(&r);
    }
    if (!c.calibrating) {
        r = ux_step(&c.pad, now, c.config ? CTL_IDLE : state());   /* config: Home as when idle */
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
    in_triggers(sticks(), c.triggers, &c.pad);
    trig = c.pad.buttons & (PAD_L2 | PAD_R2);
    remap(now);
    in_triggers_after(quick_recording() ? NULL : sticks(), trig, &c.pad);   /* recording: no remap */
    in_response(sticks(), &c.pad);
}

static int slot_ready(u8 slot)
{
    const struct set_slot *s;

    if (slot == RECEIVER) return c.radio_24g;   /* never asked for from outside */
    if (slot >= LITE_SLOTS) return 0;
    s = set_slot(slot);
    return slot_profile(s) != NULL;
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
    c.slot = slot;
    c.activity_ms = c.since_ms = plat_ms();
    memset(&c.session, 0, sizeof(c.session));
    c.session.slot = slot;
    if (slot == RECEIVER) {
        c.session.settings = &receiver_settings;
        c.session.bond = NULL;           /* the link never reads one */
        c.session.pairing = !plat_receiver_paired();
    } else {
        c.last_slot = slot;
        c.session.settings = set_slot(slot);
        c.session.bond = set_bond(slot);
        set_slot_address(slot, c.session.address);
        c.session.pairing = util_is_zero(c.session.bond->ltk, sizeof(c.session.bond->ltk));
    }
    /* A remap and a rumble step picked in quick settings last across this
     * slot's reconnects (a lost link, relink) until a disconnect, sleep or
     * another slot. */
    if (c.remap_slot != slot) {
        c.rumble_step = 0;
        quick_clear();
    }
    if (!c.remap_chosen || c.remap_slot != slot) {
        c.remap = c.session.settings->remap ? (u8)(c.session.settings->remap - 1) : REMAP_BASE;
        c.remap_slot = slot;
        c.remap_chosen = 0;
    }
    if (pairing >= 0) c.session.pairing = (u8)pairing;
    set_flush();                     /* previous session is stopped */
    if (slot == RECEIVER) {
        profile = &profile_8bitdo;
    } else {
        if (c.session.pairing && renew) set_pairing_address(slot, 1, c.session.address);
        if (c.session.settings->radio_type != PROFILE_SWITCH2) c.session.address[5] |= 0xc0;
#ifdef U2CW_REQUIRE_SLOT_SELECTION
        c.slot_chosen = 1;
#endif
        if (!set_is_ephemeral()) plat_retain_slot(slot);
        profile = slot_profile(c.session.settings);
    }
    profile->begin(&c.session);
    bt_start(profile, c.session.address, c.session.bond, c.session.pairing, NULL);
    util_log("ctl: %s slot %c", c.session.pairing ? "pairing" : "connecting", slot_names[slot]);
}

/* Config mode until ux_timeouts.config_ms without a host or, connected,
 * without a command; a lone Home, Home+Minus or the tools end it. */
static __attribute__((noinline)) void start_config(void)
{
    c.sleeping = 0;
    c.idle = 0;
    quick_clear();               /* config mode borrows the buffer a macro may hold */
    c.config = 1;
    c.slot = SLOT_NONE;
    c.since_ms = plat_ms();
    memset(&c.session, 0, sizeof(c.session));
    c.session.slot = SLOT_NONE;
    c.session.settings = set_slot(c.last_slot);
    c.session.bond = set_bond(c.last_slot);   /* no SM: nothing reads it (a zero one costs RAM) */
    c.session.pairing = 1;       /* advertised to anyone; no SM, so no bond */
    profile_config_address(c.session.address);
    profile = &profile_config;
    profile->begin(&c.session);
    bt_start(profile, c.session.address, c.session.bond, 1, NULL);
    util_log("ctl: config mode");
}

/* Only Home can connect at boot. With no retained slot the explicit slot
 * gesture is needed; a cable, reboot or updater return always stays idle. */
static void start(void)
{
    struct plat_boot b;
    u8 slot = plat_retained_slot();

#ifdef U2CW_REQUIRE_SLOT_SELECTION
    c.slot_chosen = slot < LITE_SLOTS;
#endif
    plat_boot_info(&b);
    plat_input(&c.raw);
    c.radio_24g = c.raw.radio_24g;
    if (slot >= LITE_SLOTS) slot = ux_first_slot;
    if (!slot_ready(slot)) for (slot = 0; slot < LITE_SLOTS && !slot_ready(slot); slot++) {}
    c.last_slot = slot < LITE_SLOTS ? slot : 0;
    util_log("ctl: boot cause %u, slot %u%s", b.cause, slot, c.radio_24g ? ", 2.4G" : "");
#ifdef U2CW_REQUIRE_SLOT_SELECTION
    /* A cold Home+slot chord must reach the gesture engine. Home alone
     * cannot silently choose the fallback slot when nothing was retained. */
    if (b.cause != BOOT_HOME || c.slot_chosen || c.radio_24g) ux_ignore_held();
#else
    ux_ignore_held();
#endif
    if (plat_config_asked()) {
        util_log("config mode: the updater asks");
        start_config();
        return;
    }
    if (b.cause == BOOT_HOME
#ifdef U2CW_REQUIRE_SLOT_SELECTION
        && (c.slot_chosen || c.radio_24g)
#endif
    ) {
        start_session(c.radio_24g ? RECEIVER : slot, -1, 0);
        return;
    }
    c.idle = 1;
    c.since_ms = plat_ms();
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
    c.feedback = 0;
    plat_motors(0, 0);
    if (c.stopping) return;
    c.stopping = 1;
    c.since_ms = plat_ms();
    bt_stop();
}

static void select_session(u8 kind, u8 slot, u8 renew)
{
#ifdef U2CW_REQUIRE_SLOT_SELECTION
    if (slot == SLOT_LAST && !c.radio_24g && !c.slot_chosen) return;
#endif
    if (slot == SLOT_LAST) slot = c.last_slot;
    if (c.radio_24g) slot = RECEIVER;    /* the switch on 2.4G: no slots */
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

    if (profile->end) profile->end();
    c.stopping = c.linked = c.config = 0;
    c.pending = REQ_NONE;
    c.pending_slot = SLOT_NONE;
    if (c.sleeping) return;
    if (pending == REQ_PAIR || pending == REQ_CONNECT) {
        start_session(slot, pending == REQ_PAIR ? 1 : -1, c.renew);
        if (pending == REQ_CONNECT && profile == &profile_switch2 && !c.session.pairing)
            bt_connect();
    } else if (pending == REQ_CONFIG) {
        start_config();
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
            c.activity_ms = c.since_ms = plat_ms();
            profile->begin(&c.session);          /* the first report goes out whatever it is */
        } else if (ev.kind == BT_EV_UNLINKED) {
            c.linked = 0;
            c.rumble_until = 0;
            c.feedback = 0;
            plat_motors(0, 0);
            c.since_ms = plat_ms();
            if (profile->end) profile->end();   /* config: a new window, advertising again */
            /* Match controller.c: a deliberate disconnect ends this session;
             * only a lost link stays in reconnect mode. Our own stop's
             * disconnect keeps the request it is stopping for. */
            if (!ev.lost && !c.stopping && !c.config) {
                util_log("ctl: host ended the session");
                transition(REQ_NONE, SLOT_NONE, 0);
            }
        } else if (ev.kind == BT_EV_PAIRED && c.slot == RECEIVER) {
            plat_receiver_pair(ev.bond.peer, !set_is_ephemeral());
            c.session.pairing = 0;
            util_log("ctl: receiver paired");
        } else if (ev.kind == BT_EV_PAIRED) {
            set_pairing_store(c.slot, c.session.address, &ev.bond);
            c.session.pairing = 0;
            bt_update_bond(&ev.bond);
            util_log("ctl: slot %c bond saved", slot_names[c.slot]);
        } else if (ev.kind == BT_EV_WRITE && !c.stopping && !c.sleeping) {
            if (c.config) c.since_ms = plat_ms();   /* a command: config mode stays */
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

static u8 rumble_step(const struct set_remap *m)
{
    return c.rumble_step ? (u8)(c.rumble_step - 1) : m->rumble;
}

/* Host rumble spreads over the motor's range for the step: 1 is its
 * rumble_min, 255 its rumble_max. */
static u8 motor_duty(const struct set_remap *m, u8 host, int right)
{
    u32 step = rumble_step(m), lo, hi;

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
    if (c.feedback) {                               /* a step picked: its pulses */
        u32 next;
        u8 on = ux_rumble_feedback(c.feedback, (u16)(now - c.feedback_ms), &next) ? 255 : 0;

        if (!next) c.feedback = 0;
        plat_motors(on, on);
    } else if (ux_quick(NULL)) plat_motors(0, 0);  /* quick settings: still */
    else plat_motors(motor_duty(m, h.rumble[0], 0), motor_duty(m, h.rumble[1], 1));
    if (h.ended) transition(REQ_NONE, SLOT_NONE, 0);
}

static __attribute__((noinline)) void report(u32 now)
{
    u8 out[PROFILE_REPORT_MAX], id = 0;
    u32 buttons = c.pad.buttons;
    int n;

    if (c.sleeping || c.stopping || !c.linked || !bt_input_wanted()) return;
    if (!c.turbo_up) c.pad.buttons |= c.turbo_max;
    n = profile->report(&c.pad, now, out, &id);
    c.pad.buttons = buttons;     /* a profile may change them while building */
    if (n > 0 && bt_send_input(id, out, (u16)n)) {
        c.turbo_up = c.turbo_max && !c.turbo_up;
        if (buttons & PAD_HOME) ux_reported(now);
#if PROBE_EDGES
        util_probe_sent(plat_us());
#endif
    }
}

static u32 earliest(u32 a, u32 b)
{
    return !a || (b && (s32)(b - a) < 0) ? b : a;
}

/* The lights (leds_mono.c): no player lights on this pad.  Returns the next
 * change, plat_ms. */
static __attribute__((noinline)) u32 lights(u32 now)
{
    struct led_mono m;
    struct led_frame f;
    u32 next, signal = 0;

    if (c.plug_lit && now - c.plugged_ms >= ux_timeouts.usb_plug_ms) c.plug_lit = 0;
    if (c.led_demo != SLOT_NONE && now - c.led_demo_ms >= UX_LED_DEMO_MS) {
        util_log("led demo over");
        c.led_demo = SLOT_NONE;
    }
    if (c.led_demo != SLOT_NONE) {
        led_mono_demo(&ux_led_demos[c.led_demo], now - c.led_demo_since, &m);
    } else {
        memset(&m, 0, sizeof(m));
        /* going to sleep: the stop on the way is no connect to blink for */
        m.show = c.sleeping ? MONO_DARK : ux_quick(NULL) || c.config ? MONO_QUICK : c.idle ? MONO_DARK :
                 c.linked ? MONO_CONNECTED : state() == CTL_PAIRING ? MONO_PAIRING : MONO_CONNECTING;
        m.cable = c.plug_lit && !c.sleeping;
        m.cal_lit = (u8)cal_led(now, &m.cal);
        m.signal = quick_led(now, &signal, ux_quick(NULL) ? c.input.buttons : 0);
    }
    next = earliest(led_mono(&m, now, &f), signal);
    if (memcmp(&f, &c.shown, sizeof(f))) {
        c.shown = f;
        plat_leds(&f);
    }
    if (c.plug_lit) next = earliest(next, c.plugged_ms + ux_timeouts.usb_plug_ms);
    if (c.led_demo != SLOT_NONE) next = earliest(next, now + ux_timeouts.led_frame_ms);   /* its fakes, its end */
    return next;
}

static void request(const struct ctl_request *r)
{
    switch (r->kind) {
    case REQ_NONE:
        break;
    case REQ_CONNECT:
    case REQ_PAIR:
        if (c.config && r->kind == REQ_CONNECT && r->slot == SLOT_LAST) {
            transition(REQ_NONE, SLOT_NONE, 0);     /* a lone Home leaves config mode */
            break;
        }
        if (!c.sleeping) select_session(r->kind, r->slot, r->generate_new_address);
        break;
    case REQ_CONFIG:
        if (c.sleeping) break;
        if (c.config) c.since_ms = plat_ms();   /* again: the window starts over */
        else transition(REQ_CONFIG, SLOT_NONE, 0);
        break;
    case REQ_RELINK:             /* config mode: the host is done, the pad advertises again */
        if (c.config && c.linked) bt_drop();
        else util_log("ctl: relink is not on this board");
        break;
    case REQ_DISCONNECT:
        c.remap_chosen = c.rumble_step = 0;
        quick_clear();
        if (!c.sleeping) transition(REQ_NONE, SLOT_NONE, 0);
        break;
    case REQ_REMAP:              /* quick settings: a slot key turns its remap on or off */
        if (!c.linked || c.stopping || c.config || r->slot >= LITE_SLOTS) break;
        c.remap = r->slot != c.remap ? r->slot : REMAP_BASE;
        c.remap_chosen = 1;
        c.remap_slot = c.slot;
        quick_signal(c.remap == REMAP_BASE ? SIG_REMAP_OFF : SIG_REMAP_ON, plat_ms());
        util_log("slot %c remap %c", slot_names[c.slot], c.remap < LITE_SLOTS ? slot_names[c.remap] : '-');
        break;
    case REQ_QUICK_CLEAR:        /* quick settings as the slot starts: its remap, nothing else */
        if (!c.linked || c.stopping || c.config) break;
        c.remap = c.session.settings->remap ? (u8)(c.session.settings->remap - 1) : REMAP_BASE;
        c.remap_chosen = c.rumble_step = 0;
        quick_clear();
        break;
    case REQ_QUICK_TURBO:
        if (c.linked && !c.stopping && !c.config) quick_turbo(r->slot, plat_ms());
        break;
    case REQ_QUICK_RECORD:
    case REQ_QUICK_RECORD_LOOP:
        if (c.linked && !c.stopping && !c.config) quick_record(r->slot, r->kind == REQ_QUICK_RECORD_LOOP, plat_ms());
        break;
    case REQ_RUMBLE_STEP: {      /* slot 1 up, 0 down */
        u8 step;

        if (!c.linked || c.stopping || c.config) break;
        step = rumble_step(set_remap(c.remap));
        if (r->slot && step < RUMBLE_STEPS) step++;
        if (!r->slot && step) step--;
        c.rumble_step = (u8)(step + 1);
        c.rumble_until = 0;
        c.feedback = step;
        c.feedback_ms = (u16)plat_ms();
        util_log("rumble step %u", step);
        break;
    }
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
    case REQ_REBOOT_UPDATER:
        util_log("ctl: to the updater");
        set_flush();
        plat_reboot_updater();
        break;
    case REQ_CALIBRATE:
        ux_ignore_held();
        c.calibrating = 1;
        cal_start(plat_ms());
        break;
    case REQ_LED_DEMO:
        if (r->slot != c.led_demo) {
            util_log("led demo %d", r->slot == SLOT_NONE ? -1 : (int)r->slot);
            c.led_demo_since = plat_ms();
        }
        c.led_demo = r->slot;
        c.led_demo_ms = plat_ms();
        break;
#if TOOL_PRESS
    case REQ_PRESS:
        c.press = r->ms ? r->buttons & PAD_DIGITAL : 0;
        c.press_until = plat_ms() + r->ms;
        break;
#endif
    case REQ_RUMBLE:
        c.feedback = 0;
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
    if (c.config || (!c.linked && !c.idle)) {
        deadline = c.since_ms + (c.config ? ux_timeouts.config_ms : c.session.pairing ? ux_timeouts.pair_ms :
                                 ux_timeouts.connect_ms);
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
        c.remap_chosen = c.rumble_step = 0;
        quick_clear();
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
    u32 next, power, gesture, at;

    c.status.input = &c.input;
    c.status.raw = &c.raw;
#if TOOL_PRESS
    c.status.output = &c.pad;
#endif
    memset(&c.shown, 0xff, sizeof(c.shown));   /* the first frame goes out */
    c.led_demo = SLOT_NONE;
    set_init();
    start();
    for (;;) {
        power = power_step(plat_ms());
        next = lights(plat_ms());
        gesture = ux_deadline();
        if (gesture && (!next || (s32)(gesture - next) < 0)) next = gesture;
        if (power && (!next || (s32)(power - next) < 0)) next = power;
        if (c.rumble_until && (!next || (s32)(c.rumble_until - next) < 0)) next = c.rumble_until;
        next = earliest(next, earliest(earliest(remap_deadline(), macro_deadline()), quick_deadline()));
#if TOOL_PRESS
        if (c.press) next = earliest(next, c.press_until);
#endif
        if (c.calibrating) next = plat_ms() + CAL_SAMPLE_MS;
        /* a report is built when the link wants it (bt_input_due_us: just
         * before the connection event it is for) */
        at = c.linked ? earliest(plat_us() + POLL_US, bt_input_due_us()) : 0;
#if PROBE_EDGES
        at = earliest(at, c.probe_at_us);
#endif
        plat_wait(next, at);
        if (c.rumble_until && (s32)(plat_ms() - c.rumble_until) >= 0) {
            plat_motors(0, 0);
            c.rumble_until = 0;
        }
        bluetooth();
        if (c.config) profile_config_service();
        read_input(plat_ms());
        host(plat_ms());
        report(plat_ms());
        c.status.state = state();
        c.status.slot = c.idle || c.sleeping || c.config || c.slot == RECEIVER ? SLOT_NONE : c.slot;
        c.status.usb_powered = (u8)plat_usb_present();
        plat_battery(&c.status.battery);
        c.status.low_battery = !c.status.usb_powered &&
            (c.status.battery.flags & (JM_BATTERY_LOW_VALID | JM_BATTERY_LOW)) ==
            (JM_BATTERY_LOW_VALID | JM_BATTERY_LOW);
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
static int ephemeral, retained_writes, held_ignored;
static struct bt_ev test_events[2];
static u8 event_at, event_count;

u32 plat_ms(void) { return test_ms; }
int plat_usb_present(void) { return test_usb; }
const u8 *plat_unique_id(void) { static const u8 id[16]; return id; }
const struct bt_bond *set_bond(u8 slot) { return &test_bonds[slot]; }
int set_radio_ok(u8 radio, u8 type) { (void)type; return radio <= RADIO_DONGLE; }
const struct set_slot *set_slot(u8 slot) { return &test_slots[slot]; }
void set_slot_address(u8 slot, u8 out[6]) { memcpy(out, test_addresses[slot], 6); }
void set_pairing_address(u8 slot, int renew, u8 out[6])
{ (void)slot; renewals += renew; memset(out, 0x22, 6); }
u8 plat_retained_slot(void) { return retained; }
void plat_retain_slot(u8 slot) { retained = slot; retained_writes++; }
static u8 boot_cause = BOOT_HOME;
void plat_boot_info(struct plat_boot *out) { out->cause = boot_cause; out->buttons_held = 0; }
static int config_asked;
int plat_config_asked(void) { int a = config_asked; config_asked = 0; return a; }
int bt_poll(struct bt_ev *ev)
{ if (event_at == event_count) return 0; *ev = test_events[event_at++]; return 1; }
void set_pairing_store(u8 slot, const u8 address[6], const struct bt_bond *bond)
{ test_bonds[slot] = *bond; memcpy(test_addresses[slot], address, 6); }
void bt_update_bond(const struct bt_bond *bond) { (void)bond; }
void bt_start(const struct profile *p, const u8 a[6], const struct bt_bond *k,
              int pairing, const struct bt_link *link)
{ (void)p; (void)a; (void)k; (void)pairing; (void)link; starts++; }
void bt_stop(void) { stops++; }
static int drops;
void bt_drop(void) { drops++; }
void bt_connect(void) { wakes++; }
void set_flush(void) { flushes++; }
int set_is_ephemeral(void) { return ephemeral; }
u32 ux_idle_deadline(u32 since, u32 activity, u32 usb_seen, int powered, int ephemeral)
{
    u32 offline = since + 15000, unplug = usb_seen + 5000;
    (void)activity; (void)powered; (void)ephemeral;
    return (s32)(offline - unplug) > 0 ? offline : unplug;
}
static int test_quick;
int ux_quick(u32 *since) { (void)since; return test_quick; }
/* One pulse per level, 10 ms each. */
int ux_rumble_feedback(u8 level, u32 elapsed, u32 *next)
{
    *next = level && elapsed < 10u * level ? elapsed + 1 : 0;
    return *next != 0;
}
void plat_power_off(void) { CHECK(flushes > offs); offs++; }
void plat_reboot(void) { reboots++; }
void plat_reboot_loader(void) { reboots++; }
void plat_reboot_updater(void) { reboots++; }
void ux_ignore_held(void) { held_ignored++; }
void cal_start(u32 now) { (void)now; }
void plat_motors(u8 left, u8 right) { CHECK(!left && !right); motors++; }
static int test_leds;
void plat_leds(const struct led_frame *f) { (void)f; test_leds++; }
int cal_led(u32 now, struct rgb *out) { (void)now; (void)out; return 0; }
/* leds_mono.c draws: here what it was asked for, next change at 0 */
static struct led_mono test_mono;
const struct ux_led_demo ux_led_demos[] = { { .name = "one" }, { .name = "two" } };
static u32 test_demo_t;
void led_mono_demo(const struct ux_led_demo *demo, u32 t, struct led_mono *m)
{
    memset(m, 0, sizeof(*m));
    m->show = (u8)(demo == &ux_led_demos[1] ? MONO_QUICK : MONO_PAIRING);
    test_demo_t = t;
}
u32 led_mono(const struct led_mono *in, u32 now, struct led_frame *out)
{
    (void)now;
    test_mono = *in;
    memset(out, 0, sizeof(*out));
    out->light[LIGHT_HOME].r = (u8)(in->show + 1);
    out->light[LIGHT_CABLE].r = in->cable;
    return 0;
}
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
const struct profile profile_ds5edge_ble = { .begin = test_begin };
const struct profile profile_8bitdo = { .begin = test_begin };
static int config_ends;
static void test_config_end(void) { config_ends++; }
const struct profile profile_config = { .begin = test_begin, .end = test_config_end };
void profile_config_address(u8 out[6]) { memset(out, 0x33, 6); }
void profile_config_service(void) {}
static u8 test_24g, test_receiver_known, test_receiver_saves, test_receiver[5];
void plat_input(struct in_raw *raw) { memset(raw, 0, sizeof(*raw)); raw->radio_24g = test_24g; }
int plat_receiver_paired(void) { return test_receiver_known; }
void plat_receiver_pair(const u8 a[5], int save)
{ memcpy(test_receiver, a, 5); test_receiver_known = 1; test_receiver_saves += (u8)save; }
int bt_send_message(u16 id, const u8 *data, u16 len) { (void)id; (void)data; (void)len; return 1; }
static struct set_remap test_remaps[REMAP_BASE + 1];
static u32 test_edits, remap_resets, remap_steps;
const struct set_remap *set_remap(u8 remap) { return &test_remaps[remap]; }
u32 set_edits(void) { return test_edits; }
void remap_reset(void) { remap_resets++; }
u32 remap_step(const struct set_remap *m, u32 in, u32 now, u32 *every_report, const struct remap_quick *q)
{ (void)now; (void)q; remap_steps++; *every_report = PAD_WEST; return in | m->map[0]; }
static const struct remap_quick test_quick_keys;
static u32 quick_clears;
const struct remap_quick *quick_keys(void) { return &test_quick_keys; }
void quick_clear(void) { quick_clears++; }
void quick_turbo(u8 key, u32 now) { (void)key; (void)now; }
void quick_record(u8 key, int loop, u32 now) { (void)key; (void)loop; (void)now; }
int quick_recording(void) { return 0; }
u32 quick_record_step(u32 in, u32 now) { (void)now; return in; }
u32 quick_play(u32 in, u32 out, u32 now, u32 *every) { (void)in; (void)now; (void)every; return out; }
void quick_stop(u32 now) { (void)now; }
u32 quick_deadline(void) { return 0; }
static const struct remap_macros test_macros;
const struct remap_macros *remap_macros(void) { return &test_macros; }
u32 macro_play(u8 remap, const struct remap_macros *r, u32 out, u32 now, u32 *every)
{ (void)remap; (void)r; (void)now; (void)every; return out; }
void macro_stop(void) {}
u32 macro_deadline(void) { return 0; }
void quick_signal(u8 sig, u32 now) { (void)sig; (void)now; }
u8 quick_led(u32 now, u32 *next, u32 held) { (void)now; (void)held; *next = 0; return LED_SIGNAL_NONE; }
const u8 ux_first_slot = 3;
const struct ux_timeouts ux_timeouts = {
    .idle_sleep_ms = 300000, .stop_max_ms = 12000, .connect_ms = 30000, .pair_ms = 180000,
    .usb_plug_ms = 1000, .config_ms = 60000,
};

static void test_reset(void)
{
    u8 i;

    memset(&c, 0, sizeof(c));
    c.led_demo = SLOT_NONE;
    profile = &profile_generic_ble;
    test_ms = 100;
    test_usb = stops = flushes = offs = starts = motors = renewals = reboots = wakes = test_quick = 0;
    test_24g = test_receiver_known = test_receiver_saves = 0;
    memset(test_bonds, 0, sizeof(test_bonds));
    memset(test_slots, 0, sizeof(test_slots));
    for (i = 0; i < LITE_SLOTS; i++) {
        test_slots[i].radio_type = PROFILE_GENERIC_BLE;
        test_slots[i].radio = RADIO_BLUETOOTH;
        memset(test_addresses[i], 0x11 + i, 6);
        test_addresses[i][5] |= 0xc0;
    }
    retained = SLOT_NONE;
#ifdef U2CW_REQUIRE_SLOT_SELECTION
    c.slot_chosen = 1;
#endif
    ephemeral = retained_writes = held_ignored = event_at = event_count = 0;
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
    test_reset(); test_slots[1].radio_type = PROFILE_XBOX_BLE;
    start_session(1, 1, 0); CHECK(profile == &profile_xbox_ble && (c.session.address[5] & 0xc0) == 0xc0);
    test_reset(); test_slots[1].radio_type = PROFILE_DS5EDGE_BLE;
    start_session(1, 1, 0); CHECK(profile == &profile_ds5edge_ble && c.slot == 1);
    /* Switch 2 writes save their proprietary key with the connected peer,
     * without changing another slot's bond. */
    test_reset(); test_slots[2].radio_type = PROFILE_SWITCH2;
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
    test_reset(); test_slots[2].radio_type = PROFILE_SWITCH2; test_bonds[2].ltk[0] = 1;
    retained = 2; start(); CHECK(!wakes);
    c.linked = 0;                /* offline: connecting the same slot again */
    select_session(REQ_CONNECT, 2, 0); stopped(test_ms); CHECK(wakes == 1);
    select_session(REQ_PAIR, 2, 0); stopped(test_ms); CHECK(wakes == 1);
    test_reset(); test_slots[2].radio_type = PROFILE_SWITCH2;
    select_session(REQ_CONNECT, 2, 0); stopped(test_ms); CHECK(!wakes);

    /* Disabled/unsupported slots cannot tear down the running session. */
    test_reset();
    test_slots[1].radio_type = PROFILE_NONE;
    test_slots[2].radio_type = PROFILE_XBOX;
    test_slots[3].radio = RADIO_OFF; test_slots[3].usb_type = PROFILE_XBOX;
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
#ifdef U2CW_REQUIRE_SLOT_SELECTION
    /* No retained slot: cold Home alone stays idle, including its release.
     * A held Home+slot chord remains available to the gesture engine. */
    test_reset(); c.linked = 0; boot_cause = BOOT_HOME; start();
    CHECK(c.idle && !starts && !held_ignored && !c.slot_chosen);
    select_session(REQ_CONNECT, SLOT_LAST, 0);
    CHECK(c.idle && !starts && !stops);
    select_session(REQ_CONNECT, 2, 0); stopped(test_ms);
    CHECK(starts == 1 && c.slot == 2 && c.slot_chosen);
    transition(REQ_NONE, SLOT_NONE, 0); stopped(test_ms);
    select_session(REQ_CONNECT, SLOT_LAST, 0); stopped(test_ms);
    CHECK(starts == 2 && c.slot == 2);
    /* USB/reset boots drain held keys and still require explicit selection. */
    test_reset(); c.linked = 0; boot_cause = BOOT_USB; start();
    CHECK(c.idle && !starts && held_ignored && !c.slot_chosen);
    select_session(REQ_CONNECT, SLOT_LAST, 0); CHECK(!starts && !stops);
    boot_cause = BOOT_HOME;
#endif
    /* Boot resumes the retained slot, falling back if it is disabled. */
    test_reset(); retained = 3; start(); CHECK(c.slot == 3);
    test_reset(); retained = 3; test_slots[3].radio_type = PROFILE_NONE;
    start(); CHECK(c.slot == 0);
    test_reset(); ephemeral = 1; start(); CHECK(!retained_writes);
    /* None retained: Home's slot is ux_first_slot. */
    test_reset(); boot_cause = BOOT_USB; start(); CHECK(c.idle && c.last_slot == ux_first_slot);
    boot_cause = BOOT_HOME;
    /* Only a Home boot connects; a cable's leaves the slot for Home. */
    test_reset(); retained = 2; boot_cause = BOOT_USB; start();
    CHECK(c.idle && !starts && c.last_slot == 2);
    select_session(REQ_CONNECT, SLOT_LAST, 0); stopped(test_ms);
    CHECK(starts == 1 && c.slot == 2 && !c.idle);
    test_reset(); boot_cause = BOOT_RESET; start(); CHECK(c.idle && !starts);
    boot_cause = BOOT_HOME;

    /* Lights: pairing, connecting, connected; the light under Home for a
     * while when a cable arrives; dark idle; drawn on a change only. */
    test_reset(); c.linked = 0; c.session.pairing = 1;
    test_leds = 0;
    CHECK(!lights(960) && test_mono.show == MONO_PAIRING && !test_mono.cable && test_leds == 1);
    CHECK(!lights(970) && test_leds == 1);
    c.session.pairing = 0;
    CHECK(!lights(1000) && test_mono.show == MONO_CONNECTING && test_leds == 2);
    c.linked = 1;
    CHECK(!lights(1000) && test_mono.show == MONO_CONNECTED);
    test_usb = 1; power_step(5000);
    CHECK(lights(5000) == 5000u + ux_timeouts.usb_plug_ms && test_mono.cable);
    power_step(5500);            /* still plugged: no new light */
    CHECK(!lights(5000 + ux_timeouts.usb_plug_ms) && !test_mono.cable);
    test_usb = 0; power_step(7000); test_usb = 1; power_step(7100);
    CHECK(lights(7100) == 7100u + ux_timeouts.usb_plug_ms && test_mono.cable);
    c.idle = 1; c.linked = 0; test_usb = 0; power_step(9000);
    CHECK(!lights(9000) && test_mono.show == MONO_DARK && !test_mono.cable);
    /* An LED demo: over the state until UX_LED_DEMO_MS after the last ask. */
    test_ms = 10000;
    { struct ctl_request r = { REQ_LED_DEMO, 1, 0 }; request(&r); }
    CHECK(lights(10500) == 10500u + ux_timeouts.led_frame_ms && test_mono.show == MONO_QUICK && test_demo_t == 500);
    test_ms = 20000;
    { struct ctl_request r = { REQ_LED_DEMO, 1, 0 }; request(&r); }   /* asked again: no restart */
    lights(20000); CHECK(test_demo_t == 10000);
    CHECK(!lights(20000 + UX_LED_DEMO_MS) && test_mono.show == MONO_DARK && c.led_demo == SLOT_NONE);
    /* The remap: connected only, outside calibration; its state starts
     * over with another remap slot or new settings. */
    test_reset();
    test_remaps[2].map[0] = 1; test_remaps[REMAP_BASE].map[0] = 2;
    c.remap = 2; c.pad.buttons = PAD_SOUTH;
    remap(1000);
    CHECK(c.pad.buttons == (PAD_SOUTH | 1) && c.turbo_max == PAD_WEST && remap_resets == 1 &&
          c.remap_live == 3);
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

    /* Quick settings: no remap while it is open; its lights. */
    test_reset(); test_quick = 1; c.remap = 2; remap_steps = 0; remap(1000);
    CHECK(!remap_steps && !c.remap_live);
    lights(1200); CHECK(test_mono.show == MONO_QUICK);
    /* A remap picked there: connected only; it lasts across this slot's
     * reconnects, not another slot's, a disconnect or sleep. */
    test_reset(); c.slot = 1; test_slots[1].remap = 0; c.remap = REMAP_BASE;
    { struct ctl_request r = { REQ_REMAP, 0, 0 }; request(&r); }
    CHECK(c.remap == 0 && c.remap_chosen && c.remap_slot == 1);
    start_session(1, -1, 0); CHECK(c.remap == 0);
    start_session(2, -1, 0); CHECK(c.remap == REMAP_BASE && !c.remap_chosen);
    { struct ctl_request r = { REQ_REMAP, 3, 0 }; c.linked = 1; request(&r); }
    CHECK(c.remap == 3);
    { struct ctl_request r = { REQ_REMAP, SLOT_NONE, 0 }; request(&r); }
    CHECK(c.remap == 3);                             /* no such remap slot */
    { struct ctl_request r = { REQ_REMAP, 3, 0 }; request(&r); }
    CHECK(c.remap == REMAP_BASE && c.remap_chosen);  /* again: off */
    { struct ctl_request r = { REQ_QUICK_CLEAR, SLOT_NONE, 0 }; test_slots[1].remap = 2; c.session.settings = &test_slots[1]; request(&r); }
    CHECK(c.remap == 1 && !c.remap_chosen && quick_clears);
    { struct ctl_request r = { REQ_REMAP, 1, 0 }; request(&r); }
    { struct ctl_request r = { REQ_DISCONNECT, SLOT_NONE, 0 }; request(&r); }
    CHECK(!c.remap_chosen);
    test_reset(); c.linked = 0;
    { struct ctl_request r = { REQ_REMAP, 1, 0 }; request(&r); }
    CHECK(!c.remap_chosen && c.remap == 0);
    /* A rumble step picked there: connected only, from the remap's, within
     * off..RUMBLE_STEPS; it pulses, and lasts as the remap picked there. */
    test_reset(); c.slot = c.remap_slot = 1; c.remap = 0; test_remaps[0].rumble = 3;
    { struct ctl_request r = { REQ_RUMBLE_STEP, 1, 0 }; request(&r); }
    CHECK(c.rumble_step == 5 && rumble_step(&test_remaps[0]) == 4 && c.feedback == 4);
    { struct ctl_request r = { REQ_RUMBLE_STEP, 1, 0 }; request(&r); }
    { struct ctl_request r = { REQ_RUMBLE_STEP, 1, 0 }; request(&r); }
    CHECK(rumble_step(&test_remaps[0]) == RUMBLE_STEPS);
    test_remaps[0].rumble_min[0][4] = 20; test_remaps[0].rumble_max[0][4] = 60;
    CHECK(motor_duty(&test_remaps[0], 1, 0) == 51);
    for (u8 i = 0; i < 7; i++) { struct ctl_request r = { REQ_RUMBLE_STEP, 0, 0 }; request(&r); }
    CHECK(c.rumble_step == 1 && !rumble_step(&test_remaps[0]) && !c.feedback);
    CHECK(motor_duty(&test_remaps[0], 255, 0) == 0);
    start_session(1, -1, 0); CHECK(c.rumble_step == 1);
    start_session(2, -1, 0); CHECK(!c.rumble_step && rumble_step(&test_remaps[0]) == 3);
    c.linked = 1; c.slot = 2; test_remaps[REMAP_BASE].rumble = 3;
    { struct ctl_request r = { REQ_RUMBLE_STEP, 0, 0 }; request(&r); }
    CHECK(c.rumble_step == 3);
    { struct ctl_request r = { REQ_DISCONNECT, SLOT_NONE, 0 }; request(&r); }
    CHECK(!c.rumble_step && !c.feedback);
    test_reset(); c.linked = 0;
    { struct ctl_request r = { REQ_RUMBLE_STEP, 1, 0 }; request(&r); }
    CHECK(!c.rumble_step && !c.feedback);
    /* The 2.4G switch: whatever slot is asked for, the receiver's session;
     * the Bluetooth slot stays for the way back, and nothing is retained.
     * Its pairing is the platform's, saved unless ephemeral. */
    test_reset(); c.radio_24g = 1; c.last_slot = 2;
    select_session(REQ_CONNECT, 1, 0); stopped(test_ms);
    CHECK(c.slot == RECEIVER && profile == &profile_8bitdo && c.last_slot == 2);
    CHECK(c.session.pairing && retained == SLOT_NONE && c.status.slot != RECEIVER);
    memset(test_events, 0, sizeof(test_events));
    test_events[0].kind = BT_EV_PAIRED;
    memcpy(test_events[0].bond.peer, "\x01\x02\x03\x04\x05", 5);
    event_at = 0; event_count = 1;
    bt_events();
    CHECK(test_receiver_known && test_receiver_saves == 1 && test_receiver[4] == 5 && !c.session.pairing);
    CHECK(!test_bonds[2].ltk[0] && !test_bonds[1].ltk[0]);
    /* moved during the session: it ends, idle; Home then connects the
     * other kind: the Bluetooth slot it had, a Switch 2 one with its wake */
    c.raw.radio_24g = 0; stops = 0;
    radio_switch(); CHECK(stops == 1 && c.pending == REQ_NONE);
    stopped(test_ms); CHECK(c.idle && starts == 1 && !c.radio_24g);
    test_slots[2].radio_type = PROFILE_SWITCH2; test_bonds[2].ltk[0] = 1; wakes = 0;
    select_session(REQ_CONNECT, SLOT_LAST, 0); stopped(test_ms);
    CHECK(c.slot == 2 && profile == &profile_switch2 && wakes == 1);
    c.linked = 1; c.raw.radio_24g = 1;
    radio_switch(); stopped(test_ms); CHECK(c.idle && c.slot == 2);
    select_session(REQ_CONNECT, SLOT_LAST, 0); stopped(test_ms);
    CHECK(c.slot == RECEIVER && c.last_slot == 2);
    /* idle: the switch is only noted */
    test_reset(); c.idle = 1; c.raw.radio_24g = 1; radio_switch(); CHECK(!stops && c.radio_24g);
    /* Home at boot on 2.4G: the receiver */
    test_reset(); test_24g = 1; boot_cause = BOOT_HOME; start(); CHECK(c.slot == RECEIVER && starts == 1);

    /* Config mode: instead of a session, no slot's.  Asked again, its
     * window starts over; the host leaving keeps it, advertising; a lone
     * Home ends it, as does the window without a host. */
    {
        struct ctl_request cfg = { REQ_CONFIG, SLOT_NONE, 0 }, home = { REQ_CONNECT, SLOT_LAST, 0 };
        struct ctl_request relink = { REQ_RELINK, SLOT_NONE, 0 };

        test_reset(); c.linked = 0; c.idle = 1; c.last_slot = 1; config_ends = drops = 0;
        request(&cfg); CHECK(stops == 1 && !starts && c.pending == REQ_CONFIG);
        stopped(test_ms);
        CHECK(c.config && profile == &profile_config && starts == 1 && c.slot == SLOT_NONE);
        CHECK(c.session.address[0] == 0x33 && c.session.address[5] == 0x33 && c.session.pairing);
        CHECK(power_step(test_ms + 59999) == test_ms + 60000 && !c.stopping);
        test_ms += 30000; request(&cfg); CHECK(c.since_ms == test_ms);
        power_step(test_ms + 59999); CHECK(!c.stopping);
        test_events[0].kind = BT_EV_LINKED; event_at = 0; event_count = 1; bluetooth();
        CHECK(c.linked && c.since_ms == test_ms);
        request(&relink); CHECK(drops == 1);
        test_events[0].kind = BT_EV_UNLINKED; event_at = 0; event_count = 1; bluetooth();
        CHECK(c.config && !c.stopping && !c.linked && config_ends == 1);
        request(&home); CHECK(c.stopping && c.pending == REQ_NONE);
        stopped(test_ms); CHECK(c.idle && !c.config && config_ends == 2 && profile == &profile_config);
        /* the window over: idle, and the next session is a slot's again */
        request(&cfg); stopped(test_ms); CHECK(c.config);
        power_step(test_ms + 60000); CHECK(c.stopping);
        stopped(test_ms); CHECK(c.idle && !c.config);
        select_session(REQ_CONNECT, SLOT_LAST, 0); stopped(test_ms);
        CHECK(c.slot == 1 && profile == &profile_generic_ble && !c.config);
        /* after an update the updater asks for config mode: it starts at boot, whatever the cause */
        test_reset(); c.linked = 0; boot_cause = BOOT_RESET; config_asked = 1; start();
        CHECK(c.config && profile == &profile_config && starts == 1 && !config_asked);
        boot_cause = BOOT_HOME;
    }
    return TEST_DONE("controller_lite power");
}
#endif

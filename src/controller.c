#include <string.h>
#include "pad.h"
#include "link.h"

#define LOW_BATTERY_MV   3500
#define BATTERY_OK_MV    3600
#define BATTERY_CHECK_MS 1000
#define USB_DEBOUNCE_MS  100
#define USB_GONE_MS      500     /* a USB slot's host without frames this long: unplugged */
#define USB_REARM_MS     500     /* a host's re-configuration this short keeps CONNECTED */
#define RETRY_MS         1000
#define QUIET_MS         500
#define CAL_SAMPLE_MS    2

static const char *const names[CTL_KIND_COUNT] = {
    "boot", "idle", "connecting", "pairing", "connected", "stopping", "sleeping", "rebooting",
};

/* The most reports a second, for every profile and both transports: the
 * host's polling sets the rate below it (pull), and it keeps the CPU's
 * share bounded on a host that polls faster.  On USB, and to our dongle,
 * the host's polling is the only limit worth having. */
#define REPORT_HZ 250
#define REPORT_HZ_WIRED 1000

static struct {
    u8 kind;
    u32 since;
    u8 slot;                     /* the session's slot, SLOT_NONE without one */
    u8 last_slot;
    const struct profile *prof;
    u8 usb;                      /* the session is on USB, not the radio */
    u8 radio;                    /* bt_start() until BT_EV_STOPPED */
    u8 waking;                   /* the profile's wake is running, radio off */
    u8 woke;                     /* this session has tried a wake */
    u8 wake_after_stop;          /* the session stops to wake the host, then restarts */
    u8 usb_wake;                 /* USB session: may wake a silent host by radio, until it
                                    first configures us */
    u8 usb_configured;           /* USB session: the host has configured us */
    u8 usb_arrived;              /* a USB host appeared, not yet acted on (ux_usb_autoconnect) */
    u32 usb_unready_ms;          /* CONNECTED on USB: when the host unconfigured us, 0: it has not */
    u32 wake_next;
    struct profile_session sess;
    struct set_slot sess_set;    /* sess.settings: the slot's, with its remap's mouse settings */
    u8 pair_pending;             /* pairing: the slot keeps its old address and bond until the host pairs */
    struct bt_bond pair_bond;    /* what the pairing has so far */
    u8 radio_pending;            /* the session's radio waits for the last one's BT_EV_STOPPED */
    u8 to_loader;
    u8 uboot_chord;
    u32 retry_at;
    u32 report_next;             /* the earliest the next report may be built */
    u16 report_hz;               /* the session's cap: the slot's setting or REPORT_HZ */
    u32 pull_at_us;              /* a pulled report waits until then (jit_us, pull_delay_us) */
    u8 imu_mode, begin_imu;
    u32 imu_seq, imu_next;
    u8 calibrating;
    u8 remap, remap_slot;        /* the remap in use (REMAP_BASE: the base), and whose it is */
    u8 remap_chosen;             /* picked in config mode: kept when remap_slot reconnects */
    u8 remap_live;               /* remap.c state belongs to remap_live - 1 at remap_edits */
    u32 remap_edits;
    u32 ux_buttons;              /* as ux_step left them, before the remap */
    u32 turbo_max;               /* keys flipped every report, down first */
    u8 turbo_up;
    struct imu_sample rest;      /* the newest motion sent outside config mode */
    struct set_cal cal;
    struct in_raw raw;
    struct pad pad;              /* as the host sees it */
    struct pad input;            /* calibrated, before Home gestures and the remap (padtool input) */
    struct pad activity_ref;
    u32 activity_ms;
    struct profile_host host;
    u8 rumble_logged[2];
    u8 rumble_test[2];           /* padtool rumble: these duties until rumble_test_until */
    u8 rumble_testing;
    u32 rumble_test_until;
    /* Config mode L1/R1: over the remap's rumble, as long as a remap picked
     * there (remap_chosen) and until the tools next change a setting. */
    u8 rumble_step, rumble_step_set;
    u32 rumble_step_edits;
    u8 rumble_raw;               /* the test duties are padtool's: no kick */
    u8 motor_on[2], kicking[2];  /* a motor started: full power until kick_until */
    u32 kick_until[2];
    struct led_state led;
    const struct led_scene *led_scene;   /* the one drawn: a change fades in */
    struct led_fine frame;       /* the scene as drawn */
    struct led_frame shown;      /* what the LEDs show */
    u16 dither_acc[LIGHT_COUNT][3];
    u8 dithering;                /* a dim light between steps: refresh at dither_next */
    u32 dither_next;
    u8 led_demo;                 /* the LED demo shown (ux_led_demos), SLOT_NONE: none */
    u32 led_demo_ms;             /* when it was last asked for */
    u8 reach[2];
    u32 led_next;
    u8 usb_raw, usb_powered;
    u8 usb_suspended;            /* as last logged */
    u32 usb_changed_ms, usb_plugged_ms, usb_absent_ms;
    u16 battery_mv;
    u8 low_battery;
    u32 battery_ms;
    struct ctl_stats stats;
    struct bt_stats bt_base;     /* the counters at the last reset */
    struct plat_stats plat_base;
    u32 last_report_us;
    u32 sent_buttons, sent_ux;   /* the last report handed over: its buttons, and before the remap */
} c;

static void stats_reset(u32 now)
{
    memset(&c.stats, 0, sizeof(c.stats));
    c.stats.since_ms = now;
    bt_stats(&c.bt_base);
    bt_timing(&c.stats.bt_timing, 1);
    plat_stats(&c.plat_base);
}

/* The running counters of bt.c and the platform, counted from the reset. */
static void counters_since(u32 *out, const u32 *now, const u32 *base, u32 n)
{
    while (n--) *out++ = *now++ - *base++;
}

static int session(void)
{
    return c.kind == CTL_CONNECTING || c.kind == CTL_PAIRING || c.kind == CTL_CONNECTED;
}

static void become(u8 kind, u32 now)
{
    if (kind == c.kind) return;
    util_log("state %s -> %s slot %u", names[c.kind], names[kind], c.slot);
    c.kind = kind;
    c.since = now;
}

/* The session's way to the host: the radio, or USB as the profile's device. */
static void radio_on(void)
{
    if (c.usb) {
        plat_usb_mode(c.prof->usb, c.prof->read);
        return;
    }
    /* A dongle slot pairs at the rendezvous address (link.h). */
    bt_start(c.prof, c.prof == &profile_link && c.sess.pairing ? link_pair_address : c.sess.address,
             c.sess.pairing ? NULL : c.sess.bond, c.sess.pairing, &c.sess.settings->link);
    c.radio = 1;
}

static void end_session(void)
{
    if (c.prof && c.prof->end) c.prof->end();
    if (c.waking) c.prof->wake->stop();
    c.waking = 0;
    c.wake_after_stop = 0;
    c.radio_pending = 0;
    if (c.radio) bt_stop();
    if (c.usb) plat_usb_mode(NULL, NULL);
    c.usb = 0;
    c.usb_wake = 0;
    c.usb_configured = 0;
    c.usb_unready_ms = 0;
}

int host_send(u16 id, const u8 *data, u16 len)
{
    if (c.usb) return plat_usb_send_message(id, data, len);
    return bt_send_message(id, data, len);
}

static int input_wanted(void)
{
    return c.usb ? plat_usb_input_wanted() : bt_input_wanted();
}

/* Which way a session goes: bluetooth+usb and dongle+usb are USB while a USB
 * host is there (frames on the bus, not just power) and the type has a USB
 * device. */
static int wants_usb(const struct set_slot *set, const struct profile *p)
{
    if (!p->usb) return 0;
    return set->transport == TRANSPORT_USB || (set_usb_first(set->transport) && c.usb_powered);
}

/* The slot's bond as this session knows it. */
static const struct bt_bond *session_bond(void)
{
    return c.pair_pending ? &c.pair_bond : set_bond(c.slot);
}

/* A pairing keeps what it learns to itself until the host has paired; then
 * the session's address and the bond replace the slot's. */
static void bond_update(const struct bt_bond *b, int paired)
{
    if (c.slot >= SLOTS) return;
    if (!c.pair_pending) {
        set_bond_store(c.slot, b);
        return;
    }
    c.pair_bond = *b;
    if (!paired) return;
    c.pair_pending = 0;
    set_pairing_store(c.slot, c.sess.address, b);
    c.sess.bond = set_bond(c.slot);
    util_log("slot %u paired: address and bond stored", c.slot);
}

/* The radio is down: the session is over. */
static void stopped(u32 now)
{
    c.radio = 0;
    if (c.radio_pending) {
        c.radio_pending = 0;
        radio_on();
    } else if (c.kind == CTL_CONNECTING && c.wake_after_stop) {
        c.wake_after_stop = 0;
        c.waking = !c.prof->wake->start(&c.sess);
        if (!c.waking) radio_on();
    } else if (c.kind == CTL_STOPPING) {
        c.slot = SLOT_NONE;
        c.prof = NULL;
        become(CTL_IDLE, now);
    }
}

static void stop(u32 now);

static const char *remap_name(u8 remap)
{
    static const char *const names[SLOTS] = { "a", "b", "x", "y", "up", "down", "left", "right" };
    return remap < SLOTS ? names[remap] : "base";
}

/* The remap in use: a remap slot or the base (REMAP_BASE). */
static const struct set_remap *remap_settings(void)
{
    return set_remap(c.remap);
}

static u8 rumble_step(void)
{
    return c.rumble_step_set && c.rumble_step_edits == set_edits() ? c.rumble_step : remap_settings()->rumble;
}

/* Host rumble spreads over the motor's range for the step: 1 is its
 * rumble_min, 255 its rumble_max. */
static u8 motor_duty(u8 host, int right)
{
    const struct set_remap *m = remap_settings();
    u32 step = rumble_step(), lo, hi;

    if (!host || !step) return 0;
    lo = 255u * m->rumble_min[right][step - 1] / 100;
    hi = 255u * m->rumble_max[right][step - 1] / 100;
    if (hi < lo) hi = lo;
    return (u8)(lo + (hi - lo) * (host - 1u) / 254);
}

static void start_session(struct ctl_request r, u32 now)
{
    const struct set_slot *set = set_slot(r.slot);
    const struct profile *p = profile_for(set->type);
    struct profile_session s;
    int pairing = r.kind == REQ_PAIR;
    int usb;

    if (!p) {
        util_log("slot %u: profile type %u not in this build", r.slot, set->type);
        if (session()) stop(now);
        return;
    }
    if (set->transport == TRANSPORT_USB && !p->usb) {
        util_log("slot %u: %s has no USB device", r.slot, p->name);
        if (session()) stop(now);
        return;
    }
    if (session()) end_session();
    /* An explicit pairing uses the radio (or the dongle) even with a USB host
     * present. */
    usb = wants_usb(set, p) && !(pairing && set_usb_first(set->transport));
    if (usb) pairing = 0;        /* a cable needs none; keep the radio's bond */
    else if (set_via_dongle(set->transport)) p = &profile_link;
    memset(&s, 0, sizeof(s));
    s.slot = r.slot;
    s.pairing = (u8)pairing;
    if (pairing) set_pairing_address(r.slot, r.generate_new_address, s.address);
    else set_slot_address(r.slot, s.address);
    memset(&c.pair_bond, 0, sizeof(c.pair_bond));
    c.pair_pending = (u8)pairing;
    s.bond = pairing ? &c.pair_bond : set_bond(r.slot);
    s.usb = (u8)usb;
    c.usb = (u8)usb;
    c.prof = p;
    c.sent_buttons = c.sent_ux = 0;  /* no report from this session has been handed over yet */
    c.slot = r.slot;
    c.last_slot = r.slot;
    /* A remap picked in config mode lasts across this slot's reconnects (a
     * lost link, a cable, relink) until a disconnect or sleep. */
    if (c.remap_slot != r.slot) c.rumble_step_set = 0;
    if (!c.remap_chosen || c.remap_slot != r.slot) {
        c.remap = set->remap ? (u8)(set->remap - 1) : REMAP_BASE;
        c.remap_slot = r.slot;
        c.remap_chosen = 0;
    }
    {
        const struct set_remap *m = remap_settings();

        c.sess_set = *set;
        c.sess_set.variant = m->mouse_layout;
        c.sess_set.stick_speed = m->stick_speed;
        c.sess_set.gyro_speed = m->gyro_speed;
        c.sess_set.gyro_accel = m->gyro_accel;
        s.settings = &c.sess_set;
    }
    plat_retain_slot(r.slot);
    memset(&c.host, 0, sizeof(c.host));
    c.report_hz = s.settings->report_hz ? s.settings->report_hz :
                  usb || p == &profile_link ? REPORT_HZ_WIRED : REPORT_HZ;
    c.begin_imu = p->begin(&s);
    c.sess = s;
    c.woke = 0;
    c.retry_at = 0;
    c.usb_arrived = 0;
    /* A USB session from Home or the console command may wake its host the
     * radio's way with the slot's bond (Switch 2: its wake advertising) while
     * no frames come, until the host first configures us. */
    c.usb_wake = usb && !r.quiet && set->transport == TRANSPORT_USB && p->device && p->device->transport == BT_LE &&
                 s.bond && (s.bond->le_peer[0] | s.bond->le_peer[1] | s.bond->le_peer[2] |
                            s.bond->le_peer[3] | s.bond->le_peer[4] | s.bond->le_peer[5]);
    if (c.radio && !usb) c.radio_pending = 1;
    else radio_on();
    util_log("session slot %u %s%s%s as %02x:%02x:%02x:%02x:%02x:%02x remap %s", r.slot, p->name,
             usb ? " usb" : "", pairing ? " pairing" : "", s.address[5], s.address[4], s.address[3],
             s.address[2], s.address[1], s.address[0], remap_name(c.remap));
    become(pairing ? CTL_PAIRING : CTL_CONNECTING, now);
    c.since = now;
}

/* Take the session down, to idle. */
static void stop(u32 now)
{
    if (c.kind != CTL_STOPPING) {
        end_session();
        become(CTL_STOPPING, now);
        if (!c.radio) stopped(now);
    }
}

/* Sleep or reboot: the radio goes down first (entering the loader with the
 * controller still running hung the pad off USB). */
static void shut_down(u8 kind, u32 now)
{
    c.remap_chosen = c.rumble_step_set = 0;
    if (c.radio || c.waking) end_session();
    become(kind, now);
}

static void handle(struct ctl_request r, u32 now)
{
    if (r.kind == REQ_NONE || c.kind == CTL_SLEEPING || c.kind == CTL_REBOOTING) return;
    if (r.slot == SLOT_LAST) r.slot = c.last_slot;
    switch (r.kind) {
    case REQ_CONNECT:
    case REQ_PAIR:
        if (r.slot >= SLOTS) return;
        if (r.kind == REQ_CONNECT && c.kind == CTL_CONNECTED && c.slot == r.slot) return;
        start_session(r, now);
        break;
    case REQ_DISCONNECT:
        c.remap_chosen = c.rumble_step_set = 0;
        if (session()) stop(now);
        break;
    case REQ_LED_DEMO:
        if (r.slot != c.led_demo) util_log("led demo %d", r.slot == SLOT_NONE ? -1 : (int)r.slot);
        c.led_demo = r.slot;
        c.led_demo_ms = now;
        c.led.scene = CTL_KIND_COUNT;    /* fade into it, from the start */
        c.led_next = now;
        break;
    case REQ_RUMBLE:
        c.rumble_test[0] = r.motor[0];
        c.rumble_test[1] = r.motor[1];
        c.rumble_raw = 1;
        c.rumble_testing = r.ms != 0;
        c.rumble_test_until = now + (r.ms < RUMBLE_TEST_MAX_MS ? r.ms : RUMBLE_TEST_MAX_MS);
        break;
    case REQ_RUMBLE_STEP: {
        u8 step = rumble_step();

        if (r.slot && step < RUMBLE_STEPS) step++;
        if (!r.slot && step) step--;
        c.rumble_step = step;
        c.rumble_step_set = 1;
        c.rumble_step_edits = set_edits();
        util_log("rumble step %u", step);
        c.rumble_test[0] = motor_duty(255, 0);
        c.rumble_test[1] = motor_duty(255, 1);
        c.rumble_raw = 0;
        c.rumble_testing = step != 0;
        c.rumble_test_until = now + ux_timeouts.rumble_feedback_ms;
        break;
    }
    case REQ_REMAP:
        if (c.kind != CTL_CONNECTED) return;
        c.remap = r.slot < SLOTS ? r.slot : REMAP_BASE;
        c.remap_chosen = 1;
        util_log("slot %u remap %s", c.slot, remap_name(c.remap));
        break;
    case REQ_SLEEP:
        if (!c.usb_powered) shut_down(CTL_SLEEPING, now);
        else if (session()) stop(now);
        break;
    case REQ_CALIBRATE:
        c.calibrating = 1;
        c.cal = *set_cal();
        cal_start(now);
        break;
    case REQ_STATS_RESET:
        stats_reset(now);
        break;
    case REQ_RELINK:
        /* Only while connected: the host is awake, so reconnecting wakes
         * nothing.  The new session reads the slot's settings again. */
        if (c.kind == CTL_CONNECTED) {
            struct ctl_request again = { REQ_CONNECT, c.slot, 0, 1 };

            util_log("relink slot %u", c.slot);
            start_session(again, now);
        }
        break;
    case REQ_REBOOT:
    case REQ_REBOOT_LOADER:
        c.to_loader = r.kind == REQ_REBOOT_LOADER;
        set_flush();
        shut_down(CTL_REBOOTING, now);
        break;
    }
}

static void bt_event(const struct bt_ev *e, u32 now)
{
    struct bt_bond bond;

    /* The last session's radio, still stopping. */
    if (c.radio_pending && e->kind != BT_EV_STOPPED) return;
    /* A USB session runs the radio only to wake its host: no link of its own. */
    if (c.usb && e->kind != BT_EV_STOPPED) {
        if (e->kind == BT_EV_LINKED) util_log("usb: the host linked by radio as well, ignored");
        return;
    }
    switch (e->kind) {
    case BT_EV_LINKED:
        if (c.kind == CTL_CONNECTING || c.kind == CTL_PAIRING) {
            become(CTL_CONNECTED, now);
            c.report_next = now;
            c.activity_ms = now;
        }
        break;
    case BT_EV_UNLINKED: {
        int lost = e->lost;
        /* A host command and the disconnect can arrive in the same event
         * batch, before outputs() has polled the profile's updated state. */
        if (c.kind == CTL_CONNECTED && c.prof && c.prof->host) {
            c.prof->host(now, &c.host);
            if (c.host.ended) lost = 0;
        }
        util_log("link down, reason 0x%02x (%s)", e->reason, lost ? "lost" : "ended");
        if (c.kind != CTL_CONNECTED) break;
        memset(&c.host, 0, sizeof(c.host));
        if (lost) {
            become(CTL_CONNECTING, now);
            bt_connect();
        } else {
            stop(now);
        }
        break;
    }
    case BT_EV_FAILED:
        if (c.kind != CTL_CONNECTING) break;
        /* The host did not answer: it may be asleep.  A profile that can
         * wake it gets the radio to itself once, then the session restarts. */
        if (c.prof->wake && !c.woke) {
            c.woke = 1;
            c.wake_after_stop = 1;
            bt_stop();
        } else {
            c.retry_at = now + RETRY_MS;
        }
        break;
    case BT_EV_PEER:
    case BT_EV_BONDED:
    case BT_EV_PAIRED:
        bond_update(&e->bond, e->kind == BT_EV_PAIRED);
        break;
    case BT_EV_WRITE:
        /* A pairing the profile ran itself (Switch 2): the key joins the
         * slot's bond, and the stack needs it at once. */
        if (c.prof && c.prof->write && c.prof->write(&e->write, now, &bond) && c.slot < SLOTS) {
            struct bt_bond b = *session_bond();

            memcpy(b.ltk, bond.ltk, 16);
            bond_update(&b, 1);
            bt_update_bond(&b);
        }
        if (c.kind == CTL_CONNECTED && c.prof && c.prof->host) {
            c.prof->host(now, &c.host);
            if (c.host.ended) {
                util_log("host ended session: stopping radio");
                stop(now);
            }
        }
        break;
    case BT_EV_STOPPED:
        stopped(now);
        break;
    }
}

/* A USB session: the host's writes, and whether it has the device. */
static void usb_session(u32 now)
{
    struct bt_write w;
    struct bt_bond bond;
    int wrote = 0;

    if (!c.usb || !session()) return;
#ifdef USB_SESSION_TEST_MS       /* a test build: the tool device comes back by itself */
    if (now - c.since >= USB_SESSION_TEST_MS && c.kind == CTL_CONNECTED) {
        util_log("usb: test session over");
        stop(now);
        return;
    }
#endif
    while (plat_usb_take(&w)) {
        if (w.kind == BT_WRITE_FEATURE && w.id == USB_TOOL_REPORT && w.len >= 4 &&
            !memcmp(w.data, "tool", 4)) {
            util_log("usb: back to the tool device");
            stop(now);
            return;
        }
        /* A pairing over the cable (Switch 2): the slot's bond, for the radio */
        if (c.prof->write && c.prof->write(&w, now, &bond) && c.slot < SLOTS) {
            struct bt_bond b = *set_bond(c.slot);

            /* The console's first address from 15/01 is its radio's. */
            memcpy(b.ltk, bond.ltk, 16);
            if (bond.le_peer[0] | bond.le_peer[1] | bond.le_peer[2] | bond.le_peer[3] |
                bond.le_peer[4] | bond.le_peer[5])
                memcpy(b.le_peer, bond.le_peer, 6);
            if (memcmp(&b, set_bond(c.slot), sizeof(b))) {
                util_log("usb: slot %u bond stored", c.slot);
                set_bond_store(c.slot, &b);
            }
        }
        wrote = 1;
    }
    if (wrote && c.prof->host) {
        c.prof->host(now, &c.host);
        if (c.host.ended) {
            util_log("host ended session: stopping");
            stop(now);
            return;
        }
    }
    if (c.kind == CTL_CONNECTING && c.usb_wake && !c.usb_powered && !c.radio && !c.woke) {
        util_log("usb: no host frames, waking it by radio");
        c.woke = 1;
        bt_start(c.prof, c.sess.address, c.sess.bond, 0, &c.sess.settings->link);
        c.radio = 1;
    }
    if (c.kind == CTL_CONNECTING && plat_usb_ready()) {
        c.usb_absent_ms = now;   /* configured: here, before its frames are counted */
        c.usb_wake = 0;
        c.usb_configured = 1;
        if (c.radio) {
            util_log("usb: host here, radio off");
            bt_stop();
        }
        become(CTL_CONNECTED, now);
        c.report_next = now;
        c.activity_ms = now;
    } else if (c.kind == CTL_CONNECTED && !plat_usb_ready()) {
        /* The Switch 2 configures us again as it goes to sleep: a short
         * gap is no reconnect (reports wait for it anyway). */
        if (!c.usb_unready_ms) c.usb_unready_ms = now | 1;
        else if (now - c.usb_unready_ms >= USB_REARM_MS) {
            c.usb_unready_ms = 0;
            memset(&c.host, 0, sizeof(c.host));
            become(CTL_CONNECTING, now);
        }
    } else if (plat_usb_ready()) {
        c.usb_unready_ms = 0;
    }
}

/* A USB session (usb, or bluetooth+usb or dongle+usb that found a host) ends
 * with its host: a bus suspend once it configured us (asleep; or the cable
 * went, 3 ms after the frames stop), or no frames for USB_GONE_MS.  Nothing
 * moves to the radio; the next connect chooses again.  A bluetooth+usb or
 * dongle+usb session on the radio moves to USB once a USB host is there,
 * unless it began as pairing. */
static void follow_usb(u32 now)
{
    const struct set_slot *set;

    if ((c.kind != CTL_CONNECTING && c.kind != CTL_CONNECTED) || c.slot >= SLOTS || c.waking) return;
    set = set_slot(c.slot);
    if (!c.usb) {
        struct ctl_request again = { REQ_CONNECT, c.slot, 0, 1 };

        if (c.sess.pairing || !set_usb_first(set->transport) || !wants_usb(set, profile_for(set->type))) return;
        util_log("slot %u moves to usb", c.slot);
        start_session(again, now);
        return;
    }
    if (c.radio) return;
    if (c.usb_configured && plat_usb_suspended()) {
        util_log("slot %u: usb host suspended the bus", c.slot);
        stop(now);
    } else if (!c.usb_powered && !c.usb_wake && now - c.usb_absent_ms >= USB_GONE_MS) {
        /* bluetooth+usb or dongle+usb whose host went before configuring us:
         * the radio instead, as the connect would have chosen. */
        struct ctl_request again = { REQ_CONNECT, c.slot, 0 };

        if (!c.usb_configured && set_usb_first(set->transport)) {
            util_log("slot %u: usb host gone before configuring, %s instead", c.slot,
                     set_via_dongle(set->transport) ? "the dongle" : "bluetooth");
            start_session(again, now);
        } else {
            util_log("slot %u: usb cable gone", c.slot);
            stop(now);
        }
    }
}

static u32 idle_deadline(void)
{
    /* Offline calibration owns its timeout; the 15 s idle sleep must not
     * interrupt the rest, sweep or result light. */
    if (c.calibrating) return 0;
#ifdef STAY_ON_TEST              /* a test build: the log in RAM outlives a console test */
    return 0;
#endif
    return ux_idle_deadline(c.since, c.activity_ms, c.usb_absent_ms,
                            c.usb_powered, set_is_ephemeral());
}

static void timeouts(u32 now)
{
    struct ctl_request loader = { REQ_REBOOT_LOADER, SLOT_NONE, 0 };
    u32 t = now - c.since;
    int battery = !c.usb_powered;

    switch (c.kind) {
    case CTL_BOOT:
        if (c.uboot_chord && c.usb_powered) handle(loader, now);
        else if (t >= (c.uboot_chord ? ux_timeouts.uboot_window_ms : ux_timeouts.boot_scene_ms))
            become(CTL_IDLE, now);
        break;
    case CTL_IDLE: {
        u32 at = idle_deadline();

        if (ux_usb_autoconnect && c.usb_arrived && c.usb_powered && c.last_slot < SLOTS) {
            const struct set_slot *set = set_slot(c.last_slot);
            struct ctl_request plug = { REQ_CONNECT, c.last_slot, 0, 1 };

            c.usb_arrived = 0;
            if (set->transport == TRANSPORT_USB || set_usb_first(set->transport)) {
                util_log("usb host arrived: connect slot %u", c.last_slot);
                handle(plug, now);
                break;
            }
        }
        if (at && (s32)(now - at) >= 0) shut_down(CTL_SLEEPING, now);
        break;
    }
    case CTL_CONNECTING:
        if (c.radio_pending) {
            if (t >= ux_timeouts.stop_max_ms) bt_abort();
        } else if (t >= ux_timeouts.connect_ms) stop(now);
        else if (c.waking) {
            if (c.prof->wake->step(now, &c.wake_next)) {
                c.waking = 0;
                radio_on();
            }
        } else if (c.retry_at && (s32)(now - c.retry_at) >= 0) {
            c.retry_at = 0;
            bt_connect();
        }
        break;
    case CTL_PAIRING:
        if (c.radio_pending) {
            if (t >= ux_timeouts.stop_max_ms) bt_abort();
        } else if (t >= ux_timeouts.pair_ms) stop(now);
        break;
    case CTL_CONNECTED:
        if (battery && !c.calibrating && now - c.activity_ms >= ux_timeouts.idle_sleep_ms) shut_down(CTL_SLEEPING, now);
        break;
    case CTL_STOPPING:
        if (t >= ux_timeouts.stop_max_ms) bt_abort();
        break;
    case CTL_SLEEPING:
        if (c.usb_powered && !c.radio) become(CTL_IDLE, now);
        else if (!c.radio && t >= 50) {
            set_flush();
            util_log("power off");
            plat_power_off();
        } else if (t >= ux_timeouts.stop_max_ms) bt_abort();
        break;
    case CTL_REBOOTING:          /* to the loader: after its LED launch, whose last frame stays */
        if ((!c.radio && t >= (c.to_loader ? LED_LOADER_MS : 100)) || t >= ux_timeouts.stop_max_ms) {
            if (c.to_loader) plat_reboot_loader();
            plat_reboot();
        }
        break;
    }
}

static u32 state_deadline(void)
{
    switch (c.kind) {
    case CTL_BOOT: return c.since + (c.uboot_chord ? ux_timeouts.uboot_window_ms : ux_timeouts.boot_scene_ms);
    case CTL_IDLE: return idle_deadline();
    case CTL_CONNECTING: if (c.radio_pending) return c.since + ux_timeouts.stop_max_ms;
        if (c.waking) return c.wake_next;
        if (c.usb && !c.usb_powered && !c.radio) return c.usb_absent_ms + USB_GONE_MS;
        return c.retry_at ? c.retry_at : c.since + ux_timeouts.connect_ms;
    case CTL_PAIRING: return c.since + (c.radio_pending ? ux_timeouts.stop_max_ms : ux_timeouts.pair_ms);
    case CTL_CONNECTED:          /* bt.c wakes the loop (EV_BT) when it wants a report; one
                                    waiting for its time (pull_at_us) wakes it through plat_wait */
        return input_wanted() && !c.pull_at_us ? c.report_next : plat_ms() + 100;
    default: return plat_ms() + 10;
    }
}

static void sample_power(u32 now)
{
    struct ctl_request loader = { REQ_REBOOT_LOADER, SLOT_NONE, 0 };
    u8 raw = (u8)plat_usb_present();

    if (raw != c.usb_raw) {
        c.usb_raw = raw;
        c.usb_changed_ms = now;
    }
    if (raw != c.usb_powered && now - c.usb_changed_ms >= USB_DEBOUNCE_MS) {
        c.usb_powered = raw;
        util_log("usb %s", raw ? "present" : "gone");
        if (raw) {
            c.usb_plugged_ms = now;
            c.usb_arrived = 1;
            if ((c.raw.buttons & (PAD_HOME | PAD_START)) == (PAD_HOME | PAD_START)) handle(loader, now);
        } else {
            c.usb_arrived = 0;
        }
    }
    if (c.usb_powered) c.usb_absent_ms = now;
    if ((u8)plat_usb_suspended() != c.usb_suspended) {
        c.usb_suspended = !c.usb_suspended;
        util_log("usb: bus %s", c.usb_suspended ? "suspended" : "awake");
    }
    if (now - c.battery_ms >= BATTERY_CHECK_MS) {
        c.battery_ms = now;
        c.battery_mv = plat_battery_mv();
        if (c.usb_powered) c.low_battery = 0;
        else if (c.battery_mv < LOW_BATTERY_MV) c.low_battery = 1;
        else if (c.battery_mv >= BATTERY_OK_MV) c.low_battery = 0;
    }
}

static struct rgb rgb3(const u8 *v)
{
    struct rgb r = { v[0], v[1], v[2] };
    return r;
}

static struct shade shade(u8 entry, u16 brightness)
{
    struct shade s = { set_colour(entry), brightness };
    return s;
}

/* The palette's dither_ms, or a demo's. */
static u8 dither_ms(void)
{
    u8 ms = set_brightness()->dither_ms;

    if (c.led_demo == SLOT_NONE) return ms;
    if (ux_led_demos[c.led_demo].dither == LED_DEMO_DITHER_OFF) return 0;
    if (ux_led_demos[c.led_demo].dither == LED_DEMO_DITHER_ON && !ms) return 2;
    return ms;
}

/* The LEDs from the frame drawn: again between frames while dithering. */
static void show(u32 now)
{
    u8 ms = dither_ms();
    struct led_frame f;

    c.dithering = (u8)led_dither(&c.frame, c.dither_acc, ms != 0, &f);
    if (c.dithering) c.dither_next = now + ms;
    if (memcmp(&f, &c.shown, sizeof(f))) {
        c.shown = f;
        plat_leds(&f);
    }
}


/* A motor starting from stopped: full power for the remap's rumble_kick_ms. */
static void kick(u8 motor[2], u32 now, int allowed)
{
    u32 step = rumble_step(), ms = step ? remap_settings()->rumble_kick_ms[step - 1] : 0, i;

    for (i = 0; i < 2; i++) {
        if (motor[i] && !c.motor_on[i] && allowed && ms) {
            c.kicking[i] = 1;
            c.kick_until[i] = now + ms;
        }
        if (!motor[i] || (s32)(now - c.kick_until[i]) >= 0) c.kicking[i] = 0;
        c.motor_on[i] = motor[i] != 0;
        if (c.kicking[i]) motor[i] = 255;
    }
}

static void outputs(u32 now)
{
    const struct set_slot *slot, *shown;
    const struct set_brightness *b = set_brightness();
    const struct ux_led_demo *demo = NULL;
    const struct led_scene *sc;
    struct led_inputs in;
    u32 plugged = now - c.usb_plugged_ms;
    u8 scene = c.kind, armed = ux_armed();
    int rumble = c.kind == CTL_CONNECTED && !c.calibrating;
    u8 motor[2];

    if (c.prof && c.prof->host) c.prof->host(now, &c.host);
    if (!c.host.rumble[0] != !c.rumble_logged[0] || !c.host.rumble[1] != !c.rumble_logged[1]) {
        util_log("host rumble %u %u", c.host.rumble[0], c.host.rumble[1]);
        c.rumble_logged[0] = c.host.rumble[0];
        c.rumble_logged[1] = c.host.rumble[1];
    }
    c.imu_mode = !session() ? IMU_OFF : c.prof && c.prof->host && c.host.imu ? c.host.imu : c.begin_imu;
    c.imu_next = imu_poll(now, c.imu_mode);
    slot = set_slot(c.slot < SLOTS ? c.slot : 0);
    motor[0] = rumble ? motor_duty(c.host.rumble[0], 0) : 0;
    motor[1] = rumble ? motor_duty(c.host.rumble[1], 1) : 0;
    if (c.rumble_testing && (s32)(now - c.rumble_test_until) >= 0) c.rumble_testing = 0;
    if (c.rumble_testing) {
        motor[0] = c.rumble_test[0];
        motor[1] = c.rumble_test[1];
    }
    kick(motor, now, !(c.rumble_testing && c.rumble_raw));
    plat_motors(motor[0], motor[1]);
    if ((s32)(now - c.led_next) < 0) {
        if (c.dithering && (s32)(now - c.dither_next) >= 0) show(now);
        return;
    }
    c.led_next = now + ux_timeouts.led_frame_ms;
    if (c.kind == CTL_BOOT && now - c.since >= ux_timeouts.boot_scene_ms) scene = CTL_IDLE;
    if (armed != SLOT_NONE && c.kind != CTL_CONNECTED && !c.calibrating) scene = CTL_CONNECTING;
    else armed = SLOT_NONE;
    if (c.led_demo != SLOT_NONE && now - c.led_demo_ms >= UX_LED_DEMO_MS) {
        util_log("led demo over");
        c.led_demo = SLOT_NONE;
        c.led.scene = CTL_KIND_COUNT;
    }
    if (c.led_demo != SLOT_NONE) {   /* slot A's colours, the demo's settings */
        demo = &ux_led_demos[c.led_demo];
        scene = demo->scene;
        armed = 0;
    }
    sc = demo && demo->kind == LED_DEMO_CONFIG ? &ux_config_scene :
         demo && demo->kind == LED_DEMO_LOADER ? &ux_loader_scene :
         demo ? &ux_scenes[scene] :
         c.kind == CTL_CONNECTED && ux_config(NULL) ? &ux_config_scene :
         c.kind == CTL_REBOOTING && c.to_loader ? &ux_loader_scene : &ux_scenes[scene];
    if (scene != c.led.scene || sc != c.led_scene ||
        (demo && demo->kind == LED_DEMO_LOADER && now - c.led.since_ms >= LED_LOADER_MS + 1500)) {
        c.led.scene = scene;
        c.led_scene = sc;
        c.led.since_ms = now;
        c.led.from = c.frame;    /* the new scene fades in over it */
    }
    c.led.slot = armed != SLOT_NONE ? armed : c.slot;
    if (c.led.slot == SLOT_LAST) c.led.slot = c.last_slot;
    memset(&in, 0, sizeof(in));
    shown = set_slot(c.led.slot < SLOTS ? c.led.slot : 0);
    in.connect_colour = shade(shown->connect_colour, b->connect);
    in.primary_colour = shade(shown->primary_colour, demo && demo->steady ? demo->steady : b->steady);
    in.secondary_colour = shade(shown->secondary_colour, demo && demo->steady ? demo->steady : b->steady);
    in.idle_colour = shade(ux_colours.idle, ux_colours.idle_brightness);
    in.battery_colour = shade(ux_colours.battery, ux_colours.battery_brightness);
    in.animation = demo && demo->animation ? demo->animation : b->animation;
    in.grips = demo ? demo->grips : slot->grips;
    in.player_lights = demo ? demo->players : slot->players;
    in.ephemeral = (u8)set_is_ephemeral();
    led_reach(c.reach, &c.pad);
    in.reach[0] = c.reach[0];
    in.reach[1] = c.reach[1];
    in.rumble[0] = motor[0];
    in.rumble[1] = motor[1];
    in.usb_powered = c.usb_powered;
    in.low_battery = c.low_battery;
    in.usb_plug_lit = c.usb_powered && plugged < ux_timeouts.usb_plug_ms;
    if (c.kind == CTL_CONNECTED) {
        in.has_players = c.host.has_players;
        in.players = c.host.players;
        in.players_flash = c.host.players_flash;
        in.has_lightbar = c.host.has_lightbar;
        in.lightbar = rgb3(c.host.lightbar);
    }
    if (demo) {                  /* the console's player 1 */
        in.has_players = 1;
        in.players = 1;
        in.players_flash = 0;
    }
    in.cal_lit = (u8)cal_led(now, &in.cal);
    led_frame(sc, &c.led, now, &in, &c.frame);
    show(now);
}

/* The remap whose curves and deadzones the sticks take: connected only;
 * NULL, linear and none. */
static const struct set_remap *sticks(void)
{
    return c.kind == CTL_CONNECTED ? set_remap(c.remap) : NULL;
}

/* Connected, outside config mode and calibration: the remap slot in use.
 * Its state starts over with another remap slot or new settings. */
static void remap(u32 now)
{
    u32 edits = set_edits();

    c.ux_buttons = c.pad.buttons;
    c.turbo_max = 0;
    if (c.kind != CTL_CONNECTED || c.calibrating || ux_config(NULL)) {
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

static void read_input(u32 now)
{
    struct ctl_request none = { REQ_NONE, SLOT_NONE, 0 };
    struct ctl_request r = none;

    /* Do not start a deferred Home tap's minimum down time while its report is
     * still waiting in the Bluetooth slot. Slow host polls must not compress
     * down and up into the same send opportunity. */
    if (c.kind == CTL_CONNECTED && (c.sent_ux & PAD_HOME) &&
        (c.usb ? plat_usb_input_wanted() : !bt_input_pending()))
        ux_reported(now);
    plat_input(&c.raw);
    in_map(&c.raw, &c.pad);
    cal_apply(set_cal(), sticks() ? sticks()->deadzone : NULL, &c.raw, &c.pad);
    c.input = c.pad;
    if (in_moved(&c.pad, &c.activity_ref)) {
        c.activity_ref = c.pad;
        c.activity_ms = now;
    }
    if (c.calibrating) {
        int s = cal_step(&c.raw, now, &c.cal);

        if (s == CAL_SAVE) set_cal_store(&c.cal);
        if (s == CAL_IDLE) {
            c.calibrating = 0;
            c.activity_ms = now;
            if (c.kind == CTL_IDLE) c.since = now;
            ux_ignore_held();
        }
        c.pad.buttons = 0;
    } else {
        r = ux_step(&c.pad, now, c.kind);
    }
    handle(r, now);
    remap(now);
    in_response(sticks(), &c.pad);
}

/* Remap config mode: the host sees the pad lying still where it last was,
 * the gyro at its zero-rate offset (profiles repeat their last sample when a
 * report has none, so leaving samples out would keep a turn going). */
static void at_rest(struct pad *p)
{
    const s32 *bias = p->gyro_bias;
    u32 i, k;

    for (k = 0; k < 3; k++) c.rest.gyro[k] = (s16)((bias[k] + (bias[k] < 0 ? -8 : 8)) / 16);
    for (i = 0; i < p->imu_count; i++) {
        u32 us = p->imu[i].us;

        p->imu[i] = c.rest;
        p->imu[i].us = us;
    }
}

/* Pull: a report is built when bt.c has room for it, from the newest input,
 * and at most report_hz times a second.  A report built is a report sent, so
 * the IMU samples it takes are never dropped. */
static void send_report(u32 now)
{
    u8 buf[80], id = 0;
    u32 period = 1000 / c.report_hz, buttons = c.pad.buttons;
    int n;

    c.pull_at_us = 0;
    if ((s32)(now - c.report_next) < 0 || !input_wanted()) return;
    if (!c.usb) {
        u32 at = c.sess.settings->pull_delay_us ? bt_input_wanted_us() + c.sess.settings->pull_delay_us
                                                : bt_input_due_us();

        if ((s32)(plat_us() - at) < 0) {
            c.pull_at_us = at;
            return;
        }
    }
    c.report_next = now + period;
    c.pad.imu_count = (u8)imu_take(&c.imu_seq, c.pad.imu, IMU_PER_REPORT);
    c.pad.gyro_bias_known = (u8)imu_gyro_bias(c.pad.gyro_bias);
    if (ux_config(NULL)) at_rest(&c.pad);
    else if (c.pad.imu_count) c.rest = c.pad.imu[c.pad.imu_count - 1];
    c.pad.jack = (u8)plat_usb_jack_in();
    if (!c.turbo_up) c.pad.buttons = in_dpad(buttons | c.turbo_max);
    n = c.prof->report(&c.pad, now, buf, &id);
    c.pad.buttons = buttons;     /* a profile may change them while building */
    util_trace(TR_BUILT, (u16)n);
    if (n > 0 && (c.usb ? plat_usb_send_input(id, buf, (u16)n) : bt_send_input(id, buf, (u16)n))) {
        u32 us = plat_us();

        if (c.stats.reports) util_stat_add(&c.stats.interval, us - c.last_report_us);
        if (buttons != c.sent_buttons)
            util_stat_add(&c.stats.age, us - c.raw.changed_us);
        c.last_report_us = us;
        c.sent_buttons = buttons;
        c.sent_ux = c.ux_buttons;
        c.turbo_up = c.turbo_max && !c.turbo_up;
        c.stats.reports++;
        c.stats.imu_samples += c.pad.imu_count;
    }
}

static u32 earliest(u32 a, u32 b)
{
    if (!a) return b;
    if (!b) return a;
    return (s32)(a - b) < 0 ? a : b;
}

static void boot(u32 now)
{
    struct plat_boot b;

    plat_boot_info(&b);
    set_init();
    now = plat_ms();            /* show a full boot interval after settings have loaded */
    c.kind = CTL_BOOT;
    c.since = now;
    c.slot = SLOT_NONE;
    c.last_slot = plat_retained_slot();
    c.led.scene = CTL_BOOT;
    c.led.slot = SLOT_NONE;
    c.led_demo = SLOT_NONE;
    c.remap = REMAP_BASE;
    c.usb_raw = c.usb_powered = (u8)plat_usb_present();
    c.usb_plugged_ms = c.usb_powered ? now : now - ux_timeouts.usb_plug_ms;
    c.usb_arrived = c.usb_powered;  /* plugged in at power-up (or the plug-in woke us) */
    c.battery_ms = now - BATTERY_CHECK_MS;
    util_log("boot cause %u held %05x last slot %u", b.cause, b.buttons_held, c.last_slot);
    c.uboot_chord = (b.buttons_held & (PAD_HOME | PAD_START)) == (PAD_HOME | PAD_START);
    plat_input(&c.raw);
    /* Show the boot scene before a profile or the radio starts: an
     * immediate session must not skip it by replacing CTL_BOOT. */
    outputs(now);
    if (c.uboot_chord) ux_ignore_held();
    else handle(ux_boot(b.buttons_held, b.cause == BOOT_HOME, now), now);
}

void ctl_main(void)
{
    struct usb_status st;
    struct bt_ev e;
    u32 now = plat_ms(), deadline, start, t;

    boot(now);
    stats_reset(now);
/* Time since the last mark: that step's worst and its total. */
#define STEP(i) do { u32 u = plat_us(); if (u - t > c.stats.step_max[i]) c.stats.step_max[i] = u - t; \
                         c.stats.step_sum[i] += u - t; t = u; } while (0)
/* bt.c may ask for a report while this pass runs: answer between steps, not
 * at the next pass (the connection event will not wait). */
#define PULL() do { if (c.kind == CTL_CONNECTED && input_wanted()) { send_report(now); \
                         STEP(STEP_REPORT); } } while (0)
    for (;;) {
        start = t = plat_us();
        now = plat_ms();
        read_input(now);
        STEP(STEP_INPUT);
        while (bt_poll(&e)) bt_event(&e, now);
        STEP(STEP_BT);
        /* Straight after the input: bt.c is waiting for this report. */
        if (c.kind == CTL_CONNECTED) send_report(now);
        STEP(STEP_REPORT);
        /* The counters are cumulative: bring the copy up to date only while
         * the tool device, which reads it, can be there. */
        if (c.usb_powered && !c.usb) {
            struct bt_stats b;
            struct plat_stats p;

            bt_stats(&b);
            plat_stats(&p);
            counters_since(&c.stats.bt.sent, &b.sent, &c.bt_base.sent, sizeof(b) / 4);
            bt_timing(&c.stats.bt_timing, 0);
            counters_since(&c.stats.plat.button_changes, &p.button_changes,
                           &c.plat_base.button_changes, sizeof(p) / 4);
        }
        c.stats.now_ms = now;
        st.state = c.kind;
        st.slot = c.slot;
        st.usb_powered = c.usb_powered;
        st.low_battery = c.low_battery;
        st.battery_mv = c.battery_mv;
        st.uptime_ms = now;
        st.ephemeral = (u8)set_is_ephemeral();
        st.staged = (u8)(set_staged() < 255 ? set_staged() : 255);
        st.stats = &c.stats;
        st.input = &c.input;
        st.raw = &c.raw;
        handle(usb_poll(&st), now);
        usb_session(now);
        STEP(STEP_USB);
        PULL();
        sample_power(now);
        follow_usb(now);
        STEP(STEP_POWER);
        timeouts(now);
        STEP(STEP_TIMEOUTS);
        PULL();
        outputs(now);
        STEP(STEP_OUTPUTS);
        PULL();
        set_poll(now, bt_radio_quiet() && !c.waking && c.kind != CTL_STOPPING && now - c.since >= QUIET_MS);
        STEP(STEP_SETTINGS);
        PULL();
        deadline = earliest(c.led_next, earliest(state_deadline(), earliest(ux_deadline(), c.imu_next)));
        deadline = earliest(deadline, remap_deadline());
        if (c.dithering) deadline = earliest(deadline, c.dither_next);
        if (c.rumble_testing) deadline = earliest(deadline, c.rumble_test_until);
        if (c.kicking[0]) deadline = earliest(deadline, c.kick_until[0]);
        if (c.kicking[1]) deadline = earliest(deadline, c.kick_until[1]);
        if (c.calibrating) deadline = earliest(deadline, now + CAL_SAMPLE_MS);
        c.stats.passes++;
        util_stat_add(&c.stats.pass, plat_us() - start);
        util_trace(TR_CTL_SLEEP, 0);
        util_trace(TR_CTL_WAKE, (u16)plat_wait(deadline,
                   c.kind == CTL_CONNECTED && input_wanted() ? c.pull_at_us : 0));
    }
}

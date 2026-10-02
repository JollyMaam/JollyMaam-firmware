#include <string.h>
#include "pad.h"

#define LOW_BATTERY_MV   3500
#define BATTERY_OK_MV    3600
#define BATTERY_CHECK_MS 1000
#define USB_DEBOUNCE_MS  100
#define RETRY_MS         1000
#define QUIET_MS         500
#define CAL_SAMPLE_MS    2

static const char *const names[CTL_KIND_COUNT] = {
    "boot", "idle", "connecting", "pairing", "connected", "stopping", "sleeping", "rebooting",
};

static const struct profile *profile_for(u8 type)
{
    static const struct profile *const profiles[PROFILE_COUNT] = {
        [PROFILE_SWITCH] = &profile_switch, [PROFILE_SWITCH2] = &profile_switch2,
        [PROFILE_XBOX] = &profile_xbox, [PROFILE_XBOX_BLE] = &profile_xbox_ble,
        [PROFILE_DUALSENSE] = &profile_dualsense, [PROFILE_DUALSHOCK4] = &profile_dualshock4,
        [PROFILE_GENERIC] = &profile_generic, [PROFILE_MOUSE] = &profile_mouse,
    };

    return type < PROFILE_COUNT ? profiles[type] : NULL;
}

static struct {
    u8 kind;
    u32 since;
    u8 slot;                     /* the session's slot, SLOT_NONE without one */
    u8 last_slot;
    const struct profile *prof;
    u8 radio;                    /* bt_start() until BT_EV_STOPPED */
    u8 waking;                   /* the profile's wake is running, radio off */
    u8 woke;                     /* this session has tried a wake */
    u8 wake_after_stop;          /* the session stops to wake the host, then restarts */
    u32 wake_next;
    struct profile_session sess;
    struct ctl_request next;     /* STOPPING: what comes after */
    u8 to_loader;
    u8 uboot_chord;
    u32 retry_at;
    u32 report_next;
    u8 imu_mode, begin_imu;
    u32 imu_seq, imu_next;
    u8 calibrating;
    struct set_cal cal;
    struct in_raw raw;
    struct pad pad;              /* as the host sees it */
    struct pad activity_ref;
    u32 activity_ms;
    struct profile_host host;
    struct led_state led;
    struct led_frame shown;
    u8 reach[2];
    u32 led_next;
    u8 usb_raw, usb_powered;
    u32 usb_changed_ms, usb_plugged_ms, usb_absent_ms;
    u16 battery_mv;
    u8 low_battery;
    u32 battery_ms;
} c;

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

static void radio_on(void)
{
    bt_start(c.prof, c.sess.address, c.sess.pairing ? NULL : c.sess.bond, c.sess.pairing);
    c.radio = 1;
}

static void end_session(void)
{
    if (c.prof && c.prof->end) c.prof->end();
    if (c.waking) c.prof->wake->stop();
    c.waking = 0;
    c.wake_after_stop = 0;
    if (c.radio) bt_stop();
}

static void handle(struct ctl_request r, u32 now);

/* The radio is down: the session is over. */
static void stopped(u32 now)
{
    c.radio = 0;
    if (c.kind == CTL_CONNECTING && c.wake_after_stop) {
        c.wake_after_stop = 0;
        c.waking = !c.prof->wake->start(&c.sess);
        if (!c.waking) radio_on();
    } else if (c.kind == CTL_STOPPING) {
        c.slot = SLOT_NONE;
        c.prof = NULL;
        become(CTL_IDLE, now);
        handle(c.next, now);
    }
}

static void start_session(struct ctl_request r, u32 now)
{
    const struct profile *p = profile_for(set_slot(r.slot)->type);
    struct profile_session s;
    int pairing = r.kind == REQ_PAIR;

    if (!p) {
        util_log("slot %u: profile type %u not in this build", r.slot, set_slot(r.slot)->type);
        return;
    }
    if (pairing) {
        set_bond_forget(r.slot);
        if (r.generate_new_address) set_slot_new_address(r.slot);
    }
    memset(&s, 0, sizeof(s));
    s.slot = r.slot;
    s.pairing = (u8)pairing;
    set_slot_address(r.slot, s.address);
    s.bond = set_bond(r.slot);
    s.settings = set_slot(r.slot);
    c.prof = p;
    c.slot = r.slot;
    c.last_slot = r.slot;
    plat_retain_slot(r.slot);
    memset(&c.host, 0, sizeof(c.host));
    c.begin_imu = p->begin(&s);
    c.sess = s;
    c.woke = 0;
    radio_on();
    util_log("session slot %u %s%s", r.slot, p->name, pairing ? " pairing" : "");
    become(pairing ? CTL_PAIRING : CTL_CONNECTING, now);
}

/* Take the session down, then do `next` (REQ_NONE: stay idle). */
static void stop(struct ctl_request next, u32 now)
{
    c.next = next;
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
    if (c.radio || c.waking) end_session();
    become(kind, now);
}

static void handle(struct ctl_request r, u32 now)
{
    struct ctl_request none = { REQ_NONE, SLOT_NONE, 0 };

    if (r.kind == REQ_NONE || c.kind == CTL_SLEEPING || c.kind == CTL_REBOOTING) return;
    if (r.slot == SLOT_LAST) r.slot = c.last_slot;
    switch (r.kind) {
    case REQ_CONNECT:
    case REQ_PAIR:
        if (r.slot >= SLOTS) return;
        if (r.kind == REQ_CONNECT && c.kind == CTL_CONNECTED && c.slot == r.slot) return;
        if (c.kind == CTL_STOPPING || session()) stop(r, now);
        else start_session(r, now);
        break;
    case REQ_DISCONNECT:
        if (session()) stop(none, now);
        else if (c.kind == CTL_STOPPING) c.next = none;
        break;
    case REQ_SLEEP:
        if (!c.usb_powered) shut_down(CTL_SLEEPING, now);
        else if (session()) stop(none, now);
        break;
    case REQ_CALIBRATE:
        c.calibrating = 1;
        c.cal = *set_cal();
        cal_start(now);
        break;
    case REQ_REBOOT:
    case REQ_REBOOT_LOADER:
        c.to_loader = r.kind == REQ_REBOOT_LOADER;
        set_commit();
        shut_down(CTL_REBOOTING, now);
        break;
    }
}

static void bt_event(const struct bt_ev *e, u32 now)
{
    struct ctl_request none = { REQ_NONE, SLOT_NONE, 0 };
    struct bt_bond bond;

    switch (e->kind) {
    case BT_EV_LINKED:
        if (c.kind == CTL_CONNECTING || c.kind == CTL_PAIRING) {
            become(CTL_CONNECTED, now);
            c.report_next = now;
            c.activity_ms = now;
        }
        break;
    case BT_EV_UNLINKED:
        util_log("link down, reason 0x%02x (%s)", e->reason, e->lost ? "lost" : "ended");
        if (c.kind != CTL_CONNECTED) break;
        memset(&c.host, 0, sizeof(c.host));
        if (e->lost) {
            become(CTL_CONNECTING, now);
            bt_connect();
        } else {
            stop(none, now);
        }
        break;
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
        if (c.slot < SLOTS) set_bond_store(c.slot, &e->bond);
        break;
    case BT_EV_WRITE:
        /* A pairing the profile ran itself (Switch 2): the key joins the
         * slot's bond, and the stack needs it at once. */
        if (c.prof && c.prof->write && c.prof->write(&e->write, now, &bond) && c.slot < SLOTS) {
            struct bt_bond b = *set_bond(c.slot);

            memcpy(b.ltk, bond.ltk, 16);
            set_bond_store(c.slot, &b);
            bt_update_bond(&b);
        }
        break;
    case BT_EV_STOPPED:
        stopped(now);
        break;
    }
}

static void timeouts(u32 now)
{
    struct ctl_request none = { REQ_NONE, SLOT_NONE, 0 };
    struct ctl_request loader = { REQ_REBOOT_LOADER, SLOT_NONE, 0 };
    u32 t = now - c.since;
    int battery = !c.usb_powered;

    switch (c.kind) {
    case CTL_BOOT:
        if (c.uboot_chord && c.usb_powered) handle(loader, now);
        else if (t >= (c.uboot_chord ? ux_timeouts.uboot_window_ms : ux_timeouts.boot_scene_ms))
            become(CTL_IDLE, now);
        break;
    case CTL_IDLE:
        if (battery && t >= ux_timeouts.offline_sleep_ms &&
            now - c.usb_absent_ms >= ux_timeouts.unplug_sleep_ms)
            shut_down(CTL_SLEEPING, now);
        break;
    case CTL_CONNECTING:
        if (t >= ux_timeouts.connect_ms) stop(none, now);
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
        if (t >= ux_timeouts.pair_ms) stop(none, now);
        break;
    case CTL_CONNECTED:
        if (battery && now - c.activity_ms >= ux_timeouts.idle_sleep_ms) shut_down(CTL_SLEEPING, now);
        break;
    case CTL_STOPPING:
        if (t >= ux_timeouts.stop_max_ms) bt_abort();
        break;
    case CTL_SLEEPING:
        if (c.usb_powered && !c.radio) become(CTL_IDLE, now);
        else if (!c.radio && t >= 50) {
            set_commit();
            util_log("power off");
            plat_power_off();
        } else if (t >= ux_timeouts.stop_max_ms) bt_abort();
        break;
    case CTL_REBOOTING:
        if ((!c.radio && t >= 100) || t >= ux_timeouts.stop_max_ms) {
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
    case CTL_IDLE: return c.since + ux_timeouts.offline_sleep_ms;
    case CTL_CONNECTING: if (c.waking) return c.wake_next;
        return c.retry_at ? c.retry_at : c.since + ux_timeouts.connect_ms;
    case CTL_PAIRING: return c.since + ux_timeouts.pair_ms;
    case CTL_CONNECTED: return c.report_next;
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
            if ((c.raw.buttons & (PAD_HOME | PAD_START)) == (PAD_HOME | PAD_START)) handle(loader, now);
        }
    }
    if (c.usb_powered) c.usb_absent_ms = now;
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

static void outputs(u32 now)
{
    const struct set_slot *slot;
    struct led_inputs in;
    struct led_frame f;
    u32 plugged = now - c.usb_plugged_ms;
    u8 scene = c.kind;
    int rumble = c.kind == CTL_CONNECTED && !c.calibrating;

    if (c.prof && c.prof->host) c.prof->host(now, &c.host);
    c.imu_mode = !session() ? IMU_OFF : c.prof && c.prof->host && c.host.imu ? c.host.imu : c.begin_imu;
    c.imu_next = imu_poll(now, c.imu_mode);
    slot = set_slot(c.slot < SLOTS ? c.slot : 0);
    rumble = rumble && slot->rumble;
    plat_motors(rumble ? c.host.rumble[0] : 0, rumble ? c.host.rumble[1] : 0);
    if ((s32)(now - c.led_next) < 0) return;
    c.led_next = now + ux_timeouts.led_frame_ms;
    if (c.kind == CTL_BOOT && now - c.since >= ux_timeouts.boot_scene_ms) scene = CTL_IDLE;
    if (scene != c.led.scene) {
        c.led.scene = scene;
        c.led.since_ms = now;
    }
    c.led.slot = c.kind == CTL_STOPPING && c.next.kind != REQ_NONE ? c.next.slot : c.slot;
    if (c.led.slot == SLOT_LAST) c.led.slot = c.last_slot;
    memset(&in, 0, sizeof(in));
    in.slot_colour = rgb3(set_slot(c.led.slot < SLOTS ? c.led.slot : 0)->colour);
    in.connected_colour = rgb3(set_slot(c.led.slot < SLOTS ? c.led.slot : 0)->connected_colour);
    in.idle_colour = rgb3(set_prefs()->idle_colour);
    in.battery_colour = rgb3(set_prefs()->battery_colour);
    in.grips = slot->grips;
    led_reach(c.reach, &c.pad);
    in.reach[0] = c.reach[0];
    in.reach[1] = c.reach[1];
    in.rumble[0] = rumble ? c.host.rumble[0] : 0;
    in.rumble[1] = rumble ? c.host.rumble[1] : 0;
    in.usb_powered = c.usb_powered;
    in.low_battery = c.low_battery;
    if (c.usb_powered && plugged < 2u * ux_timeouts.usb_plug_flash_ms)
        in.usb_plug_flash = plugged < ux_timeouts.usb_plug_flash_ms ? 1 : 2;
    if (c.kind == CTL_CONNECTED) {
        in.has_players = c.host.has_players;
        in.players = c.host.players;
        in.players_flash = c.host.players_flash;
        in.has_lightbar = c.host.has_lightbar;
        in.lightbar = rgb3(c.host.lightbar);
    }
    in.cal_lit = (u8)cal_led(now, &in.cal);
    led_frame(&ux_scenes[scene], &c.led, now, &in, &f);
    if (memcmp(&f, &c.shown, sizeof(f))) {
        c.shown = f;
        plat_leds(&f);
    }
}

static void read_input(u32 now)
{
    struct ctl_request none = { REQ_NONE, SLOT_NONE, 0 };
    struct ctl_request r = none;

    plat_input(&c.raw);
    in_map(&c.raw, &c.pad);
    cal_apply(set_cal(), &c.raw, &c.pad);
    if (in_moved(&c.pad, &c.activity_ref)) {
        c.activity_ref = c.pad;
        c.activity_ms = now;
    }
    if (c.calibrating) {
        int s = cal_step(&c.raw, now, &c.cal);

        if (s == CAL_SAVE) set_cal_store(&c.cal);
        if (s == CAL_IDLE) {
            c.calibrating = 0;
            ux_ignore_held();
        }
        c.pad.buttons = 0;
        c.pad.hat = HAT_NEUTRAL;
    } else {
        r = ux_step(&c.pad, now, c.kind);
    }
    handle(r, now);
}

static void send_report(u32 now)
{
    u8 buf[80], id = 0;
    u32 period = 1000 / (c.prof->report_hz ? c.prof->report_hz : 125);
    int n;

    if ((s32)(now - c.report_next) < 0) return;
    c.report_next += period;
    if ((s32)(now - c.report_next) >= 0) c.report_next = now + period;
    c.pad.imu_count = (u8)imu_take(&c.imu_seq, c.pad.imu, IMU_PER_REPORT);
    n = c.prof->report(&c.pad, now, buf, &id);
    if (n > 0) bt_send_input(id, buf, (u16)n);
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
    c.kind = CTL_BOOT;
    c.since = now;
    c.slot = SLOT_NONE;
    c.last_slot = plat_retained_slot();
    c.led.scene = CTL_BOOT;
    c.led.slot = SLOT_NONE;
    c.usb_raw = c.usb_powered = (u8)plat_usb_present();
    c.usb_plugged_ms = now - 1000;
    c.battery_ms = now - BATTERY_CHECK_MS;
    util_log("boot cause %u held %04x last slot %u", b.cause, b.buttons_held, c.last_slot);
    c.uboot_chord = (b.buttons_held & (PAD_HOME | PAD_START)) == (PAD_HOME | PAD_START);
    plat_input(&c.raw);
    if (c.uboot_chord) ux_ignore_held();
    else if (!(c.raw.buttons & PAD_HOME)) handle(ux_boot(b.buttons_held, b.hat_held, CTL_BOOT), now);
}

void ctl_main(void)
{
    struct usb_status st;
    struct bt_ev e;
    u32 now = plat_ms(), deadline;

    boot(now);
    for (;;) {
        now = plat_ms();
        read_input(now);
        while (bt_poll(&e)) bt_event(&e, now);
        st.state = c.kind;
        st.slot = c.slot;
        st.usb_powered = c.usb_powered;
        st.low_battery = c.low_battery;
        st.battery_mv = c.battery_mv;
        st.uptime_ms = now;
        handle(usb_poll(&st), now);
        sample_power(now);
        timeouts(now);
        if (c.kind == CTL_CONNECTED) send_report(now);
        outputs(now);
        set_poll(now, bt_radio_quiet() && !c.waking && c.kind != CTL_STOPPING && now - c.since >= QUIET_MS);
        deadline = earliest(c.led_next, earliest(state_deadline(), earliest(ux_deadline(), c.imu_next)));
        if (c.calibrating) deadline = earliest(deadline, now + CAL_SAMPLE_MS);
        plat_wait(deadline);
    }
}

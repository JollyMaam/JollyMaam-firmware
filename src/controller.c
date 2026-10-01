#include <string.h>
#include "pad.h"

#define LOW_BATTERY_MV   3500
#define BATTERY_OK_MV    3600
#define BATTERY_CHECK_MS 1000
#define USB_DEBOUNCE_MS  100

static const char *const names[CTL_KIND_COUNT] = {
    "boot", "idle", "connecting", "pairing", "connected", "stopping", "sleeping", "rebooting",
};

static struct {
    u8 kind;
    u32 since;
    u8 slot;                     /* the session's slot, SLOT_NONE without one */
    u8 last_slot;
    struct ctl_request next;     /* STOPPING: what comes after */
    u8 to_loader;                /* REBOOTING */
    u8 uboot_chord;              /* Home+Start held at boot */
    u8 calibrating;
    struct pad pad;              /* as the host would see it */
    struct pad activity_ref;
    u32 activity_ms;
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

static int radio_on(void)
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

static void start_session(u8 kind, u8 slot, u32 now)
{
    c.slot = slot;
    c.last_slot = slot;
    plat_retain_slot(slot);
    become(kind, now);
}

/* Take the radio down and then do `next` (REQ_NONE: stay idle). */
static void stop(struct ctl_request next, u32 now)
{
    c.next = next;
    become(CTL_STOPPING, now);
}

static void handle(struct ctl_request r, u32 now)
{
    struct ctl_request none = { REQ_NONE, SLOT_NONE, 0 };

    if (r.kind == REQ_NONE || c.kind == CTL_SLEEPING || c.kind == CTL_REBOOTING) return;
    if (r.slot == SLOT_LAST) r.slot = c.last_slot;
    switch (r.kind) {
    case REQ_CONNECT:
    case REQ_PAIR:
        if (r.slot == SLOT_NONE) return;
        if (r.kind == REQ_CONNECT && c.kind == CTL_CONNECTED && c.slot == r.slot) return;
        if (c.kind == CTL_STOPPING || radio_on()) stop(r, now);
        else start_session(r.kind == REQ_PAIR ? CTL_PAIRING : CTL_CONNECTING, r.slot, now);
        break;
    case REQ_DISCONNECT:
        if (c.kind == CTL_STOPPING || radio_on()) stop(none, now);
        break;
    case REQ_SLEEP:
        if (c.usb_powered) {
            if (c.kind == CTL_STOPPING || radio_on()) stop(none, now);
        } else {
            become(CTL_SLEEPING, now);
        }
        break;
    case REQ_CALIBRATE:
        util_log("calibration: not in this build yet");
        break;
    case REQ_REBOOT:
    case REQ_REBOOT_LOADER:
        c.to_loader = r.kind == REQ_REBOOT_LOADER;
        become(CTL_REBOOTING, now);
        break;
    }
}

/* What happens with time alone: timeouts, sleep, the end of a stop. */
static void timeouts(u32 now)
{
    struct ctl_request none = { REQ_NONE, SLOT_NONE, 0 };
    u32 in_state = now - c.since;
    int battery = !c.usb_powered;

    switch (c.kind) {
    case CTL_BOOT:
        if (c.uboot_chord && c.usb_powered) handle((struct ctl_request){ REQ_REBOOT_LOADER, 0, 0 }, now);
        else if (in_state >= (c.uboot_chord ? ux_timeouts.uboot_window_ms : ux_timeouts.boot_scene_ms))
            become(CTL_IDLE, now);
        break;
    case CTL_IDLE:
        if (battery && in_state >= ux_timeouts.offline_sleep_ms &&
            now - c.usb_absent_ms >= ux_timeouts.unplug_sleep_ms)
            become(CTL_SLEEPING, now);
        break;
    case CTL_CONNECTING:
        if (in_state >= ux_timeouts.connect_ms) stop(none, now);
        break;
    case CTL_PAIRING:
        if (in_state >= ux_timeouts.pair_ms) stop(none, now);
        break;
    case CTL_CONNECTED:
        if (battery && now - c.activity_ms >= ux_timeouts.idle_sleep_ms) become(CTL_SLEEPING, now);
        break;
    case CTL_STOPPING:
        /* No radio yet: the stop is immediate. */
        c.slot = SLOT_NONE;
        become(CTL_IDLE, now);
        handle(c.next, now);
        break;
    case CTL_SLEEPING:
        if (c.usb_powered) {
            become(CTL_IDLE, now);
        } else if (in_state >= 50) {
            util_log("power off");
            plat_power_off();
        }
        break;
    case CTL_REBOOTING:
        if (in_state >= 100) {
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
    case CTL_CONNECTING: return c.since + ux_timeouts.connect_ms;
    case CTL_PAIRING: return c.since + ux_timeouts.pair_ms;
    default: return 0;
    }
}

static void sample_power(u32 now)
{
    u8 raw = (u8)plat_usb_present();

    if (raw != c.usb_raw) {
        c.usb_raw = raw;
        c.usb_changed_ms = now;
    }
    if (raw != c.usb_powered && now - c.usb_changed_ms >= USB_DEBOUNCE_MS) {
        c.usb_powered = raw;
        if (raw) {
            c.usb_plugged_ms = now;
            if ((c.pad.buttons & (PAD_HOME | PAD_START)) == (PAD_HOME | PAD_START))
                handle((struct ctl_request){ REQ_REBOOT_LOADER, 0, 0 }, now);
        }
        util_log("usb %s", raw ? "present" : "gone");
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

static void lights(u32 now)
{
    const struct set_slot *slot;
    struct led_inputs in;
    struct led_frame f;
    u32 plugged = now - c.usb_plugged_ms;
    u8 scene = c.kind;

    if (c.kind == CTL_BOOT && now - c.since >= ux_timeouts.boot_scene_ms) scene = CTL_IDLE;
    if (scene != c.led.scene) {
        c.led.scene = scene;
        c.led.since_ms = now;
    }
    c.led.slot = c.kind == CTL_STOPPING && c.next.kind != REQ_NONE ? c.next.slot : c.slot;
    slot = set_slot(c.led.slot == SLOT_NONE ? 0 : c.led.slot);
    memset(&in, 0, sizeof(in));
    in.slot_colour = (struct rgb){ slot->colour[0], slot->colour[1], slot->colour[2] };
    in.idle_colour = (struct rgb){ set_prefs()->idle_colour[0], set_prefs()->idle_colour[1],
                                   set_prefs()->idle_colour[2] };
    in.battery_colour = (struct rgb){ set_prefs()->battery_colour[0], set_prefs()->battery_colour[1],
                                      set_prefs()->battery_colour[2] };
    in.grips = slot->grips;
    led_reach(c.reach, &c.pad);
    in.reach[0] = c.reach[0];
    in.reach[1] = c.reach[1];
    in.usb_powered = c.usb_powered;
    in.low_battery = c.low_battery;
    if (c.usb_powered && plugged < 2u * ux_timeouts.usb_plug_flash_ms)
        in.usb_plug_flash = plugged < ux_timeouts.usb_plug_flash_ms ? 1 : 2;
    led_frame(&ux_scenes[scene], &c.led, now, &in, &f);
    if (memcmp(&f, &c.shown, sizeof(f))) {
        c.shown = f;
        plat_leds(&f);
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
    struct in_raw raw;

    plat_boot_info(&b);
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
    plat_input(&raw);
    in_map(&raw, &c.pad);
    if (c.uboot_chord) ux_ignore_held();
    else if (!(c.pad.buttons & PAD_HOME)) handle(ux_boot(b.buttons_held, b.hat_held, CTL_BOOT), now);
}

void ctl_main(void)
{
    struct in_raw raw;
    struct usb_status st;
    u32 now = plat_ms(), ev, deadline;

    boot(now);
    for (;;) {
        now = plat_ms();
        plat_input(&raw);
        in_map(&raw, &c.pad);
        if (in_moved(&c.pad, &c.activity_ref)) {
            c.activity_ref = c.pad;
            c.activity_ms = now;
        }
        handle(ux_step(&c.pad, now, c.kind), now);
        st.state = c.kind;
        st.slot = c.slot;
        st.usb_powered = c.usb_powered;
        st.low_battery = c.low_battery;
        st.battery_mv = c.battery_mv;
        st.uptime_ms = now;
        handle(usb_poll(&st), now);
        sample_power(now);
        timeouts(now);
        if ((s32)(now - c.led_next) >= 0) {
            lights(now);
            c.led_next = now + ux_timeouts.led_frame_ms;
        }
        deadline = earliest(c.led_next, earliest(state_deadline(), ux_deadline()));
        if (c.kind == CTL_STOPPING || c.kind == CTL_SLEEPING || c.kind == CTL_REBOOTING)
            deadline = now + 10;
        ev = plat_wait(deadline);
        (void)ev;
    }
}

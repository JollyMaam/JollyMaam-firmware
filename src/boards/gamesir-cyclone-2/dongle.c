#include <string.h>
#include "dongle.h"

/* The dongle's main loop (gamesir-cyclone-2-dongle): USB as on the pad (identify, status,
 * the log, reboot, the loader; see usb.c) and Bluetooth in bt_dongle.c on
 * its own thread.  While our pad is linked the dongle is the pad's slot type
 * on USB, built by the same profile code the pad uses on its own cable. */

#define REBOOT_DELAY_MS 100      /* the command's status reaches the host first */

#ifndef UNITTEST
/* The pad gone this long, the personality goes and USB is the tool device
 * again; a shorter drop keeps it, with the controls at rest. */
#define LINK_GRACE_MS   3000
#define REPORT_IDLE_MS  8        /* unchanged state is sent this often */

static struct {
    const struct profile *prof;  /* the personality on USB, NULL: the tool device */
    struct set_slot set;         /* what begin() reads, from the pad's config */
    struct profile_session sess;
    u8 begin_imu;
    struct pad pad;              /* the newest state from the pad, motion since the last report */
    s32 gyro_bias[3];            /* the pad's, from its config */
    u8 gyro_bias_known;
    u8 fresh;
    u8 linked;
    u32 lost_ms;
    u32 report_ms;
    struct profile_host host;
    u32 output_ms;
    u8 tool_only;                /* padtool tool-only: never a personality, until power loss */
    u8 tool_held;                /* hub mode ended by its host: the tool device until the pad is gone */
} d;

int host_send(u16 id, const u8 *data, u16 len)
{
    return d.prof ? plat_usb_send_message(id, data, len) : 0;
}

static void at_rest(struct pad *p)
{
    memset(p, 0, sizeof(*p));
}

static void drop_personality(const char *why)
{
    if (!d.prof) return;
    util_log("dongle: %s is no longer on USB (%s)", d.prof->name, why);
    if (d.prof->end) d.prof->end();
    d.prof = NULL;
    plat_usb_mode(NULL, NULL);
}

/* For debugging the link: the log and padtool stay reachable while a pad is
 * linked.  Its input still arrives (and shows in the link rate); it goes
 * nowhere.  Kept through soft reboots (padtool reboot), not a power loss. */
static void hub_off_usb(const char *why);
static void hub_stop_output(void);
#define TOOL_ONLY_KEPT 0x746f6f6cu
static u32 tool_only_kept RETAINED;

int dongle_tool_only(int on)
{
    if ((on != 0 && on != 1) || on == d.tool_only) return d.tool_only;
    d.tool_only = (u8)on;
    tool_only_kept = on ? TOOL_ONLY_KEPT : 0;
    if (!on) {
        util_log("dongle: tool only off (a pad's next config puts its type on USB)");
        return 0;
    }
    util_log("dongle: tool only until power loss (a linked pad's type is not put on USB)");
    drop_personality("tool only");
    hub_off_usb("tool only");
    hub_stop_output();
    return 1;
}

/* The pad says what the slot is: the dongle becomes that on USB. */
static void configure(const struct link_config *c)
{
    const struct profile *p = profile_for(c->type);
    u32 k;

    for (k = 0; k < 3; k++) d.gyro_bias[k] = c->gyro_bias[k];
    d.gyro_bias_known = c->gyro_bias_known;
    if (!p || !p->usb) {
        util_log("dongle: type %u has no USB device", c->type);
        drop_personality("unsupported type");
        return;
    }
    if (d.tool_only) return;
    if (d.prof == p && d.set.variant == c->variant) return;
    drop_personality("new type");
    memset(&d.set, 0, sizeof(d.set));
    d.set.type = c->type;
    d.set.variant = c->variant;
    d.set.stick_speed = c->stick_speed;
    d.set.gyro_speed = c->gyro_speed;
    d.set.gyro_accel = c->gyro_accel;
    memset(&d.sess, 0, sizeof(d.sess));
    memcpy(d.sess.address, c->address, 6);
    d.sess.settings = &d.set;
    d.sess.usb = 1;
    d.prof = p;
    d.begin_imu = p->begin(&d.sess);
    at_rest(&d.pad);
    d.fresh = 1;
    memset(&d.host, 0, sizeof(d.host));
    util_log("dongle: %s on USB", p->name);
    plat_usb_mode(p->usb, p->read);
}

/* The newest state replaces the old; motion samples add up until a report
 * takes them (IMU_PER_REPORT at most, the newest). */
static void take_input(void)
{
    struct pad p;
    u32 i, k = 0;
    struct imu_sample all[2 * IMU_PER_REPORT];

    if (!dongle_bt_take_input(0, &p)) return;
    for (i = 0; i < d.pad.imu_count; i++) all[k++] = d.pad.imu[i];
    for (i = 0; i < p.imu_count; i++) all[k++] = p.imu[i];
    p.imu_count = (u8)(k > IMU_PER_REPORT ? IMU_PER_REPORT : k);
    memcpy(p.imu, all + (k - p.imu_count), p.imu_count * sizeof(all[0]));
    d.pad = p;
    d.fresh = 1;
}

static void personality(u32 now)
{
    struct bt_write w;
    struct bt_bond bond;
    struct link_config c;
    u8 buf[80], id = 0, out[LINK_OUTPUT_SIZE];
    int n, linked = dongle_bt_linked(0);

    if (dongle_bt_take_config(0, &c) && !d.tool_held) configure(&c);
    take_input();
    if (linked != d.linked) {
        d.linked = (u8)linked;
        if (!linked) {
            d.tool_held = 0;
            d.lost_ms = now;
            at_rest(&d.pad);
            d.fresh = 1;
        }
    }
    if (!d.prof) return;
    if (!linked && now - d.lost_ms >= LINK_GRACE_MS) {
        drop_personality("the pad is gone");
        return;
    }
    while (plat_usb_take(&w)) {
        if (w.kind == BT_WRITE_FEATURE && w.id == USB_TOOL_REPORT && w.len >= 4 &&
            !memcmp(w.data, "tool", 4)) {
            drop_personality("the host asked");
            d.tool_held = (u8)linked;
            return;
        }
        if (d.prof->write) d.prof->write(&w, now, &bond);
    }
    if (plat_usb_input_wanted() && (d.fresh || now - d.report_ms >= REPORT_IDLE_MS)) {
        memcpy(d.pad.gyro_bias, d.gyro_bias, sizeof(d.gyro_bias));
        d.pad.gyro_bias_known = d.gyro_bias_known;
        n = d.prof->report(&d.pad, now, buf, &id);
        d.pad.imu_count = 0;
        d.fresh = 0;
        d.report_ms = now;
        if (n > 0) plat_usb_send_input(id, buf, (u16)n);
    }
    if (d.prof->host) d.prof->host(now, &d.host);
    link_pack_output(&d.host, d.host.imu ? d.host.imu : d.begin_imu, (u8)plat_usb_ready(), out);
    dongle_bt_output(0, out);
}

/* ---- hub mode (link.h): the link's packets as they are, for padhub ---------- */

#define HUB_QUIET_MS   (5 * LINK_OUTPUT_MS)   /* no output from the PC: the pad's rumble stops */

static const u8 hub_descriptor[] = {
    0x06, 0x00, 0xff,            /* Usage Page (vendor 0xff00) */
    0x09, 0x01,                  /* Usage (1) */
    0xa1, 0x01,                  /* Collection (Application) */
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,   /* bytes */
    0x85, HUB_REPORT_IN, 0x09, 0x02, 0x95, HUB_REPORT_SIZE, 0x81, 0x02,
    0x85, HUB_REPORT_OUT, 0x09, 0x03, 0x95, HUB_REPORT_SIZE, 0x91, 0x02,
    0xc0,
};

static const struct usb_identity hub_usb = {
    .vendor_id = HUB_VENDOR_ID, .product_id = HUB_PRODUCT_ID, .bcd_device = 0x0100,
    .manufacturer = USB_MANUFACTURER, .product = "Cyclone 2 Hub",
    .kind = USB_HID, .report_ids = 1, .interval = 1, .in_size = 64, .out_size = 64, .max_power = 50,
    .hid_descriptor = hub_descriptor, .hid_descriptor_len = sizeof(hub_descriptor),
};

static struct {
    u8 on;
    u8 usb;                      /* hub_usb is on USB */
    u8 next;                     /* the slot whose input goes first: they take turns */
    u8 staged;                   /* the slot whose input waits for the endpoint */
    u32 lost_ms;                 /* the last time a slot was linked */
    u32 stats_ms;
    struct hub_slot {
        u8 linked;
        u8 input[LINK_INPUT_SIZE];
        u8 fresh;                /* input waits */
        u8 config[LINK_CONFIG_SIZE];
        u8 config_waits, down_waits;
        u8 output[LINK_OUTPUT_SIZE];
        u32 output_ms;           /* the PC's last output */
        u32 merged, sent;        /* HUB_STATS */
    } s[HUB_SLOTS];
} hub;

/* No host owns these outputs through a mode change. */
static void hub_stop_output(void)
{
    u8 out[LINK_OUTPUT_SIZE] = { 0 };
    u32 i;

    for (i = 0; i < HUB_SLOTS; i++) dongle_bt_output((u8)i, out);
}

/* padhub turns it on (RAM only: a replugged dongle is itself again). */
int dongle_hub(int on)
{
    if (on != 0 && on != 1) return hub.on;
    if (on == hub.on) return hub.on;
    if (on) drop_personality("hub mode");
    else hub_off_usb("hub mode off");
    hub_stop_output();
    hub.on = (u8)on;
    memset(hub.s, 0, sizeof(hub.s));
    d.linked = d.tool_held = 0;
    dongle_bt_slots(on ? HUB_SLOTS : 1);
    util_log("dongle: hub mode %s", on ? "on" : "off");
    return hub.on;
}

static void hub_off_usb(const char *why)
{
    if (!hub.usb) return;
    util_log("dongle: hub is no longer on USB (%s)", why);
    hub.usb = 0;
    plat_usb_mode(NULL, NULL);
}

/* A packet for the PC. */
static int hub_send(u8 slot, u8 kind, const u8 *payload, u32 len)
{
    static u8 r[HUB_REPORT_SIZE];

    memset(r, 0, sizeof(r));
    r[0] = slot;
    r[1] = kind;
    if (len) memcpy(r + 2, payload, len);
    if (kind == HUB_INPUT) return plat_usb_send_input(HUB_REPORT_IN, r, sizeof(r));
    return plat_usb_send_message(HUB_REPORT_IN, r, sizeof(r));
}

static __attribute__((noinline)) void hub_step(u32 now)   /* its frame is not added to ctl_main's */
{
    struct link_config c;
    struct bt_write w;
    struct hub_slot *h;
    u32 i, any = 0;

    for (i = 0; i < HUB_SLOTS; i++) {
        int linked = dongle_bt_linked((u8)i);

        h = &hub.s[i];
        if (dongle_bt_take_config((u8)i, &c)) {
            link_pack_config(&c, h->config);
            h->config_waits = 1;
            if (!hub.usb && !d.tool_only) {
                util_log("dongle: hub on USB");
                hub.usb = 1;
                hub.lost_ms = now;
                plat_usb_mode(&hub_usb, NULL);
            }
        }
        if (dongle_bt_take_raw_input((u8)i, h->input)) {
            h->merged += h->fresh;
            h->fresh = 1;
        }
        if (linked != h->linked) {
            h->linked = (u8)linked;
            if (!linked) {
                h->down_waits = 1;
                h->config_waits = h->fresh = 0;
            } else {
                memset(h->output, 0, sizeof(h->output));
                h->output_ms = now;
            }
        }
        any |= (u32)linked;
    }
    if (any) hub.lost_ms = now;
    if (!hub.usb) return;
    if (!any && now - hub.lost_ms >= LINK_GRACE_MS) {
        hub_off_usb("the pads are gone");
        return;
    }
    while (plat_usb_take(&w)) {
        if (w.kind == BT_WRITE_FEATURE && w.id == USB_TOOL_REPORT && w.len >= 4) {
            /* padhub closing: hub mode ends, the tool device stays until slot 0's pad links again. */
            if (!memcmp(w.data, "tool", 4)) {
                int linked = dongle_bt_linked(0);

                dongle_hub(0);
                d.linked = d.tool_held = (u8)linked;
                return;
            }
            /* padtool pair SLOT: the tool device is not on USB to ask. */
            if (w.len >= 5 && !memcmp(w.data, "pair", 4)) dongle_bt_forget(w.data[4]);
            continue;
        }
        if (w.kind == BT_WRITE_OUTPUT && w.id == HUB_REPORT_OUT && w.len >= 2 + LINK_OUTPUT_SIZE &&
            w.data[0] < HUB_SLOTS && w.data[1] == HUB_OUTPUT) {
            h = &hub.s[w.data[0]];
            memcpy(h->output, w.data + 2, LINK_OUTPUT_SIZE);
            h->output_ms = now;
            dongle_bt_output(w.data[0], h->output);
        }
    }
    for (i = 0; i < HUB_SLOTS; i++) {
        /* No padhub (or it hangs): nothing keeps the motors running. */
        h = &hub.s[i];
        if (now - h->output_ms >= HUB_QUIET_MS && (h->output[0] || h->output[1])) {
            h->output[0] = h->output[1] = 0;
            dongle_bt_output((u8)i, h->output);
        }
    }
    if (!plat_usb_ready()) return;
    for (i = 0; i < HUB_SLOTS; i++) {
        h = &hub.s[i];
        if (h->down_waits && hub_send((u8)i, HUB_DOWN, NULL, 0)) h->down_waits = 0;
        if (h->config_waits && hub_send((u8)i, HUB_CONFIG, h->config, LINK_CONFIG_SIZE)) h->config_waits = 0;
    }
    if (now - hub.stats_ms >= 1000) {
        hub.stats_ms = now;
        for (i = 0; i < HUB_SLOTS; i++) {
            struct link_stats ls;
            u8 p[20];

            h = &hub.s[i];
            if (!h->linked) continue;
            dongle_bt_stats((u8)i, &ls);
            util_put32(p, ls.inputs);
            util_put32(p + 4, ls.replaced);
            util_put32(p + 8, ls.lost);
            util_put32(p + 12, h->merged);
            util_put32(p + 16, h->sent);
            hub_send((u8)i, HUB_STATS, p, sizeof(p));
        }
    }
    /* One input waits for the endpoint, which takes it in its interrupt as
     * it frees: keep it filled, from the next slot with one; a newer input
     * of the slot waiting replaces it. */
    if (plat_usb_input_staged()) {
        h = &hub.s[hub.staged];
        if (h->fresh && hub_send(hub.staged, HUB_INPUT, h->input, LINK_INPUT_SIZE)) {
            h->fresh = 0;
            h->merged++;         /* the one it replaced */
        }
        return;
    }
    for (i = 0; i < HUB_SLOTS; i++) {
        u8 k = (u8)((hub.next + i) % HUB_SLOTS);

        h = &hub.s[k];
        if (h->fresh && hub_send(k, HUB_INPUT, h->input, LINK_INPUT_SIZE)) {
            h->fresh = 0;
            h->sent++;
            hub.staged = k;
            hub.next = (u8)((k + 1) % HUB_SLOTS);
            break;
        }
    }
}

/* Each second, each slot whose pad sent something. */
static __attribute__((noinline)) void log_stats(void)
{
    static struct link_stats last[HUB_SLOTS];
    struct link_stats ls;
    u32 i;

    for (i = 0; i < HUB_SLOTS; i++) {
        dongle_bt_stats((u8)i, &ls);
        if (ls.inputs != last[i].inputs)
            util_log("link %u: %u/s in, %u replaced, %u lost", i, ls.inputs - last[i].inputs,
                     ls.replaced - last[i].replaced, ls.lost - last[i].lost);
        last[i] = ls;
    }
}

void ctl_main(void)
{
    struct usb_status st;
    struct ctl_request r;
    u8 was_down = 0;
    u32 reboot_at = 0, stats_ms = 0;
    int to_loader = 0, to_updater = 0;

    memset(&st, 0, sizeof(st));
    st.state = CTL_IDLE;
    st.slot = SLOT_NONE;
    st.usb_powered = 1;
    util_log("dongle up");
    if (tool_only_kept == TOOL_ONLY_KEPT) {
        d.tool_only = 1;
        util_log("dongle: still tool only");
    }
    dongle_bt_start();
    for (;;) {
        u32 now = plat_ms();
        u32 changed_us = 0;
        int is_down = (plat_lines(&changed_us) & 1) != 0;

        /* The button's one job (besides the loader at plug-in): pair. */
        if (is_down && !was_down) {
            util_log("dongle: button -> pair");
            dongle_bt_forget(0);
        }
        was_down = (u8)is_down;

        if (hub.on) hub_step(now);
        else personality(now);
        if (now - stats_ms >= 1000) {
            log_stats();
            stats_ms = now;
        }
        st.uptime_ms = now;
        st.state = dongle_bt_state();
        st.ephemeral = (u8)set_is_ephemeral();
        r = usb_poll(&st);
        if (r.kind == REQ_PAIR) dongle_bt_forget(r.slot < HUB_SLOTS ? r.slot : 0);   /* padtool pair [SLOT] */
        if ((r.kind == REQ_REBOOT || r.kind == REQ_REBOOT_LOADER || r.kind == REQ_REBOOT_UPDATER) &&
            !reboot_at) {
            to_loader = r.kind == REQ_REBOOT_LOADER;
            to_updater = r.kind == REQ_REBOOT_UPDATER;
            util_log(to_loader ? "reboot to the loader" : to_updater ? "reboot to the updater" : "reboot");
            reboot_at = now + REBOOT_DELAY_MS;
        }
        if (reboot_at && (s32)(plat_ms() - reboot_at) >= 0) {
            if (to_loader) plat_reboot_loader();
            if (to_updater) plat_reboot_updater();
            plat_reboot();
        }
        {
            u32 next_wake = reboot_at ? reboot_at : now + 1000;   /* the button wakes it (EV_BUTTONS) */

            /* New state (EV_BT) and a report taken (EV_USB) wake it sooner. */
            if (d.prof) next_wake = d.report_ms + REPORT_IDLE_MS;
            if (hub.usb) next_wake = now + LINK_OUTPUT_MS;   /* HUB_QUIET_MS, the grace */
            plat_wait(next_wake, 0);
        }
    }
}
#endif

/* usb.c's settings commands: the dongle has no settings yet. */
u32 set_describe(u32 n, u8 *out, u32 max) { (void)n; (void)out; (void)max; return 0; }
u32 set_read(u8 target, u8 *out, u32 max) { (void)target; (void)out; (void)max; return 0; }
u32 set_write(const u8 *items, u32 len, u8 *results, u32 max)
{
    (void)items; (void)len; (void)results; (void)max;
    return 0;
}
void set_apply(void) {}
void set_drop(void) {}

/* Set on the control thread; read by the Bluetooth thread and SDK callers. */
static volatile u8 ephemeral;

void set_ephemeral(void)
{
    if (ephemeral) return;
    ephemeral = 1;
    util_log("dongle: ephemeral (bond changes stay in RAM until reboot)");
}

int set_is_ephemeral(void)
{
    return ephemeral;
}

/* The app's bond writes only. The SDK can still save its own radio trim,
 * as it does on the pad when app settings are ephemeral. */
int dongle_store_write(u16 key, const void *buf, u16 len)
{
    if (ephemeral) return len;
    return plat_store_write(key, buf, len);
}

#ifdef UNITTEST
static u32 writes;
static u16 last_key, last_len;
static const void *last_buf;
static int write_result;

int plat_store_write(u16 key, const void *buf, u16 len)
{
    writes++;
    last_key = key;
    last_buf = buf;
    last_len = len;
    return write_result;
}
void util_log(const char *fmt, ...) { (void)fmt; }

int main(void)
{
    static u8 bond[24];

    CHECK(!set_is_ephemeral());
    write_result = sizeof(bond);
    CHECK(dongle_store_write(1, bond, sizeof(bond)) == sizeof(bond));
    CHECK(writes == 1 && last_key == 1 && last_buf == bond && last_len == sizeof(bond));
    write_result = -7;
    CHECK(dongle_store_write(1, bond, sizeof(bond)) == -7);
    CHECK(writes == 2);
    set_ephemeral();
    set_ephemeral();
    CHECK(set_is_ephemeral());
    CHECK(dongle_store_write(1, bond, sizeof(bond)) == sizeof(bond));
    memset(bond, 0, sizeof(bond)); /* forgetting the live bond is also ephemeral */
    CHECK(dongle_store_write(1, bond, sizeof(bond)) == sizeof(bond));
    CHECK(writes == 2);

    return TEST_DONE("dongle_ephemeral");
}
#endif

#include <string.h>
#include "dongle.h"

/* The dongle's main loop (BOARD=dongle): USB as on the pad (identify, status,
 * the log, reboot, the loader; see usb.c) and Bluetooth in dongle_bt.c on
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
    u8 tool_only;                /* padtool tool-only: never a personality, until reboot */
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
 * nowhere. */
void dongle_tool_only(void)
{
    if (d.tool_only) return;
    d.tool_only = 1;
    util_log("dongle: tool only until reboot (a linked pad's type is not put on USB)");
    drop_personality("tool only");
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

    if (!dongle_bt_take_input(&p)) return;
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
    int n, linked = dongle_bt_linked();

    if (dongle_bt_take_config(&c)) configure(&c);
    take_input();
    if (linked != d.linked) {
        d.linked = (u8)linked;
        if (!linked) {
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
    dongle_bt_output(out);
}

void ctl_main(void)
{
    struct usb_status st;
    struct ctl_request r;
    u8 was_down = 0;
    struct link_stats ls, ls_last;
    u32 reboot_at = 0, stats_ms = 0;
    int to_loader = 0;

    memset(&st, 0, sizeof(st));
    memset(&ls_last, 0, sizeof(ls_last));
    st.state = CTL_IDLE;
    st.slot = SLOT_NONE;
    st.usb_powered = 1;
    util_log("dongle up");
    dongle_bt_start();
    for (;;) {
        u32 now = plat_ms();
        u32 changed_us = 0;
        int is_down = (plat_lines(&changed_us) & 1) != 0;

        /* The button's one job (besides the loader at plug-in): pair. */
        if (is_down && !was_down) {
            util_log("dongle: button -> pair");
            dongle_bt_forget();
        }
        was_down = (u8)is_down;

        personality(now);
        if (now - stats_ms >= 1000 && d.linked) {
            dongle_bt_stats(&ls);
            util_log("link: %u/s in, %u replaced, %u lost", ls.inputs - ls_last.inputs,
                     ls.replaced - ls_last.replaced, ls.lost - ls_last.lost);
            ls_last = ls;
            stats_ms = now;
        }
        st.uptime_ms = now;
        st.state = dongle_bt_state();
        st.ephemeral = (u8)set_is_ephemeral();
        r = usb_poll(&st);
        if (r.kind == REQ_PAIR) dongle_bt_forget();     /* padtool pair: any slot */
        if ((r.kind == REQ_REBOOT || r.kind == REQ_REBOOT_LOADER) && !reboot_at) {
            to_loader = r.kind == REQ_REBOOT_LOADER;
            util_log(to_loader ? "reboot to the loader" : "reboot");
            reboot_at = now + REBOOT_DELAY_MS;
        }
        if (reboot_at && (s32)(plat_ms() - reboot_at) >= 0) {
            if (to_loader) plat_reboot_loader();
            plat_reboot();
        }
        {
            u32 next_wake = reboot_at ? reboot_at : now + 1000;   /* the button wakes it (EV_BUTTONS) */

            /* New state (EV_BT) and a report taken (EV_USB) wake it sooner. */
            if (d.prof) next_wake = d.report_ms + REPORT_IDLE_MS;
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

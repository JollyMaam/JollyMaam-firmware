#include <string.h>
#include "dongle.h"

/* The dongle's main loop (BOARD=dongle): USB as on the pad (identify, status,
 * the log, reboot, the loader; architecture/usb.md) and Bluetooth in
 * dongle_bt.c on its own thread. */

#define REBOOT_DELAY_MS 100      /* the command's status reaches the host first */

#ifndef UNITTEST
void ctl_main(void)
{
    struct usb_status st;
    struct ctl_request r;
    u32 reboot_at = 0;
    int to_loader = 0;

    memset(&st, 0, sizeof(st));
    st.state = CTL_IDLE;
    st.slot = SLOT_NONE;
    st.usb_powered = 1;
    util_log("dongle up");
    dongle_bt_start();
    for (;;) {
        st.uptime_ms = plat_ms();
        st.state = dongle_bt_state();
        st.ephemeral = (u8)set_is_ephemeral();
        r = usb_poll(&st);
        if (r.kind == REQ_PAIR) dongle_bt_forget();     /* padtool pair: any slot */
        if ((r.kind == REQ_REBOOT || r.kind == REQ_REBOOT_LOADER) && !reboot_at) {
            to_loader = r.kind == REQ_REBOOT_LOADER;
            util_log(to_loader ? "reboot to the loader" : "reboot");
            reboot_at = plat_ms() + REBOOT_DELAY_MS;
        }
        if (reboot_at && (s32)(plat_ms() - reboot_at) >= 0) {
            if (to_loader) plat_reboot_loader();
            plat_reboot();
        }
        plat_wait(reboot_at ? reboot_at : plat_ms() + 1000, 0);
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
    u8 bond[24] = {0};

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

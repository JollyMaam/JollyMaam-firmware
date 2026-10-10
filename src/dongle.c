#include <string.h>
#include "dongle.h"

/* The dongle's main loop (BOARD=dongle): USB as on the pad (identify, status,
 * the log, reboot, the loader; architecture/usb.md) and Bluetooth in
 * dongle_bt.c on its own thread. */

#define REBOOT_DELAY_MS 100      /* the command's status reaches the host first */

int dongle_btn_update(struct dongle_btn *b, u32 now, int is_down, u32 hold_threshold_ms)
{
    if (is_down) {
        if (!b->down) {
            b->down = 1;
            b->hold_fired = 0;
            b->press_ms = now;
            return DONGLE_BTN_DOWN;
        }
        if (!b->hold_fired && (s32)(now - b->press_ms) >= (s32)hold_threshold_ms) {
            b->hold_fired = 1;
            return DONGLE_BTN_HOLD;
        }
    } else {
        if (b->down) {
            b->down = 0;
            if (!b->hold_fired) {
                return DONGLE_BTN_TAP;
            }
        }
    }
    return DONGLE_BTN_NONE;
}

#ifndef UNITTEST
void ctl_main(void)
{
    struct usb_status st;
    struct ctl_request r;
    struct dongle_btn btn;
    u32 reboot_at = 0;
    int to_loader = 0;

    memset(&st, 0, sizeof(st));
    memset(&btn, 0, sizeof(btn));
    st.state = CTL_IDLE;
    st.slot = SLOT_NONE;
    st.usb_powered = 1;
    util_log("dongle up");
    dongle_bt_start();
    for (;;) {
        u32 now = plat_ms();
        u32 changed_us = 0;
        int is_down = (plat_lines(&changed_us) & 1) != 0;
        int act = dongle_btn_update(&btn, now, is_down, DONGLE_BTN_HOLD_PAIR_MS);

        if (act == DONGLE_BTN_DOWN) {
            util_log("dongle: button down");
        } else if (act == DONGLE_BTN_HOLD) {
            util_log("dongle: button hold (%u ms) -> pair", (unsigned)(now - btn.press_ms));
            dongle_bt_forget();
        } else if (act == DONGLE_BTN_TAP) {
            util_log("dongle: button tap (%u ms)", (unsigned)(now - btn.press_ms));
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
        u32 next_wake = reboot_at ? reboot_at : (btn.down ? now + 50 : now + 1000);
        plat_wait(next_wake, 0);
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

    /* Button gesture state machine tests */
    struct dongle_btn b = {0};
    CHECK(dongle_btn_update(&b, 100, 0, 1500) == DONGLE_BTN_NONE);
    CHECK(dongle_btn_update(&b, 100, 1, 1500) == DONGLE_BTN_DOWN);
    CHECK(dongle_btn_update(&b, 200, 1, 1500) == DONGLE_BTN_NONE);
    CHECK(dongle_btn_update(&b, 300, 0, 1500) == DONGLE_BTN_TAP);
    CHECK(dongle_btn_update(&b, 400, 0, 1500) == DONGLE_BTN_NONE);

    /* Hold triggers hold-to-pair */
    CHECK(dongle_btn_update(&b, 500, 1, 1500) == DONGLE_BTN_DOWN);
    CHECK(dongle_btn_update(&b, 1999, 1, 1500) == DONGLE_BTN_NONE);
    CHECK(dongle_btn_update(&b, 2000, 1, 1500) == DONGLE_BTN_HOLD);
    CHECK(dongle_btn_update(&b, 2100, 1, 1500) == DONGLE_BTN_NONE);
    CHECK(dongle_btn_update(&b, 2200, 0, 1500) == DONGLE_BTN_NONE);

    return TEST_DONE("dongle_ephemeral");
}
#endif

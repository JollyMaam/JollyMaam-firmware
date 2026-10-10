#include <string.h>
#include "pad.h"
#include "dongle_link.h"

/* High-rate custom dongle profile for GameSir Cyclone 2.
 * Connects directly to our custom dongle over Classic Bluetooth L2CAP.
 * Delivers full absolute state + multi-sample stick history for lossless
 * high-frequency reconstruction, with downlink rumble/LED forwarding. */

static const u8 dongle_descriptor[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x05,        /* Usage (Game Pad) */
    0xa1, 0x01,        /* Collection (Application) */
    0x85, DONGLE_REPORT_ID,
    0x05, 0x09,        /*   Usage Page (Button) */
    0x19, 0x01, 0x29, 0x10, /*   Usage Min (1), Usage Max (16) */
    0x15, 0x00, 0x25, 0x01, /*   Logical Min (0), Logical Max (1) */
    0x75, 0x01, 0x95, 0x10, /*   Report Size (1), Report Count (16) */
    0x81, 0x02,        /*   Input (Data, Var, Abs) */
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */
    0x09, 0x39,        /*   Usage (Hat switch) */
    0x15, 0x00, 0x25, 0x07, /*   Logical Min (0), Logical Max (7) */
    0x75, 0x04, 0x95, 0x01, /*   Report Size (4), Report Count (1) */
    0x81, 0x42,        /*   Input (Data, Var, Abs, Null) */
    0x75, 0x04, 0x95, 0x01, /*   Report Size (4), Report Count (1) */
    0x81, 0x01,        /*   Input (Const) */
    /* Vendor custom buffer (the full dongle_uplink_pkt fields) */
    0x06, 0x00, 0xff,  /*   Usage Page (Vendor 0xFF00) */
    0x09, 0x01,        /*   Usage (Vendor Usage 1) */
    0x15, 0x00, 0x26, 0xff, 0x00, /* Logical Min 0, Max 255 */
    0x75, 0x08, 0x95, DONGLE_UPLINK_FULL_SIZE - 4, /* remaining bytes */
    0x81, 0x02,        /*   Input (Data, Var, Abs) */
    0x09, 0x02,        /*   Usage (Vendor Usage 2) */
    0x75, 0x08, 0x95, sizeof(struct dongle_downlink_pkt),
    0x91, 0x02,        /*   Output (Data, Var, Abs) */
    0xc0,              /* End Collection */
};

static u8 seq_num;
static struct dongle_sticks history_ring[3];
static u8 history_len;
static struct profile_host host_state;
static u32 last_downlink_ms;

static u8 dongle_begin(const struct profile_session *s)
{
    (void)s;
    seq_num = 0;
    history_len = 0;
    memset(history_ring, 0, sizeof(history_ring));
    memset(&host_state, 0, sizeof(host_state));
    last_downlink_ms = 0;
    return IMU_OFF;
}

static void dongle_end(void)
{
    memset(&host_state, 0, sizeof(host_state));
}

static int dongle_report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    struct dongle_uplink_pkt *pkt = (struct dongle_uplink_pkt *)out;
    u8 count;

    pkt->seq = seq_num++;
    pkt->flags = 0;
    if (p->imu_count) pkt->flags |= 0x04;
    pkt->buttons = p->buttons;
    pkt->hat = p->hat;
    pkt->l2 = p->l2;
    pkt->r2 = p->r2;
    pkt->timestamp_us = now * 1000;

    pkt->current.lx = p->lx;
    pkt->current.ly = p->ly;
    pkt->current.rx = p->rx;
    pkt->current.ry = p->ry;

    /* Copy preceding stick samples for loss-free 1000 Hz reconstruction */
    count = history_len;
    if (count > 3) count = 3;
    pkt->history_count = count;
    for (u8 i = 0; i < count; i++) {
        pkt->history[i] = history_ring[i];
    }

    /* Shift history ring and insert current */
    history_ring[2] = history_ring[1];
    history_ring[1] = history_ring[0];
    history_ring[0] = pkt->current;
    if (history_len < 3) history_len++;

    if (p->imu_count) {
        pkt->accel[0] = p->imu[0].accel[0];
        pkt->accel[1] = p->imu[0].accel[1];
        pkt->accel[2] = p->imu[0].accel[2];
        pkt->gyro[0] = p->imu[0].gyro[0];
        pkt->gyro[1] = p->imu[0].gyro[1];
        pkt->gyro[2] = p->imu[0].gyro[2];
    } else {
        memset(pkt->accel, 0, sizeof(pkt->accel));
        memset(pkt->gyro, 0, sizeof(pkt->gyro));
    }

    *id = DONGLE_REPORT_ID;
    return p->imu_count ? DONGLE_UPLINK_FULL_SIZE : DONGLE_UPLINK_BASE_SIZE;
}

static int dongle_write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    (void)bond;
    if (w->kind != BT_WRITE_OUTPUT) return 0;
    if (w->len < sizeof(struct dongle_downlink_pkt)) return 0;
    if (w->id != DONGLE_CMD_ID && w->id != 0) return 0;

    const struct dongle_downlink_pkt *cmd = (const struct dongle_downlink_pkt *)w->data;
    host_state.rumble[0] = cmd->motor_left;
    host_state.rumble[1] = cmd->motor_right;
    host_state.has_lightbar = 1;
    host_state.lightbar[0] = cmd->led_home_r;
    host_state.lightbar[1] = cmd->led_home_g;
    host_state.lightbar[2] = cmd->led_home_b;
    host_state.has_players = 1;
    host_state.players = cmd->player_leds;
    last_downlink_ms = now;
    return 1;
}

static void dongle_host(u32 now, struct profile_host *out)
{
    /* Failsafe watchdog: cut rumble if link dropped for > 150 ms */
    if (last_downlink_ms && (s32)(now - last_downlink_ms) > 150) {
        host_state.rumble[0] = 0;
        host_state.rumble[1] = 0;
    }
    *out = host_state;
}

static const struct bt_device dongle_device = {
    .transport = BT_CLASSIC,
    .name = "Cyclone 2 Dongle",
    .class_of_device = 0x002508,     /* gamepad */
    .vendor_id = 0x3537,
    .product_id = 0x1001,
    .version = 0x0100,
    .hid_descriptor = dongle_descriptor,
    .hid_descriptor_len = sizeof(dongle_descriptor),
    .sniff_slots = 0,               /* Active mode: no sniff */
    .sniff_attempt = 0,
};

const struct profile profile_dongle = {
    .name = "dongle",
    .device = &dongle_device,
    .begin = dongle_begin,
    .end = dongle_end,
    .report = dongle_report,
    .write = dongle_write,
    .host = dongle_host,
};

#ifdef UNITTEST
int main(void)
{
    struct pad p;
    u8 buf[128];
    u8 id = 0;

    memset(&p, 0, sizeof(p));
    p.buttons = PAD_A | PAD_START;
    p.hat = HAT_UP;
    p.lx = 1000;
    p.ly = -2000;
    p.rx = 3000;
    p.ry = -4000;
    p.l2 = 12345;
    p.r2 = 54321;

    dongle_begin(NULL);
    int len = dongle_report(&p, 50, buf, &id);
    CHECK(id == DONGLE_REPORT_ID);
    CHECK(len == DONGLE_UPLINK_BASE_SIZE);

    struct dongle_uplink_pkt *pkt = (struct dongle_uplink_pkt *)buf;
    CHECK(pkt->seq == 0);
    CHECK(pkt->buttons == (PAD_A | PAD_START));
    CHECK(pkt->hat == HAT_UP);
    CHECK(pkt->l2 == 12345 && pkt->r2 == 54321);
    CHECK(pkt->current.lx == 1000 && pkt->current.ly == -2000);
    CHECK(pkt->current.rx == 3000 && pkt->current.ry == -4000);
    CHECK(pkt->history_count == 0);

    /* Second report: history_count should be 1 */
    p.lx = 1050;
    len = dongle_report(&p, 52, buf, &id);
    CHECK(pkt->seq == 1);
    CHECK(pkt->current.lx == 1050);
    CHECK(pkt->history_count == 1);
    CHECK(pkt->history[0].lx == 1000);

    /* Downlink write test */
    struct dongle_downlink_pkt cmd;
    cmd.seq = 5;
    cmd.flags = 0;
    cmd.motor_left = 180;
    cmd.motor_right = 90;
    cmd.led_home_r = 255;
    cmd.led_home_g = 128;
    cmd.led_home_b = 0;
    cmd.player_leds = 0x01;

    struct bt_write w;
    w.kind = BT_WRITE_OUTPUT;
    w.id = DONGLE_CMD_ID;
    w.len = sizeof(cmd);
    memcpy(w.data, &cmd, sizeof(cmd));

    CHECK(dongle_write(&w, 100, NULL) == 1);
    struct profile_host host;
    dongle_host(100, &host);
    CHECK(host.rumble[0] == 180 && host.rumble[1] == 90);
    CHECK(host.lightbar[0] == 255 && host.lightbar[1] == 128);

    /* Watchdog timeout after 151 ms */
    dongle_host(300, &host);
    CHECK(host.rumble[0] == 0 && host.rumble[1] == 0);

    dongle_end();
    return TEST_DONE("profile_dongle");
}
#endif

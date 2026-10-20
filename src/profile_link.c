#include <string.h>
#include "pad.h"
#include "link.h"

/* The pad's side of the link to our dongle (link.h): a Classic HID device
 * that sends the pad's state and the slot's type, and takes rumble and
 * lights back.  The packing is shared with the dongle, and so is the table
 * of profiles by type. */

const struct profile *profile_for(u8 type)
{
    static const struct profile *const profiles[PROFILE_COUNT] = {
        [PROFILE_SWITCH] = &profile_switch, [PROFILE_SWITCH2] = &profile_switch2,
        [PROFILE_XBOX] = &profile_xbox, [PROFILE_XBOX_BLE] = &profile_xbox_ble,
        [PROFILE_DUALSENSE] = &profile_dualsense, [PROFILE_DUALSHOCK4] = &profile_dualshock4,
        [PROFILE_GENERIC] = &profile_generic, [PROFILE_MOUSE] = &profile_mouse,
        [PROFILE_GENERIC_BLE] = &profile_generic_ble, [PROFILE_NONE] = NULL,
    };

    return type < PROFILE_COUNT ? profiles[type] : NULL;
}

/* ---- the packets ----------------------------------------------------------- */

static void put16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static u16 get16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

static void put32(u8 *p, u32 v)
{
    put16(p, (u16)v);
    put16(p + 2, (u16)(v >> 16));
}

static u32 get32(const u8 *p)
{
    return get16(p) | (u32)get16(p + 2) << 16;
}

void link_pack_input(const struct pad *p, u8 seq, u8 flags, u8 out[LINK_INPUT_SIZE])
{
    u8 n = p->imu_count > LINK_IMU_MAX ? LINK_IMU_MAX : p->imu_count;
    const struct imu_sample *s = p->imu + (p->imu_count - n);   /* the newest n */
    u32 i, k;

    memset(out, 0, LINK_INPUT_SIZE);
    out[0] = seq;
    out[1] = (u8)((flags & ~3u) | n);
    put32(out + 2, p->buttons);
    put16(out + 6, (u16)p->lx);
    put16(out + 8, (u16)p->ly);
    put16(out + 10, (u16)p->rx);
    put16(out + 12, (u16)p->ry);
    put16(out + 14, p->l2);
    put16(out + 16, p->r2);
    if (n) {
        put32(out + 18, s[n - 1].us);
        if (n > 1) put16(out + 22, (u16)(s[1].us - s[0].us));
    }
    for (i = 0; i < n; i++)
        for (k = 0; k < 3; k++) {
            put16(out + 24 + 12 * i + 2 * k, (u16)s[i].accel[k]);
            put16(out + 30 + 12 * i + 2 * k, (u16)s[i].gyro[k]);
        }
}

int link_unpack_input(const u8 *in, u16 len, struct pad *p, u8 *seq, u8 *flags)
{
    u32 i, k, newest;

    if (len < LINK_INPUT_SIZE) return 0;
    memset(p, 0, sizeof(*p));
    *seq = in[0];
    *flags = in[1];
    p->buttons = get32(in + 2);
    p->lx = (s16)get16(in + 6);
    p->ly = (s16)get16(in + 8);
    p->rx = (s16)get16(in + 10);
    p->ry = (s16)get16(in + 12);
    p->l2 = get16(in + 14);
    p->r2 = get16(in + 16);
    p->imu_count = in[1] & 3;
    if (p->imu_count > LINK_IMU_MAX) p->imu_count = LINK_IMU_MAX;
    newest = get32(in + 18);
    for (i = 0; i < p->imu_count; i++) {
        for (k = 0; k < 3; k++) {
            p->imu[i].accel[k] = (s16)get16(in + 24 + 12 * i + 2 * k);
            p->imu[i].gyro[k] = (s16)get16(in + 30 + 12 * i + 2 * k);
        }
        p->imu[i].us = i + 1 == p->imu_count ? newest : newest - get16(in + 22);
    }
    return 1;
}

void link_pack_config(const struct link_config *c, u8 out[LINK_CONFIG_SIZE])
{
    memset(out, 0, LINK_CONFIG_SIZE);
    out[0] = LINK_VERSION;
    out[1] = c->type;
    out[2] = c->variant;
    put16(out + 4, c->stick_speed);
    put16(out + 6, c->gyro_speed);
    put16(out + 8, c->gyro_accel);
    memcpy(out + 10, c->address, 6);
}

int link_unpack_config(const u8 *in, u16 len, struct link_config *c)
{
    if (len < LINK_CONFIG_SIZE || in[0] != LINK_VERSION) return 0;
    c->type = in[1];
    c->variant = in[2];
    c->stick_speed = get16(in + 4);
    c->gyro_speed = get16(in + 6);
    c->gyro_accel = get16(in + 8);
    memcpy(c->address, in + 10, 6);
    return 1;
}

void link_pack_output(const struct profile_host *h, u8 imu, u8 usb_ready, u8 out[LINK_OUTPUT_SIZE])
{
    memset(out, 0, LINK_OUTPUT_SIZE);
    out[0] = h->rumble[0];
    out[1] = h->rumble[1];
    out[2] = (u8)((h->has_players ? LINK_HAS_PLAYERS : 0) | (h->has_lightbar ? LINK_HAS_LIGHTBAR : 0) |
                  (usb_ready ? LINK_USB_READY : 0));
    out[3] = h->players;
    out[4] = h->players_flash;
    memcpy(out + 5, h->lightbar, 3);
    out[8] = imu;
}

int link_unpack_output(const u8 *in, u16 len, struct profile_host *h, u8 *usb_ready)
{
    if (len < LINK_OUTPUT_SIZE) return 0;
    memset(h, 0, sizeof(*h));
    h->rumble[0] = in[0];
    h->rumble[1] = in[1];
    h->has_players = (in[2] & LINK_HAS_PLAYERS) != 0;
    h->has_lightbar = (in[2] & LINK_HAS_LIGHTBAR) != 0;
    h->players = in[3];
    h->players_flash = in[4];
    memcpy(h->lightbar, in + 5, 3);
    h->imu = in[8];
    *usb_ready = (in[2] & LINK_USB_READY) != 0;
    return 1;
}

/* ---- the pad's profile ---------------------------------------------------------- */

/* Vendor reports only: no host but the dongle reads them. */
static const u8 descriptor[] = {
    0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
    0x85, LINK_INPUT_ID, 0x09, 0x20, 0x95, LINK_INPUT_SIZE, 0x81, 0x02,
    0x85, LINK_CONFIG_ID, 0x09, 0x22, 0x95, LINK_CONFIG_SIZE, 0x81, 0x02,
    0x85, LINK_OUTPUT_ID, 0x09, 0x21, 0x95, LINK_OUTPUT_SIZE, 0x91, 0x02,
    0xc0,
};

/* The config goes first and then once a second: the session outlives a lost
 * link (the pad pages the dongle again) and the dongle may have restarted,
 * and the dongle only becomes the slot's type on USB once it has it.  It
 * ignores a config that changes nothing. */
#define CONFIG_EVERY_MS 1000

static struct {
    u8 config[LINK_CONFIG_SIZE];
    u8 config_sent;
    u32 config_ms;
    u8 seq;
    u8 usb_ready;
    struct profile_host host;
} lk;

static u8 begin(const struct profile_session *s)
{
    struct link_config c;

    memset(&lk, 0, sizeof(lk));
    memset(&c, 0, sizeof(c));
    if (s->settings) {
        c.type = s->settings->type;
        c.variant = s->settings->variant;
        c.stick_speed = s->settings->stick_speed;
        c.gyro_speed = s->settings->gyro_speed;
        c.gyro_accel = s->settings->gyro_accel;
    }
    memcpy(c.address, s->address, 6);
    link_pack_config(&c, lk.config);
    return IMU_OFF;              /* until the dongle's host asks for motion */
}

static int report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    if ((!lk.config_sent || now - lk.config_ms >= CONFIG_EVERY_MS) &&
        host_send(LINK_CONFIG_ID, lk.config, LINK_CONFIG_SIZE)) {
        lk.config_sent = 1;
        lk.config_ms = now;
    }
    link_pack_input(p, lk.seq++, 0, out);
    *id = LINK_INPUT_ID;
    return LINK_INPUT_SIZE;
}

static int write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    (void)now;
    (void)bond;
    if (w->kind == BT_WRITE_OUTPUT && w->id == LINK_OUTPUT_ID)
        link_unpack_output(w->data, w->len, &lk.host, &lk.usb_ready);
    return 0;
}

static void host(u32 now, struct profile_host *out)
{
    (void)now;
    *out = lk.host;
}

static const struct bt_device device = {
    .transport = BT_CLASSIC,
    .name = LINK_NAME,
    .class_of_device = 0x002508,
    .vendor_id = 0x3537, .product_id = 0x1001, .version = 0x0100,
    .hid_descriptor = descriptor, .hid_descriptor_len = sizeof(descriptor),
    /* No sniff: the dongle, as master, polls an active link. */
};

const struct profile profile_link = {
    .name = "link", .device = &device,
    .begin = begin, .report = report, .write = write, .host = host,
};

#ifdef UNITTEST
static u8 sent_id, sent[LINK_CONFIG_SIZE];
int host_send(u16 id, const u8 *data, u16 len) { sent_id = (u8)id; memcpy(sent, data, len); return 1; }
const struct profile profile_switch, profile_switch2, profile_xbox, profile_xbox_ble,
    profile_dualsense, profile_dualshock4, profile_generic, profile_mouse, profile_generic_ble;

int main(void)
{
    struct set_slot set;
    struct profile_session s = { 2, 0, { 1, 2, 3, 4, 5, 6 }, 0, &set };
    struct pad p, q;
    struct link_config c;
    struct bt_write w;
    struct profile_host h, g;
    u8 out[64], id, seq, flags, ready;

    memset(&set, 0, sizeof(set));
    memset(&c, 0, sizeof(c));
    set.type = PROFILE_DUALSENSE;
    set.gyro_speed = 40;
    CHECK(begin(&s) == IMU_OFF);
    memset(&p, 0, sizeof(p));
    p.buttons = PAD_A | PAD_HOME | PAD_LEFT;
    p.lx = -32768; p.ly = 32767; p.rx = 5; p.ry = -6;
    p.l2 = 65535; p.r2 = 7;
    p.imu_count = 3;             /* the newest two travel */
    p.imu[0].gyro[0] = 1; p.imu[1].gyro[0] = 2; p.imu[2].gyro[0] = 3;
    p.imu[1].us = 1000; p.imu[2].us = 5460; p.imu[2].accel[2] = -4096;
    CHECK(report(&p, 0, out, &id) == LINK_INPUT_SIZE && id == LINK_INPUT_ID);
    CHECK(sent_id == LINK_CONFIG_ID && link_unpack_config(sent, LINK_CONFIG_SIZE, &c));
    CHECK(c.type == PROFILE_DUALSENSE && c.gyro_speed == 40 && c.address[5] == 6);
    CHECK(link_unpack_input(out, LINK_INPUT_SIZE, &q, &seq, &flags) && seq == 0);
    CHECK(q.buttons == p.buttons && q.lx == -32768 && q.ly == 32767);
    CHECK(q.rx == 5 && q.ry == -6 && q.l2 == 65535 && q.r2 == 7 && q.imu_count == 2);
    CHECK(q.imu[0].gyro[0] == 2 && q.imu[1].gyro[0] == 3 && q.imu[1].accel[2] == -4096);
    CHECK(q.imu[1].us == 5460 && q.imu[0].us == 1000);
    sent_id = 0;
    report(&p, 999, out, &id);
    CHECK(!sent_id && out[0] == 1);                 /* the config goes first */
    report(&p, 1000, out, &id);
    CHECK(sent_id == LINK_CONFIG_ID);               /* and again a second later */
    CHECK(!link_unpack_input(out, LINK_INPUT_SIZE - 1, &q, &seq, &flags));

    memset(&g, 0, sizeof(g));
    g.rumble[0] = 200; g.rumble[1] = 9;
    g.has_players = 1; g.players = 0x3; g.players_flash = 0x4;
    g.has_lightbar = 1; g.lightbar[2] = 77;
    memset(&w, 0, sizeof(w));
    w.kind = BT_WRITE_OUTPUT; w.id = LINK_OUTPUT_ID; w.len = LINK_OUTPUT_SIZE;
    link_pack_output(&g, IMU_MOTION, 1, w.data);
    write(&w, 0, NULL);
    host(0, &h);
    CHECK(h.rumble[0] == 200 && h.rumble[1] == 9 && h.has_players && h.players == 3);
    CHECK(h.players_flash == 4 && h.has_lightbar && h.lightbar[2] == 77 && h.imu == IMU_MOTION);
    CHECK(link_unpack_output(w.data, w.len, &h, &ready) && ready);
    CHECK(profile_for(PROFILE_NONE) == NULL);
    CHECK(profile_for(PROFILE_COUNT) == NULL && profile_for(PROFILE_XBOX) == &profile_xbox);
    return TEST_DONE("profile_link");
}
#endif

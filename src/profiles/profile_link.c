#include <string.h>
#include "pad.h"
#include "link.h"

/* The pad's side of the link to our dongle (link.h): an LE peripheral
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
    out[3] = c->gyro_bias_known ? 1 : 0;
    put16(out + 4, c->stick_speed);
    put16(out + 6, c->gyro_speed);
    put16(out + 8, c->gyro_accel);
    memcpy(out + 10, c->address, 6);
    put16(out + 16, (u16)c->gyro_bias[0]);
    put16(out + 18, (u16)c->gyro_bias[1]);
    put16(out + 20, (u16)c->gyro_bias[2]);
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
    c->gyro_bias_known = in[3] & 1;
    c->gyro_bias[0] = (s16)get16(in + 16);
    c->gyro_bias[1] = (s16)get16(in + 18);
    c->gyro_bias[2] = (s16)get16(in + 20);
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

/* The GATT server: one service, its characteristics at link.h's fixed
 * handles.  BTstack's database format: per entry its size, flags, handle,
 * UUID (128-bit with flag 0x0200) and value; flags 0x0100: answered by
 * bt_pad.c. */
#define LE16(v)  (u8)(v), (u8)((v) >> 8)
#define UUID(n)  0x4b, 0x4e, 0x49, 0x4c, 0x32, 0xc2, 0x8a, 0x5d, 0x91, 0x4e, 0x3a, 0x7f, n, 0x00, 0x6c, 0xc2
#define SERVICE(h)            LE16(24), LE16(0x0002), LE16(h), LE16(0x2800), UUID(0)
#define CHARACTERISTIC(h, props, n)  LE16(27), LE16(0x0002), LE16(h), LE16(0x2803), props, LE16((h) + 1), UUID(n)
#define VALUE(h, flags, n)    LE16(22), LE16(0x0300 | (flags)), LE16(h), UUID(n)
#define CCC(h)                LE16(8), LE16(0x010e), LE16(h), LE16(0x2902)
enum { READ = 0x02, WRITE_COMMAND = 0x04, WRITE = 0x08, NOTIFY = 0x10 };

static const u8 att_db[] = {
    1,
    SERVICE(0x0001),
    CHARACTERISTIC(LINK_INPUT_HANDLE - 1, READ | NOTIFY, 1),
    VALUE(LINK_INPUT_HANDLE, READ, 1),
    CCC(LINK_INPUT_HANDLE + 1),
    CHARACTERISTIC(LINK_CONFIG_HANDLE - 1, NOTIFY, 2),
    VALUE(LINK_CONFIG_HANDLE, 0, 2),
    CCC(LINK_CONFIG_HANDLE + 1),
    CHARACTERISTIC(LINK_OUTPUT_HANDLE - 1, WRITE_COMMAND, 3),
    VALUE(LINK_OUTPUT_HANDLE, WRITE_COMMAND, 3),
    CHARACTERISTIC(LINK_KEY_HANDLE - 1, WRITE, 4),
    VALUE(LINK_KEY_HANDLE, WRITE, 4),
    0, 0,
};

/* The config goes first and then once a second: the session outlives a lost
 * link (the pad pages the dongle again) and the dongle may have restarted,
 * and the dongle only becomes the slot's type on USB once it has it.  It
 * ignores a config that changes nothing but the gyro bias, which it takes
 * for the profiles' motion maths. */
#define CONFIG_EVERY_MS 1000

static struct {
    struct link_config config;
    u8 config_sent;
    u32 config_ms;
    u8 seq;
    u8 usb_ready;
    struct profile_host host;
} lk;

static u8 begin(const struct profile_session *s)
{
    struct link_config *c = &lk.config;

    memset(&lk, 0, sizeof(lk));
    if (s->settings) {
        c->type = s->settings->type;
        c->variant = s->settings->variant;
        c->stick_speed = s->settings->stick_speed;
        c->gyro_speed = s->settings->gyro_speed;
        c->gyro_accel = s->settings->gyro_accel;
    }
    memcpy(c->address, s->address, 6);
    return IMU_OFF;              /* until the dongle's host asks for motion */
}

static s16 clamp16(s32 v)
{
    return (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static int report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    if (!lk.config_sent || now - lk.config_ms >= CONFIG_EVERY_MS) {
        u8 config[LINK_CONFIG_SIZE];
        u32 k;

        for (k = 0; k < 3; k++) lk.config.gyro_bias[k] = clamp16(p->gyro_bias[k]);
        lk.config.gyro_bias_known = p->gyro_bias_known;
        link_pack_config(&lk.config, config);
        if (host_send(LINK_CONFIG_HANDLE, config, LINK_CONFIG_SIZE)) {
            lk.config_sent = 1;
            lk.config_ms = now;
        }
    }
    link_pack_input(p, lk.seq++, 0, out);
    *id = 0;
    return LINK_INPUT_SIZE;
}

/* The dongle's output; or, pairing, the key it chose: the bond's LTK. */
static int write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    (void)now;
    if (w->kind != BT_WRITE_ATT) return 0;
    if (w->id == LINK_OUTPUT_HANDLE) link_unpack_output(w->data, w->len, &lk.host, &lk.usb_ready);
    if (w->id != LINK_KEY_HANDLE || w->len != LINK_KEY_SIZE) return 0;
    memset(bond, 0, sizeof(*bond));
    memcpy(bond->ltk, w->data, LINK_KEY_SIZE);
    return 1;
}

static void host(u32 now, struct profile_host *out)
{
    (void)now;
    *out = lk.host;
}

static u8 advertising(const struct bt_bond *bond, int pairing, int wake, u8 out[31])
{
    (void)bond;
    (void)wake;
    memcpy(out, link_adv, LINK_ADV_FLAGS);
    out[LINK_ADV_FLAGS] = pairing ? LINK_ADV_PAIRING : 0;
    return LINK_ADV_SIZE;
}

/* No SMP (the key comes in link.h's pairing) and no interval of its own:
 * the dongle sets it as it connects. */
static const struct bt_device device = {
    .transport = BT_LE,
    .name = LINK_NAME,
    .att_db = att_db,
    .input_handle = LINK_INPUT_HANDLE,
    .advertising = advertising,
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
    struct bt_bond bond;
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
    p.gyro_bias_known = 1; p.gyro_bias[0] = -300; p.gyro_bias[2] = 40000;
    CHECK(report(&p, 0, out, &id) == LINK_INPUT_SIZE);
    CHECK(sent_id == LINK_CONFIG_HANDLE && link_unpack_config(sent, LINK_CONFIG_SIZE, &c));
    CHECK(c.type == PROFILE_DUALSENSE && c.gyro_speed == 40 && c.address[5] == 6);
    CHECK(c.gyro_bias_known && c.gyro_bias[0] == -300 && !c.gyro_bias[1] && c.gyro_bias[2] == 32767);
    CHECK(link_unpack_input(out, LINK_INPUT_SIZE, &q, &seq, &flags) && seq == 0);
    CHECK(q.buttons == p.buttons && q.lx == -32768 && q.ly == 32767);
    CHECK(q.rx == 5 && q.ry == -6 && q.l2 == 65535 && q.r2 == 7 && q.imu_count == 2);
    CHECK(q.imu[0].gyro[0] == 2 && q.imu[1].gyro[0] == 3 && q.imu[1].accel[2] == -4096);
    CHECK(q.imu[1].us == 5460 && q.imu[0].us == 1000);
    sent_id = 0;
    report(&p, 999, out, &id);
    CHECK(!sent_id && out[0] == 1);                 /* the config goes first */
    report(&p, 1000, out, &id);
    CHECK(sent_id == LINK_CONFIG_HANDLE);           /* and again a second later */
    CHECK(!link_unpack_input(out, LINK_INPUT_SIZE - 1, &q, &seq, &flags));

    memset(&g, 0, sizeof(g));
    g.rumble[0] = 200; g.rumble[1] = 9;
    g.has_players = 1; g.players = 0x3; g.players_flash = 0x4;
    g.has_lightbar = 1; g.lightbar[2] = 77;
    memset(&w, 0, sizeof(w));
    w.kind = BT_WRITE_ATT; w.id = LINK_OUTPUT_HANDLE; w.len = LINK_OUTPUT_SIZE;
    link_pack_output(&g, IMU_MOTION, 1, w.data);
    CHECK(!write(&w, 0, &bond));
    host(0, &h);
    CHECK(h.rumble[0] == 200 && h.rumble[1] == 9 && h.has_players && h.players == 3);
    CHECK(h.players_flash == 4 && h.has_lightbar && h.lightbar[2] == 77 && h.imu == IMU_MOTION);
    CHECK(link_unpack_output(w.data, w.len, &h, &ready) && ready);
    w.id = LINK_KEY_HANDLE; w.len = LINK_KEY_SIZE;
    memset(w.data, 0x5a, LINK_KEY_SIZE); w.data[0] = 1;
    CHECK(write(&w, 0, &bond) && bond.ltk[0] == 1 && bond.ltk[15] == 0x5a);
    w.len = 15;
    CHECK(!write(&w, 0, &bond));
    CHECK(advertising(&bond, 1, 0, out) == LINK_ADV_SIZE && out[LINK_ADV_FLAGS] == LINK_ADV_PAIRING);
    CHECK(advertising(&bond, 0, 1, out) == LINK_ADV_SIZE && !out[LINK_ADV_FLAGS] && out[10] == 'K');
    CHECK(test_att_uuid(att_db, LINK_INPUT_HANDLE) == 0x10000 && test_att_uuid(att_db, LINK_INPUT_HANDLE + 1) == 0x2902);
    CHECK(test_att_uuid(att_db, LINK_CONFIG_HANDLE) == 0x10000 && test_att_uuid(att_db, LINK_CONFIG_HANDLE + 1) == 0x2902);
    CHECK(test_att_uuid(att_db, LINK_OUTPUT_HANDLE) == 0x10000 && test_att_uuid(att_db, LINK_KEY_HANDLE) == 0x10000);
    CHECK(test_att_uuid(att_db, LINK_KEY_HANDLE - 1) == 0x2803 && !test_att_uuid(att_db, LINK_KEY_HANDLE + 1));
    CHECK(profile_for(PROFILE_NONE) == NULL);
    CHECK(profile_for(PROFILE_COUNT) == NULL && profile_for(PROFILE_XBOX) == &profile_xbox);
    return TEST_DONE("profile_link");
}
#endif

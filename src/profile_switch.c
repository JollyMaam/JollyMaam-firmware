#include <string.h>
#include "pad.h"

/* Nintendo Switch Pro Controller over Classic HID.  The console drives the
 * session with subcommands (output report 0x01); every one gets an answer in
 * input report 0x21.  Byte layouts: dekuNukem's Nintendo_Switch_Reverse_
 * Engineering notes, as implemented by SDL. */

#define REPORT_REPLY  0x21
#define REPORT_FULL   0x30
#define REPORT_SIMPLE 0x3f
#define OUT_SUBCMD    0x01
#define OUT_RUMBLE    0x10
#define FULL_SIZE     48
#define SIMPLE_SIZE   11
#define KEEPALIVE_MS  100
#define TRIGGER_BUTTON (TRIGGER_MAX / 8)

/* The stock pad's descriptor, byte for byte a real Pro Controller's. */
static const u8 descriptor[170] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x06, 0x01, 0xff, 0x85, 0x21, 0x09,
    0x21, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02, 0x85, 0x30, 0x09, 0x30, 0x75,
    0x08, 0x95, 0x30, 0x81, 0x02, 0x85, 0x31, 0x09, 0x31, 0x75, 0x08, 0x96,
    0x69, 0x01, 0x81, 0x02, 0x85, 0x32, 0x09, 0x32, 0x75, 0x08, 0x96, 0x69,
    0x01, 0x81, 0x02, 0x85, 0x33, 0x09, 0x33, 0x75, 0x08, 0x96, 0x69, 0x01,
    0x81, 0x02, 0x85, 0x3f, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39,
    0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x05, 0x09,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x09, 0x33, 0x09, 0x34, 0x16, 0x00, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0x06, 0x01, 0xff, 0x85, 0x01, 0x09,
    0x01, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02, 0x85, 0x10, 0x09, 0x10, 0x75,
    0x08, 0x95, 0x09, 0x91, 0x02, 0x85, 0x11, 0x09, 0x11, 0x75, 0x08, 0x95,
    0x30, 0x91, 0x02, 0x85, 0x12, 0x09, 0x12, 0x75, 0x08, 0x95, 0x30, 0x91,
    0x02, 0xc0,
};

/* The controller's     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x02, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0x01, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3b, 0x34, 0x3b, 0x34, 0x3b, 0x34, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x00, 0x06, 0x60, 0x00, 0x08, 0x80, 0x00, 0x06, 0x60, 0x00, 0x08,
    0x80, 0x00, 0x06, 0x60, 0x00, 0x06, 0x60, 0xff, 0x32, 0x32, 0x32, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x50, 0xfd, 0x00, 0x00,
    0xc6, 0x0f, 0x0f, 0x30, 0x61, 0xae, 0x90, 0xd9, 0xd4, 0x14, 0x54, 0x41,
    0x15, 0x54, 0xc7, 0x79, 0x9c, 0x33, 0x36, 0x63, 0x0f, 0x30, 0x61, 0xae,
    0x90, 0xd9, 0xd4, 0x14, 0x54, 0x41, 0x15, 0x54, 0xc7, 0x79, 0x9c, 0x33,
    0x36, 0x63, flash from 0x6000: serial, IMU and stick factory
 * calibration, colours.  A byte outside the regions reads erased (0xff). */
static const u8 spi_window[170] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x02, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0x01, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3b, 0x34, 0x3b, 0x34, 0x3b, 0x34, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x00, 0x06, 0x60, 0x00, 0x08, 0x80, 0x00, 0x06, 0x60, 0x00, 0x08,
    0x80, 0x00, 0x06, 0x60, 0x00, 0x06, 0x60, 0xff, 0x32, 0x32, 0x32, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x50, 0xfd, 0x00, 0x00,
    0xc6, 0x0f, 0x0f, 0x30, 0x61, 0xae, 0x90, 0xd9, 0xd4, 0x14, 0x54, 0x41,
    0x15, 0x54, 0xc7, 0x79, 0x9c, 0x33, 0x36, 0x63, 0x0f, 0x30, 0x61, 0xae,
    0x90, 0xd9, 0xd4, 0x14, 0x54, 0x41, 0x15, 0x54, 0xc7, 0x79, 0x9c, 0x33,
    0x36, 0x63,
};
static const struct { u16 address; u8 length; } spi_regions[] = {
    { 0x6000, 32 }, { 0x6020, 24 }, { 0x603d, 18 }, { 0x6050, 12 },
    { 0x6080, 6 }, { 0x6086, 18 }, { 0x6098, 18 },
};

enum {
    JC_Y = 1 << 0, JC_X = 1 << 1, JC_B = 1 << 2, JC_A = 1 << 3, JC_R = 1 << 6, JC_ZR = 1 << 7,
    JC_MINUS = 1 << 8, JC_PLUS = 1 << 9, JC_RSTICK = 1 << 10, JC_LSTICK = 1 << 11,
    JC_HOME = 1 << 12, JC_CAP = 1 << 13, JC_DOWN = 1 << 16, JC_UP = 1 << 17,
    JC_RIGHT = 1 << 18, JC_LEFT = 1 << 19, JC_L = 1 << 22, JC_ZL = 1 << 23,
};

static struct {
    struct pad pad;              /* latest, for subcommand replies */
    u8 address[6];
    u8 mode;
    u8 timer;
    u8 rumble_on, imu_on, stopped;
    u8 rumble[2];
    u8 players, players_flash, has_players;
    u32 simple_sent;
} sw;

static u8 spi_byte(u32 a)
{
    u32 i;

    for (i = 0; i < sizeof(spi_regions) / sizeof(spi_regions[0]); i++)
        if (a >= spi_regions[i].address && a - spi_regions[i].address < spi_regions[i].length)
            return spi_window[a - 0x6000];
    return 0xff;
}

static u16 axis12(s16 v)
{
    s32 s = (s32)v * 0x600;

    s += s < 0 ? -(STICK_MAX / 2) : STICK_MAX / 2;
    return (u16)(0x800 + s / STICK_MAX);
}

static void stick(u8 *out, s16 x, s16 y)
{
    u16 px = axis12(x), py = axis12(y);

    out[0] = (u8)px;
    out[1] = (u8)(px >> 8 | py << 4);
    out[2] = (u8)(py >> 4);
}

/* The 12-byte block every report starts with.  0x8e: battery full, not
 * charging, Bluetooth (0x81 would announce a wired controller). */
static void state(u8 *s)
{
    static const u32 hat_bits[HAT_NEUTRAL] = {
        JC_UP, JC_UP | JC_RIGHT, JC_RIGHT, JC_DOWN | JC_RIGHT, JC_DOWN, JC_DOWN | JC_LEFT,
        JC_LEFT, JC_UP | JC_LEFT,
    };
    const struct pad *p = &sw.pad;
    u32 b = 0;

    memset(s, 0, 12);
    sw.timer += 3;
    s[0] = sw.timer;
    s[1] = 0x8e;
    /* Nintendo positions: the pad's printed labels are Xbox-ordered. */
    if (p->buttons & PAD_X) b |= JC_Y;
    if (p->buttons & PAD_Y) b |= JC_X;
    if (p->buttons & PAD_A) b |= JC_B;
    if (p->buttons & PAD_B) b |= JC_A;
    if (p->buttons & PAD_L1) b |= JC_L;
    if (p->buttons & PAD_R1) b |= JC_R;
    if (p->l2 > TRIGGER_BUTTON) b |= JC_ZL;
    if (p->r2 > TRIGGER_BUTTON) b |= JC_ZR;
    if (p->buttons & PAD_SELECT) b |= JC_MINUS;
    if (p->buttons & PAD_START) b |= JC_PLUS;
    if (p->buttons & PAD_L3) b |= JC_LSTICK;
    if (p->buttons & PAD_R3) b |= JC_RSTICK;
    if (p->buttons & PAD_HOME) b |= JC_HOME;
    if (p->buttons & PAD_SCREENSHOT) b |= JC_CAP;
    if (p->hat < HAT_NEUTRAL) b |= hat_bits[p->hat];
    s[2] = (u8)b;
    s[3] = (u8)(b >> 8);
    s[4] = (u8)(b >> 16);
    stick(s + 5, p->lx, (s16)-p->ly);
    stick(s + 8, p->rx, (s16)-p->ry);
}

static void simple(u8 *r)
{
    const struct pad *p = &sw.pad;

    memset(r, 0, SIMPLE_SIZE);
    if (p->buttons & PAD_A) r[0] |= 0x01;
    if (p->buttons & PAD_B) r[0] |= 0x02;
    if (p->buttons & PAD_X) r[0] |= 0x04;
    if (p->buttons & PAD_Y) r[0] |= 0x08;
    if (p->buttons & PAD_L1) r[0] |= 0x10;
    if (p->buttons & PAD_R1) r[0] |= 0x20;
    if (p->l2 > TRIGGER_BUTTON) r[0] |= 0x40;
    if (p->r2 > TRIGGER_BUTTON) r[0] |= 0x80;
    if (p->buttons & PAD_SELECT) r[1] |= 0x01;
    if (p->buttons & PAD_START) r[1] |= 0x02;
    if (p->buttons & PAD_R3) r[1] |= 0x04;
    if (p->buttons & PAD_L3) r[1] |= 0x08;
    if (p->buttons & PAD_HOME) r[1] |= 0x10;
    if (p->buttons & PAD_SCREENSHOT) r[1] |= 0x20;
    r[2] = p->hat;
    util_put16(r + 3, (u16)(p->lx + 32768));
    util_put16(r + 5, (u16)(p->ly + 32768));
    util_put16(r + 7, (u16)(p->rx + 32768));
    util_put16(r + 9, (u16)(p->ry + 32768));
}

/* One motor from a 4-byte HD rumble field: the high-band amplitude as the
 * strength; ERM motors cannot play the two carriers.  00 01 40 40 is off. */
static u8 strength(const u8 *d)
{
    u8 amp = d[1] & 0xfe;

    if (!amp && d[3] == 0x40) return 0;
    return (u8)(amp * 255u / 0xc8);
}

static void rumble(const u8 *d)
{
    sw.rumble[0] = sw.rumble_on ? strength(d) : 0;
    sw.rumble[1] = sw.rumble_on ? strength(d + 4) : 0;
}

static void reply(u8 cmd, const u8 *arg, u16 n)
{
    static const u8 name[24] = { 0x00, 0x25, 0x08, 'P', 'r', 'o', ' ', 'C', 'o', 'n', 't', 'r',
                                 'o', 'l', 'l', 'e', 'r', 0, 0, 0, 0, 0, 0, 0x68 };
    u8 out[FULL_SIZE], *ack = out + 12, *d = out + 14;
    u32 i;

    memset(out, 0, sizeof(out));
    state(out);
    ack[0] = cmd == 0x10 ? 0x90 : 0x80;
    ack[1] = cmd;
    switch (cmd) {
    case 0x01:  /* pairing: phase 1 answers with our address, little-endian */
        if (n) {
            ack[0] = 0x81;
            d[0] = arg[0];
            if (arg[0] == 1) {
                memcpy(d + 1, sw.address, 6);
                memcpy(d + 7, name, sizeof(name));
            }
        }
        break;
    case 0x02:  /* device info: firmware 3.48, Pro Controller, address big-endian */
        ack[0] = 0x82;
        d[0] = 0x03;
        d[1] = 0x48;
        d[2] = 0x03;
        d[3] = 0x02;
        for (i = 0; i < 6; i++) d[4 + i] = sw.address[5 - i];
        d[10] = d[11] = 0x01;
        break;
    case 0x03:  /* report mode */
        if (n && (arg[0] == REPORT_SIMPLE || arg[0] == REPORT_FULL)) sw.mode = arg[0];
        break;
    case 0x04:  /* trigger buttons elapsed time */
        ack[0] = 0x83;
        for (i = 0; i < 14; i += 2) util_put16(d + i, 100);
        break;
    case 0x06:  /* HCI state: 0 = the console disconnects us */
        if (n && arg[0] == 0) sw.stopped = 1;
        break;
    case 0x10:  /* SPI read: address (32 bits) and length echoed, then the bytes */
        if (n >= 5) {
            u32 a = util_get32(arg);
            u8 len = arg[4] > 29 ? 29 : arg[4];

            memcpy(d, arg, 5);
            d[4] = len;
            for (i = 0; i < len; i++) d[5 + i] = spi_byte(a + i);
        }
        break;
    case 0x30:  /* player lights: low nibble on, high nibble flashing */
        if (n) {
            sw.has_players = 1;
            sw.players = arg[0] & 0xf;
            sw.players_flash = arg[0] >> 4;
        }
        break;
    case 0x40:
        sw.imu_on = n && arg[0];
        break;
    case 0x48:
        sw.rumble_on = n && arg[0];
        if (!sw.rumble_on) sw.rumble[0] = sw.rumble[1] = 0;
        break;
    }
    bt_send_message(REPORT_REPLY, out, sizeof(out));
}

static u8 begin(const struct profile_session *s)
{
    memset(&sw, 0, sizeof(sw));
    sw.pad.hat = HAT_NEUTRAL;
    /* Full reports from the start: a console that opens the channels and
     * sees only sparse 0x3f reports drops the link within 300 ms. */
    sw.mode = REPORT_FULL;
    memcpy(sw.address, s->address, 6);
    return IMU_OFF;
}

static int report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    int changed = memcmp(&sw.pad, p, sizeof(*p));

    sw.pad = *p;
    if (sw.stopped) return 0;
    if (sw.mode == REPORT_FULL) {
        memset(out, 0, FULL_SIZE);
        state(out);
        *id = REPORT_FULL;
        return FULL_SIZE;
    }
    if (!changed && now - sw.simple_sent < KEEPALIVE_MS) return 0;
    sw.simple_sent = now;
    simple(out);
    *id = REPORT_SIMPLE;
    return SIMPLE_SIZE;
}

static int write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    (void)now;
    (void)bond;
    if (w->kind != BT_WRITE_OUTPUT) return 0;
    if (w->id == OUT_RUMBLE && w->len >= 9) rumble(w->data + 1);
    if (w->id == OUT_SUBCMD && w->len >= 10) {
        rumble(w->data + 1);
        reply(w->data[9], w->data + 10, w->len - 10);
    }
    return 0;
}

static void host(u32 now, struct profile_host *h)
{
    (void)now;
    h->rumble[0] = sw.rumble[0];
    h->rumble[1] = sw.rumble[1];
    h->has_players = sw.has_players;
    h->players = sw.players;
    h->players_flash = sw.players_flash;
    h->imu = sw.imu_on ? IMU_MOTION : IMU_OFF;
}

static const struct bt_device device = {
    .transport = BT_CLASSIC,
    .name = "Pro Controller",
    .class_of_device = 0x002508,
    .vendor_id = 0x057e, .product_id = 0x2009, .version = 0x0001,
    .hid_descriptor = descriptor, .hid_descriptor_len = sizeof(descriptor),
    .sniff_slots = 24, .sniff_attempt = 1,
};

const struct profile profile_switch = {
    .name = "switch",
    .report_hz = 66,
    .device = &device,
    .begin = begin,
    .report = report,
    .write = write,
    .host = host,
};

#ifdef UNITTEST
static u8 sent[64];
static u16 sent_len;
int bt_send_message(u8 id, const u8 *data, u16 len)
{
    sent[0] = id;
    memcpy(sent + 1, data, len);
    sent_len = len;
    return 1;
}
void util_put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
u32 util_get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

int main(void)
{
    struct profile_session s = { 0, 0, { 1, 2, 3, 4, 5, 6 }, 0, 0 };
    struct pad p;
    struct bt_write w;
    struct profile_host h;
    u8 out[64], id;

    CHECK(begin(&s) == IMU_OFF);
    memset(&p, 0, sizeof(p));
    p.hat = HAT_NEUTRAL;
    p.buttons = PAD_A;
    p.lx = STICK_MAX;
    CHECK(report(&p, 0, out, &id) == FULL_SIZE && id == REPORT_FULL);
    CHECK(out[1] == 0x8e && out[2] == JC_B);
    CHECK((out[5] | (out[6] & 0xf) << 8) == 0x800 + 0x600);

    memset(&w, 0, sizeof(w));
    w.kind = BT_WRITE_OUTPUT;
    w.id = OUT_SUBCMD;
    w.len = 15;
    w.data[9] = 0x10;                       /* SPI read 0x603d, 18 bytes */
    w.data[10] = 0x3d; w.data[11] = 0x60; w.data[14] = 18;
    write(&w, 0, 0);
    CHECK(sent[0] == REPORT_REPLY && sent[13] == 0x90 && sent[14] == 0x10);
    CHECK(sent[1 + 14 + 4] == 18 && sent[1 + 14 + 5] == spi_window[0x3d]);

    w.data[9] = 0x02;
    write(&w, 0, 0);
    CHECK(sent[13] == 0x82 && sent[1 + 14 + 4] == 6 && sent[1 + 14 + 9] == 1);

    w.data[9] = 0x30; w.data[10] = 0x21; w.len = 11;
    write(&w, 0, 0);
    host(0, &h);
    CHECK(h.has_players && h.players == 1 && h.players_flash == 2);

    w.data[9] = 0x48; w.data[10] = 1;
    write(&w, 0, 0);
    w.id = OUT_RUMBLE; w.len = 9;
    w.data[2] = 0xc8; w.data[4] = 0x40;
    write(&w, 0, 0);
    host(0, &h);
    CHECK(h.rumble[0] == 255 && h.rumble[1] == 0);

    w.id = OUT_SUBCMD; w.len = 11; w.data[9] = 0x03; w.data[10] = REPORT_SIMPLE;
    write(&w, 0, 0);
    CHECK(report(&p, 1000, out, &id) == SIMPLE_SIZE && id == REPORT_SIMPLE && out[0] == 1);
    CHECK(report(&p, 1010, out, &id) == 0);
    CHECK(report(&p, 1200, out, &id) == SIMPLE_SIZE);
    return TEST_DONE("profile_switch");
}
#endif

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
#define OUT_USB       0x80       /* USB only: the cable's own handshake */
#define REPORT_USB    0x81       /* its answers */
#define USB_SIZE      63         /* every report on USB, after its id */
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

/* The wired Pro Controller's descriptor (as GP2040-CE reproduces it): every
 * report 63 bytes after its id. */
static const u8 usb_descriptor[203] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0a, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0a, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0b, 0x29, 0x0e, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0b, 0x01, 0x00, 0x01, 0x00, 0xa1, 0x00, 0x0b, 0x30, 0x00,
    0x01, 0x00, 0x0b, 0x31, 0x00, 0x01, 0x00, 0x0b, 0x32, 0x00, 0x01, 0x00, 0x0b, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xc0, 0x0b,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19, 0x0f, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x34, 0x81, 0x03, 0x06, 0x00, 0xff, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3f, 0x81, 0x03, 0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3f,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3f, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3f, 0x91, 0x83, 0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3f, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3f, 0x91, 0x83, 0xc0,
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
    u8 usb;
    u8 rumble[2];
    u8 players, players_flash, has_players;
    u32 simple_sent;
    u32 now;                     /* ms, from the last report() or write() */
    struct imu_sample motion[3]; /* the newest three samples, newest first */
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
    /* Time in 5 ms units, the sample period: it advances by one per new
     * motion sample, as the stock firmware's does, and between reports
     * that carry no new sample it stays. */
    s[0] = (u8)(sw.now / 5);
    s[1] = sw.usb ? 0x81 : 0x8e;
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
    u8 out[USB_SIZE], *ack = out + 12, *d = out + 14;
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
    case 0x06:  /* HCI state: 0 = end this connection */
        if (n) util_log("switch: host HCI state %u", arg[0]);
        if (n && arg[0] == 0) {
            sw.stopped = 1;
            return; /* The host is ending the link; do not queue a reply. */
        }
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
    host_send(REPORT_REPLY, out, sw.usb ? USB_SIZE : FULL_SIZE);
}

/* The USB handshake: status (the controller type and our address, big-
 * endian), handshake and baud rate answered; "HID only" and the rest not. */
static void usb_handshake(u8 cmd)
{
    u8 out[USB_SIZE];
    u32 i;

    memset(out, 0, sizeof(out));
    out[0] = cmd;
    if (cmd == 0x01) {
        out[2] = 0x03;
        for (i = 0; i < 6; i++) out[3 + i] = sw.address[5 - i];
    }
    if (cmd <= 0x03) host_send(REPORT_USB, out, sizeof(out));
}

static u8 begin(const struct profile_session *s)
{
    memset(&sw, 0, sizeof(sw));
    sw.usb = s->usb;
    sw.pad.hat = HAT_NEUTRAL;
    /* Full reports from the start: a console that opens the channels and
     * sees only sparse 0x3f reports drops the link within 300 ms. */
    sw.mode = REPORT_FULL;
    memcpy(sw.address, s->address, 6);
    return IMU_OFF;
}

/* The three newest motion samples, newest first (SDL's HIDAPI driver reads
 * slot 0 as the newest, as the stock firmware sends it), a sliding window: a
 * report with one new sample shifts the other two along, one with none
 * repeats them.  Accelerometer halved, accelerometer x and gyro x and y
 * negated: the Pro Controller's axes. */
static void motion(u8 *out, const struct pad *p)
{
    u32 k, a;

    for (k = 0; k < p->imu_count; k++) {         /* p->imu is oldest first */
        sw.motion[2] = sw.motion[1];
        sw.motion[1] = sw.motion[0];
        sw.motion[0] = p->imu[k];
    }
    for (k = 0; k < 3; k++) {
        const struct imu_sample *m = &sw.motion[k];

        for (a = 0; a < 3; a++) {
            util_put16(out + 12 * k + 2 * a, (u16)(a == 0 ? -m->accel[a] / 2 : m->accel[a] / 2));
            util_put16(out + 12 * k + 6 + 2 * a, (u16)(a < 2 ? -m->gyro[a] : m->gyro[a]));
        }
    }
}

static int report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    int changed = memcmp(&sw.pad, p, PAD_CONTROLS_SIZE);

    sw.now = now;
    sw.pad = *p;
    if (sw.stopped) return 0;
    if (sw.mode == REPORT_FULL || sw.usb) {
        memset(out, 0, USB_SIZE);
        state(out);
        if (sw.imu_on) motion(out + 12, p);
        *id = REPORT_FULL;
        return sw.usb ? USB_SIZE : FULL_SIZE;
    }
    if (!changed && now - sw.simple_sent < KEEPALIVE_MS) return 0;
    sw.simple_sent = now;
    simple(out);
    *id = REPORT_SIMPLE;
    return SIMPLE_SIZE;
}

static int write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    (void)bond;
    sw.now = now;
    if (w->kind != BT_WRITE_OUTPUT) return 0;
    if (w->id == OUT_USB && w->len >= 1) usb_handshake(w->data[0]);
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
    h->ended = sw.stopped;
}

static const struct bt_device device = {
    .transport = BT_CLASSIC,
    .name = "Pro Controller",
    .class_of_device = 0x002508,
    .vendor_id = 0x057e, .product_id = 0x2009, .version = 0x0001,
    .hid_descriptor = descriptor, .hid_descriptor_len = sizeof(descriptor),
    .sniff_slots = 24, .sniff_attempt = 1,
};

static const struct usb_identity usb = {
    .vendor_id = 0x057e, .product_id = 0x2009, .bcd_device = 0x0210,
    .manufacturer = "Nintendo Co., Ltd.", .product = "Pro Controller", .serial = "000000000001",
    .kind = USB_HID, .report_ids = 1, .interval = 8, .in_size = 64, .out_size = 64,
    .max_power = 250, .hid_descriptor = usb_descriptor, .hid_descriptor_len = sizeof(usb_descriptor),
};

const struct profile profile_switch = {
    .name = "switch",
    .device = &device,
    .usb = &usb,
    .begin = begin,
    .report = report,
    .write = write,
    .host = host,
#if !defined(UNITTEST) && !defined(BOARD_DONGLE)
    .wake = &profile_switch_wake,
#endif
};

/* ---- Switch 2 Pro Controller, over LE ------------------------------------------
 * Commands written to 0x0014/0x0016 are answered with notifications on
 * 0x001e.  Pairing is the console's own key exchange (command 0x15), not
 * SMP.  Byte layouts: ndeadly's Pro Controller 2 capture. */

#define S2_REPORT_SIZE   63
#define S2_CMD_PREFIX    0x21    /* vibration data in front of a command on 0x0016 */
#define S2_RESP_PREFIX   0x0e
#define S2_MAX_DATA      0x78
#define S2_INFO_1        0x0003
#define S2_INFO_2        0x0007
#define S2_INPUT         0x000e
#define S2_VIBRATION     0x0012
#define S2_COMMAND       0x0014
#define S2_VIB_COMMAND   0x0016
#define S2_RESPONSE      0x001e

static const u8 s2_att_db[] = {
    0x01, 0x18, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x80, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51,
    0x8f, 0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00, 0x1b, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03,
    0x28, 0x02, 0x03, 0x00, 0x81, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f, 0x30, 0x4e, 0x64, 0x19,
    0x5d, 0xaf, 0xc5, 0x00, 0x16, 0x00, 0x02, 0x03, 0x03, 0x00, 0x81, 0xd2, 0x6b, 0xf9, 0x56, 0x19,
    0x51, 0x8f, 0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00, 0x1b, 0x00, 0x02, 0x00, 0x04, 0x00,
    0x03, 0x28, 0x08, 0x05, 0x00, 0x82, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f, 0x30, 0x4e, 0x64,
    0x19, 0x5d, 0xaf, 0xc5, 0x00, 0x16, 0x00, 0x08, 0x03, 0x05, 0x00, 0x82, 0xd2, 0x6b, 0xf9, 0x56,
    0x19, 0x51, 0x8f, 0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00, 0x1b, 0x00, 0x02, 0x00, 0x06,
    0x00, 0x03, 0x28, 0x02, 0x07, 0x00, 0x83, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f, 0x30, 0x4e,
    0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00, 0x16, 0x00, 0x02, 0x03, 0x07, 0x00, 0x83, 0xd2, 0x6b, 0xf9,
    0x56, 0x19, 0x51, 0x8f, 0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00, 0x18, 0x00, 0x02, 0x00,
    0x08, 0x00, 0x00, 0x28, 0xd0, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe, 0x89,
    0xbe, 0xe9, 0x7d, 0xab, 0x1b, 0x00, 0x02, 0x00, 0x09, 0x00, 0x03, 0x28, 0x12, 0x0a, 0x00, 0xd2,
    0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab, 0x16,
    0x00, 0x12, 0x03, 0x0a, 0x00, 0xd2, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe,
    0x89, 0xbe, 0xe9, 0x7d, 0xab, 0x08, 0x00, 0x0e, 0x01, 0x0b, 0x00, 0x02, 0x29, 0x16, 0x00, 0x0e,
    0x03, 0x0c, 0x00, 0xcb, 0x6e, 0x48, 0x80, 0xdf, 0x95, 0x57, 0x95, 0xee, 0x4d, 0x24, 0x5a, 0x10,
    0x55, 0x9d, 0x67, 0x1b, 0x00, 0x02, 0x00, 0x0d, 0x00, 0x03, 0x28, 0x12, 0x0e, 0x00, 0xf8, 0xc0,
    0xfc, 0x5f, 0x75, 0x32, 0x58, 0x82, 0x19, 0x46, 0x3e, 0xec, 0x6c, 0x86, 0x92, 0x74, 0x16, 0x00,
    0x12, 0x03, 0x0e, 0x00, 0xf8, 0xc0, 0xfc, 0x5f, 0x75, 0x32, 0x58, 0x82, 0x19, 0x46, 0x3e, 0xec,
    0x6c, 0x86, 0x92, 0x74, 0x08, 0x00, 0x0e, 0x01, 0x0f, 0x00, 0x02, 0x29, 0x16, 0x00, 0x0e, 0x03,
    0x10, 0x00, 0xcb, 0x6e, 0x48, 0x80, 0xdf, 0x95, 0x57, 0x95, 0xee, 0x4d, 0x24, 0x5a, 0x10, 0x55,
    0x9d, 0x67, 0x1b, 0x00, 0x02, 0x00, 0x11, 0x00, 0x03, 0x28, 0x04, 0x12, 0x00, 0x05, 0x2b, 0xf7,
    0x31, 0x0c, 0x63, 0x39, 0xa9, 0x7d, 0x42, 0x58, 0x92, 0x51, 0x3f, 0x48, 0xcc, 0x16, 0x00, 0x04,
    0x03, 0x12, 0x00, 0x05, 0x2b, 0xf7, 0x31, 0x0c, 0x63, 0x39, 0xa9, 0x7d, 0x42, 0x58, 0x92, 0x51,
    0x3f, 0x48, 0xcc, 0x1b, 0x00, 0x02, 0x00, 0x13, 0x00, 0x03, 0x28, 0x04, 0x14, 0x00, 0x05, 0xf0,
    0xe5, 0x4f, 0xa5, 0x1e, 0x44, 0xaf, 0x6c, 0x4e, 0xb7, 0x8e, 0xc9, 0x4a, 0x9d, 0x64, 0x16, 0x00,
    0x04, 0x03, 0x14, 0x00, 0x05, 0xf0, 0xe5, 0x4f, 0xa5, 0x1e, 0x44, 0xaf, 0x6c, 0x4e, 0xb7, 0x8e,
    0xc9, 0x4a, 0x9d, 0x64, 0x1b, 0x00, 0x02, 0x00, 0x15, 0x00, 0x03, 0x28, 0x04, 0x16, 0x00, 0x79,
    0xb3, 0xe8, 0x09, 0x98, 0x6f, 0xaf, 0x8e, 0xb5, 0x40, 0x55, 0x69, 0x7e, 0xbc, 0xac, 0x3d, 0x16,
    0x00, 0x04, 0x03, 0x16, 0x00, 0x79, 0xb3, 0xe8, 0x09, 0x98, 0x6f, 0xaf, 0x8e, 0xb5, 0x40, 0x55,
    0x69, 0x7e, 0xbc, 0xac, 0x3d, 0x1b, 0x00, 0x02, 0x00, 0x17, 0x00, 0x03, 0x28, 0x04, 0x18, 0x00,
    0x8d, 0x9f, 0xf5, 0x5d, 0x3e, 0xd2, 0xf7, 0xa4, 0xf7, 0x4d, 0xae, 0xfd, 0x3d, 0x42, 0x47, 0x41,
    0x16, 0x00, 0x04, 0x03, 0x18, 0x00, 0x8d, 0x9f, 0xf5, 0x5d, 0x3e, 0xd2, 0xf7, 0xa4, 0xf7, 0x4d,
    0xae, 0xfd, 0x3d, 0x42, 0x47, 0x41, 0x1b, 0x00, 0x02, 0x00, 0x19, 0x00, 0x03, 0x28, 0x10, 0x1a,
    0x00, 0x6a, 0x83, 0x11, 0xb1, 0x15, 0x53, 0x0a, 0xa2, 0x36, 0x4d, 0xd8, 0xd9, 0x61, 0xa9, 0x65,
    0xc7, 0x16, 0x00, 0x10, 0x03, 0x1a, 0x00, 0x6a, 0x83, 0x11, 0xb1, 0x15, 0x53, 0x0a, 0xa2, 0x36,
    0x4d, 0xd8, 0xd9, 0x61, 0xa9, 0x65, 0xc7, 0x08, 0x00, 0x0e, 0x01, 0x1b, 0x00, 0x02, 0x29, 0x16,
    0x00, 0x0e, 0x03, 0x1c, 0x00, 0x79, 0xf9, 0xa4, 0xed, 0xbb, 0xe3, 0xd2, 0x9c, 0x5b, 0x49, 0x58,
    0xf3, 0x8c, 0xdf, 0x46, 0xb7, 0x1b, 0x00, 0x02, 0x00, 0x1d, 0x00, 0x03, 0x28, 0x10, 0x1e, 0x00,
    0xe0, 0x57, 0x76, 0xa7, 0x6b, 0x32, 0x49, 0xa5, 0x95, 0x4e, 0x78, 0x42, 0x7d, 0x9f, 0x6d, 0x50,
    0x16, 0x00, 0x10, 0x03, 0x1e, 0x00, 0xe0, 0x57, 0x76, 0xa7, 0x6b, 0x32, 0x49, 0xa5, 0x95, 0x4e,
    0x78, 0x42, 0x7d, 0x9f, 0x6d, 0x50, 0x08, 0x00, 0x0e, 0x01, 0x1f, 0x00, 0x02, 0x29, 0x16, 0x00,
    0x0e, 0x03, 0x20, 0x00, 0x79, 0xf9, 0xa4, 0xed, 0xbb, 0xe3, 0xd2, 0x9c, 0x5b, 0x49, 0x58, 0xf3,
    0x8c, 0xdf, 0x46, 0xb7, 0x1b, 0x00, 0x02, 0x00, 0x21, 0x00, 0x03, 0x28, 0x10, 0x22, 0x00, 0x80,
    0x2a, 0x6d, 0x40, 0x6f, 0xf8, 0x15, 0xab, 0x41, 0x42, 0x1c, 0x84, 0xd2, 0x69, 0xbd, 0xd3, 0x16,
    0x00, 0x10, 0x03, 0x22, 0x00, 0x80, 0x2a, 0x6d, 0x40, 0x6f, 0xf8, 0x15, 0xab, 0x41, 0x42, 0x1c,
    0x84, 0xd2, 0x69, 0xbd, 0xd3, 0x08, 0x00, 0x0e, 0x01, 0x23, 0x00, 0x02, 0x29, 0x16, 0x00, 0x0e,
    0x03, 0x24, 0x00, 0x79, 0xf9, 0xa4, 0xed, 0xbb, 0xe3, 0xd2, 0x9c, 0x5b, 0x49, 0x58, 0xf3, 0x8c,
    0xdf, 0x46, 0xb7, 0x1b, 0x00, 0x02, 0x00, 0x25, 0x00, 0x03, 0x28, 0x12, 0x26, 0x00, 0xde, 0x7f,
    0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab, 0x16, 0x00,
    0x12, 0x03, 0x26, 0x00, 0xde, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe, 0x89,
    0xbe, 0xe9, 0x7d, 0xab, 0x08, 0x00, 0x0e, 0x01, 0x27, 0x00, 0x02, 0x29, 0x16, 0x00, 0x0e, 0x03,
    0x28, 0x00, 0xcb, 0x6e, 0x48, 0x80, 0xdf, 0x95, 0x57, 0x95, 0xee, 0x4d, 0x24, 0x5a, 0x10, 0x55,
    0x9d, 0x67, 0x1b, 0x00, 0x02, 0x00, 0x29, 0x00, 0x03, 0x28, 0x04, 0x2a, 0x00, 0xdf, 0x7f, 0xdf,
    0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab, 0x16, 0x00, 0x04,
    0x03, 0x2a, 0x00, 0xdf, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe, 0x89, 0xbe,
    0xe9, 0x7d, 0xab, 0x0a, 0x00, 0x02, 0x00, 0x2b, 0x00, 0x00, 0x28, 0x00, 0x18, 0x0d, 0x00, 0x02,
    0x00, 0x2c, 0x00, 0x03, 0x28, 0x02, 0x2d, 0x00, 0x00, 0x2a, 0x16, 0x00, 0x02, 0x00, 0x2d, 0x00,
    0x00, 0x2a, 0x50, 0x72, 0x6f, 0x20, 0x43, 0x6f, 0x6e, 0x74, 0x72, 0x6f, 0x6c, 0x6c, 0x65, 0x72,
    0x0d, 0x00, 0x02, 0x00, 0x2e, 0x00, 0x03, 0x28, 0x02, 0x2f, 0x00, 0x01, 0x2a, 0x0a, 0x00, 0x02,
    0x00, 0x2f, 0x00, 0x01, 0x2a, 0xc4, 0x03, 0x0a, 0x00, 0x02, 0x00, 0x30, 0x00, 0x00, 0x28, 0x01,
    0x18, 0x00, 0x00,
};

static const u8 s2_key_b1[16] = {
    0x5c, 0xf6, 0xee, 0x79, 0x2c, 0xdf, 0x05, 0xe1, 0xba, 0x2b, 0x63, 0x25, 0xc4, 0x1a, 0x5f, 0x10,
};

/* The console reads this "flash": device info, IMU and stick calibration. */
static const struct { u32 address; u8 len; const u8 *data; } s2_flash[] = {
    { 0x13000, 37, (const u8 *)"\x01\x00" "CYCLONE2-SW2\x00\x00\x00\x00\x7e\x05\x69\x20\x01\x06\x01"
                               "\x23\x23\x23\xa0\xa0\xa0\xe6\xe6\xe6\x32\x32\x32" },
    { 0x13040, 16, (const u8 *)"\x3b\xe0\xd3\x41\xc6\x60\x6a\xbc\x4d\xd7\xa2\xbb\x71\x1e\xdd\x37" },
    { 0x13080, 40, (const u8 *)"\x01\xad\xd9\x9a\x55\x56\x65\xa0\x00\x0a\xa0\x00\x0a\xe2\x20\x0e"
                               "\xe2\x20\x0e\x9a\xad\xd9\x9a\xad\xd9\x0a\xa5\x50\x0a\xa5\x50\x2f"
                               "\xf6\x62\x2f\xf6\x62\x0a\xff\xff" },
    { 0x130a8, 9, (const u8 *)"\xb3\x67\x83\x2e\x66\x5e\x3a\x06\x5f" },
    { 0x130c0, 40, (const u8 *)"\x01\xad\xd9\x9a\x55\x56\x65\xa0\x00\x0a\xa0\x00\x0a\xe2\x20\x0e"
                               "\xe2\x20\x0e\x9a\xad\xd9\x9a\xad\xd9\x0a\xa5\x50\x0a\xa5\x50\x2f"
                               "\xf6\x62\x2f\xf6\x62\x0a\xff\xff" },
    { 0x130e8, 9, (const u8 *)"\x2c\x08\x84\xd1\x65\x63\x2a\x26\x62" },
    { 0x13100, 24, (const u8 *)"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                               "\xa6\xf2\x62\xbd\xa8\x00\x08\x3d\x2f\xed\x20\x41" },
};

static struct {
    u8 counter, ready;
    u8 address[6];
    u8 ltk[16];                  /* A1 ^ B1, byte-reversed, as the AES uses it */
    u8 ltk_valid;
    u8 rumble[2];
    u8 pulse;
    u32 pulse_until;
    u8 feature_mask, features;   /* command 0x0c; bit 2 is motion */
    u8 has_players, players, players_flash;
    s32 q[4];                    /* orientation from the gyro, w x y z, Q30 */
    u32 last_us;
    u16 sample_prev;
    u8 have_last;
    u8 usb;
    struct imu_sample motion;    /* USB: the newest sample */
} s2;

#define S2_FEATURE_MOTION 0x04

static void s2_flash_read(u32 a, u8 *out, u16 n)
{
    u32 i, k;

    memset(out, 0xff, n);
    for (i = 0; i < sizeof(s2_flash) / sizeof(s2_flash[0]); i++)
        for (k = 0; k < n; k++)
            if (a + k >= s2_flash[i].address && a + k < s2_flash[i].address + s2_flash[i].len)
                out[k] = s2_flash[i].data[a + k - s2_flash[i].address];
}

static void s2_stick(u8 *out, s16 x, s16 y)
{
    s32 sx = (s32)x * 0x7ff, sy = (s32)y * 0x7ff;
    u16 px, py;

    sx += sx < 0 ? -(STICK_MAX / 2) : STICK_MAX / 2;
    sy += sy < 0 ? -(STICK_MAX / 2) : STICK_MAX / 2;
    px = (u16)(0x800 + sx / STICK_MAX);
    py = (u16)(0x800 + sy / STICK_MAX);
    out[0] = (u8)px;
    out[1] = (u8)(px >> 8 | py << 4);
    out[2] = (u8)(py >> 4);
}

/* ---- Switch 2 motion: report bytes 0x0e.. (block length, then the block) ---
 * Mode 12, the 30-byte block a Pro Controller 2 sends: its orientation as a
 * quaternion integrated from the gyro (starting level, drifting like the real
 * one), compressed to its three smallest components, and the accelerometer.
 * The console gets no gyro rates.  Format: cyclone2-esp32/s2motion/recordings/
 * NOTES.md (decoded with PIMU's layout and checked against Joy-Con 2 captures). */

#define S2_MODE_ORIENTATION 12
#define S2_ONE (1 << 30)         /* fixed point: no FPU on this chip (it is emulated, and slow) */
/* Half the rotation, in Q30 radians, for gyro in 1/256 dps (16 LSB per dps,
 * bias in 1/16 LSB) over dt us: pi / 180 / 256 / 1e6 / 2 * 2^30 = 38382 / 2^20. */
#define S2_HALF_ANGLE(raw, dt) ((s32)(((s64)(raw) * (s64)(dt) * 38382) >> 20))
#define S2_ACCEL_SHIFT 15        /* 8192 LSB per g to 1 g = 2^28 */

/* Our sensor axis for each Switch 2 axis, and its sign: x, y, z. */
static const s8 s2_axis[3] = { 2, -1, 3 };   /* x <- our y, y <- -our x: roll and pitch were swapped (2026-10-02) */

static s32 s2_pick(const s16 v[3], int k, s32 scale, s32 offset_a, const s32 *offset)
{
    int a = (s2_axis[k] < 0 ? -s2_axis[k] : s2_axis[k]) - 1;
    s32 r = v[a] * scale - (offset ? offset[a] : offset_a);

    return s2_axis[k] < 0 ? -r : r;
}

static s32 s2_mul(s32 a, s32 b)
{
    return (s32)(((s64)a * b) >> 30);
}

/* q <- q * (1, w dt / 2), then normalised (one Newton step: |q| stays near 1). */
static void s2_integrate(const struct imu_sample *m, const s32 bias[3])
{
    s32 w[3], a, b, c, d, n, f;
    u32 dt = m->us - s2.last_us;
    int k;

    if (!s2.have_last || dt > 20000) dt = 0;
    s2.last_us = m->us;
    s2.have_last = 1;
    for (k = 0; k < 3; k++) w[k] = S2_HALF_ANGLE(s2_pick(m->gyro, k, 16, 0, bias), dt);
    a = s2.q[0]; b = s2.q[1]; c = s2.q[2]; d = s2.q[3];
    s2.q[0] = a - s2_mul(b, w[0]) - s2_mul(c, w[1]) - s2_mul(d, w[2]);
    s2.q[1] = b + s2_mul(a, w[0]) + s2_mul(c, w[2]) - s2_mul(d, w[1]);
    s2.q[2] = c + s2_mul(a, w[1]) - s2_mul(b, w[2]) + s2_mul(d, w[0]);
    s2.q[3] = d + s2_mul(a, w[2]) + s2_mul(b, w[1]) - s2_mul(c, w[0]);
    n = s2_mul(s2.q[0], s2.q[0]) + s2_mul(s2.q[1], s2.q[1]) + s2_mul(s2.q[2], s2.q[2]) +
        s2_mul(s2.q[3], s2.q[3]);
    f = (3 << 29) - (n >> 1);
    for (k = 0; k < 4; k++) s2.q[k] = s2_mul(s2.q[k], f);
}

static void s2_put32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static s32 s2_abs(s32 v)
{
    return v < 0 ? -v : v;
}

static void s2_motion(u8 *out, const struct pad *p)
{
    s32 bias[3];
    u32 c[3];
    u16 sample, delta;
    int i, k, big = 0;
    u8 *b = out + 1;             /* out[0]: block length */

    imu_gyro_bias(bias);
    for (i = 0; i < p->imu_count; i++) s2_integrate(&p->imu[i], bias);
    sample = (u16)((s2.last_us / 1250) & 0xfff);
    delta = (u16)((sample - s2.sample_prev) & 0xfff);
    s2.sample_prev = sample;
    for (k = 1; k < 4; k++)
        if (s2_abs(s2.q[k]) > s2_abs(s2.q[big])) big = k;
    /* Each other component as its ratio to the largest, -1..1 mapped to 0..2^32. */
    for (k = 0; k < 4; k++) {
        s64 r;

        if (k == big) continue;
        r = ((s64)s2.q[k] << 31) / s2.q[big] + ((s64)1 << 31);
        c[(k + 3 - big) % 4] = r < 0 ? 0 : r > 0xffffffffLL ? 0xffffffffu : (u32)r;
    }
    out[0] = 30;
    memset(b, 0, 30);
    b[0] = (u8)sample;
    b[1] = (u8)((sample >> 8) | (delta << 4));
    b[2] = (u8)(delta >> 4);
    b[3] = S2_MODE_ORIENTATION;
    b[4] = (u8)big;
    b[5] = (u8)(c[0] >> 6); b[6] = (u8)(c[0] >> 14); b[7] = (u8)(c[0] >> 22);
    b[8] = (u8)((c[0] >> 30) | ((c[1] >> 6) & 1) << 7);
    b[9] = (u8)(c[1] >> 7); b[10] = (u8)(c[1] >> 15); b[11] = (u8)(c[1] >> 23);
    b[12] = (u8)((c[1] >> 31) | (c[2] & 0xc0));
    b[13] = (u8)(c[2] >> 8); b[14] = (u8)(c[2] >> 16); b[15] = (u8)(c[2] >> 24);
    if (p->imu_count) {
        const struct imu_sample *m = &p->imu[p->imu_count - 1];

        for (k = 0; k < 3; k++) s2_put32(b + 16 + 4 * k, (u32)s2_pick(m->accel, k, 1 << S2_ACCEL_SHIFT, 0, NULL));
    }
}

static int s2_report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    static const u32 hat_bits[HAT_NEUTRAL] = {
        0x0800, 0x0a00, 0x0200, 0x0300, 0x0100, 0x0500, 0x0400, 0x0c00,
    };
    u32 b = 0;

    (void)now;
    if (!s2.ready) return 0;
    memset(out, 0, S2_REPORT_SIZE);
    out[0] = ++s2.counter;
    out[1] = 0x20;
    /* By position: the pad's A (south) is the Switch's B. */
    if (p->buttons & PAD_A) b |= 0x0001;
    if (p->buttons & PAD_B) b |= 0x0002;
    if (p->buttons & PAD_X) b |= 0x0008;
    if (p->buttons & PAD_Y) b |= 0x0004;
    if (p->buttons & PAD_R1) b |= 0x0010;
    if (p->r2 > TRIGGER_BUTTON) b |= 0x0020;
    if (p->buttons & PAD_START) b |= 0x0040;
    if (p->buttons & PAD_R3) b |= 0x0080;
    if (p->buttons & PAD_L1) b |= 0x1000;
    if (p->l2 > TRIGGER_BUTTON) b |= 0x2000;
    if (p->buttons & PAD_SELECT) b |= 0x4000;
    if (p->buttons & PAD_L3) b |= 0x8000;
    if (p->buttons & PAD_HOME) b |= 0x010000;
    if (p->buttons & PAD_SCREENSHOT) b |= 0x020000;
    if (p->buttons & PAD_R4) b |= 0x040000;
    if (p->buttons & PAD_L4) b |= 0x080000;
    if (p->buttons & PAD_M) b |= 0x100000;      /* C */
    if (p->hat < HAT_NEUTRAL) b |= hat_bits[p->hat];
    out[2] = (u8)b;
    out[3] = (u8)(b >> 8);
    out[4] = (u8)(b >> 16);
    s2_stick(out + 5, p->lx, (s16)-p->ly);
    s2_stick(out + 8, p->rx, (s16)-p->ry);
    out[11] = 0x38;
    if (s2.features & S2_FEATURE_MOTION) s2_motion(out + 14, p);
    *id = 0;
    return S2_REPORT_SIZE;
}

/* One motor from a vibration block: the stronger band's amplitude. */
static u8 s2_amplitude(const u8 *block)
{
    const u8 *s = block + 1;
    u16 high = (u16)((s[2] & 0x0f) << 12 | (s[1] & 0xfc) << 4);
    u16 low = (u16)(s[4] << 8 | (s[3] & 0xc0));
    u16 m = high > low ? high : low;

    return m >= 29000 ? 255 : (u8)(m * 255u / 29000);
}

static void s2_rumble(const u8 *d, u16 n)
{
    if (n < S2_CMD_PREFIX) return;
    s2.rumble[0] = s2_amplitude(d + 0x01);
    s2.rumble[1] = s2_amplitude(d + 0x11);
}

/* The data part of a command's answer; returns its length. */
static u8 s2_command(const u8 *c, u16 n, u8 *out, u32 now, int *bonded)
{
    static const u8 firmware[12] = { 0x02, 0x01, 0x04, 0x02, 0x0c, 0, 0, 0, 0, 0x02, 0x03, 0 };
    static const u8 unknown_1103[29] = {
        0x01, 0x20, 0x03, 0x00, 0x00, 0x0a, 0xe8, 0x1c, 0x3b, 0x79, 0x7d, 0x8b, 0x3a, 0x0a, 0xe8,
        0x9c, 0x42, 0x58, 0xa0, 0x0b, 0x42, 0x0a, 0xe8, 0x9c, 0x41, 0x58, 0xa0, 0x0b, 0x41,
    };
    u8 a1[16], b2[16];
    u32 i;

    switch (c[0] << 8 | c[3]) {
    case 0x010c:
        out[0] = 0x61; out[1] = 0x12; out[2] = 0x50; out[3] = 0x10;
        return 4;
    case 0x0204:            /* flash read: length at 8, address at 12 */
        if (n < 15 || c[8] > S2_MAX_DATA - 8) return 0;
        memcpy(out, c + 8, 8);
        out[1] = 0;
        s2_flash_read((u32)c[12] | (u32)c[13] << 8 | (u32)c[14] << 16, out + 8, c[8]);
        return (u8)(c[8] + 8);
    case 0x0201:            /* USB flash read (SDL's): 0x40 bytes from the address at 12 */
        if (n < 16) return 0;
        memcpy(out, c + 8, 8);
        out[0] = 0x40;
        s2_flash_read((u32)c[12] | (u32)c[13] << 8 | (u32)c[14] << 16, out + 8, 0x40);
        return 0x48;
    case 0x030d:            /* USB: start the reports */
        s2.ready = 1;
        return 0;
    case 0x0701:
        out[0] = 0;
        return 1;
    case 0x0c01:
        memset(out, 0, 12);
        out[4] = 0x07; out[5] = 0x07; out[6] = 0x01;
        return 12;
    case 0x0c02:                 /* set the mask the enable/disable commands obey */
        if (n > 8) s2.feature_mask = c[8];
        memset(out, 0, 4);
        return 4;
    case 0x0c03:
        s2.feature_mask = 0;
        memset(out, 0, 4);
        return 4;
    case 0x0c04:
        s2.ready = 1;
        if (n > 8 && (c[8] & s2.feature_mask & ~s2.features & S2_FEATURE_MOTION)) {
            s2.q[0] = S2_ONE;    /* motion starts level, like a real controller */
            s2.q[1] = s2.q[2] = s2.q[3] = 0;
            s2.have_last = 0;
        }
        if (n > 8 && (c[8] & s2.feature_mask & ~s2.features & S2_FEATURE_MOTION))
            util_log("switch2: host turns motion on");
        if (n > 8) s2.features |= c[8] & s2.feature_mask;
        memset(out, 0, 4);
        return 4;
    case 0x0c05:
        if (n > 8 && (c[8] & s2.features & S2_FEATURE_MOTION)) util_log("switch2: host turns motion off");
        if (n > 8) s2.features &= (u8)~c[8];
        memset(out, 0, 4);
        return 4;
    case 0x1001:
        memcpy(out, firmware, sizeof(firmware));
        return sizeof(firmware);
    case 0x1101:
        out[0] = 1; out[1] = out[2] = out[3] = 0;
        return 4;
    case 0x1103:
        memcpy(out, unknown_1103, sizeof(unknown_1103));
        return sizeof(unknown_1103);
    case 0x1501:            /* our address, least significant byte first */
        out[0] = 1; out[1] = 4; out[2] = 1;
        memcpy(out + 3, s2.address, 6);
        return 9;
    case 0x1504:            /* the console's A1: our key is A1 ^ B1 */
        if (n < 9 + 16) return 0;
        for (i = 0; i < 16; i++) s2.ltk[i] = c[9 + 15 - i] ^ s2_key_b1[15 - i];
        s2.ltk_valid = 1;
        out[0] = 1;
        memcpy(out + 1, s2_key_b1, 16);
        return 17;
    case 0x1502:            /* the console's challenge, answered with AES under the key */
        if (n < 9 + 16 || !s2.ltk_valid) return 0;
        for (i = 0; i < 16; i++) a1[i] = c[9 + 15 - i];
        util_aes128(s2.ltk, a1, b2);
        out[0] = 1;
        memcpy(out + 1, b2, 16);
        return 17;
    case 0x1503:            /* finished: the bond is the key */
        if (n < 9) return 0;
        *bonded = s2.ltk_valid;
        out[0] = 1;
        return 1;
    case 0x1601:
        memset(out, 0, 0x18);
        return 0x18;
    case 0x1801:
        memset(out, 0, 8);
        out[2] = 0x40; out[3] = 0xf0; out[6] = 0x60;
        return 8;
    case 0x0907:            /* player lights: as Switch 1, low nibble lit, high flashing */
        if (n >= 9) {
            s2.has_players = 1;
            s2.players = c[8] & 0xf;
            s2.players_flash = c[8] >> 4;
            util_log("switch2: player lights %02x", c[8]);
        }
        return 0;
    case 0x0a02:            /* a short buzz */
        if (n >= 9 && c[8]) {
            s2.pulse = 1;
            s2.pulse_until = now + 120;
        }
        return 0;
    }
    return 0;
}

static int s2_write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    u8 resp[S2_RESP_PREFIX + 8 + S2_MAX_DATA];
    const u8 *c;
    u16 n, offset;
    u8 len;
    int bonded = 0;

    /* USB: rumble in output report 0x02 (a sequence byte, then the two
     * motors' blocks 16 apart); commands on the bulk pipe, answered there
     * without the LE response's prefix.  A bond made over USB is not kept. */
    if (s2.usb && w->kind == BT_WRITE_OUTPUT && w->id == 0x02 && w->len >= 0x16) {
        s2.rumble[0] = s2_amplitude(w->data);
        s2.rumble[1] = s2_amplitude(w->data + 0x10);
        return 0;
    }
    if (s2.usb && w->kind == BT_WRITE_BULK && w->len >= 8) {
        memset(resp, 0, sizeof(resp));
        len = s2_command(w->data, w->len, resp + 8, now, &bonded);
        memcpy(resp, w->data, 4);
        resp[1] = 0x01;
        resp[4] = 0x10;
        resp[5] = 0x78;
        if (w->data[0] != 0x02) util_log("switch2: usb cmd %02x %02x", w->data[0], w->data[3]);
        host_send(HOST_BULK, resp, (u16)(8 + len));
        return 0;
    }
    if (w->kind != BT_WRITE_ATT) return 0;
    if (w->id == S2_VIBRATION) {
        s2_rumble(w->data, w->len);
        return 0;
    }
    if (w->id != S2_COMMAND && w->id != S2_VIB_COMMAND) return 0;
    offset = w->len >= S2_CMD_PREFIX + 8 ? S2_CMD_PREFIX : 0;
    if (w->id == S2_VIB_COMMAND && offset) s2_rumble(w->data, S2_CMD_PREFIX);
    if (w->len < offset + 8) return 0;
    c = w->data + offset;
    n = w->len - offset;
    memset(resp, 0, sizeof(resp));
    len = s2_command(c, n, resp + S2_RESP_PREFIX + 8, now, &bonded);
    memcpy(resp + S2_RESP_PREFIX, c, 4);
    resp[S2_RESP_PREFIX + 4] = 0x10;
    resp[S2_RESP_PREFIX + 5] = 0x78;
    if (c[1] == 0x91) resp[S2_RESP_PREFIX + 1] = 1;
    if (c[0] != 0x02) util_log("switch2: cmd %02x %02x", c[0], c[3]);
    host_send(S2_RESPONSE, resp, (u16)(S2_RESP_PREFIX + 8 + len));
    if (!bonded) return 0;
    memset(bond, 0, sizeof(*bond));
    memcpy(bond->ltk, s2.ltk, 16);
    return 1;
}

static void s2_host(u32 now, struct profile_host *h)
{
    int pulse = s2.pulse && (s32)(now - s2.pulse_until) < 0;

    h->rumble[0] = pulse ? 0x80 : s2.rumble[0];
    h->rumble[1] = pulse ? 0x80 : s2.rumble[1];
    /* Motion's 40-byte block in the report is still sent as zeros: its packing
     * is not known.  The sensor runs so the samples are there when it is. */
    h->imu = s2.features & S2_FEATURE_MOTION ? IMU_MOTION : IMU_OFF;
    h->has_players = s2.has_players;
    h->players = s2.players;
    h->players_flash = s2.players_flash;
}

static int s2_read(u8 kind, u16 id, u16 offset, u8 *out, u16 max)
{
    static const u8 info1[] = { 0x04, 0x00, 0x05, 0x00, 0x01, 0x01, 0x00 };
    static const u8 info2[] = { 0x36, 0x80, 0x74, 0xee, 0xbb, 0x3d, 0x8e, 0x13 };

    (void)offset;
    if (kind != BT_WRITE_ATT || max < 8) return -1;
    if (id == S2_INFO_1) {
        memcpy(out, info1, sizeof(info1));
        return sizeof(info1);
    }
    if (id == S2_INFO_2) {
        memcpy(out, info2, sizeof(info2));
        return sizeof(info2);
    }
    return -1;
}

/* Bonded: the console's address and the wake flag (0x81) that a sleeping
 * Switch 2 answers.  The flag goes once the first link ends. */
static u8 s2_advertising(const struct bt_bond *bond, int pairing, int wake, u8 *d)
{
    int bonded = !pairing && (bond->le_peer[0] | bond->le_peer[1] | bond->le_peer[2] |
                              bond->le_peer[3] | bond->le_peer[4] | bond->le_peer[5]);

    memset(d, 0, 31);
    d[0] = 0x02; d[1] = 0x01; d[2] = 0x06;
    d[3] = 0x1b; d[4] = 0xff; d[5] = 0x53; d[6] = 0x05;
    d[7] = 0x01; d[9] = 0x03; d[10] = 0x7e; d[11] = 0x05;
    d[12] = 0x69; d[13] = 0x20; d[15] = 0x01;
    d[16] = bonded && wake ? 0x81 : 0;
    if (bonded) memcpy(d + 17, bond->le_peer, 6);
    d[23] = 0x0f;
    return 31;
}

/* USB: a 64-byte report laid out as SDL's Switch 2 driver reads it: buttons
 * at 4-7 (after the id), 12-bit sticks at 10, the motion sample's time at
 * 42, accelerometer at 48 (4096 a g) and gyro at 54 (16.4 a degree a
 * second), in SDL's axes.  Report id 0x09 is a guess: the real descriptor
 * is not known here. */
#define S2_USB_REPORT 0x09
#define S2_USB_SIZE   63

static int s2_usb_report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    static const u8 hat_bits[HAT_NEUTRAL] = { 0x02, 0x06, 0x04, 0x05, 0x01, 0x09, 0x08, 0x0a };
    const struct imu_sample *m = &s2.motion;
    u16 b = p->buttons;

    (void)now;
    if (p->imu_count) s2.motion = p->imu[p->imu_count - 1];
    memset(out, 0, S2_USB_SIZE);
    out[0] = ++s2.counter;
    /* By position: the pad's A (south) is the Switch's B. */
    out[4] = (u8)((b & PAD_X ? 0x01 : 0) | (b & PAD_Y ? 0x02 : 0) | (b & PAD_A ? 0x04 : 0) |
                  (b & PAD_B ? 0x08 : 0) | (b & PAD_R1 ? 0x40 : 0) | (p->r2 > TRIGGER_BUTTON ? 0x80 : 0));
    out[5] = (u8)((b & PAD_SELECT ? 0x01 : 0) | (b & PAD_START ? 0x02 : 0) | (b & PAD_R3 ? 0x04 : 0) |
                  (b & PAD_L3 ? 0x08 : 0) | (b & PAD_HOME ? 0x10 : 0) |
                  (b & PAD_SCREENSHOT ? 0x20 : 0) | (b & PAD_M ? 0x40 : 0));
    out[6] = (u8)((p->hat < HAT_NEUTRAL ? hat_bits[p->hat] : 0) | (b & PAD_L1 ? 0x40 : 0) |
                  (p->l2 > TRIGGER_BUTTON ? 0x80 : 0));
    out[7] = (u8)((b & PAD_R4 ? 0x01 : 0) | (b & PAD_L4 ? 0x02 : 0));
    s2_stick(out + 10, p->lx, (s16)-p->ly);
    s2_stick(out + 13, p->rx, (s16)-p->ry);
    if (s2.features & S2_FEATURE_MOTION && m->us) {
        s2_put32(out + 42, m->us ? m->us : 1);
        util_put16(out + 48, (u16)(s16)(-m->accel[1] / 2));
        util_put16(out + 50, (u16)(s16)(m->accel[0] / 2));
        util_put16(out + 52, (u16)(s16)(m->accel[2] / 2));
        util_put16(out + 54, (u16)m->gyro[1]);
        util_put16(out + 56, (u16)(s16)-m->gyro[0]);
        util_put16(out + 58, (u16)m->gyro[2]);
    }
    *id = S2_USB_REPORT;
    return S2_USB_SIZE;
}

static int s2_any_report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    return s2.usb ? s2_usb_report(p, now, out, id) : s2_report(p, now, out, id);
}

static const u8 s2_usb_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
    0x85, S2_USB_REPORT, 0x09, 0x09, 0x95, S2_USB_SIZE, 0x81, 0x02,
    0x85, 0x02, 0x09, 0x02, 0x95, 0x3f, 0x91, 0x02,
    0xc0,
};

static const struct usb_identity s2_usb = {
    .vendor_id = 0x057e, .product_id = 0x2069, .bcd_device = 0x0100,
    .manufacturer = "Nintendo", .product = "Switch 2 Pro Controller",
    .kind = USB_HID_BULK, .report_ids = 1, .interval = 4, .in_size = 64, .out_size = 64,
    .max_power = 250, .hid_descriptor = s2_usb_descriptor, .hid_descriptor_len = sizeof(s2_usb_descriptor),
};

static u8 s2_begin(const struct profile_session *s)
{
    memset(&s2, 0, sizeof(s2));
    s2.usb = s->usb;
    s2.ready = s->usb;           /* a cable needs no start */
    s2.q[0] = S2_ONE;
    memcpy(s2.address, s->address, 6);
    if (!s->pairing && s->bond) {
        memcpy(s2.ltk, s->bond->ltk, 16);
        s2.ltk_valid = 1;
    }
    return IMU_OFF;
}

static const struct bt_device s2_device = {
    .transport = BT_LE,
    .name = "Pro Controller",
    .att_db = s2_att_db,
    .input_handle = S2_INPUT,
    .advertising = s2_advertising,
};

const struct profile profile_switch2 = {
    .name = "switch2", .device = &s2_device, .usb = &s2_usb,
    .begin = s2_begin, .report = s2_any_report, .write = s2_write, .host = s2_host, .read = s2_read,
};

#ifdef UNITTEST
static u8 sent[64];
static u16 sent_len;
int host_send(u16 id, const u8 *data, u16 len)
{
    sent[0] = (u8)id;
    memcpy(sent + 1, data, len);
    sent_len = len;
    return 1;
}
void util_log(const char *fmt, ...) { (void)fmt; }
void util_aes128(const u8 key[16], const u8 in[16], u8 out[16]) { u32 i; for (i = 0; i < 16; i++) out[i] = in[i] ^ key[i]; }
void util_put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
void imu_gyro_bias(s32 out[3]) { out[0] = out[1] = out[2] = 0; }
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
    /* The sleep capture contains report 01, subcommand 06, argument 00.
     * Missing arguments and other HCI states must not end the session. */
    w.id = OUT_SUBCMD; w.data[9] = 0x06; w.len = 10;
    write(&w, 1300, 0); host(1300, &h);
    CHECK(!h.ended);
    w.len = 11; w.data[10] = 1;
    write(&w, 1301, 0); host(1301, &h);
    CHECK(!h.ended);
    w.kind = BT_WRITE_FEATURE; w.data[10] = 0;
    write(&w, 1302, 0); host(1302, &h);
    CHECK(!h.ended);
    w.kind = BT_WRITE_OUTPUT;
    sent_len = 0;
    write(&w, 1303, 0); host(1303, &h);
    CHECK(h.ended && report(&p, 1400, out, &id) == 0);
    CHECK(sent_len == 0);
    w.data[10] = 1;
    write(&w, 1401, 0); host(1401, &h);
    CHECK(h.ended); /* latched until the next session */
    begin(&s); host(1500, &h);
    CHECK(!h.ended && report(&p, 1500, out, &id) == FULL_SIZE);
    {   /* Switch 2: pairing exchange and the bond it returns */
        struct profile_session s2s = { 3, 1, { 6, 5, 4, 3, 2, 1 }, 0, 0 };
        struct bt_bond bond;
        u8 adv[31];

        s2_begin(&s2s);
        memset(&w, 0, sizeof(w));
        w.kind = BT_WRITE_ATT; w.id = S2_COMMAND; w.len = 9 + 16;
        w.data[0] = 0x15; w.data[3] = 0x04;
        w.data[9 + 15] = 0xaa;
        CHECK(s2_write(&w, 0, &bond) == 0 && s2.ltk[0] == (0xaa ^ s2_key_b1[15]));
        CHECK(sent[0] == S2_RESPONSE && sent[1 + S2_RESP_PREFIX] == 0x15);
        w.data[3] = 0x03; w.len = 9;
        CHECK(s2_write(&w, 0, &bond) == 1 && bond.ltk[0] == s2.ltk[0]);
        struct profile_host h;

        w.data[0] = 0x0c; w.data[3] = 0x02; w.data[8] = 0x2f; w.len = 12;
        s2_write(&w, 0, &bond);
        w.data[3] = 0x04;
        s2_write(&w, 0, &bond);
        CHECK(s2.ready && s2_report(&p, 0, out, &id) == S2_REPORT_SIZE && (out[2] & 1));
        p.buttons = PAD_M;
        CHECK(s2_report(&p, 1, out, &id) == S2_REPORT_SIZE && (out[4] & 0x10));
        p.buttons = 0;
        CHECK(s2_report(&p, 2, out, &id) == S2_REPORT_SIZE && !(out[4] & 0x10));
        p.buttons = PAD_A;
        s2_host(0, &h);
        CHECK(h.imu == IMU_MOTION);
        {
            /* 60 degrees about Z in one second, then decode the block the way
             * the Joy-Con captures were decoded. */
            struct pad mp;
            u32 t, q1, q2, q3;
            float r[3], m, qz, qw;
            int i;

            memset(&mp, 0, sizeof(mp));
            mp.hat = HAT_NEUTRAL;
            for (t = 0; t <= 1000000; t += 4000) {
                mp.imu_count = 1;
                mp.imu[0].us = t;
                mp.imu[0].gyro[2] = 60 * 16;
                mp.imu[0].accel[2] = 8192;
                s2_report(&mp, 0, out, &id);
            }
            CHECK(out[14] == 30 && out[15 + 3] == 12 && (out[15 + 4] & 3) == 0);
            q1 = (u32)out[20] << 6 | (u32)out[21] << 14 | (u32)out[22] << 22 | (u32)(out[23] & 3) << 30;
            q2 = (u32)(out[23] >> 7) << 6 | (u32)out[24] << 7 | (u32)out[25] << 15 | (u32)out[26] << 23 |
                 (u32)(out[27] & 1) << 31;
            q3 = (u32)(out[27] & 0xc0) | (u32)out[28] << 8 | (u32)out[29] << 16 | (u32)out[30] << 24;
            r[0] = ((float)q1 / 4294967296.0f - 0.5f) * 2;
            r[1] = ((float)q2 / 4294967296.0f - 0.5f) * 2;
            r[2] = ((float)q3 / 4294967296.0f - 0.5f) * 2;
            m = 1.0f / (1.0f + r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            qw = m;              /* squared, to avoid sqrt in the test */
            qz = r[2] * r[2] * m;
            CHECK(r[0] > -0.01f && r[0] < 0.01f && r[1] > -0.01f && r[1] < 0.01f);
            CHECK(qw > 0.73f && qw < 0.77f && qz > 0.23f && qz < 0.27f);  /* cos^2 30, sin^2 30 */
            i = out[31] | out[32] << 8 | out[33] << 16 | out[34] << 24;
            CHECK(out[15 + 24] == 0 && out[15 + 27] == 0x10 && i == 0);   /* accel z = 1 g = 2^28 */
        }
        w.data[3] = 0x05; w.data[8] = S2_FEATURE_MOTION;
        s2_write(&w, 0, &bond);
        s2_host(0, &h);
        CHECK(h.imu == IMU_OFF);
        memset(&bond, 0, sizeof(bond));
        bond.le_peer[0] = 9;
        CHECK(s2_advertising(&bond, 0, 1, adv) == 31 && adv[16] == 0x81 && adv[17] == 9);
        CHECK(s2_advertising(&bond, 0, 0, adv) == 31 && adv[16] == 0);
        CHECK(s2_advertising(&bond, 1, 1, adv) == 31 && adv[17] == 0);
    }
    return TEST_DONE("profile_switch");
}
#endif

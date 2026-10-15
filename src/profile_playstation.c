#include <string.h>
#include "pad.h"

/* DualSense (Edge) and DualShock 4 over Classic HID.  Both start in their
 * extended report (0x31, 0x11): SDL runs a pad already sending it in full
 * mode (gyro, touchpad, paddles, rumble).  Layouts follow Linux
 * hid-playstation; neither has been tried on a PlayStation.
 *
 * Over USB both send report 0x01: the same state block without the
 * Bluetooth header and CRC, and take output 0x05 (DualShock 4) or 0x02
 * (DualSense), whose bytes are the Bluetooth output's after its header.
 * Feature reports are the same without the CRC; the DualShock 4 reads its
 * calibration as 0x02 and its address as 0x12 there.  A PlayStation wants
 * its own authentication over USB, which this does not have. */

#define DS_EXTENDED      0x31
#define DS4_EXTENDED     0x11
#define EXTENDED_SIZE    77
#define TOUCH_TAP_MS     200
#define TOUCH_CLICK_MS   60
#define TRIGGER_BUTTON   0x20
#define USB_STATE        0x01
#define USB_STATE_SIZE   63
#define DS4_USB_OUTPUT   0x05
#define DS_USB_OUTPUT    0x02

/* USB: report 0x01, the state block as hid-playstation reads it (sticks, hat
 * and 14 buttons, a counter, triggers, then the rest as vendor bytes); the
 * output report; the feature reports the drivers read. */
#define USB_DESCRIPTOR(output_id, output_size, features...) \
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, USB_STATE, \
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xff, 0x00, \
    0x75, 0x08, 0x95, 0x04, 0x81, 0x02, \
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, \
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, \
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0e, 0x81, 0x02, \
    0x06, 0x00, 0xff, 0x09, 0x20, 0x75, 0x06, 0x95, 0x01, 0x15, 0x00, 0x25, 0x3f, 0x81, 0x02, \
    0x05, 0x01, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, \
    0x06, 0x00, 0xff, 0x09, 0x21, 0x95, USB_STATE_SIZE - 9, 0x81, 0x02, \
    0x85, output_id, 0x09, 0x22, 0x95, output_size, 0x91, 0x02, \
    features 0xc0
#define FEATURE(id, size) 0x85, id, 0x09, id, 0x95, size, 0xb1, 0x02,

static const u8 ds4_usb_descriptor[] = {
    USB_DESCRIPTOR(DS4_USB_OUTPUT, 31, FEATURE(0x02, 36) FEATURE(0x12, 15) FEATURE(0xa3, 48))
};

static const u8 ds_usb_descriptor[] = {
    USB_DESCRIPTOR(DS_USB_OUTPUT, 62, FEATURE(0x05, 40) FEATURE(0x09, 19) FEATURE(0x20, 63))
};

static const u8 ds_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x09, 0x39, 0x15, 0x00, 0x25,
    0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x0e, 0x81, 0x02, 0x75, 0x06, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01, 0x09,
    0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0x06, 0x00,
    0xff, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x85, 0x31, 0x09, 0x31, 0x95, 0x4d, 0x81, 0x02,
    0x09, 0x3b, 0x91, 0x02, 0x85, 0x32, 0x09, 0x32, 0x95, 0x8d, 0x81, 0x02, 0x09, 0x3c, 0x91, 0x02,
    0x85, 0x33, 0x09, 0x33, 0x95, 0xcd, 0x81, 0x02, 0x09, 0x3d, 0x91, 0x02, 0x85, 0x34, 0x09, 0x34,
    0x96, 0x0d, 0x01, 0x81, 0x02, 0x09, 0x3e, 0x91, 0x02, 0x85, 0x35, 0x09, 0x35, 0x96, 0x4d, 0x01,
    0x81, 0x02, 0x09, 0x3f, 0x91, 0x02, 0x85, 0x36, 0x09, 0x36, 0x96, 0x8d, 0x01, 0x81, 0x02, 0x09,
    0x40, 0x91, 0x02, 0x85, 0x37, 0x09, 0x37, 0x96, 0xcd, 0x01, 0x81, 0x02, 0x09, 0x41, 0x91, 0x02,
    0x85, 0x38, 0x09, 0x38, 0x96, 0x0d, 0x02, 0x81, 0x02, 0x09, 0x42, 0x91, 0x02, 0x85, 0x39, 0x09,
    0x39, 0x96, 0x22, 0x02, 0x81, 0x02, 0x09, 0x43, 0x91, 0x02, 0x06, 0x80, 0xff, 0x85, 0x05, 0x09,
    0x33, 0x95, 0x28, 0xb1, 0x02, 0x85, 0x08, 0x09, 0x34, 0x95, 0x2f, 0xb1, 0x02, 0x85, 0x09, 0x09,
    0x24, 0x95, 0x13, 0xb1, 0x02, 0x85, 0x20, 0x09, 0x26, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x22, 0x09,
    0x40, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x80, 0x09, 0x28, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x81, 0x09,
    0x29, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x82, 0x09, 0x2a, 0x95, 0x09, 0xb1, 0x02, 0x85, 0x83, 0x09,
    0x2b, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf0, 0x09, 0x47, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf1, 0x09,
    0x48, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf2, 0x09, 0x49, 0x95, 0x0f, 0xb1, 0x02, 0xc0,
};

static const u8 ds4_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x09, 0x39, 0x15, 0x00, 0x25,
    0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x0e, 0x81, 0x02, 0x75, 0x06, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01, 0x09,
    0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0x06, 0x04,
    0xff, 0x85, 0x02, 0x09, 0x24, 0x95, 0x24, 0xb1, 0x02, 0x85, 0xa3, 0x09, 0x25, 0x95, 0x30, 0xb1,
    0x02, 0x85, 0x05, 0x09, 0x26, 0x95, 0x28, 0xb1, 0x02, 0x85, 0x06, 0x09, 0x27, 0x95, 0x34, 0xb1,
    0x02, 0x85, 0x07, 0x09, 0x28, 0x95, 0x30, 0xb1, 0x02, 0x85, 0x08, 0x09, 0x29, 0x95, 0x2f, 0xb1,
    0x02, 0x85, 0x09, 0x09, 0x2a, 0x95, 0x13, 0xb1, 0x02, 0x06, 0x03, 0xff, 0x85, 0x03, 0x09, 0x21,
    0x95, 0x26, 0xb1, 0x02, 0x85, 0x04, 0x09, 0x22, 0x95, 0x2e, 0xb1, 0x02, 0x85, 0xf0, 0x09, 0x47,
    0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf1, 0x09, 0x48, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf2, 0x09, 0x49,
    0x95, 0x0f, 0xb1, 0x02, 0x06, 0x00, 0xff, 0x85, 0x11, 0x09, 0x20, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, 0x4d, 0x81, 0x02, 0x09, 0x21, 0x91, 0x02, 0x85, 0x12, 0x09, 0x22, 0x95, 0x8d,
    0x81, 0x02, 0x09, 0x23, 0x91, 0x02, 0x85, 0x13, 0x09, 0x24, 0x95, 0xcd, 0x81, 0x02, 0x09, 0x25,
    0x91, 0x02, 0x85, 0x14, 0x09, 0x26, 0x96, 0x0d, 0x01, 0x81, 0x02, 0x09, 0x27, 0x91, 0x02, 0x85,
    0x15, 0x09, 0x28, 0x96, 0x4d, 0x01, 0x81, 0x02, 0x09, 0x29, 0x91, 0x02, 0x85, 0x16, 0x09, 0x2a,
    0x96, 0x8d, 0x01, 0x81, 0x02, 0x09, 0x2b, 0x91, 0x02, 0x85, 0x17, 0x09, 0x2c, 0x96, 0xcd, 0x01,
    0x81, 0x02, 0x09, 0x2d, 0x91, 0x02, 0x85, 0x18, 0x09, 0x2e, 0x96, 0x0d, 0x02, 0x81, 0x02, 0x09,
    0x2f, 0x91, 0x02, 0x85, 0x19, 0x09, 0x30, 0x96, 0x22, 0x02, 0x81, 0x02, 0x09, 0x31, 0x91, 0x02,
    0xc0,
};

static struct {
    u8 ds4;
    u8 usb;
    u8 address[6];
    u16 gyro_speed, gyro_accel;
    u8 counter;
    u32 sequence;
    struct imu_sample motion;
    struct imu_pointer pointer;
    /* M puts a finger on the touchpad and the gyro moves it; a tap of M
     * is the touchpad click. */
    struct { u16 w, h; u8 down, id, clicking; s32 x, y; u32 start, click; } touch;
    u8 rumble[2];
    u8 has_lightbar, lightbar[3];
    u8 has_players, players;
} ps;

/* CRC-32 (IEEE, reflected, as zlib's), four bits at a time. */
static u32 crc32(u32 crc, const u8 *d, u16 n)
{
    static const u32 t[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };

    crc = ~crc;
    while (n--) {
        crc ^= *d++;
        crc = (crc >> 4) ^ t[crc & 15];
        crc = (crc >> 4) ^ t[crc & 15];
    }
    return ~crc;
}

/* The CRC over the HIDP header (0xa1 input, 0xa3 feature), the report id and
 * the body, in the body's last four bytes. */
static void seal(u8 header, u8 id, u8 *body, u16 size)
{
    u8 prefix[2] = { header, id };

    util_put32(body + size - 4, crc32(crc32(0, prefix, 2), body, (u16)(size - 4)));
}

static u8 stick8(s16 v)
{
    return (u8)(((s32)v + 32768) >> 8);
}

/* Hat and faces (by position: the bottom one is cross), shoulders and
 * sticks, then PS, the touchpad click, and the DualSense's mute and paddles. */
static void buttons(const struct pad *p, u8 *out)
{
    u32 b = p->buttons;

    out[0] = in_hat(b);
    if (b & PAD_X) out[0] |= 0x10;
    if (b & PAD_A) out[0] |= 0x20;
    if (b & PAD_B) out[0] |= 0x40;
    if (b & PAD_Y) out[0] |= 0x80;
    out[1] = 0;
    if (b & PAD_L1) out[1] |= 0x01;
    if (b & PAD_R1) out[1] |= 0x02;
    if ((p->l2 >> 8) > TRIGGER_BUTTON) out[1] |= 0x04;
    if ((p->r2 >> 8) > TRIGGER_BUTTON) out[1] |= 0x08;
    if (b & PAD_SELECT) out[1] |= 0x10;
    if (b & PAD_START) out[1] |= 0x20;
    if (b & PAD_L3) out[1] |= 0x40;
    if (b & PAD_R3) out[1] |= 0x80;
    out[2] = 0;
    if (b & PAD_HOME) out[2] |= 0x01;
    if (b & PAD_M) out[2] |= 0x02;
    if (!ps.ds4) {
        if (b & PAD_SCREENSHOT) out[2] |= 0x04;
        if (b & PAD_L4) out[2] |= 0x40;
        if (b & PAD_R4) out[2] |= 0x80;
    }
}

/* Gyro then accelerometer, in the controllers' own frame. */
static void motion(u8 *out)
{
    const struct imu_sample *s = &ps.motion;

    util_put16(out + 0, (u16)s->gyro[1]);
    util_put16(out + 2, (u16)s->gyro[2]);
    util_put16(out + 4, (u16)s->gyro[0]);
    util_put16(out + 6, (u16)(s16)-s->accel[1]);
    util_put16(out + 8, (u16)s->accel[2]);
    util_put16(out + 10, (u16)(s16)-s->accel[0]);
}

static void touch_update(struct pad *p, u32 now)
{
    int held = (p->buttons & PAD_M) != 0;
    s32 x, y;

    p->buttons &= ~(u32)PAD_M;
    /* Taken every report, so turns made without the finger are dropped. */
    imu_pointer(&ps.pointer, p->imu, p->imu_count, ps.gyro_speed, ps.gyro_accel, &x, &y);
    if (ps.touch.clicking) {
        if (now - ps.touch.click < TOUCH_CLICK_MS && !held) {
            p->buttons |= PAD_M;
            return;
        }
        ps.touch.clicking = ps.touch.down = 0;
    }
    if (held && !ps.touch.down) {
        ps.touch.down = 1;
        ps.touch.id = (u8)((ps.touch.id + 1) & 0x7f);
        ps.touch.x = ps.touch.w / 2;
        ps.touch.y = ps.touch.h / 2;
        ps.touch.start = now;
    } else if (held) {
        ps.touch.x += x;
        ps.touch.y += y;
        if (ps.touch.x < 0) ps.touch.x = 0;
        if (ps.touch.x > ps.touch.w - 1) ps.touch.x = ps.touch.w - 1;
        if (ps.touch.y < 0) ps.touch.y = 0;
        if (ps.touch.y > ps.touch.h - 1) ps.touch.y = ps.touch.h - 1;
    } else if (ps.touch.down) {
        if (now - ps.touch.start < TOUCH_TAP_MS) {
            ps.touch.clicking = 1;
            ps.touch.click = now;
            p->buttons |= PAD_M;
        } else {
            ps.touch.down = 0;
        }
    }
}

/* One touch point: bit 7 of the first byte is "no finger". */
static void touch_point(u8 *pt)
{
    if (!ps.touch.down) {
        pt[0] = 0x80;
        return;
    }
    pt[0] = ps.touch.id;
    pt[1] = (u8)ps.touch.x;
    pt[2] = (u8)(((ps.touch.x >> 8) & 0x0f) | ((ps.touch.y & 0x0f) << 4));
    pt[3] = (u8)(ps.touch.y >> 4);
}

static u8 begin(const struct profile_session *s)
{
    u8 ds4 = ps.ds4;

    memset(&ps, 0, sizeof(ps));
    ps.ds4 = ds4;
    ps.usb = s->usb;
    memcpy(ps.address, s->address, 6);
    ps.gyro_speed = s->settings ? s->settings->gyro_speed : 32;
    ps.gyro_accel = s->settings ? s->settings->gyro_accel : 250;
    ps.touch.w = 1920;
    ps.touch.h = ds4 ? 942 : 1080;
    return IMU_MOTION;
}

static u8 begin_ds(const struct profile_session *s)
{
    ps.ds4 = 0;
    return begin(s);
}

static u8 begin_ds4(const struct profile_session *s)
{
    ps.ds4 = 1;
    return begin(s);
}

static int report(const struct pad *in, u32 now, u8 *r, u8 *id)
{
    struct pad p = *in;
    u8 *st;

    if (p.imu_count) ps.motion = p.imu[p.imu_count - 1];
    touch_update(&p, now);
    memset(r, 0, EXTENDED_SIZE);
    if (ps.ds4) {
        /* Two bytes of Bluetooth header (0xc0: HID data follows), the
         * 32-byte state USB also sends, touch frames, the CRC. */
        st = r + 2;
        r[0] = 0xc0;
        st[0] = stick8(p.lx);
        st[1] = stick8(p.ly);
        st[2] = stick8(p.rx);
        st[3] = stick8(p.ry);
        buttons(&p, st + 4);
        st[6] = (u8)((st[6] & 0x03) | (u8)(ps.counter << 2));
        st[7] = (u8)(p.l2 >> 8);
        st[8] = (u8)(p.r2 >> 8);
        motion(st + 12);
        /* A DualShock 4 reports pitch with the opposite sign to the
         * DualSense, whose layout motion() follows. */
        util_put16(st + 12, (u16)(s16)-(s16)(st[12] | st[13] << 8));
        util_put16(st + 9, (u16)((ps.motion.us * 3ull) >> 4));    /* 16/3 us units */
        st[29] = 0x0a;                                            /* no cable, full */
        r[34] = 1;
        r[35] = ps.counter;
        touch_point(r + 36);
        r[40] = 0x80;
        if (ps.usb) {
            st[29] = 0x1b;                                        /* cable, full */
            memmove(r, st, USB_STATE_SIZE);
            *id = USB_STATE;
            ps.counter++;
            return USB_STATE_SIZE;
        }
        seal(0xa1, DS4_EXTENDED, r, EXTENDED_SIZE);
        *id = DS4_EXTENDED;
    } else {
        st = r + 1;
        r[0] = (u8)(ps.counter << 4);
        st[0] = stick8(p.lx);
        st[1] = stick8(p.ly);
        st[2] = stick8(p.rx);
        st[3] = stick8(p.ry);
        st[4] = (u8)(p.l2 >> 8);
        st[5] = (u8)(p.r2 >> 8);
        st[6] = ps.counter;
        buttons(&p, st + 7);
        util_put32(st + 11, ps.sequence++);
        motion(st + 15);
        util_put32(st + 27, ps.motion.us * 3);                    /* 1/3 us units */
        st[31] = 0x16;
        touch_point(st + 32);
        st[36] = 0x80;
        st[52] = 0x0a;
        if (ps.usb) {
            st[52] = 0x2a;                                        /* full, on the cable */
            memmove(r, st, USB_STATE_SIZE);
            *id = USB_STATE;
            ps.counter++;
            return USB_STATE_SIZE;
        }
        seal(0xa1, DS_EXTENDED, r, EXTENDED_SIZE);
        *id = DS_EXTENDED;
    }
    ps.counter++;
    return EXTENDED_SIZE;
}

/* Output report: flags say which of rumble, lightbar and player lights it
 * sets; the rest stay as they were. */
static int write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    u32 head = ps.usb ? 0 : 2;   /* Bluetooth's header in front */
    const u8 *e = w->data + head;
    u32 n, count;

    (void)now;
    (void)bond;
    if (w->kind != BT_WRITE_OUTPUT) return 0;
    if (ps.ds4) {
        if (w->id != (ps.usb ? DS4_USB_OUTPUT : DS4_EXTENDED) || w->len < head + 10) return 0;
        if (e[0] & 0x01) {
            ps.rumble[0] = e[4];
            ps.rumble[1] = e[3];
        }
        if (e[0] & 0x02) {
            ps.has_lightbar = 1;
            memcpy(ps.lightbar, e + 5, 3);
        }
        return 0;
    }
    if (w->id != (ps.usb ? DS_USB_OUTPUT : DS_EXTENDED) || w->len < head + 39) return 0;
    if ((e[0] & 0x01) || (e[38] & 0x04)) {
        ps.rumble[0] = e[3];
        ps.rumble[1] = e[2];
    } else if (!e[1]) {
        ps.rumble[0] = ps.rumble[1] = 0;
    }
    if (w->len < head + 47) return 0;
    if (e[1] & 0x04) {
        ps.has_lightbar = 1;
        memcpy(ps.lightbar, e + 44, 3);
    }
    if (e[1] & 0x10) {
        /* Five lights lit symmetrically; the count is the player number,
         * shown the way a Nintendo pad shows it (first light at the top). */
        static const u8 nintendo[] = { 0x0, 0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6 };

        for (n = e[43] & 0x1f, count = 0; n; n &= n - 1) count++;
        ps.has_players = 1;
        ps.players = nintendo[count];
    }
    return 0;
}

static void host(u32 now, struct profile_host *h)
{
    (void)now;
    h->rumble[0] = ps.rumble[0];
    h->rumble[1] = ps.rumble[1];
    h->has_lightbar = ps.has_lightbar;
    memcpy(h->lightbar, ps.lightbar, 3);
    h->has_players = ps.has_players;
    h->players = ps.players;
    h->imu = IMU_MOTION;
}

/* 8000 counts at 500 degrees a second and 8192 a g: what the sensor gives.
 * Bluetooth DualShock 4 order: the three gyro plus points, then minus. */
static void calibration(u8 *r, int ds4)
{
    u32 i;

    for (i = 0; i < 3; i++) {
        util_put16(r + 6 + 4 * i, 8000);
        util_put16(r + 8 + 4 * i, (u16)(s16)-8000);
    }
    if (ds4)
        for (i = 0; i < 3; i++) {
            util_put16(r + 6 + 2 * i, 8000);
            util_put16(r + 12 + 2 * i, (u16)(s16)-8000);
        }
    util_put16(r + 18, 500);
    util_put16(r + 20, 500);
    for (i = 0; i < 3; i++) {
        util_put16(r + 22 + 4 * i, 8192);
        util_put16(r + 24 + 4 * i, (u16)(s16)-8192);
    }
}

/* Feature reports the host reads: constant data, and our address. */
static int read(u8 kind, u16 id, u16 offset, u8 *r, u16 size)
{
    (void)offset;
    if (kind != BT_WRITE_FEATURE) return -1;
    memset(r, 0, size);
    if (ps.ds4) {
        switch (id) {
        case 0x02:               /* USB: the gyro's plus and minus points interleaved */
            if (size < 34) return -1;
            calibration(r, !ps.usb);
            return size;
        case 0x12:               /* USB: our address */
            if (!ps.usb || size < 15) return -1;
            memcpy(r, ps.address, 6);
            return size;
        case 0x05:
            if (size < 38) return -1;
            calibration(r, 1);
            seal(0xa3, (u8)id, r, size);
            return size;
        case 0xa3:
            if (size < 42) return -1;
            memcpy(r, "Sep 30 2026", 11);
            memcpy(r + 16, "00:00:00", 8);
            util_put16(r + 34, 0x0100);
            util_put16(r + 40, 0x0100);
            return size;
        }
        return -1;
    }
    switch (id) {
    case 0x05:
        if (size < 40) return -1;
        calibration(r, 0);
        break;
    case 0x09:
        if (size < 10) return -1;
        memcpy(r, ps.address, 6);
        break;
    case 0x20:
        if (size < 49) return -1;
        util_put16(r + 43, 0x0400);
        break;
    default:
        return -1;
    }
    if (!ps.usb) seal(0xa3, (u8)id, r, size);
    return size;
}

static const struct bt_device ds_device = {
    .transport = BT_CLASSIC, .name = "DualSense Edge Controller", .class_of_device = 0x002508,
    .vendor_id = 0x054c, .product_id = 0x0df2, .version = 0x0100,
    .hid_descriptor = ds_descriptor, .hid_descriptor_len = sizeof(ds_descriptor),
    .sniff_slots = 24, .sniff_attempt = 1,
};

static const struct bt_device ds4_device = {
    .transport = BT_CLASSIC, .name = "Wireless Controller", .class_of_device = 0x002508,
    .vendor_id = 0x054c, .product_id = 0x09cc, .version = 0x0100,
    .hid_descriptor = ds4_descriptor, .hid_descriptor_len = sizeof(ds4_descriptor),
    .sniff_slots = 24, .sniff_attempt = 1,
};

/* The wired DualSense; Linux and SDL take the Edge's ID only with its own
 * longer reports. */
static const struct usb_identity ds_usb = {
    .vendor_id = 0x054c, .product_id = 0x0ce6, .bcd_device = 0x0100,
    .manufacturer = "Sony Interactive Entertainment", .product = "DualSense Wireless Controller",
    .kind = USB_HID, .report_ids = 1, .interval = 1, .in_size = 64, .out_size = 64,
    .max_power = 250, .hid_descriptor = ds_usb_descriptor, .hid_descriptor_len = sizeof(ds_usb_descriptor),
};

static const struct usb_identity ds4_usb = {
    .vendor_id = 0x054c, .product_id = 0x09cc, .bcd_device = 0x0100,
    .manufacturer = "Sony Interactive Entertainment", .product = "Wireless Controller",
    .kind = USB_HID, .report_ids = 1, .interval = 1, .in_size = 64, .out_size = 64,
    .max_power = 250, .hid_descriptor = ds4_usb_descriptor, .hid_descriptor_len = sizeof(ds4_usb_descriptor),
};

/* 125 reports a second: at 250 the IMU starved (measured 2026-09-30). */
const struct profile profile_dualsense = {
    .name = "dualsense", .device = &ds_device, .usb = &ds_usb,
    .begin = begin_ds, .report = report, .write = write, .host = host, .read = read,
};

const struct profile profile_dualshock4 = {
    .name = "dualshock4", .device = &ds4_device, .usb = &ds4_usb,
    .begin = begin_ds4, .report = report, .write = write, .host = host, .read = read,
};

#ifdef UNITTEST
void util_put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
void util_put32(u8 *p, u32 v) { util_put16(p, (u16)v); util_put16(p + 2, (u16)(v >> 16)); }
void imu_pointer(struct imu_pointer *p, const struct imu_sample *s, int n, u16 per_degree, u16 accel,
                 s32 *x, s32 *y)
{
    (void)p; (void)s; (void)n; (void)per_degree; (void)accel;
    *x = 10;
    *y = 0;
}

int main(void)
{
    struct profile_session s = { 0, 0, { 1, 2, 3, 4, 5, 6 }, 0, 0 };
    struct pad p;
    struct bt_write w;
    struct profile_host h;
    u8 r[96], id;
    u8 prefix[2] = { 0xa1, DS_EXTENDED };

    CHECK(begin_ds(&s) == IMU_MOTION);
    memset(&p, 0, sizeof(p));
    p.buttons = PAD_A | PAD_HOME;
    p.lx = STICK_MAX;
    p.imu_count = 1;
    p.imu[0].gyro[1] = 100;
    CHECK(report(&p, 0, r, &id) == EXTENDED_SIZE && id == DS_EXTENDED);
    CHECK(r[1] == 0xff && r[8] == (HAT_NEUTRAL | 0x20) && (r[10] & 1));
    CHECK(r[16] == 100);
    CHECK((u32)(r[73] | r[74] << 8 | r[75] << 16 | (u32)r[76] << 24) == crc32(crc32(0, prefix, 2), r, 73));

    p.buttons = PAD_M;
    report(&p, 0, r, &id);
    CHECK(ps.touch.down && ps.touch.x == 960);
    report(&p, 10, r, &id);
    CHECK(ps.touch.x == 970);
    p.buttons = 0;
    report(&p, 20, r, &id);
    CHECK(ps.touch.clicking && (r[10] & 2));

    memset(&w, 0, sizeof(w));
    w.kind = BT_WRITE_OUTPUT; w.id = DS_EXTENDED; w.len = 2 + 47;
    w.data[2] = 0x01; w.data[3] = 0x04 | 0x10; w.data[4] = 30; w.data[5] = 200;
    w.data[2 + 43] = 0x04; w.data[2 + 44] = 9; w.data[2 + 45] = 8; w.data[2 + 46] = 7;
    write(&w, 0, 0);
    host(0, &h);
    CHECK(h.rumble[0] == 200 && h.rumble[1] == 30);
    CHECK(h.has_lightbar && h.lightbar[0] == 9 && h.has_players && h.players == 1);
    w.data[2 + 43] = 0x1f;       /* five lights: player 5, first and fourth */
    write(&w, 0, 0);
    host(0, &h);
    CHECK(h.players == 0x9);

    CHECK(read(BT_WRITE_FEATURE, 0x09, 0, r, 20) == 20 && r[0] == 1 && r[5] == 6);

    begin_ds4(&s);
    p.buttons = PAD_B;
    CHECK(report(&p, 0, r, &id) == EXTENDED_SIZE && id == DS4_EXTENDED && r[0] == 0xc0);
    CHECK(r[6] == (HAT_NEUTRAL | 0x40));
    w.id = DS4_EXTENDED; w.len = 12;
    memset(w.data, 0, sizeof(w.data));
    w.data[2] = 0x03; w.data[5] = 11; w.data[6] = 22; w.data[7] = 1; w.data[8] = 2; w.data[9] = 3;
    write(&w, 0, 0);
    host(0, &h);
    CHECK(h.rumble[0] == 22 && h.rumble[1] == 11 && h.lightbar[2] == 3);
    CHECK(read(BT_WRITE_FEATURE, 0x02, 0, r, 37) == 37 && r[6] == (8000 & 0xff));
    /* Wired: report 0x01 is the state block alone; output and features
     * without the Bluetooth header and CRC. */
    s.usb = 1;
    begin_ds4(&s);
    p.buttons = PAD_B;
    p.lx = STICK_MAX;
    CHECK(report(&p, 0, r, &id) == USB_STATE_SIZE && id == USB_STATE);
    CHECK(r[0] == 0xff && r[4] == (HAT_NEUTRAL | 0x40) && r[29] == 0x1b);
    memset(&w, 0, sizeof(w));
    w.kind = BT_WRITE_OUTPUT; w.id = DS4_USB_OUTPUT; w.len = 31;
    w.data[0] = 0x03; w.data[3] = 5; w.data[4] = 6; w.data[5] = 7;
    write(&w, 0, 0);
    host(0, &h);
    CHECK(h.rumble[0] == 6 && h.rumble[1] == 5 && h.lightbar[0] == 7);
    CHECK(read(BT_WRITE_FEATURE, 0x12, 0, r, 15) == 15 && r[0] == 1 && r[5] == 6);
    begin_ds(&s);
    CHECK(report(&p, 0, r, &id) == USB_STATE_SIZE && id == USB_STATE && r[52] == 0x2a);
    CHECK(read(BT_WRITE_FEATURE, 0x05, 0, r, 40) == 40 && r[39] == 0);      /* no CRC */
    return TEST_DONE("profile_playstation");
}
#endif

#include <string.h>
#include "pad.h"

/* The stock 8BitDo receiver of the Ultimate 2C Wireless (2dc8:310a, an
 * Xbox 360 pad over XInput): what its pad sends it over 8BitDo's 2.4 GHz
 * link, and the rumble its replies carry.  The link itself (BT_8BITDO) is
 * the platform's; it fills in the frame's channel bytes and checksum.
 *
 * The 18-byte frame: 0x90, the pad's two channels (link), the buttons as a
 * 32-bit word, the four axes as 0x7fff minus the XInput value, LT, RT, an
 * additive checksum (link).  The receiver maps button bits 0-13 and 17
 * (Home) and nothing else: Capture has no bit.  Its reply: 0x91, the two
 * motors, three bytes, a checksum. */

#define FRAME_SIZE 18

static u8 rumble[2];

static const struct { u32 pad; u8 bit; } buttons[] = {
    { PAD_START, 0 }, { PAD_L3, 1 }, { PAD_R3, 2 }, { PAD_SELECT, 3 },
    { PAD_X, 4 }, { PAD_Y, 5 }, { PAD_RIGHT, 6 }, { PAD_LEFT, 7 },
    { PAD_DOWN, 8 }, { PAD_UP, 9 }, { PAD_L1, 10 }, { PAD_R1, 11 },
    { PAD_B, 12 }, { PAD_A, 13 }, { PAD_HOME, 17 },
};

static u16 axis(s32 v)                   /* the receiver reports 0x7fff - this */
{
    return (u16)(0x7fff - v);
}

static s32 up(s16 v)                     /* XInput's Y grows upward, ours downward */
{
    return v == -32768 ? 32767 : -v;
}

static u8 begin(const struct profile_session *s)
{
    (void)s;
    rumble[0] = rumble[1] = 0;
    return 0;
}

/* The frame, every time: the link sends it when it differs from the last
 * and repeats the last one on its own. */
static int report(const struct pad *p, u32 now, u8 *out, u8 *id)
{
    u32 b = 0, i;

    (void)now;
    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (p->buttons & buttons[i].pad) b |= 1u << buttons[i].bit;
    memset(out, 0, FRAME_SIZE);
    out[0] = 0x90;
    util_put32(out + 3, b);
    util_put16(out + 7, axis(p->lx));
    util_put16(out + 9, axis(up(p->ly)));
    util_put16(out + 11, axis(p->rx));
    util_put16(out + 13, axis(up(p->ry)));
    out[15] = (u8)(p->l2 >> 8);
    out[16] = (u8)(p->r2 >> 8);
    *id = 0;
    return FRAME_SIZE;
}

/* The receiver's reply (the link checked its checksum). */
static int write(const struct bt_write *w, u32 now, struct bt_bond *bond)
{
    (void)now;
    (void)bond;
    if (w->kind == BT_WRITE_OUTPUT && w->len >= 3 && w->data[0] == 0x91) {
        rumble[0] = w->data[1];
        rumble[1] = w->data[2];
    }
    return 0;
}

static void host(u32 now, struct profile_host *h)
{
    (void)now;
    h->rumble[0] = rumble[0];
    h->rumble[1] = rumble[1];
}

static const struct bt_device device = { .transport = BT_8BITDO, .name = "8BitDo receiver" };

const struct profile profile_8bitdo = {
    .name = "8bitdo", .device = &device,
    .begin = begin, .report = report, .write = write, .host = host,
};

#ifdef UNITTEST
#include "test.h"

void util_put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
void util_put32(u8 *p, u32 v) { util_put16(p, (u16)v); util_put16(p + 2, (u16)(v >> 16)); }

int main(void)
{
    struct pad p;
    struct profile_host h;
    struct bt_write w;
    u8 out[FRAME_SIZE], id;

    memset(&p, 0, sizeof(p));
    begin(NULL);
    p.buttons = PAD_A | PAD_HOME;
    p.ly = 1000;                         /* down */
    p.l2 = 0xff00;
    CHECK(report(&p, 0, out, &id) == FRAME_SIZE && out[0] == 0x90);
    CHECK(out[3] == 0 && out[4] == 0x20 && out[5] == 0x02);     /* bits 13 and 17 */
    CHECK(out[7] == 0xff && out[8] == 0x7f);                    /* lx 0 */
    CHECK((u16)(out[9] | out[10] << 8) == (u16)(0x7fff + 1000));  /* XInput -1000 */
    CHECK(out[15] == 0xff && out[16] == 0);
    w.kind = BT_WRITE_OUTPUT;
    w.len = 7;
    memcpy(w.data, "\x91\x40\x80\x00\x00\x00\x51", 7);
    write(&w, 0, NULL);
    host(0, &h);
    CHECK(h.rumble[0] == 0x40 && h.rumble[1] == 0x80);
    return TEST_DONE("8BitDo receiver frame");
}
#endif

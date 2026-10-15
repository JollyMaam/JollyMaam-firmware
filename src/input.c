#include "pad.h"

#define MOVED_STICK   2048
#define MOVED_TRIGGER 4096

/* Sticks and triggers are filled in by cal_apply() (input_calibration.c). */
void in_map(const struct in_raw *raw, struct pad *pad)
{
    u32 b = raw->buttons;
    if ((b & PAD_UP) && (b & PAD_DOWN)) b &= ~(u32)(PAD_UP | PAD_DOWN);
    if ((b & PAD_LEFT) && (b & PAD_RIGHT)) b &= ~(u32)(PAD_LEFT | PAD_RIGHT);
    pad->buttons = b;
}


static int far(s32 a, s32 b, s32 limit)
{
    return a - b >= limit || b - a >= limit;
}

int in_moved(const struct pad *now, const struct pad *ref)
{
    return now->buttons != ref->buttons ||
           far(now->lx, ref->lx, MOVED_STICK) || far(now->ly, ref->ly, MOVED_STICK) ||
           far(now->rx, ref->rx, MOVED_STICK) || far(now->ry, ref->ry, MOVED_STICK) ||
           far(now->l2, ref->l2, MOVED_TRIGGER) || far(now->r2, ref->r2, MOVED_TRIGGER);
}

/* Q15 radius, 0..32767. Concave keeps more travel near the centre;
 * the S has high slope at both ends and zero slope at the midpoint. */
static u32 response(u32 r, u8 mode, const u8 *points)
{
    const u32 full = 32767;
    u32 t, k, a, b;

    if (r >= full) return full;
    switch (mode) {
    case STICK_CONCAVE:
        return r * r / full;
    case STICK_S_CURVE:
        if (2 * r <= full) return 2 * r * (full - r) / full;
        t = full - r;
        return full - 2 * t * (full - t) / full;
    case STICK_CUSTOM:
        t = r * 8;
        k = t / full;
        t %= full;
        a = k ? points[k - 1] : 0;
        b = k == STICK_CURVE_POINTS ? 255 : points[k];
        return (a * (full - t) + b * t + 127) / 255;
    default:
        return r;
    }
}

static u32 radius_of(s32 x, s32 y)
{
    u32 n = (u32)(x * x) + (u32)(y * y);
    u32 root = 0, bit = 1u << 30;

    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else root >>= 1;
        bit >>= 2;
    }
    return root;
}

static void stick_response(s16 *x, s16 *y, u8 mode, const u8 *points)
{
    u32 radius, output;

    if (mode == STICK_LINEAR) return;   /* preserve calibrated samples exactly */
    radius = radius_of(*x, *y);
    if (!radius) return;
    output = response(radius, mode, points);
    *x = (s16)((s32)*x * (s32)output / (s32)radius);
    *y = (s16)((s32)*y * (s32)output / (s32)radius);
}

void in_response(const struct set_slot *slot, struct pad *pad)
{
    stick_response(&pad->lx, &pad->ly, slot->left_curve, slot->left_curve_points);
    stick_response(&pad->rx, &pad->ry, slot->right_curve, slot->right_curve_points);
}

#ifdef UNITTEST
int main(void)
{
    struct set_slot slot = { 0 };
    struct pad pad = { .lx = 12000, .ly = -16000, .rx = -32767, .ry = 0,
                       .buttons = PAD_A, .l2 = 1234 };
    struct pad original = pad;
    u8 points[STICK_CURVE_POINTS] = { 0, 0, 64, 128, 192, 255, 255 };
    u32 mode, r, last;

    struct in_raw raw = { 0 };

    CHECK(in_hat(PAD_UP | PAD_LEFT) == HAT_UP_LEFT && in_hat(PAD_DOWN) == HAT_DOWN);
    CHECK(in_hat(PAD_RIGHT) == HAT_RIGHT && in_hat(0) == HAT_NEUTRAL);
    raw.buttons = PAD_A | PAD_UP | PAD_DOWN | PAD_RIGHT;
    in_map(&raw, &pad);
    CHECK(pad.buttons == (PAD_A | PAD_RIGHT) && in_hat(pad.buttons) == HAT_RIGHT);
    raw.buttons = PAD_DPAD;
    in_map(&raw, &pad);
    CHECK(pad.buttons == 0 && in_hat(pad.buttons) == HAT_NEUTRAL);
    pad = original;
    in_response(&slot, &pad);
    CHECK(!memcmp(&pad, &original, sizeof(pad)));
    for (mode = STICK_LINEAR; mode <= STICK_CUSTOM; mode++) {
        last = 0;
        CHECK(response(0, mode, points) == 0);
        CHECK(response(32767, mode, points) == 32767);
        for (r = 0; r <= 32767; r++) {
            u32 out = response(r, mode, points);
            CHECK(out >= last && out <= 32767);
            last = out;
        }
    }
    CHECK(response(16384, STICK_CONCAVE, points) == 8192);
    CHECK(response(8192, STICK_S_CURVE, points) > 8192);
    CHECK(response(24576, STICK_S_CURVE, points) < 24576);
    CHECK(response(4095, STICK_CUSTOM, points) == 0);
    CHECK(response(28672, STICK_CUSTOM, points) == 32767);
    slot.left_curve = STICK_CONCAVE;
    slot.right_curve = STICK_S_CURVE;
    in_response(&slot, &pad);
    CHECK(pad.lx > 0 && pad.ly < 0 && pad.lx < original.lx);
    CHECK(pad.lx * 4 + pad.ly * 3 >= -4 && pad.lx * 4 + pad.ly * 3 <= 4);
    CHECK(pad.rx == original.rx && pad.ry == 0);
    CHECK(pad.buttons == PAD_A && pad.l2 == 1234);
    pad.lx = pad.ly = 0;
    in_response(&slot, &pad);
    CHECK(pad.lx == 0 && pad.ly == 0);
    return TEST_DONE("input response");
}
#endif

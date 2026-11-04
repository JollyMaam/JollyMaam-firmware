#include "pad.h"

#define MOVED_STICK   2048
#define MOVED_TRIGGER 4096

/* Sticks and triggers are filled in by cal_apply() (input_calibration.c). */
void in_map(const struct in_raw *raw, struct pad *pad)
{
    pad->buttons = in_dpad(raw->buttons);
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

/* Uniform point interpolation in Q15 radius, with fixed 0/full endpoints. */
static u32 response(u32 r, const u8 *points)
{
    const u32 full = 32767;
    u32 t, k, a, b;

    if (r >= full) return full;
    t = r * (STICK_CURVE_POINTS + 1);
    k = t / full;
    t %= full;
    a = k ? points[k - 1] : 0;
    b = k == STICK_CURVE_POINTS ? 255 : points[k];
    return (a * (full - t) + b * t + 127) / 255;
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

static void stick_response(s16 *x, s16 *y, u8 enabled, const u8 *points)
{
    u32 radius, output;

    if (!enabled) return;   /* preserve calibrated samples exactly */
    radius = radius_of(*x, *y);
    if (!radius) return;
    output = response(radius, points);
    *x = (s16)((s32)*x * (s32)output / (s32)radius);
    *y = (s16)((s32)*y * (s32)output / (s32)radius);
}

static s16 flip(s16 v)
{
    return v == -32768 ? 32767 : (s16)-v;
}

static u32 reach(const s16 *v)
{
    return (u32)((s32)v[0] * v[0]) + (u32)((s32)v[1] * v[1]);
}

/* Each physical stick, after its curve: inverted, then onto the host stick it
 * drives.  Two on one host stick: the one pushed further wins. */
static void stick_route(const struct set_remap *m, s16 *xy[2][2])
{
    s16 v[2][2], out[2][2] = { { 0, 0 }, { 0, 0 } };
    u8 used[2] = { 0, 0 };
    u32 s, a, to;

    for (s = 0; s < 2; s++)
        for (a = 0; a < 2; a++)
            v[s][a] = m->invert[s][a] ? flip(*xy[s][a]) : *xy[s][a];
    for (s = 0; s < 2; s++) {
        if (m->maps_to[s] == STICK_NOTHING) continue;
        to = m->maps_to[s] == STICK_OTHER ? 1 - s : s;
        if (used[to] && reach(v[s]) <= reach(out[to])) continue;
        out[to][0] = v[s][0];
        out[to][1] = v[s][1];
        used[to] = 1;
    }
    for (s = 0; s < 2; s++)
        for (a = 0; a < 2; a++) *xy[s][a] = out[s][a];
}

void in_response(const struct set_remap *m, struct pad *pad)
{
    s16 *xy[2][2] = { { &pad->lx, &pad->ly }, { &pad->rx, &pad->ry } };

    if (!m) return;
    stick_response(&pad->lx, &pad->ly, m->left_curve_enabled, m->left_curve_points);
    stick_response(&pad->rx, &pad->ry, m->right_curve_enabled, m->right_curve_points);
    stick_route(m, xy);
}

/* Percent of travel: how far below the press point a pressed trigger lets go
 * (so it does not chatter there), and how far a rapid one moves to change. */
#define TRIGGER_HYSTERESIS 3
#define TRIGGER_RAPID      5
#define TRIGGER_STEP(pct)  ((u32)(pct) * TRIGGER_MAX / 100)

static int trigger_step(struct in_trigger *t, u8 press, u8 mode, u32 v)
{
    u32 at = TRIGGER_STEP(press ? press : 1), rapid = TRIGGER_STEP(TRIGGER_RAPID);
    u32 rest = press > TRIGGER_HYSTERESIS ? TRIGGER_STEP(press - TRIGGER_HYSTERESIS) : 0;
    u8 was = t->down;

    if (mode != TRIGGER_RAPID) {
        t->down = (u8)(t->down ? v > rest : v >= at);
        t->turn = (u16)v;
        return t->down;
    }
    if (t->down) {
        if (v > t->turn) t->turn = (u16)v;
        t->down = (u8)(v > rest && v + rapid > t->turn);
    } else {
        if (v < t->turn) t->turn = (u16)v;
        t->down = (u8)(t->turn > rest ? v >= t->turn + rapid : v >= at);
    }
    if (t->down != was) t->turn = (u16)v;
    return t->down;
}

void in_triggers(const struct set_remap *m, struct in_trigger t[2], struct pad *pad)
{
    u16 *v[2] = { &pad->l2, &pad->r2 };
    u32 i;

    pad->buttons &= ~(u32)(PAD_L2 | PAD_R2);
    for (i = 0; i < 2; i++) {
        if (!m) {
            t[i].down = 0;
            t[i].turn = 0;
            continue;
        }
        if (trigger_step(&t[i], m->trigger_press[i], m->trigger_mode[i], *v[i]))
            pad->buttons |= i ? PAD_R2 : PAD_L2;
        if (m->trigger_mode[i] != TRIGGER_ANALOG) *v[i] = t[i].down ? TRIGGER_MAX : 0;
    }
}

#ifdef UNITTEST
static u16 pct(u32 p) { return (u16)TRIGGER_STEP(p); }

static void triggers_test(void)
{
    struct set_remap slot = { 0 };
    struct in_trigger t[2] = { { 0 } };
    struct pad pad = { 0 };
    u32 i;

    /* Analog: pressed from the press point, let go 3% below; values as they are. */
    slot.trigger_press[0] = 20; slot.trigger_press[1] = 50;
    pad.l2 = pct(19); pad.r2 = pct(60);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == PAD_R2 && pad.l2 == pct(19) && pad.r2 == pct(60));
    pad.l2 = pct(20); pad.r2 = pct(48);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == (PAD_L2 | PAD_R2));
    pad.l2 = pct(17) + 1; pad.r2 = pct(47);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == PAD_L2);
    pad.l2 = pct(17);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == 0);
    /* Press 0 counts as 1%: a trigger at rest is never pressed. */
    slot.trigger_press[0] = 0;
    pad.l2 = 0; pad.r2 = 0;
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons);
    pad.l2 = pct(1);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == PAD_L2);
    pad.l2 = 1;
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == PAD_L2);
    pad.l2 = 0;
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons);

    /* Hair: full or nothing, at the same points. */
    slot.trigger_press[0] = 10; slot.trigger_mode[0] = TRIGGER_HAIR;
    pad.l2 = pct(9);
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons && pad.l2 == 0);
    pad.l2 = pct(10);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == PAD_L2 && pad.l2 == TRIGGER_MAX);
    pad.l2 = pct(8);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == PAD_L2 && pad.l2 == TRIGGER_MAX);
    pad.l2 = pct(7);
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons && pad.l2 == 0);

    /* Rapid: lets go 5% up from the deepest point, presses 5% down from the
     * highest, anywhere past the release point. */
    slot.trigger_mode[0] = TRIGGER_RAPID;
    pad.l2 = pct(80);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == PAD_L2 && pad.l2 == TRIGGER_MAX);
    pad.l2 = pct(76);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == PAD_L2);
    pad.l2 = pct(75);
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons && pad.l2 == 0);
    pad.l2 = pct(60);
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons);
    pad.l2 = pct(64);
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons);
    pad.l2 = pct(65);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == PAD_L2);
    for (i = 66; i <= 100; i++) {                  /* deeper: stays pressed */
        pad.l2 = pct(i);
        in_triggers(&slot, t, &pad);
        CHECK(pad.buttons == PAD_L2);
    }
    pad.l2 = pct(95);
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons);
    pad.l2 = pct(6);                               /* to rest: the press point again */
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons);
    pad.l2 = pct(9) + 100;
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons);
    pad.l2 = pct(10);
    in_triggers(&slot, t, &pad);
    CHECK(pad.buttons == PAD_L2);
    pad.l2 = pct(2);                               /* below the release point: let go */
    in_triggers(&slot, t, &pad);
    CHECK(!pad.buttons);

    /* No remap: nothing pressed, values untouched, state cleared. */
    pad.l2 = pct(90); pad.r2 = pct(90);
    in_triggers(&slot, t, &pad);
    CHECK(t[0].down);
    pad.buttons = PAD_SOUTH | PAD_L2;
    pad.l2 = pct(90);
    in_triggers(NULL, t, &pad);
    CHECK(pad.buttons == PAD_SOUTH && pad.l2 == pct(90) && !t[0].down && !t[1].down);
}

int main(void)
{
    struct set_remap slot = { 0 };
    struct pad pad = { .lx = 12000, .ly = -16000, .rx = -32767, .ry = 0,
                       .buttons = PAD_SOUTH, .l2 = 1234 };
    struct pad original = pad;
    u8 points[STICK_CURVE_POINTS] = { 1, 4, 9, 16, 25, 36, 49, 64, 81, 100, 121, 143, 168, 195, 224 };
    u32 r, last;

    struct in_raw raw = { 0 };

    CHECK(in_hat(PAD_UP | PAD_LEFT) == HAT_UP_LEFT && in_hat(PAD_DOWN) == HAT_DOWN);
    CHECK(in_hat(PAD_RIGHT) == HAT_RIGHT && in_hat(0) == HAT_NEUTRAL);
    raw.buttons = PAD_SOUTH | PAD_UP | PAD_DOWN | PAD_RIGHT;
    in_map(&raw, &pad);
    CHECK(pad.buttons == (PAD_SOUTH | PAD_RIGHT) && in_hat(pad.buttons) == HAT_RIGHT);
    raw.buttons = PAD_DPAD;
    in_map(&raw, &pad);
    CHECK(pad.buttons == 0 && in_hat(pad.buttons) == HAT_NEUTRAL);
    pad = original;
    in_response(&slot, &pad);
    CHECK(!memcmp(&pad, &original, sizeof(pad)));
    in_response(NULL, &pad);
    CHECK(!memcmp(&pad, &original, sizeof(pad)));
    last = 0;
    CHECK(response(0, points) == 0);
    CHECK(response(32767, points) == 32767);
    for (r = 0; r <= 32767; r++) {
        u32 out = response(r, points);
        CHECK(out >= last && out <= 32767);
        last = out;
    }
    CHECK(response(16384, points) > 8100 && response(16384, points) < 8300);
    memcpy(slot.left_curve_points, points, sizeof(points));
    memcpy(slot.right_curve_points, points, sizeof(points));
    slot.left_curve_enabled = slot.right_curve_enabled = 1;
    in_response(&slot, &pad);
    CHECK(pad.lx > 0 && pad.ly < 0 && pad.lx < original.lx);
    CHECK(pad.lx * 4 + pad.ly * 3 >= -4 && pad.lx * 4 + pad.ly * 3 <= 4);
    CHECK(pad.rx == original.rx && pad.ry == 0);
    CHECK(pad.buttons == PAD_SOUTH && pad.l2 == 1234);
    slot.left_curve_enabled = slot.right_curve_enabled = 0;
    pad = original;
    in_response(&slot, &pad);
    CHECK(!memcmp(&pad, &original, sizeof(pad)));
    CHECK(!memcmp(slot.left_curve_points, points, sizeof(points)));
    pad.lx = pad.ly = 0;
    in_response(&slot, &pad);
    CHECK(pad.lx == 0 && pad.ly == 0);
    /* Inverted, sent elsewhere: each setting stays with its physical stick. */
    memset(&slot, 0, sizeof(slot));
    pad = original;
    pad.ry = -32768;
    slot.invert[0][1] = slot.invert[1][1] = 1;
    in_response(&slot, &pad);
    CHECK(pad.lx == 12000 && pad.ly == 16000 && pad.rx == -32767 && pad.ry == 32767);
    slot.maps_to[0] = slot.maps_to[1] = STICK_OTHER;          /* swapped */
    pad = original;
    in_response(&slot, &pad);
    CHECK(pad.lx == -32767 && pad.ly == 0 && pad.rx == 12000 && pad.ry == 16000);
    slot.maps_to[1] = STICK_NOTHING;                           /* left to the right, right off */
    pad = original;
    in_response(&slot, &pad);
    CHECK(pad.lx == 0 && pad.ly == 0 && pad.rx == 12000 && pad.ry == 16000);
    slot.maps_to[1] = STICK_OTHER; slot.maps_to[0] = STICK_SAME; /* both on the left: further wins */
    pad = original;
    in_response(&slot, &pad);
    CHECK(pad.lx == -32767 && pad.ly == 0 && pad.rx == 0 && pad.ry == 0);
    pad.rx = 100;
    pad.lx = 12000; pad.ly = -16000;
    in_response(&slot, &pad);
    CHECK(pad.lx == 12000 && pad.ly == 16000 && pad.rx == 0);
    CHECK(pad.buttons == PAD_SOUTH && pad.l2 == 1234);
    triggers_test();
    return TEST_DONE("input response");
}
#endif

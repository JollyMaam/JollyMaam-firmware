#include "pad.h"

u32 in_keys(const struct pad *p)
{
    static const u8 hats[] = {1,3,2,6,4,12,8,9,0};
    return p->buttons | (u32)hats[p->hat < HAT_NEUTRAL ? p->hat : HAT_NEUTRAL] << 16;
}

void in_set_keys(struct pad *p, u32 keys)
{
    static const u8 hats[] = {8,0,2,1,4,8,3,2,6,7,8,0,5,6,4,8};
    u32 directions = (keys >> 16) & 15;
    /* Opposite directions cancel independently, leaving the other axis. */
    if ((directions & 5) == 5) directions &= ~5u;
    if ((directions & 10) == 10) directions &= ~10u;
    p->buttons = (u16)(keys & 0x7fff);
    p->hat = hats[directions];
}

void in_remap(const struct set_mapping *m, struct pad *p)
{
    u32 keys = in_keys(p), out = 0, i;
    s16 sticks[4] = {p->lx, p->ly, p->rx, p->ry}, mapped[4];
    u16 triggers[3] = {0, p->l2, p->r2};
    for (i = 0; i < 20; i++) if (keys & (1u << i)) out |= m->digital[i];
    in_set_keys(p, out);
    for (i = 0; i < 4; i++) {
        u8 source = m->axis[i];
        s32 value = source ? sticks[(source - 1) % 4] : 0;
        if (source > 4) value = -value;
        if (value > STICK_MAX) value = STICK_MAX;
        mapped[i] = (s16)value;
    }
    p->lx = mapped[0]; p->ly = mapped[1]; p->rx = mapped[2]; p->ry = mapped[3];
    p->l2 = triggers[m->axis[4] % 3]; p->r2 = triggers[m->axis[5] % 3];
}

#ifdef UNITTEST
int main(void)
{
    struct set_mapping m = {0};
    struct pad p = {.buttons = PAD_A | PAD_M, .hat = HAT_UP_LEFT,
                    .lx = 1234, .ly = -32768, .rx = -9876, .ry = 42, .l2 = 123, .r2 = 456};
    struct pad original = p;
    u32 i;
    for (i = 0; i < 20; i++) m.digital[i] = 1u << i;
    for (i = 0; i < 4; i++) m.axis[i] = i + 1;
    m.axis[4] = 1; m.axis[5] = 2;
    in_remap(&m, &p); CHECK(!memcmp(&p, &original, sizeof(p)));
    m.digital[0] = PAD_B | PAD_X; m.digital[5] = 0;
    m.digital[16] = PAD_R3; m.digital[19] = 1u << 18;
    m.axis[0] = 3; m.axis[1] = 6; m.axis[2] = 1; m.axis[3] = 0;
    m.axis[4] = 2; m.axis[5] = 1;
    in_remap(&m, &p);
    CHECK(p.buttons == (PAD_B | PAD_X | PAD_R3) && p.hat == HAT_DOWN);
    CHECK(p.lx == -9876 && p.ly == 32767 && p.rx == 1234 && !p.ry);
    CHECK(p.l2 == 456 && p.r2 == 123);
    in_set_keys(&p, 15u << 16); CHECK(p.hat == HAT_NEUTRAL);
    in_set_keys(&p, 7u << 16); CHECK(p.hat == HAT_RIGHT);
    /* Multiple physical keys targeting one output stay down until all release. */
    m.digital[0] = m.digital[1] = PAD_X;
    p.buttons = PAD_A | PAD_B; p.hat = HAT_NEUTRAL;
    in_remap(&m, &p); CHECK(p.buttons == PAD_X);
    p.buttons = PAD_B; in_remap(&m, &p); CHECK(p.buttons == PAD_X);
    p.buttons = 0; in_remap(&m, &p); CHECK(!p.buttons);
    for (i = 0; i <= HAT_NEUTRAL; i++) {
        p.hat = i; in_set_keys(&p, in_keys(&p)); CHECK(p.hat == i);
    }
    return TEST_DONE("mapping");
}
#endif

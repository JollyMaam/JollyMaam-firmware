#include <string.h>
#include "pad.h"

#define PLAYERS 4
#define REACH_QUIET 24
#define REACH_DECAY 12

static const struct led_wave charging = LED_BLINK(800, 1600);
static const struct led_wave low_battery = LED_BLINK(150, 2850);
static const struct led_wave player_flash = LED_BLINK(250, 250);

static u8 level(const struct led_wave *w, u32 t, u32 pos, u32 count)
{
    u32 period, phase, rise, lit;

    switch (w->kind) {
    case WAVE_SOLID:
        return 255;
    case WAVE_BLINK:
        period = (u32)w->a + w->b;
        return period && t % period < w->a ? 255 : 0;
    case WAVE_BREATHE:
        period = w->a ? w->a : 1;
        phase = (t + period / 2) % period;
        rise = phase < period / 2 ? phase * 510 / period : (period - phase) * 510 / period;
        if (rise > 255) rise = 255;
        return (u8)(rise * rise / 255);
    case WAVE_SWEEP:
        if (count < 2) return 255;
        period = 2 * (count - 1);
        phase = t / (w->a ? w->a : 1) % period;
        lit = phase < count ? phase : period - phase;
        return pos == lit ? 255 : 0;
    default:
        return 0;
    }
}

static struct rgb dim(struct rgb c, u32 l)
{
    struct rgb o = { (u8)((c.r * l + 127) / 255), (u8)((c.g * l + 127) / 255),
                     (u8)((c.b * l + 127) / 255) };
    return o;
}

/* A B X Y light their own bar light; the d-pad slots all but that one. */
static u32 slot_lights(u8 slot)
{
    u32 one = 1u << (slot % PLAYERS);
    return slot < PLAYERS ? one : 0xf & ~one;
}

static u32 bits(u32 m)
{
    u32 n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

static void bar(const struct led_scene *sc, const struct led_state *s, u32 now, u32 t,
                const struct led_inputs *in, struct rgb c, struct led_frame *out)
{
    u32 on = 0, flash = 0, pos = 0, count, i;

    if (sc->bar == BAR_ALL) {
        on = 0xf;
    } else if (sc->bar == BAR_PLAYER && in->has_players) {
        on = in->players & 0xf;
        flash = in->players_flash & 0xf & ~on;
    } else if (s->slot != SLOT_NONE) {
        on = slot_lights(s->slot);
    }
    count = bits(on);
    for (i = 0; i < PLAYERS; i++) {
        u32 l = 0;
        if (on & (1u << i)) l = level(&sc->bar_wave, t, pos++, count);
        else if (flash & (1u << i))
            l = level(&sc->bar_wave, t, 0, 1) * level(&player_flash, now, 0, 1) / 255;
        out->light[LIGHT_P0 + i] = dim(c, l);
    }
}

static void grips(const struct led_scene *sc, u32 t, const struct led_inputs *in,
                  struct rgb c, struct led_frame *out)
{
    int lightbar = in->grips == GRIPS_LIGHTBAR && in->has_lightbar &&
                   (in->lightbar.r || in->lightbar.g || in->lightbar.b);
    u32 side;

    if (sc->grips == GRIPS_WAVE) {
        out->light[LIGHT_GRIP_L] = out->light[LIGHT_GRIP_R] = dim(c, level(&sc->grips_wave, t, 0, 1));
        return;
    }
    for (side = 0; side < 2; side++) {
        u32 l = lightbar ? 255 : 0;
        if ((in->grips == GRIPS_RUMBLE || in->grips == GRIPS_BOTH) && in->rumble[side] > l)
            l = in->rumble[side];
        if ((in->grips == GRIPS_INPUT || in->grips == GRIPS_BOTH) && in->reach[side] > l)
            l = in->reach[side];
        out->light[LIGHT_GRIP_L + side] = dim(lightbar ? in->lightbar : c, l);
    }
}

void led_frame(const struct led_scene *sc, const struct led_state *s, u32 now,
               const struct led_inputs *in, struct led_frame *out)
{
    u32 t = now - s->since_ms;
    struct rgb c = sc->colour == COLOUR_SLOT && s->slot != SLOT_NONE ? in->slot_colour
                                                                     : in->idle_colour;
    struct rgb *home = &out->light[LIGHT_HOME];

    memset(out, 0, sizeof(*out));
    *home = dim(c, level(&sc->home, t, 0, 1));
    bar(sc, s, now, t, in, c, out);
    grips(sc, t, in, c, out);
    if (in->usb_powered && (s->scene == CTL_BOOT || s->scene == CTL_IDLE))
        *home = dim(in->battery_colour, level(&charging, now, 0, 1));
    if (in->low_battery && level(&low_battery, now, 0, 1)) *home = in->battery_colour;
    if (in->usb_plug_flash == 1) *home = s->slot != SLOT_NONE ? c : in->battery_colour;
    if (in->usb_plug_flash == 2) memset(home, 0, sizeof(*home));
    if (in->cal_lit) *home = in->cal;
}

static u8 stick_reach(s16 x, s16 y)
{
    s32 ax = x < 0 ? -(s32)x : x, ay = y < 0 ? -(s32)y : y, m = ax > ay ? ax : ay;
    return (u8)(m >= STICK_MAX ? 255 : m >> 7);
}

static u8 max8(u8 a, u8 b)
{
    return a > b ? a : b;
}

/* How busy each half of the pad is, for grips that follow input.  Call once
 * per frame; it fades by itself. */
void led_reach(u8 reach[2], const struct pad *p)
{
    const u16 left_buttons = PAD_L1 | PAD_L4 | PAD_L3 | PAD_SELECT;
    const u16 right_buttons = PAD_R1 | PAD_R4 | PAD_R3 | PAD_START | PAD_FACE;
    u8 now[2];
    u32 side;

    now[0] = max8(stick_reach(p->lx, p->ly), p->l2 >> 8);
    now[1] = max8(stick_reach(p->rx, p->ry), p->r2 >> 8);
    if ((p->buttons & left_buttons) || p->hat != HAT_NEUTRAL) now[0] = 255;
    if (p->buttons & right_buttons) now[1] = 255;
    for (side = 0; side < 2; side++) {
        u8 faded = reach[side] > REACH_DECAY ? reach[side] - REACH_DECAY : 0;
        reach[side] = max8(now[side] < REACH_QUIET ? 0 : now[side], faded);
    }
}

#ifdef UNITTEST
static const struct led_scene connecting = {
    COLOUR_SLOT, LED_BLINK(500, 500), BAR_SLOT, LED_SOLID, GRIPS_WAVE, LED_OFF,
};
static const struct led_scene connected = {
    COLOUR_SLOT, LED_SOLID, BAR_PLAYER, LED_SOLID, GRIPS_SLOT_SETTING, LED_OFF,
};
static const struct led_scene idle = {
    COLOUR_IDLE, LED_OFF, BAR_ALL, LED_OFF, GRIPS_WAVE, LED_OFF,
};

static int same(struct rgb a, struct rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

int main(void)
{
    struct led_inputs in;
    struct led_state s = { CTL_CONNECTING, 1, 1000 };
    struct led_frame f;
    struct rgb red = { 255, 0, 0 }, dark = { 0, 0, 0 }, white = { 5, 5, 5 };
    struct pad p;
    u8 reach[2] = { 0, 0 };

    memset(&in, 0, sizeof(in));
    in.slot_colour = red;
    in.idle_colour = white;
    in.battery_colour = (struct rgb){ 5, 2, 0 };

    led_frame(&connecting, &s, 1100, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], red));
    CHECK(same(f.light[LIGHT_P1], red) && same(f.light[LIGHT_P0], dark));
    led_frame(&connecting, &s, 1600, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], dark));

    s.slot = 5;   /* down: all bar lights but the second */
    led_frame(&connecting, &s, 1100, &in, &f);
    CHECK(same(f.light[LIGHT_P1], dark) && same(f.light[LIGHT_P0], red));

    s.scene = CTL_CONNECTED;
    s.slot = 0;
    in.has_players = 1;
    in.players = 0x4;
    in.grips = GRIPS_LIGHTBAR;
    in.has_lightbar = 1;
    in.lightbar = (struct rgb){ 0, 0, 200 };
    led_frame(&connected, &s, 2000, &in, &f);
    CHECK(same(f.light[LIGHT_P2], red) && same(f.light[LIGHT_P0], dark));
    CHECK(same(f.light[LIGHT_GRIP_L], in.lightbar));

    s.scene = CTL_IDLE;
    s.slot = SLOT_NONE;
    in.usb_powered = 1;
    led_frame(&idle, &s, 100, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], in.battery_colour));
    in.cal_lit = 1;
    in.cal = white;
    led_frame(&idle, &s, 100, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], white));

    memset(&p, 0, sizeof(p));
    p.hat = HAT_NEUTRAL;
    p.buttons = PAD_A;
    led_reach(reach, &p);
    CHECK(reach[0] == 0 && reach[1] == 255);
    p.buttons = 0;
    led_reach(reach, &p);
    CHECK(reach[1] == 255 - REACH_DECAY);
    return TEST_DONE("leds");
}
#endif

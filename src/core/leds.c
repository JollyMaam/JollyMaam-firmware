#include <string.h>
#include "pad.h"

#define PLAYERS 4
#define REACH_QUIET 24
#define REACH_DECAY 12
#define K_ONE        65536u      /* a wave's full light */
#define FADE_IN_MS   250         /* a new scene over the last frame of the old; a blink's edges */
#define RAINBOW_MS   6000        /* once round the colours */
#define HEARTBEAT_MS 1200
#define SWEEP_WIDTH  (K_ONE * 5 / 4)    /* how far a sweep's glow reaches, in lights */

static const struct led_wave low_battery = LED_BLINK(150, 2850);
static const struct led_wave player_flash = LED_BLINK(250, 250);
static const struct led_wave fade = LED_BREATHE(4000);
static const struct led_wave quick_breathe = LED_BREATHE(1600);

/* Ephemeral mode: Home goes dark twice, briefly, every few seconds. */
#define WINK_PERIOD_MS 4000
#define WINK_MS        100

/* ---- light ------------------------------------------------------------------
 * A channel times its brightness (tenths of the LEDs' steps), 0-650250.  The
 * LEDs' PWM is linear in light, so colours mix here; a frame keeps 1/256 of
 * a step (struct led_fine), and led_dither() makes the LEDs' 8 bits of it. */

struct light { u32 r, g, b; };

#define FULL 2550                /* an rgb value at full strength: 255 steps, in tenths */

static const struct light dark;

static struct light lit(struct shade s)
{
    struct light l = { (u32)s.rgb.r * s.brightness, (u32)s.rgb.g * s.brightness, (u32)s.rgb.b * s.brightness };
    return l;
}

static struct shade at(struct shade s, u16 brightness)
{
    s.brightness = brightness;
    return s;
}

static struct light lit_rgb(struct rgb c)
{
    struct light l = { c.r * FULL, c.g * FULL, c.b * FULL };
    return l;
}

/* k of K_ONE of it. */
static struct light scale(struct light l, u32 k)
{
    l.r = (u32)((u64)l.r * k >> 16);
    l.g = (u32)((u64)l.g * k >> 16);
    l.b = (u32)((u64)l.b * k >> 16);
    return l;
}

/* From a (k 0) to b (k K_ONE). */
static u32 mix1(u32 a, u32 b, u32 k)
{
    return (u32)((s64)a + (((s64)b - a) * k >> 16));
}

static struct light mix(struct light a, struct light b, u32 k)
{
    struct light l = { mix1(a.r, b.r, k), mix1(a.g, b.g, k), mix1(a.b, b.b, k) };
    return l;
}

/* In 1/256 steps, and back. */
static u16 fine1(u32 v)
{
    v = (v * 128 + FULL / 4) / (FULL / 2);
    return (u16)(v > 255 * 256 ? 255 * 256 : v);
}

static void put(u16 out[3], struct light l)
{
    out[0] = fine1(l.r);
    out[1] = fine1(l.g);
    out[2] = fine1(l.b);
}

static struct light got(const u16 in[3])
{
    struct light l = { (in[0] * (FULL / 2) + 64) / 128, (in[1] * (FULL / 2) + 64) / 128,
                       (in[2] * (FULL / 2) + 64) / 128 };
    return l;
}

/* ---- shapes -----------------------------------------------------------------
 * Waves work in how bright a light looks (0..K_ONE) and return light: about
 * its square, so a fade looks even rather than rushing at the dark end. */

static u32 bright(u32 p)
{
    return (u32)((u64)p * p >> 16);
}

/* Eased in and out (smoothstep). */
static u32 ease(u32 p)
{
    u64 x = p;

    return (u32)(x * x * (3 * (u64)K_ONE - 2 * x) >> 32);
}

static u32 wave(const struct led_wave *w, u32 t, u32 pos, u32 count)
{
    u32 period, phase, edge;
    u64 x, span;

    switch (w->kind) {
    case WAVE_SOLID:
        return K_ONE;
    case WAVE_BLINK:            /* fading as a new scene does: up as it goes on, down in the off time */
        period = (u32)w->a + w->b;
        if (!period) return 0;
        phase = t % period;
        if (phase < w->a) {
            edge = w->a / 2 < FADE_IN_MS ? w->a / 2 : FADE_IN_MS;
            return edge && phase < edge ? ease(phase * K_ONE / edge) : K_ONE;
        }
        edge = w->b / 2 < FADE_IN_MS ? w->b / 2 : FADE_IN_MS;
        phase -= w->a;
        return edge && phase < edge ? ease(K_ONE - phase * K_ONE / edge) : 0;
    case WAVE_BREATHE:          /* brightest at its start */
        period = w->a ? w->a : 1;
        phase = (t + period / 2) % period;
        x = (u64)(phase < period / 2 ? phase : period - phase) * 2 * K_ONE / period;
        return bright(ease((u32)(x > K_ONE ? K_ONE : x)));
    case WAVE_SWEEP:            /* to and fro, a step from one light to the next */
        if (count < 2) return K_ONE;
        span = (u64)(count - 1) * K_ONE;
        x = (u64)(t % (2 * (count - 1) * (w->a ? w->a : 1))) * K_ONE / (w->a ? w->a : 1);
        if (x > span) x = 2 * span - x;
        x = x > (u64)pos * K_ONE ? x - (u64)pos * K_ONE : (u64)pos * K_ONE - x;
        return x >= SWEEP_WIDTH ? 0 : bright((u32)(K_ONE - x * K_ONE / SWEEP_WIDTH));
    default:
        return 0;
    }
}

/* Two beats, the second softer, then a rest. */
static u32 heartbeat(u32 t)
{
    static const u16 beats[2][2] = { { 0, 100 }, { 250, 60 } };   /* when, height in percent */
    u32 phase = t % HEARTBEAT_MS, best = 0, i;

    for (i = 0; i < 2; i++) {
        u32 since = phase - beats[i][0], p = 0;

        if (phase < beats[i][0]) continue;
        if (since < 60) p = since * K_ONE / 60;
        else if (since < 60 + 250) p = (60 + 250 - since) * K_ONE / 250;
        p = p * beats[i][1] / 100;
        if (p > best) best = p;
    }
    return bright(ease(best));
}

/* Full colour round the hue circle, 0-1535. */
static struct shade hue(u32 h, u16 brightness)
{
    u8 f = (u8)(h % 256);
    struct shade s = { { 0, 0, 0 }, brightness };

    switch (h / 256 % 6) {
    case 0: s.rgb = (struct rgb){ 255, f, 0 }; break;
    case 1: s.rgb = (struct rgb){ (u8)(255 - f), 255, 0 }; break;
    case 2: s.rgb = (struct rgb){ 0, 255, f }; break;
    case 3: s.rgb = (struct rgb){ 0, (u8)(255 - f), 255 }; break;
    case 4: s.rgb = (struct rgb){ f, 0, 255 }; break;
    default: s.rgb = (struct rgb){ 255, 0, (u8)(255 - f) }; break;
    }
    return s;
}

static u32 rainbow(u32 t)
{
    return t % RAINBOW_MS * 1536 / RAINBOW_MS;
}

/* ---- the lights ------------------------------------------------------------- */

static int winking(u32 now)
{
    u32 t = now % WINK_PERIOD_MS;
    return t < WINK_MS || (t >= 2 * WINK_MS && t < 3 * WINK_MS);
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

/* Connected: the slot's players setting.  Host: the console's player number
 * in the secondary colour, the other lights in the primary, so the bar never
 * reads as black case; dark until the console sets one.  The animated ones
 * are lit at the animation brightness. */
static void players(const struct led_scene *sc, u32 now, u32 t, const struct led_inputs *in,
                    struct led_fine *out)
{
    struct light c = lit(in->primary_colour), number_colour = lit(in->secondary_colour);
    u32 k = wave(&sc->bar_wave, t, 0, 1), number = 0, i;

    switch (in->player_lights) {
    case PLAYERS_HOST:
        if (!in->has_players) return;
        number = in->players & 0xf;
        if (now % ((u32)player_flash.a + player_flash.b) < player_flash.a)   /* switched, not faded */
            number |= in->players_flash & 0xf;
        break;
    case PLAYERS_FADE:
        c = mix(lit(at(in->secondary_colour, in->animation)), lit(at(in->primary_colour, in->animation)),
                wave(&fade, t, 0, 1));
        break;
    case PLAYERS_PRIMARY: break;
    case PLAYERS_SECONDARY: c = lit(in->secondary_colour); break;
    case PLAYERS_RAINBOW:        /* the colours run down the bar */
        for (i = 0; i < PLAYERS; i++)
            put(out->light[LIGHT_P0 + i], scale(lit(hue(rainbow(t) + 1536 - i * 128, in->animation)), k));
        return;
    default: return;
    }
    for (i = 0; i < PLAYERS; i++)
        put(out->light[LIGHT_P0 + i], scale(number & (1u << i) ? number_colour : c, k));
}

static void bar(const struct led_scene *sc, const struct led_state *s, u32 now, u32 t,
                const struct led_inputs *in, struct shade c, struct led_fine *out)
{
    u32 on = 0, pos = 0, count, i;

    if (sc->bar == BAR_PLAYER) {
        players(sc, now, t, in, out);
        return;
    }
    if (sc->bar == BAR_ALL) on = 0xf;
    else if (s->slot != SLOT_NONE) on = slot_lights(s->slot);
    count = bits(on);
    for (i = 0; i < PLAYERS; i++)
        put(out->light[LIGHT_P0 + i], scale(lit(c), on & (1u << i) ? wave(&sc->bar_wave, t, pos++, count) : 0));
}

/* Connected: the slot's grips setting.  The reactive ones brighten from the
 * secondary colour (steady) to the primary (animation brightness) with
 * rumble or with how busy that half of the pad is. */
static void grips(const struct led_scene *sc, u32 t, const struct led_inputs *in,
                  struct shade c, struct led_fine *out)
{
    struct light primary_fx = lit(at(in->primary_colour, in->animation));
    struct light l;
    u32 side, r;

    if (sc->grips == GRIPS_WAVE) {
        put(out->light[LIGHT_GRIP_L], scale(lit(c), wave(&sc->grips_wave, t, 0, 1)));
        memcpy(out->light[LIGHT_GRIP_R], out->light[LIGHT_GRIP_L], sizeof(out->light[0]));
        return;
    }
    for (side = 0; side < 2; side++) {
        switch (in->grips) {
        case GRIPS_HOST: l = in->has_lightbar ? lit_rgb(in->lightbar) : dark; break;
        case GRIPS_FADE:
            l = mix(lit(at(in->secondary_colour, in->animation)), primary_fx, wave(&fade, t, 0, 1));
            break;
        case GRIPS_RUMBLE:
        case GRIPS_INPUT:
        case GRIPS_BOTH:
            r = 0;
            if (in->grips != GRIPS_INPUT) r = in->rumble[side];
            if (in->grips != GRIPS_RUMBLE && in->reach[side] > r) r = in->reach[side];
            l = mix(lit(in->secondary_colour), primary_fx, bright(r * 257));
            break;
        case GRIPS_PRIMARY: l = lit(in->primary_colour); break;
        case GRIPS_SECONDARY: l = lit(in->secondary_colour); break;
        case GRIPS_RAINBOW: l = lit(hue(rainbow(t) + side * 768, in->animation)); break;
        case GRIPS_HEARTBEAT: l = mix(lit(in->secondary_colour), primary_fx, heartbeat(t)); break;
        default: l = dark; break;
        }
        put(out->light[LIGHT_GRIP_L + side], l);
    }
}

/* ---- the whole pad ------------------------------------------------------------ */

/* How far down the pad a light is: Home, the bar top to bottom, the grips. */
static const u8 depth[LIGHT_COUNT] = { 0, 1, 2, 3, 4, 5, 5, 0, 1, 1 };
#define DEPTHS 6

#define QUICK_HUE_STEP 200      /* colour between one light and the next down */
#define LOADER_FILL_MS  500      /* the light runs down the pad */
#define LOADER_FLASH_MS 300      /* everything bright, then down to what is held */
#define LOADER_HOLD     (K_ONE * 35 / 100)
static const struct rgb loader_green = { 0, 255, 0 };

static u32 loader(u32 t, u32 d)
{
    u64 head;
    u32 at = d * K_ONE, k = 0;

    if (t >= LOADER_FILL_MS) {   /* the flash, and down to the hold */
        t -= LOADER_FILL_MS;
        return t >= LOADER_FLASH_MS ? LOADER_HOLD
                                    : LOADER_HOLD + (u32)((u64)(K_ONE - LOADER_HOLD) * (K_ONE - ease(t * K_ONE / LOADER_FLASH_MS)) >> 16);
    }
    head = (u64)t * (DEPTHS + 1) * K_ONE / LOADER_FILL_MS;    /* where the light is, in lights */
    if (head > at) k = LOADER_HOLD;                          /* passed: lit behind it */
    if (head + K_ONE > at && head < at + 2 * K_ONE) {        /* near: its glow */
        u32 off = (u32)(head > at ? head - at : at - head);
        u32 glow = off >= K_ONE ? 0 : bright(K_ONE - off);

        if (glow > k) k = glow;
    }
    return k;
}

static void whole_pad(const struct led_scene *sc, u32 t, const struct led_inputs *in, struct led_fine *out)
{
    u32 i, k = K_ONE / 3 + (u32)((u64)wave(&quick_breathe, t, 0, 1) * 2 / 3);

    for (i = 0; i < LIGHT_UNUSED; i++) {
        if (sc->effect == EFFECT_QUICK)          /* the colours flow down the pad, breathing */
            put(out->light[i], scale(lit(hue(rainbow(2 * t) + 6 * 1536 - depth[i] * QUICK_HUE_STEP, in->animation)), k));
        else if (sc->effect == EFFECT_LOADER)
            put(out->light[i], scale(lit((struct shade){ loader_green, in->animation }), loader(t, depth[i])));
    }
}

void led_frame(const struct led_scene *sc, const struct led_state *s, u32 now,
               const struct led_inputs *in, struct led_fine *out)
{
    u32 t = now - s->since_ms, i;
    struct shade c = s->slot == SLOT_NONE ? in->idle_colour
                   : sc->colour == COLOUR_CONNECT ? in->connect_colour
                   : sc->colour == COLOUR_PRIMARY ? in->primary_colour : in->idle_colour;
    u16 *home = out->light[LIGHT_HOME];
    u32 k;

    memset(out, 0, sizeof(*out));
    if (sc->effect) {
        whole_pad(sc, t, in, out);
    } else {
        put(home, scale(lit(c), wave(&sc->home, t, 0, 1)));
        bar(sc, s, now, t, in, c, out);
        grips(sc, t, in, c, out);
        if (in->usb_powered && (s->scene == CTL_BOOT || s->scene == CTL_IDLE))
            put(home, dark);
    }
    if (t < FADE_IN_MS) {
        k = ease(t * K_ONE / FADE_IN_MS);
        for (i = 0; i < LIGHT_COUNT; i++)
            put(out->light[i], mix(got(s->from.light[i]), got(out->light[i]), k));
    }
    /* What follows shows at once, over the scene. */
    if (in->low_battery && (k = wave(&low_battery, now, 0, 1)))
        put(home, mix(got(home), lit(in->battery_colour), k));
    if (in->ephemeral && winking(now)) put(home, dark);
    if (in->signal) put(home, in->signal == LED_SIGNAL_LIT ? lit(in->primary_colour) : dark);
    if (in->cal_lit) put(home, lit_rgb(in->cal));
    /* USB acknowledgement is one solid interval, then the normal scene.
     * Idle on USB stays dark; there is no charging animation. */
    if (in->usb_plug_lit) put(home, lit(in->battery_colour));
}

/* Only dim lights are dithered: there a step is a big part of the light (one
 * step at 1 of 255 is all of it), and higher up rounding is not seen. */
#define DITHER_BELOW (16 * 256)

int led_dither(const struct led_fine *f, u16 acc[LIGHT_COUNT][3], int dither, struct led_frame *out)
{
    u32 i, k, v, more = 0;

    for (i = 0; i < LIGHT_COUNT; i++)
        for (k = 0; k < 3; k++) {
            u8 *o = k == 0 ? &out->light[i].r : k == 1 ? &out->light[i].g : &out->light[i].b;

            v = f->light[i][k];
            if (dither && v < DITHER_BELOW && (v & 255)) {
                v += acc[i][k];
                acc[i][k] = (u16)(v & 255);
                more = 1;
            } else {
                v += 128;
                acc[i][k] = 0;
            }
            *o = (u8)(v >> 8 > 255 ? 255 : v >> 8);
        }
    return (int)more;
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
    const u32 left_buttons = PAD_L1 | PAD_L4 | PAD_L3 | PAD_SELECT | PAD_DPAD;
    const u32 right_buttons = PAD_R1 | PAD_R4 | PAD_R3 | PAD_START | PAD_FACE;
    u8 now[2];
    u32 side;

    now[0] = max8(stick_reach(p->lx, p->ly), p->l2 >> 8);
    now[1] = max8(stick_reach(p->rx, p->ry), p->r2 >> 8);
    if (p->buttons & left_buttons) now[0] = 255;
    if (p->buttons & right_buttons) now[1] = 255;
    for (side = 0; side < 2; side++) {
        u8 faded = reach[side] > REACH_DECAY ? reach[side] - REACH_DECAY : 0;
        reach[side] = max8(now[side] < REACH_QUIET ? 0 : now[side], faded);
    }
}

#ifdef UNITTEST
static const struct led_scene connecting = {
    COLOUR_CONNECT, LED_BLINK(500, 500), BAR_ALL, LED_BLINK(500, 500), GRIPS_WAVE, LED_OFF,
};
static const struct led_scene pairing = {
    COLOUR_CONNECT, LED_BLINK(120, 120), BAR_ALL, LED_SWEEP(120), GRIPS_WAVE, LED_OFF,
};
static const struct led_scene stopping = {
    COLOUR_CONNECT, LED_OFF, BAR_ALL, LED_OFF, GRIPS_WAVE, LED_OFF,
};
static const struct led_scene connected = {
    COLOUR_PRIMARY, LED_SOLID, BAR_PLAYER, LED_SOLID, GRIPS_SLOT_SETTING, LED_OFF,
};
static const struct led_scene idle = {
    COLOUR_IDLE, LED_OFF, BAR_ALL, LED_OFF, GRIPS_WAVE, LED_OFF,
};

/* A frame at the LEDs' steps, rounded. */
static void frame(const struct led_scene *sc, const struct led_state *s, u32 now,
                  const struct led_inputs *in, struct led_frame *out)
{
    struct led_fine fine;
    u16 acc[LIGHT_COUNT][3];

    led_frame(sc, s, now, in, &fine);
    led_dither(&fine, acc, 0, out);
}

static int same(struct rgb a, struct rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static int bright_as(struct rgb a)
{
    return a.r + a.g + a.b;
}

int main(void)
{
    struct led_inputs in;
    struct led_state s = { CTL_CONNECTING, 1, 1000 };
    struct led_frame f, g;
    struct rgb red = { 255, 0, 0 }, dark_rgb = { 0, 0, 0 }, white = { 5, 5, 5 };
    struct rgb dim_red = { 2, 0, 0 }, dim_green = { 0, 1, 0 };
    struct pad p;
    u8 reach[2] = { 0, 0 };
    u32 t, last;

    memset(&in, 0, sizeof(in));
    in.connect_colour = (struct shade){ { 255, 0, 0 }, 2550 };
    in.primary_colour = (struct shade){ { 255, 0, 0 }, 20 };
    in.secondary_colour = (struct shade){ { 0, 255, 0 }, 10 };
    in.idle_colour = (struct shade){ { 255, 255, 255 }, 50 };
    in.battery_colour = (struct shade){ { 255, 102, 0 }, 50 };
    in.animation = 400;

    /* The light maths: 1/256 of a step kept, rounded only for the LEDs. */
    {
        struct led_fine fine;
        u16 acc[LIGHT_COUNT][3] = { { 0 } };
        u32 i, sum = 0;

        memset(&fine, 0, sizeof(fine));
        put(fine.light[0], lit(in.primary_colour));
        put(fine.light[1], scale(lit(in.connect_colour), K_ONE / 2));
        put(fine.light[2], lit((struct shade){ { 255, 0, 0 }, 15 }));     /* 1.5 */
        put(fine.light[3], lit((struct shade){ { 255, 0, 0 }, 405 }));    /* 40.5: not dim */
        CHECK(fine.light[0][0] == 2 * 256 && fine.light[2][0] == 384);
        CHECK(!led_dither(&fine, acc, 0, &f) && f.light[2].r == 2);       /* rounded */
        CHECK(same(f.light[0], dim_red) && f.light[1].r == 128 && f.light[3].r == 41);
        for (i = 0; i < 8; i++) {                                          /* 1, 2, 1, 2: 1.5 */
            CHECK(led_dither(&fine, acc, 1, &f));
            CHECK(f.light[2].r == 1 + (i & 1) && f.light[0].r == 2 && f.light[3].r == 41);
            sum += f.light[2].r;
        }
        CHECK(sum == 12);
        memset(&fine.light[2], 0, sizeof(fine.light[2]));
        CHECK(!led_dither(&fine, acc, 1, &f));                             /* whole steps: nothing to do */
    }
    CHECK(ease(0) == 0 && ease(K_ONE) == K_ONE && ease(K_ONE / 2) == K_ONE / 2);
    CHECK(bright(K_ONE) == K_ONE && bright(K_ONE / 2) == K_ONE / 4);

    /* Connecting: Home and all four blink together, with soft edges. */
    frame(&connecting, &s, 1300, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], red));
    CHECK(same(f.light[LIGHT_P1], red) && same(f.light[LIGHT_P3], red));
    frame(&connecting, &s, 1800, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], dark_rgb) && same(f.light[LIGHT_P0], dark_rgb));
    for (t = 0, last = 0; t <= FADE_IN_MS; t += 5) {   /* up as a new scene fades in */
        u32 k = wave(&connecting.home, t, 0, 1);

        CHECK(k >= last && k == ease(t * K_ONE / FADE_IN_MS));
        last = k;
    }
    for (t = 500; t <= 500 + FADE_IN_MS; t += 5) {     /* and down in the off time */
        u32 k = wave(&connecting.home, t, 0, 1);

        CHECK(k <= last);
        last = k;
    }
    CHECK(wave(&connecting.home, 499, 0, 1) == K_ONE && !last);
    /* A quick blink is all edges: up and down over half of each. */
    CHECK(wave(&pairing.home, 30, 0, 1) == K_ONE / 2 && wave(&pairing.home, 60, 0, 1) == K_ONE &&
          wave(&pairing.home, 120 + 30, 0, 1) == K_ONE / 2 && !wave(&pairing.home, 120 + 60, 0, 1));
    /* Breathing eases out of its brightest and back. */
    CHECK(wave(&fade, 0, 0, 1) == K_ONE && wave(&fade, 2000, 0, 1) == 0);
    CHECK(wave(&fade, 1000, 0, 1) > K_ONE / 8 && wave(&fade, 1000, 0, 1) < K_ONE / 2);

    /* Pairing: the sweep glides; one light at a time is brightest. */
    for (t = 0; t < 720; t += 10) {
        u32 i, top = 0, top_k = 0;

        for (i = 0; i < 4; i++)
            if (wave(&pairing.bar_wave, t, i, 4) > top_k) top_k = wave(&pairing.bar_wave, t, i, 4), top = i;
        if (t == 0) CHECK(top == 0 && top_k == K_ONE);
        if (t == 360) CHECK(top == 3 && top_k == K_ONE);
        if (t == 180) CHECK(wave(&pairing.bar_wave, t, 1, 4) == wave(&pairing.bar_wave, t, 2, 4) &&
                            top_k < K_ONE && !wave(&pairing.bar_wave, t, 0, 4));   /* between two */
    }

    /* A new scene fades in over the old frame. */
    s.scene = CTL_STOPPING;
    s.slot = 5;   /* disconnecting must not show the slot on the player bar */
    memset(&s.from, 0, sizeof(s.from));
    s.from.light[LIGHT_P0][0] = 255 * 256;
    frame(&stopping, &s, 1000, &in, &f);
    CHECK(same(f.light[LIGHT_P0], red));
    frame(&stopping, &s, 1000 + FADE_IN_MS / 2, &in, &f);
    CHECK(f.light[LIGHT_P0].r > 0 && f.light[LIGHT_P0].r < 255);
    frame(&stopping, &s, 1000 + FADE_IN_MS, &in, &f);
    CHECK(same(f.light[LIGHT_P0], dark_rgb) && same(f.light[LIGHT_P1], dark_rgb) &&
          same(f.light[LIGHT_P2], dark_rgb) && same(f.light[LIGHT_P3], dark_rgb));
    memset(&s.from, 0, sizeof(s.from));

    s.scene = CTL_CONNECTED;
    s.slot = 0;
    in.player_lights = PLAYERS_HOST;
    in.grips = GRIPS_HOST;
    frame(&connected, &s, 2000, &in, &f);   /* nothing from the console: dark */
    CHECK(same(f.light[LIGHT_P0], dark_rgb) && same(f.light[LIGHT_GRIP_L], dark_rgb));
    in.has_players = 1;
    in.players = 0x4;
    in.has_lightbar = 1;
    in.lightbar = (struct rgb){ 0, 0, 200 };
    frame(&connected, &s, 2000, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], dim_red));
    CHECK(same(f.light[LIGHT_GRIP_L], in.lightbar));
    CHECK(same(f.light[LIGHT_P2], dim_green) && same(f.light[LIGHT_P0], dim_red));
    in.players_flash = 0x1;     /* flashing: the number's colour half the time */
    frame(&connected, &s, 2100, &in, &f);
    CHECK(same(f.light[LIGHT_P0], dim_green));
    frame(&connected, &s, 2300, &in, &f);
    CHECK(same(f.light[LIGHT_P0], dim_red));
    in.player_lights = PLAYERS_SECONDARY;
    in.grips = GRIPS_PRIMARY;
    frame(&connected, &s, 2000, &in, &f);
    CHECK(same(f.light[LIGHT_P2], dim_green) && same(f.light[LIGHT_GRIP_R], dim_red));
    /* Reactive grips: resting in the secondary, at effect_level when busy. */
    in.grips = GRIPS_RUMBLE;
    in.rumble[1] = 255;
    frame(&connected, &s, 2000, &in, &f);
    CHECK(same(f.light[LIGHT_GRIP_L], dim_green) && same(f.light[LIGHT_GRIP_R], ((struct rgb){ 40, 0, 0 })));
    /* Animations are lit at effect_level, far above the steady lights. */
    in.grips = GRIPS_FADE;
    in.player_lights = PLAYERS_FADE;
    frame(&connected, &s, 1000 + 4000, &in, &f);
    CHECK(same(f.light[LIGHT_GRIP_L], ((struct rgb){ 40, 0, 0 })) && same(f.light[LIGHT_P3], ((struct rgb){ 40, 0, 0 })));
    frame(&connected, &s, 3000, &in, &f);
    CHECK(same(f.light[LIGHT_GRIP_L], ((struct rgb){ 0, 40, 0 })));
    in.grips = GRIPS_RAINBOW;
    in.player_lights = PLAYERS_RAINBOW;
    frame(&connected, &s, 1000 + RAINBOW_MS, &in, &f);
    CHECK(same(f.light[LIGHT_GRIP_L], ((struct rgb){ 40, 0, 0 })) && same(f.light[LIGHT_GRIP_R], ((struct rgb){ 0, 40, 40 })));
    CHECK(!same(f.light[LIGHT_P0], f.light[LIGHT_P1]) && bright_as(f.light[LIGHT_P2]) >= 40);
    frame(&connected, &s, 1000 + RAINBOW_MS / 3, &in, &f);
    CHECK(same(f.light[LIGHT_GRIP_L], ((struct rgb){ 0, 40, 0 })));
    in.grips = GRIPS_HEARTBEAT;
    frame(&connected, &s, 1000 + HEARTBEAT_MS + 60, &in, &f);         /* the first beat's top */
    frame(&connected, &s, 1000 + HEARTBEAT_MS + 250 + 60, &in, &g);   /* the second's, softer */
    CHECK(same(f.light[LIGHT_GRIP_L], ((struct rgb){ 40, 0, 0 })));
    CHECK(g.light[LIGHT_GRIP_L].r > 2 && g.light[LIGHT_GRIP_L].r < 40);
    frame(&connected, &s, 1000 + HEARTBEAT_MS + 1000, &in, &f);       /* resting */
    CHECK(same(f.light[LIGHT_GRIP_L], dim_green));
    in.player_lights = PLAYERS_OFF;
    in.grips = GRIPS_OFF;
    frame(&connected, &s, 2000, &in, &f);
    CHECK(same(f.light[LIGHT_P2], dark_rgb) && same(f.light[LIGHT_GRIP_R], dark_rgb));

    /* Ephemeral: Home winks. */
    in.ephemeral = 1;
    frame(&connected, &s, 8050, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], dark_rgb));
    frame(&connected, &s, 8150, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], dim_red));
    in.ephemeral = 0;

    s.scene = CTL_IDLE;
    s.slot = SLOT_NONE;
    s.since_ms = 0;
    in.usb_powered = 1;
    frame(&idle, &s, 1000, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], dark_rgb));
    in.usb_plug_lit = 1;
    frame(&idle, &s, 100, &in, &f);      /* at once, not faded in */
    CHECK(same(f.light[LIGHT_HOME], ((struct rgb){ 5, 2, 0 })));
    in.usb_plug_lit = 0;
    frame(&idle, &s, 2400, &in, &f); CHECK(same(f.light[LIGHT_HOME], dark_rgb));
    s.scene = CTL_IDLE;
    s.slot = SLOT_NONE;
    in.cal_lit = 1;
    in.cal = white;
    frame(&idle, &s, 100, &in, &f);
    CHECK(same(f.light[LIGHT_HOME], white));

    /* The whole pad: to the loader, a light runs down and fills it, a flash,
     * then the dim green the LEDs hold; quick settings, colours down the pad. */
    {
        static const struct led_scene to_loader = { .effect = EFFECT_LOADER }, quick = { .effect = EFFECT_QUICK };
        u32 i, held = 400 * 35 / 100 / 10;

        s.scene = CTL_REBOOTING;
        s.since_ms = 0;
        in.cal_lit = in.usb_powered = 0;
        frame(&to_loader, &s, 300, &in, &f);
        CHECK(f.light[LIGHT_HOME].g == held && f.light[LIGHT_P3].g > held && !f.light[LIGHT_GRIP_L].r);
        CHECK(f.light[LIGHT_GRIP_L].g < held);                     /* not reached yet */
        for (t = 260, last = 0; t < LOADER_FILL_MS; t += 20) {     /* once passed, never below the hold */
            frame(&to_loader, &s, t, &in, &f);
            CHECK(f.light[LIGHT_P0].g >= held);
        }
        frame(&to_loader, &s, LOADER_FILL_MS, &in, &f);
        for (i = 0; i < LIGHT_UNUSED; i++) CHECK(same(f.light[i], ((struct rgb){ 0, 40, 0 })));
        frame(&to_loader, &s, LED_LOADER_MS, &in, &f);
        for (i = 0; i < LIGHT_UNUSED; i++) CHECK(same(f.light[i], ((struct rgb){ 0, (u8)held, 0 })));
        CHECK(LOADER_FILL_MS + LOADER_FLASH_MS <= LED_LOADER_MS - 2 * 20);   /* held for the last frames */
        frame(&quick, &s, 1000, &in, &f);
        CHECK(!same(f.light[LIGHT_HOME], f.light[LIGHT_P1]) && !same(f.light[LIGHT_P1], f.light[LIGHT_P3]));
        CHECK(same(f.light[LIGHT_GRIP_L], f.light[LIGHT_GRIP_R]) && bright_as(f.light[LIGHT_HOME]) > 0);
        frame(&quick, &s, 1500, &in, &g);
        CHECK(!same(f.light[LIGHT_HOME], g.light[LIGHT_HOME]));   /* flowing */
    }

    memset(&p, 0, sizeof(p));
    p.buttons = PAD_SOUTH;
    led_reach(reach, &p);
    CHECK(reach[0] == 0 && reach[1] == 255);
    p.buttons = 0;
    led_reach(reach, &p);
    CHECK(reach[1] == 255 - REACH_DECAY);
    return TEST_DONE("leds");
}
#endif

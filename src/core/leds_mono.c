#include <string.h>
#include "pad.h"

/* One-colour lights (LEDS_MONO, pad.h): the 8BitDo pads, the same on both.
 * Levels of 255; a board shows each light at its strongest component, on or
 * off where it cannot dim. */

#define HOME       24            /* the Home ring: 24 looks right lit steadily */
#define RED        32            /* LIGHT_PAIRING */
#define CONNECT_MS 250           /* a blink: on, then off as long */
#define PAIR_MS    120
#define QUICK_MS   300           /* quick settings: Home, then LIGHT_CABLE */
#define FLASH_MS   250           /* a flashing player light */
#define CAL_MS     20            /* the calibration light's frames */
#define PLAYERS    4

static void light(struct led_frame *f, u32 n, u8 level)
{
    f->light[n].r = f->light[n].g = f->light[n].b = level;
}

u32 led_mono(const struct led_mono *in, u32 now, struct led_frame *out)
{
    u32 on = in->show == MONO_QUICK ? QUICK_MS : in->show == MONO_PAIRING ? PAIR_MS : CONNECT_MS;
    u32 at = now % (2 * on), edge = now - at + (at < on ? on : 2 * on), next = 0, i;
    u32 flash = now % (2 * FLASH_MS);
    u8 players = in->players;

    memset(out, 0, sizeof(*out));
    if (in->cal_lit) {           /* the calibration routine's own light */
        out->light[LIGHT_HOME] = in->cal;
        return now + CAL_MS;
    }
    switch (in->show) {
    case MONO_QUICK:
        light(out, at < on ? LIGHT_HOME : LIGHT_CABLE, at < on ? HOME : 255);
        if (in->signal) light(out, LIGHT_HOME, in->signal == LED_SIGNAL_LIT ? HOME : 0);
        return edge;
    case MONO_PAIRING:
        if (at < on) light(out, LIGHT_PAIRING, RED);
        /* fall through */
    case MONO_CONNECTING:
        if (at < on) light(out, LIGHT_HOME, HOME);
        next = edge;
        break;
    case MONO_CONNECTED:
        light(out, LIGHT_HOME, HOME);
        if (in->players_flash) {
            if (flash < FLASH_MS) players |= in->players_flash;
            next = now - flash + (flash < FLASH_MS ? FLASH_MS : 2 * FLASH_MS);
        }
        for (i = 0; i < PLAYERS; i++)
            if (players & (1u << i)) light(out, LIGHT_P0 + i, 255);
        break;
    }
    if (in->signal) light(out, LIGHT_HOME, in->signal == LED_SIGNAL_LIT ? HOME : 0);   /* quick.c's */
    if (in->cable) light(out, LIGHT_CABLE, 255);
    return next;
}

void led_mono_demo(const struct ux_led_demo *d, u32 t, struct led_mono *m)
{
    memset(m, 0, sizeof(*m));
    m->show = d->kind == LED_DEMO_QUICK ? MONO_QUICK : d->scene == CTL_CONNECTING ? MONO_CONNECTING :
              d->scene == CTL_PAIRING ? MONO_PAIRING : d->scene == CTL_CONNECTED ? MONO_CONNECTED : MONO_DARK;
    if (m->show == MONO_CONNECTED) {     /* the console's player 1 */
        m->players = 1;
        m->players_flash = d->players_flash;
    }
    m->cable = (d->fake & LED_FAKE_PLUG) && t % LED_DEMO_REPEAT_MS < ux_timeouts.usb_plug_ms;
    if (d->fake & LED_FAKE_CAL) {
        m->cal_lit = 1;
        cal_led_demo(t, &m->cal);
    }
}

#ifdef UNITTEST
#define LIT(l) (f.light[l].r)
const struct ux_timeouts ux_timeouts = { .usb_plug_ms = 1000 };
void cal_led_demo(u32 t, struct rgb *out) { *out = (struct rgb){ 0, 0, (u8)(t ? 1 : 2) }; }

int main(void)
{
    struct led_mono m;
    struct led_frame f;
    u32 i;

    memset(&m, 0, sizeof(m));
    CHECK(!led_mono(&m, 1000, &f));
    for (i = 0; i < LIGHT_COUNT; i++) CHECK(!LIT(i));

    /* Pairing: red with a faster Home blink; connecting: Home alone. */
    m.show = MONO_PAIRING;
    CHECK(led_mono(&m, 960, &f) == 1080 && LIT(LIGHT_HOME) == HOME && LIT(LIGHT_PAIRING) == RED);
    CHECK(led_mono(&m, 1080, &f) == 1200 && !LIT(LIGHT_HOME) && !LIT(LIGHT_PAIRING) && !LIT(LIGHT_CABLE));
    m.show = MONO_CONNECTING;
    CHECK(led_mono(&m, 1000, &f) == 1250 && LIT(LIGHT_HOME) && !LIT(LIGHT_PAIRING));
    CHECK(led_mono(&m, 1250, &f) == 1500 && !LIT(LIGHT_HOME));
    for (i = LIGHT_P0; i <= LIGHT_P3; i++) CHECK(!LIT(i));

    /* Connected: Home steady; the host's player number, a flashing one blinks. */
    m.show = MONO_CONNECTED;
    m.players = 1;
    CHECK(!led_mono(&m, 1000, &f) && LIT(LIGHT_HOME) == HOME && LIT(LIGHT_P0) == 255 &&
          !LIT(LIGHT_P1) && !LIT(LIGHT_CABLE) && !LIT(LIGHT_PAIRING));
    m.players_flash = 4;
    CHECK(led_mono(&m, 1000, &f) == 1250 && LIT(LIGHT_P0) && LIT(LIGHT_P2));
    CHECK(led_mono(&m, 1250, &f) == 1500 && LIT(LIGHT_P0) && !LIT(LIGHT_P2));
    m.players = m.players_flash = 0;

    /* A cable: LIGHT_CABLE, whatever Home shows. */
    m.cable = 1;
    CHECK(!led_mono(&m, 1000, &f) && LIT(LIGHT_CABLE) == 255 && LIT(LIGHT_HOME));
    m.show = MONO_DARK;
    CHECK(!led_mono(&m, 1000, &f) && LIT(LIGHT_CABLE) && !LIT(LIGHT_HOME));

    /* Quick settings: Home and LIGHT_CABLE in turn. */
    m.show = MONO_QUICK;
    CHECK(led_mono(&m, 1200, &f) == 1500 && LIT(LIGHT_HOME) && !LIT(LIGHT_CABLE));
    CHECK(led_mono(&m, 1500, &f) == 1800 && !LIT(LIGHT_HOME) && LIT(LIGHT_CABLE));

    /* Quick settings' feedback on Home, over the scene. */
    m.show = MONO_CONNECTED; m.signal = LED_SIGNAL_DARK;
    led_mono(&m, 1000, &f); CHECK(!LIT(LIGHT_HOME));
    m.signal = LED_SIGNAL_LIT; m.show = MONO_DARK;
    led_mono(&m, 1000, &f); CHECK(LIT(LIGHT_HOME) == HOME);
    m.show = MONO_QUICK; led_mono(&m, 1500, &f); CHECK(LIT(LIGHT_HOME) == HOME);
    m.signal = LED_SIGNAL_NONE;

    /* Calibrating: its light on Home alone, every frame. */
    m.cal_lit = 1;
    m.cal = (struct rgb){ 0, 40, 0 };
    CHECK(led_mono(&m, 1000, &f) == 1000 + CAL_MS && f.light[LIGHT_HOME].g == 40 && !LIT(LIGHT_CABLE));

    /* Demos: the scene, player 1, and what they stand in for. */
    {
        const struct ux_led_demo pairing = { .scene = CTL_PAIRING };
        const struct ux_led_demo flash = { .scene = CTL_CONNECTED, .players_flash = 2 };
        const struct ux_led_demo quick = { .scene = CTL_CONNECTED, .kind = LED_DEMO_QUICK };
        const struct ux_led_demo plug = { .scene = CTL_IDLE, .fake = LED_FAKE_PLUG };
        const struct ux_led_demo cal = { .scene = CTL_IDLE, .fake = LED_FAKE_CAL };

        led_mono_demo(&pairing, 0, &m); CHECK(m.show == MONO_PAIRING && !m.players);
        led_mono_demo(&flash, 0, &m); CHECK(m.show == MONO_CONNECTED && m.players == 1 && m.players_flash == 2);
        led_mono_demo(&quick, 0, &m); CHECK(m.show == MONO_QUICK);
        led_mono_demo(&plug, 500, &m); CHECK(m.show == MONO_DARK && m.cable);
        led_mono_demo(&plug, 1500, &m); CHECK(!m.cable);
        led_mono_demo(&plug, 3200, &m); CHECK(m.cable);
        led_mono_demo(&cal, 5, &m); CHECK(m.cal_lit && m.cal.b == 1);
    }
    return TEST_DONE("leds_mono");
}
#endif

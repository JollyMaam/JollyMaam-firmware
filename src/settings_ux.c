#include "pad.h"

/* How the pad feels: every fixed time, what each state looks like, and what
 * Home does.  Edit here and rebuild. */

const struct ux_timeouts ux_timeouts = {
    .home_grace_ms = 150,        /* connected: Home alone longer goes to the host */
    .home_tap_ms = 60,           /* how long a short Home press is shown to the host */
    .connect_ms = 30000,
    .pair_ms = 180000,
    .idle_sleep_ms = 300000,     /* connected, no input, on battery */
    .offline_sleep_ms = 15000,   /* radio off, on battery */
    .unplug_sleep_ms = 5000,     /* after USB goes away */
    .stop_max_ms = 12000,
    .boot_scene_ms = 500,
    .uboot_window_ms = 3000,     /* Home+Start at boot: USB may arrive this late */
    .led_frame_ms = 20,
    .usb_plug_flash_ms = 150,
};

const struct led_scene ux_scenes[CTL_KIND_COUNT] = {
    [CTL_BOOT] =       { COLOUR_IDLE, LED_SOLID,         BAR_ALL,    LED_OFF,       GRIPS_WAVE, LED_OFF },
    [CTL_IDLE] =       { COLOUR_IDLE, LED_OFF,           BAR_ALL,    LED_OFF,       GRIPS_WAVE, LED_OFF },
    [CTL_CONNECTING] = { COLOUR_SLOT, LED_BLINK(500, 500), BAR_SLOT, LED_SOLID,     GRIPS_WAVE, LED_OFF },
    [CTL_PAIRING] =    { COLOUR_SLOT, LED_BLINK(120, 120), BAR_ALL,  LED_SWEEP(120), GRIPS_WAVE, LED_OFF },
    [CTL_CONNECTED] =  { COLOUR_SLOT, LED_SOLID,         BAR_PLAYER, LED_SOLID,     GRIPS_SLOT_SETTING, LED_OFF },
    [CTL_STOPPING] =   { COLOUR_SLOT, LED_OFF,           BAR_SLOT,   LED_SOLID,     GRIPS_WAVE, LED_OFF },
    [CTL_SLEEPING] =   { COLOUR_IDLE, LED_OFF,           BAR_ALL,    LED_OFF,       GRIPS_WAVE, LED_OFF },
    [CTL_REBOOTING] =  { COLOUR_IDLE, LED_OFF,           BAR_ALL,    LED_OFF,       GRIPS_WAVE, LED_OFF },
};

#define SLOT_KEY 0x10000u        /* in .with: one of A B X Y up down left right */
#define TAP      0xffffffffu     /* in .hold_ms: on a release, nothing else fired */

enum { ALWAYS, CONNECTED, NOT_CONNECTED, OFFLINE };

static const struct gesture {
    const char *name;
    u32 with;                    /* held with Home; 0 = Home alone */
    u8 when;                     /* judged when Home went down */
    u32 hold_ms;                 /* 0 = at once */
    u8 request;
    u8 slot;                     /* for rows without a slot key */
} gestures[] = {
    { "home",         0,          CONNECTED,     TAP,  REQ_HOME_TAP,   SLOT_NONE },
    { "connect last", 0,          OFFLINE,       TAP,  REQ_CONNECT,    SLOT_LAST },
    { "sleep",        0,          ALWAYS,        5000, REQ_SLEEP,      SLOT_NONE },
    { "connect",      SLOT_KEY,   NOT_CONNECTED, 0,    REQ_CONNECT,    SLOT_NONE },
    { "switch",       SLOT_KEY,   CONNECTED,     2000, REQ_CONNECT,    SLOT_NONE },
    { "pair",         SLOT_KEY,   ALWAYS,        5000, REQ_PAIR,       SLOT_NONE },
    { "disconnect",   PAD_SELECT, ALWAYS,        2000, REQ_DISCONNECT, SLOT_NONE },
    { "calibrate",    PAD_M,      ALWAYS,        3000, REQ_CALIBRATE,  SLOT_NONE },
};
#define GESTURES (sizeof(gestures) / sizeof(gestures[0]))

/* ---- the engine ------------------------------------------------------------
 * A gesture starts when Home goes down and ends when it comes up.  The first
 * thing held with Home is its chord; a chord that changes ends the gesture
 * and nothing more fires until all is let go.  Each row fires at most once. */

static struct {
    u8 active, live, draining, chorded, fired, slot, tap, state;
    u32 home_ms, chord_ms, tap_ms, chord, done;
} ux;

static u16 chord_buttons(void)
{
    u32 b = 0, i;

    for (i = 0; i < GESTURES; i++) b |= gestures[i].with;
    return (u16)(b & ~(u32)(PAD_HOME | PAD_FACE));
}

/* Slot keys, a bit per slot: A B X Y, then up down left right. */
static u32 held(const struct pad *p, u8 *slot)
{
    static const u8 hat_keys[HAT_NEUTRAL] = {
        0x10, 0x10 | 0x80, 0x80, 0x20 | 0x80, 0x20, 0x20 | 0x40, 0x40, 0x10 | 0x40,
    };
    u8 keys = 0, i;

    if (p->buttons & PAD_A) keys |= 1;
    if (p->buttons & PAD_B) keys |= 2;
    if (p->buttons & PAD_X) keys |= 4;
    if (p->buttons & PAD_Y) keys |= 8;
    if (p->hat < HAT_NEUTRAL) keys |= hat_keys[p->hat];
    *slot = SLOT_NONE;
    for (i = 0; i < SLOTS; i++)
        if (keys == 1u << i) *slot = i;
    return (p->buttons & chord_buttons()) | (keys ? SLOT_KEY : 0);
}

static int applies(u8 when, u8 state)
{
    switch (when) {
    case CONNECTED: return state == CTL_CONNECTED;
    case NOT_CONNECTED: return state != CTL_CONNECTED;
    case OFFLINE: return state == CTL_IDLE || state == CTL_BOOT;
    default: return 1;
    }
}

static int matches(const struct gesture *g, u32 chord, u8 slot, u8 state)
{
    return g->with == chord && applies(g->when, state) &&
           (!(chord & SLOT_KEY) || slot != SLOT_NONE);
}

static struct ctl_request fire(u32 i, u8 slot, u32 now)
{
    struct ctl_request r = { gestures[i].request, slot != SLOT_NONE ? slot : gestures[i].slot, 0 };

    ux.done |= 1u << i;
    ux.fired = 1;
    util_log("ux: %s", gestures[i].name);
    if (r.kind == REQ_HOME_TAP) {
        ux.tap = 1;
        ux.tap_ms = now;
        r.kind = REQ_NONE;
    }
    return r;
}

static struct ctl_request due(u32 now, int release, u32 chord, u8 slot)
{
    struct ctl_request none = { REQ_NONE, SLOT_NONE, 0 };
    u32 i;

    for (i = 0; i < GESTURES; i++) {
        const struct gesture *g = &gestures[i];

        if ((ux.done & (1u << i)) || !matches(g, chord, slot, ux.state)) continue;
        if (release ? g->hold_ms == TAP : g->hold_ms != TAP && now - ux.chord_ms >= g->hold_ms)
            return fire(i, slot, now);
    }
    return none;
}

static struct ctl_request step(const struct pad *p, u32 now, u8 state)
{
    struct ctl_request none = { REQ_NONE, SLOT_NONE, 0 };
    int home = (p->buttons & PAD_HOME) != 0;
    u8 slot;
    u32 chord = held(p, &slot);

    if (ux.draining) {
        if (!home && !chord) ux.draining = 0;
        return none;
    }
    if (!ux.active) {
        if (!home) return none;
        ux.active = 1;
        ux.live = ux.fired = 0;
        ux.chorded = chord != 0;
        ux.state = state;
        ux.home_ms = ux.chord_ms = now;
        ux.chord = chord;
        ux.slot = slot;
        ux.done = 0;
    }
    if (!home) {
        ux.active = 0;
        if (ux.live) return none;
        ux.draining = chord != 0;
        return !ux.chorded && !ux.fired ? due(now, 1, 0, SLOT_NONE) : none;
    }
    if (ux.live) return due(now, 0, 0, SLOT_NONE);
    if (chord != ux.chord || slot != ux.slot) {
        if (ux.chorded || ux.fired) {
            ux.active = 0;
            ux.draining = 1;
            return none;
        }
        ux.chorded = 1;
        ux.chord = chord;
        ux.slot = slot;
        ux.chord_ms = now;
    } else if (!ux.chorded && !ux.fired && ux.state == CTL_CONNECTED &&
               now - ux.home_ms >= ux_timeouts.home_grace_ms) {
        ux.live = 1;
    }
    return due(now, 0, ux.chord, ux.slot);
}

/* Follows Home through `pad`, takes out what the host must not see (Home and
 * the chord during a gesture), puts back a short Home tap, and returns at
 * most one request. */
struct ctl_request ux_step(struct pad *p, u32 now, u8 state)
{
    struct ctl_request r = step(p, now, state);

    if ((ux.active && !ux.live) || ux.draining) {
        p->buttons &= (u16)~(PAD_HOME | PAD_FACE | chord_buttons());
        p->hat = HAT_NEUTRAL;
    }
    if (ux.tap) {
        if (now - ux.tap_ms < ux_timeouts.home_tap_ms) p->buttons |= PAD_HOME;
        else ux.tap = 0;
    }
    return r;
}

/* The next time ux_step has something to do without a button change. */
u32 ux_deadline(void)
{
    u32 next = 0, i, t;

    if (ux.tap) next = ux.tap_ms + ux_timeouts.home_tap_ms;
    if (!ux.active) return next;
    if (!ux.live && !ux.chorded && !ux.fired && ux.state == CTL_CONNECTED) {
        t = ux.home_ms + ux_timeouts.home_grace_ms;
        if (!next || (s32)(t - next) < 0) next = t;
    }
    for (i = 0; i < GESTURES; i++) {
        const struct gesture *g = &gestures[i];

        if ((ux.done & (1u << i)) || g->hold_ms == TAP ||
            !matches(g, ux.live ? 0 : ux.chord, ux.live ? SLOT_NONE : ux.slot, ux.state))
            continue;
        t = ux.chord_ms + g->hold_ms;
        if (!next || (s32)(t - next) < 0) next = t;
    }
    return next ? next : 0;
}

void ux_ignore_held(void)
{
    ux.active = ux.live = 0;
    ux.draining = 1;
}

/* A press that woke the pad and was let go before the loop ran. */
struct ctl_request ux_boot(u16 buttons, u8 hat, u8 state)
{
    struct ctl_request none = { REQ_NONE, SLOT_NONE, 0 };
    struct pad p = { buttons, hat, 0, 0, 0, 0, 0, 0 };
    u8 slot;
    u32 chord = held(&p, &slot), i;

    if (!(buttons & PAD_HOME)) return none;
    ux.state = state;
    ux.done = 0;
    for (i = 0; i < GESTURES; i++) {
        const struct gesture *g = &gestures[i];

        if (!matches(g, chord, slot, state)) continue;
        if (chord ? g->hold_ms == 0 : g->hold_ms == TAP) return fire(i, slot, 0);
    }
    return none;
}

#ifdef UNITTEST
void util_log(const char *fmt, ...) { (void)fmt; }

static u32 now;
static u8 state;
static struct pad pad, seen;
static struct ctl_request got[16];
static int ngot;

static void run(u32 ms)
{
    while (ms--) {
        struct ctl_request r;

        now++;
        seen = pad;
        r = ux_step(&seen, now, state);
        if (r.kind != REQ_NONE && ngot < 16) got[ngot++] = r;
    }
}

static void reset(u8 s)
{
    state = s;
    memset(&pad, 0, sizeof(pad));
    pad.hat = HAT_NEUTRAL;
    run(10);
    ngot = 0;
}

static void press(u16 b) { pad.buttons |= b; run(1); }
static void release(u16 b) { pad.buttons &= (u16)~b; run(1); }

static int got1(u8 kind, u8 slot)
{
    return ngot == 1 && got[0].kind == kind && got[0].slot == slot;
}

int main(void)
{
    reset(CTL_IDLE);
    press(PAD_HOME); run(80); release(PAD_HOME);
    CHECK(got1(REQ_CONNECT, SLOT_LAST));

    reset(CTL_IDLE);
    press(PAD_HOME); press(PAD_B);
    CHECK(got1(REQ_CONNECT, 1));
    run(4990);
    CHECK(ngot == 1);
    run(20);
    CHECK(ngot == 2 && got[1].kind == REQ_PAIR && got[1].slot == 1);
    CHECK(!(seen.buttons & (PAD_HOME | PAD_B)));
    release(PAD_HOME); release(PAD_B); run(50);
    CHECK(ngot == 2);

    reset(CTL_IDLE);
    pad.hat = HAT_LEFT; press(PAD_HOME);
    CHECK(got1(REQ_CONNECT, 6));

    reset(CTL_CONNECTED);
    press(PAD_HOME); press(PAD_B); run(1900); release(PAD_B); release(PAD_HOME);
    CHECK(ngot == 0);
    reset(CTL_CONNECTED);
    press(PAD_B); press(PAD_HOME); run(1990);
    CHECK(ngot == 0);
    CHECK(ux_deadline() == now + 10);
    run(20);
    CHECK(got1(REQ_CONNECT, 1));

    reset(CTL_CONNECTED);
    press(PAD_HOME); run(50);
    CHECK(!(seen.buttons & PAD_HOME));
    release(PAD_HOME);
    CHECK(seen.buttons & PAD_HOME);
    CHECK(ux_deadline() == now + 60);
    run(70);
    CHECK(!(seen.buttons & PAD_HOME) && ngot == 0);

    reset(CTL_CONNECTED);
    press(PAD_HOME); run(200);
    CHECK(seen.buttons & PAD_HOME);
    press(PAD_B); run(2500);
    CHECK(seen.buttons & PAD_B && ngot == 0);
    release(PAD_B); release(PAD_HOME);

    reset(CTL_CONNECTED);
    press(PAD_SELECT); press(PAD_HOME); run(1900);
    CHECK(ngot == 0 && !(seen.buttons & PAD_SELECT));
    run(200);
    CHECK(got1(REQ_DISCONNECT, SLOT_NONE));
    release(PAD_HOME);
    CHECK(!(seen.buttons & PAD_SELECT));
    release(PAD_SELECT);

    reset(CTL_IDLE);
    press(PAD_HOME); run(5010); release(PAD_HOME);
    CHECK(got1(REQ_SLEEP, SLOT_NONE));

    reset(CTL_CONNECTED);
    press(PAD_HOME); press(PAD_B); run(500); press(PAD_A); run(6000);
    release(PAD_A); release(PAD_B); release(PAD_HOME);
    CHECK(ngot == 0);

    reset(CTL_IDLE);
    pad.buttons = PAD_HOME | PAD_A;
    ux_ignore_held();
    run(6000); release(PAD_A); release(PAD_HOME);
    CHECK(ngot == 0);

    reset(CTL_BOOT);
    got[0] = ux_boot(PAD_HOME, HAT_NEUTRAL, CTL_BOOT);
    CHECK(got[0].kind == REQ_CONNECT && got[0].slot == SLOT_LAST);
    got[0] = ux_boot(PAD_HOME | PAD_X, HAT_NEUTRAL, CTL_BOOT);
    CHECK(got[0].kind == REQ_CONNECT && got[0].slot == 2);
    got[0] = ux_boot(PAD_X, HAT_NEUTRAL, CTL_BOOT);
    CHECK(got[0].kind == REQ_NONE);
    return TEST_DONE("settings_ux");
}
#endif

#include "pad.h"

/* Fixed M shortcuts, timeouts and LED scenes. */

const struct ux_timeouts ux_timeouts = {
    .m_grace_ms = 150,          /* wait for an M chord before sending M alone */
    .tap_ms = 60,               /* minimum synthesized down after its first report handoff */
    .connect_ms = 30000,
    .pair_ms = 180000,
    .idle_sleep_ms = 300000,     /* connected, no input, on battery */
    .offline_sleep_ms = 15000,   /* radio off, on battery */
    .unplug_sleep_ms = 5000,     /* after USB goes away */
    .stop_max_ms = 12000,
    .boot_scene_ms = 500,
    .uboot_window_ms = 3000,     /* Home+Start at boot: USB may arrive this late */
    .led_frame_ms = 20,
    .usb_plug_ms = 1000,
};

/* Palette entries 0 and 1 are these roles (settings.c); the brightness is here. */
const struct ux_colours ux_colours = {
    .idle = { 0, 5 },
    .battery = { 1, 5 },
};

const struct led_scene ux_scenes[CTL_KIND_COUNT] = {
    [CTL_BOOT] =       { COLOUR_IDLE,    LED_SOLID,           BAR_ALL,    LED_OFF,             GRIPS_WAVE, LED_OFF },
    [CTL_IDLE] =       { COLOUR_IDLE,    LED_OFF,             BAR_ALL,    LED_OFF,             GRIPS_WAVE, LED_OFF },
    [CTL_CONNECTING] = { COLOUR_CONNECT, LED_BLINK(500, 500), BAR_ALL,    LED_BLINK(500, 500), GRIPS_WAVE, LED_OFF },
    [CTL_PAIRING] =    { COLOUR_CONNECT, LED_BLINK(120, 120), BAR_ALL,    LED_SWEEP(120),      GRIPS_WAVE, LED_OFF },
    [CTL_CONNECTED] =  { COLOUR_PRIMARY, LED_SOLID,           BAR_PLAYER, LED_SOLID,           GRIPS_SLOT_SETTING, LED_OFF },
    [CTL_STOPPING] =   { COLOUR_CONNECT, LED_OFF,             BAR_SLOT,   LED_SOLID,           GRIPS_WAVE, LED_OFF },
    [CTL_SLEEPING] =   { COLOUR_IDLE,    LED_OFF,             BAR_ALL,    LED_OFF,             GRIPS_WAVE, LED_OFF },
    [CTL_REBOOTING] =  { COLOUR_IDLE,    LED_OFF,             BAR_ALL,    LED_OFF,             GRIPS_WAVE, LED_OFF },
};

/* Temporary development indication. Set enabled to 0 once power transitions
 * are proven on the physical pad. Independent of slot and normal scenes. */
const struct ux_power_debug ux_power_debug = {
    .enabled = 1, .wake_ms = 1000, .sleep_ms = 1000,
    .wake_colour = {0, 12, 0}, .sleep_colour = {12, 0, 0},
};

#define DIGITAL_MASK 0x000f7fffu
static const u8 keys_to_host = 0;
static const struct gesture { u32 keys; u16 hold_ms; u8 request, slot; } gestures[] = {
    {PAD_M | PAD_A, 1500, REQ_CONNECT, 0},
    {PAD_M | PAD_B, 1500, REQ_CONNECT, 1},
    {PAD_M | PAD_X, 1500, REQ_CONNECT, 2},
    {PAD_M | PAD_Y, 1500, REQ_CONNECT, 3},
    {PAD_M | (1u << 16), 1500, REQ_CONNECT, 4},
    {PAD_M | (1u << 18), 1500, REQ_CONNECT, 5},
    {PAD_M | (1u << 19), 1500, REQ_CONNECT, 6},
    {PAD_M | (1u << 17), 1500, REQ_CONNECT, 7},
    {PAD_M | PAD_SELECT, 1500, REQ_DISCONNECT, SLOT_NONE},
    {PAD_M | PAD_L3, 3000, REQ_CALIBRATE, SLOT_NONE},
    {PAD_M | PAD_R3, 3000, REQ_CALIBRATE, SLOT_NONE},
    {PAD_M | PAD_A, 5000, REQ_PAIR, 0},
    {PAD_M | PAD_B, 5000, REQ_PAIR, 1},
    {PAD_M | PAD_X, 5000, REQ_PAIR, 2},
    {PAD_M | PAD_Y, 5000, REQ_PAIR, 3},
    {PAD_M | (1u << 16), 5000, REQ_PAIR, 4},
    {PAD_M | (1u << 18), 5000, REQ_PAIR, 5},
    {PAD_M | (1u << 19), 5000, REQ_PAIR, 6},
    {PAD_M | (1u << 17), 5000, REQ_PAIR, 7},
};
#define GESTURES (sizeof(gestures) / sizeof(gestures[0]))

/* Only converts the physical d-pad to/from a chord mask; no output remapping. */
static u32 held(const struct pad *p)
{
    static const u8 hats[] = {1,3,2,6,4,12,8,9,0};
    return p->buttons | (u32)hats[p->hat < HAT_NEUTRAL ? p->hat : HAT_NEUTRAL] << 16;
}
static void show(struct pad *p, u32 keys)
{
    static const u8 hats[] = {8,0,2,1,4,8,3,2,6,7,8,0,5,6,4,8};
    p->buttons = (u16)keys;
    p->hat = hats[(keys >> 16) & 15];
}

/* A captured physical chord is never reinterpreted when keys change: drain
 * it until release. */
static struct {
    u32 started, chord_at, keys, captured, tap, tap_release_at, done;
    u32 home_at;
    u8 active, live, chorded, fired, draining;
    u8 tap_reported, home_down, home_fired, home_ignore;
} ux;

static struct ctl_request request(u8 kind, u8 slot)
{
    struct ctl_request r = {kind, slot, 0};
    return r;
}

static u32 earlier(u32 a, u32 b)
{
    return !a || (b && (s32)(b - a) < 0) ? b : a;
}

struct ctl_request ux_step(struct pad *p, u32 now, u8 state)
{
    struct ctl_request r = request(REQ_NONE, SLOT_NONE);
    u32 keys = held(p), modifier = PAD_M, i, output = keys;
    int home = (keys & PAD_HOME) != 0;
    int disconnected = state == CTL_BOOT || state == CTL_IDLE || state == CTL_CONNECTING ||
                       state == CTL_PAIRING || state == CTL_STOPPING;

    /* Home remains a normal host key. Offline, a tap connects the last slot;
     * a five-second hold powers off. Boot wake is handled by ux_boot(). */
    if (ux.home_ignore) {
        output &= ~PAD_HOME;
        keys &= ~PAD_HOME;
        if (!home) ux.home_ignore = 0;
    } else {
        if (home && !ux.home_down) { ux.home_at = now; ux.home_fired = 0; }
        if (home && keys != PAD_HOME) ux.home_fired = 1;
        if (home && !ux.home_fired && now - ux.home_at >= 5000u) {
            ux.home_fired = 1;
            r = request(REQ_SLEEP, SLOT_NONE);
        }
        if (!home && ux.home_down && !ux.home_fired && (state == CTL_IDLE || state == CTL_BOOT))
            r = request(REQ_CONNECT, SLOT_LAST);
    }
    ux.home_down = home;

    if (state != CTL_CONNECTED) ux.tap = 0; /* a lost link must not replay an old tap */
    if (ux.tap && ux.tap_reported && (s32)(now - ux.tap_release_at) >= 0) ux.tap = 0;
    if (ux.draining) {
        if (!keys_to_host) output &= ~ux.captured;
        if (!(keys & ux.captured)) ux.draining = 0;
    } else {
        if (!ux.active && (keys & modifier) == modifier) {
            ux.active = 1;
            ux.live = ux.chorded = ux.fired = 0;
            ux.done = 0;
            ux.started = ux.chord_at = now;
            ux.keys = modifier;
            ux.captured = modifier;
        }
        if (ux.active) {
            if ((keys & modifier) != modifier) {
                if (!ux.live && !ux.chorded && !ux.fired && !keys_to_host && state == CTL_CONNECTED) {
                    ux.tap |= modifier;
                    ux.tap_reported = 0;
                }
                ux.active = 0;
                ux.draining = (keys & ux.captured) != 0;
            } else if (!ux.live || keys_to_host || disconnected) {
                if (ux.chorded && keys != ux.keys) {
                    ux.active = 0;
                    ux.draining = 1;
                    ux.captured |= keys;
                } else {
                    if (!ux.chorded && (keys_to_host || disconnected || now == ux.started ||
                                        now - ux.started < ux_timeouts.m_grace_ms)) {
                        for (i = 0; i < GESTURES; i++) {
                            const struct gesture *g = &gestures[i];
                            if (!g->request || g->keys != keys) continue;
                            ux.chorded = 1;
                            ux.live = 0;
                            ux.keys = keys;
                            ux.chord_at = now;
                            ux.captured |= keys;
                            break;
                        }
                    }
                    if (!ux.chorded && !keys_to_host && now - ux.started >= ux_timeouts.m_grace_ms)
                        ux.live = 1;
                    if (ux.chorded) for (i = 0; i < GESTURES; i++) {
                        const struct gesture *g = &gestures[i];
                        u32 hold_ms = g->hold_ms;
                        if (g->request == REQ_CONNECT && disconnected)
                            hold_ms = 0;
                        if ((ux.done & (1u << i)) || !g->request || g->keys != keys || now - ux.chord_at < hold_ms) continue;
                        ux.done |= 1u << i;
                        ux.fired = 1;
                        r = request(g->request, g->slot);
                        break;
                    }
                }
            }
            if (!keys_to_host && !ux.live) output &= ~ux.captured;
        }
    }
    show(p, output | ux.tap);
    /* A tap is held until its down report leaves the Bluetooth slot, then
     * for tap_ms so the host can observe it. Keep its release
     * deadline armed until acknowledgement, so a sleeping loop still runs. */
    if (ux.tap && !ux.tap_reported) ux.tap_release_at = now + ux_timeouts.tap_ms;
    return r;
}

void ux_reported(u32 now)
{
    if (ux.tap && !ux.tap_reported) {
        ux.tap_reported = 1;
        ux.tap_release_at = now + ux_timeouts.tap_ms;
    }
}

u32 ux_deadline(void)
{
    u32 next = 0, i;
    if (ux.tap) next = ux.tap_release_at;
    if (ux.home_down && !ux.home_fired && !ux.home_ignore)
        next = earlier(next, ux.home_at + 5000u);
    if (!ux.active) return next;
    if (!ux.live && !ux.chorded && !ux.fired && !keys_to_host)
        next = earlier(next, ux.started + ux_timeouts.m_grace_ms);
    if (ux.chorded && (!ux.live || keys_to_host)) for (i = 0; i < GESTURES; i++) {
        const struct gesture *g = &gestures[i];
        if (!(ux.done & (1u << i)) && g->request && g->keys == ux.keys) next = earlier(next, ux.chord_at + g->hold_ms);
    }
    return next;
}

void ux_ignore_held(void)
{
    ux.active = ux.live = ux.chorded = ux.fired = 0;
    ux.draining = 1;
    ux.captured = DIGITAL_MASK;
    ux.tap = 0;
    ux.home_ignore = 1;
}

struct ctl_request ux_boot(u16 buttons, u8 hat, u32 now)
{
    struct pad p = {0};
    struct ctl_request r;
    ux.home_ignore = (buttons & PAD_HOME) != 0;
    p.buttons = buttons;
    p.hat = hat;
    r = ux_step(&p, now, CTL_BOOT);
    if (r.kind != REQ_NONE) return r;
    return request(buttons & PAD_HOME ? REQ_CONNECT : REQ_NONE, SLOT_LAST);
}

#ifdef UNITTEST
static u32 now;
static struct pad pad, seen;
static struct ctl_request last;
static u8 state;
static void run(u32 ms)
{
    while (ms--) { now++; seen = pad; last = ux_step(&seen, now, state); }
}
static void reset(u8 next_state)
{
    memset(&ux, 0, sizeof(ux));
    memset(&pad, 0, sizeof(pad));
    pad.hat = HAT_NEUTRAL;
    state = next_state;
    now = 100;
    run(1);
}
int main(void)
{
    u32 i;
    reset(CTL_CONNECTED); pad.buttons = PAD_M; run(ux_timeouts.m_grace_ms);
    CHECK(!seen.buttons); run(1); CHECK(seen.buttons == PAD_M);
    pad.buttons |= PAD_A; run(2000); CHECK(last.kind == REQ_NONE);

    reset(CTL_CONNECTED); pad.buttons = PAD_M; run(10); pad.buttons = 0; run(1);
    CHECK(seen.buttons == PAD_M);
    run(1000); CHECK(seen.buttons == PAD_M); /* a blocked/not-ready host cannot erase down */
    ux_reported(now); CHECK(ux_deadline() == now + ux_timeouts.tap_ms);
    run(ux_timeouts.tap_ms - 1); CHECK(seen.buttons == PAD_M);
    ux_reported(now); /* more reports cannot indefinitely extend the pulse */
    run(1); CHECK(!seen.buttons);
    reset(CTL_CONNECTED); pad.buttons = PAD_M; run(10); pad.buttons = 0; run(1);
    state = CTL_IDLE; run(1); CHECK(!seen.buttons && !ux.tap);
    state = CTL_CONNECTED; run(1); CHECK(!seen.buttons);
    for (i = 0; i < 11; i++) {
        u32 keys = gestures[i].keys, hold_ms = gestures[i].hold_ms;
        reset(CTL_CONNECTED); show(&pad, keys); run(hold_ms);
        CHECK(last.kind == REQ_NONE && !seen.buttons && seen.hat == HAT_NEUTRAL);
        run(1); CHECK(last.kind == gestures[i].request && last.slot == gestures[i].slot);
        run(10); CHECK(last.kind == REQ_NONE);
        show(&pad, keys & ~PAD_M); run(1); CHECK(!seen.buttons && seen.hat == HAT_NEUTRAL);
        show(&pad, 0); run(1);
    }
    reset(CTL_CONNECTED); pad.buttons = PAD_M | PAD_B; run(1501); CHECK(last.kind == REQ_CONNECT);
    run(3500); CHECK(last.kind == REQ_PAIR && last.slot == 1);
    run(1); CHECK(last.kind == REQ_NONE);
    reset(CTL_CONNECTED); pad.buttons = PAD_M; run(100); pad.buttons |= PAD_B; run(1500);
    CHECK(last.kind == REQ_NONE); run(1); CHECK(last.kind == REQ_CONNECT);
    reset(CTL_CONNECTED); pad.buttons = PAD_M | PAD_B; run(100); pad.buttons |= PAD_A; run(6000);
    CHECK(last.kind == REQ_NONE && !seen.buttons);
    pad.buttons = 0; run(100); CHECK(!seen.buttons && !ux.tap);
    reset(CTL_CONNECTED); pad.buttons = PAD_M | PAD_L1; run(ux_timeouts.m_grace_ms);
    CHECK(seen.buttons == PAD_L1 && !ux.chorded); run(1); CHECK(seen.buttons == pad.buttons);
    reset(CTL_CONNECTED); pad.buttons = PAD_M; run(1);
    now += ux_timeouts.m_grace_ms; pad.buttons |= PAD_B; run(1);
    CHECK(seen.buttons == pad.buttons && !ux.chorded);
    reset(CTL_CONNECTED); now = 0xffffffc0u; pad.buttons = PAD_M; run(ux_timeouts.m_grace_ms + 1); CHECK(seen.buttons == PAD_M);
    reset(CTL_BOOT); ux_ignore_held(); pad.buttons = PAD_HOME | PAD_M | PAD_B; run(6000);
    CHECK(last.kind == REQ_NONE && !seen.buttons);

    /* Every slot works at initial latch, even if Home was also held. */
    for (i = 0; i < 8; i++) {
        reset(CTL_BOOT); show(&pad, gestures[i].keys | PAD_HOME);
        last = ux_boot(pad.buttons, pad.hat, now);
        CHECK(last.kind == REQ_CONNECT && last.slot == i);
        state = CTL_CONNECTING; run(1); CHECK(last.kind == REQ_NONE && !seen.buttons && seen.hat == HAT_NEUTRAL);
        run(4999); CHECK(last.kind == REQ_PAIR && last.slot == i);
    }
    reset(CTL_BOOT); last = ux_boot(PAD_HOME, HAT_NEUTRAL, now);
    CHECK(last.kind == REQ_CONNECT && last.slot == SLOT_LAST);
    pad.buttons = PAD_HOME; state = CTL_CONNECTING; run(100); CHECK(!seen.buttons);
    pad.buttons |= PAD_M | PAD_X; run(1); CHECK(last.kind == REQ_CONNECT && last.slot == 2);
    reset(CTL_BOOT); ux_boot(PAD_M, HAT_NEUTRAL, now);
    pad.buttons = PAD_M; run(10); pad.buttons |= PAD_B; run(1); CHECK(last.kind == REQ_CONNECT && last.slot == 1);
    reset(CTL_BOOT); ux_boot(PAD_M, HAT_NEUTRAL, now);
    pad.buttons = PAD_M; run(300); pad.buttons |= PAD_B; run(1);
    CHECK(last.kind == REQ_CONNECT && last.slot == 1 && !seen.buttons); /* after grace, still waking */
    /* Disconnected slot selection has no wake-time expiry. It also works
     * after M's grace, since there is no connected host receiving M. */
    for (i = CTL_BOOT; i <= CTL_STOPPING; i++) {
        if (i == CTL_CONNECTED) continue;
        reset((u8)i); run(5000);
        pad.buttons = PAD_M; run(ux_timeouts.m_grace_ms + 100);
        pad.buttons |= PAD_B; run(1);
        CHECK(last.kind == REQ_CONNECT && last.slot == 1 && !seen.buttons);
        state = CTL_CONNECTED; run(1); CHECK(last.kind == REQ_NONE);
        run(4999); CHECK(last.kind == REQ_PAIR && last.slot == 1);
    }
    /* A normal connected press still waits 1.5 seconds, even just after boot. */
    reset(CTL_BOOT); ux_boot(0, HAT_NEUTRAL, now); state = CTL_CONNECTED;
    pad.buttons = PAD_M | PAD_B; run(1500); CHECK(last.kind == REQ_NONE);
    run(1); CHECK(last.kind == REQ_CONNECT);
    reset(CTL_IDLE); pad.buttons = PAD_HOME; run(1); pad.buttons = 0; run(1);
    CHECK(last.kind == REQ_CONNECT && last.slot == SLOT_LAST);
    reset(CTL_IDLE); pad.buttons = PAD_M; run(10); pad.buttons = 0; run(1);
    CHECK(!seen.buttons && !ux.tap);
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(5001); CHECK(last.kind == REQ_SLEEP);
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME | PAD_A; run(6000); CHECK(last.kind == REQ_NONE);
    return TEST_DONE("settings_ux");
}
#endif

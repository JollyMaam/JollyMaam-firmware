#include <string.h>
#include "pad.h"

/* Home gestures, timeouts and LED scenes. */

const struct ux_timeouts ux_timeouts = {
    .home_grace_ms = 250,       /* wait for a Home chord before sending Home alone */
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
    .config_ms = 30000,          /* remap config mode, from its last key press */
    .rumble_feedback_ms = 1000,
};


/* Idle, a USB host appearing (a plug-in, or the host waking) connects the
 * last slot if it is usb or bluetooth+usb.  Off: the tool device stays, for
 * flashing. */
const u8 ux_usb_autoconnect = 0;

/* Idle may sleep only after both the offline and unplug waits. Scheduling
 * either earlier deadline after it has expired would spin the control loop
 * while the other condition still prevents sleep, starving the SDK idle task. */
u32 ux_idle_deadline(u32 idle_since, u32 activity_ms, u32 usb_seen_ms,
                     int usb_powered, int ephemeral)
{
    u32 offline, unplug;
    if (usb_powered) return 0;
    if (ephemeral) return activity_ms + ux_timeouts.idle_sleep_ms;
    offline = idle_since + ux_timeouts.offline_sleep_ms;
    unplug = usb_seen_ms + ux_timeouts.unplug_sleep_ms;
    return (s32)(offline - unplug) > 0 ? offline : unplug;
}

/* Palette roles: white idle, orange battery (settings.c); brightness here,
 * in tenths of the LEDs' smallest step.  The rest is the palette's. */
const struct ux_colours ux_colours = {
    .idle = 0, .idle_brightness = 50 * LED_DEFAULT_BRIGHTNESS_SCALE,
    .battery = 3, .battery_brightness = 50 * LED_DEFAULT_BRIGHTNESS_SCALE,
};

/* padtool led-demo shows these in turn. */
const struct ux_led_demo ux_led_demos[] = {
    { "connecting: Home and the bar blink, soft edges", CTL_CONNECTING },
    { "pairing: a quick blink, a light gliding along the bar", CTL_PAIRING },
    { "connected, steady: Home, players and grips at steady_brightness", CTL_CONNECTED, LED_DEMO_SCENE, GRIPS_PRIMARY, PLAYERS_HOST },
    { "fade: grips and players", CTL_CONNECTED, LED_DEMO_SCENE, GRIPS_FADE, PLAYERS_FADE },
    { "input: press buttons, move the sticks; each grip follows its half", CTL_CONNECTED, LED_DEMO_SCENE, GRIPS_INPUT, PLAYERS_PRIMARY },
    { "rainbow: grips and players", CTL_CONNECTED, LED_DEMO_SCENE, GRIPS_RAINBOW, PLAYERS_RAINBOW },
    { "heartbeat: grips", CTL_CONNECTED, LED_DEMO_SCENE, GRIPS_HEARTBEAT, PLAYERS_PRIMARY },
    { "steady 1.5, dithered", CTL_CONNECTED, LED_DEMO_SCENE, GRIPS_PRIMARY, PLAYERS_PRIMARY, 15, 0, LED_DEMO_DITHER_ON },
    { "steady 1.5, not dithered (shows 2)", CTL_CONNECTED, LED_DEMO_SCENE, GRIPS_PRIMARY, PLAYERS_PRIMARY, 15, 0, LED_DEMO_DITHER_OFF },
    { "steady 0.5, dithered", CTL_CONNECTED, LED_DEMO_SCENE, GRIPS_PRIMARY, PLAYERS_PRIMARY, 5, 0, LED_DEMO_DITHER_ON },
    { "dim fade up to 3, dithered", CTL_CONNECTED, LED_DEMO_SCENE, GRIPS_FADE, PLAYERS_FADE, 0, 30, LED_DEMO_DITHER_ON },
    { "dim fade up to 3, not dithered", CTL_CONNECTED, LED_DEMO_SCENE, GRIPS_FADE, PLAYERS_FADE, 0, 30, LED_DEMO_DITHER_OFF },
    { "remap config mode (Home+M): colours flowing down the pad", CTL_CONNECTED, LED_DEMO_CONFIG },
    { "restarting into the flash loader: a green launch, then held", CTL_REBOOTING, LED_DEMO_LOADER },
};
const u8 ux_led_demo_count = sizeof(ux_led_demos) / sizeof(ux_led_demos[0]);

const struct led_scene ux_config_scene = { .effect = EFFECT_CONFIG };
const struct led_scene ux_loader_scene = { .effect = EFFECT_LOADER };

const struct led_scene ux_scenes[CTL_KIND_COUNT] = {
    [CTL_BOOT] =       { COLOUR_IDLE,    LED_SOLID,           BAR_ALL,    LED_OFF,             GRIPS_WAVE, LED_OFF },
    [CTL_IDLE] =       { COLOUR_IDLE,    LED_OFF,             BAR_ALL,    LED_OFF,             GRIPS_WAVE, LED_OFF },
    [CTL_CONNECTING] = { COLOUR_CONNECT, LED_BLINK(500, 500), BAR_ALL,    LED_BLINK(500, 500), GRIPS_WAVE, LED_OFF },
    [CTL_PAIRING] =    { COLOUR_CONNECT, LED_BLINK(120, 120), BAR_ALL,    LED_SWEEP(120),      GRIPS_WAVE, LED_OFF },
    [CTL_CONNECTED] =  { COLOUR_PRIMARY, LED_SOLID,           BAR_PLAYER, LED_SOLID,           GRIPS_SLOT_SETTING, LED_OFF },
    [CTL_STOPPING] =   { COLOUR_CONNECT, LED_OFF,             BAR_ALL,    LED_OFF,           GRIPS_WAVE, LED_OFF },
    [CTL_SLEEPING] =   { COLOUR_IDLE,    LED_OFF,             BAR_ALL,    LED_OFF,             GRIPS_WAVE, LED_OFF },
    [CTL_REBOOTING] =  { COLOUR_IDLE,    LED_OFF,             BAR_ALL,    LED_OFF,             GRIPS_WAVE, LED_OFF },
};

/* Home+M: remap config mode, which ux_step runs itself.  Every input is held
 * back from the host; releasing a slot key picks that remap slot, releasing
 * Home or M picks the base remap, ux_timeouts.config_ms without a press leaves it as it
 * was.  Releasing L1 or R1 lowers or raises the rumble step and stays. */
#define REQ_CONFIG 0xff

/* Where a gesture works (offline: any state but connected), and whether it
 * fires on release rather than at hold_ms. */
enum { OFFLINE = 1, CONNECTED = 2, RELEASE = 4 };

/* keys held with Home, hold, request, slot, flags.
 * The REQ_CONNECT rows also name the slot keys of config mode. */
static const struct gesture { u32 keys; u16 hold_ms; u8 request, slot, flags; } gestures[] = {
    {PAD_SELECT, 0, REQ_DISCONNECT, SLOT_NONE, OFFLINE | CONNECTED | RELEASE},
    {PAD_A, 0, REQ_CONNECT, 0, OFFLINE | RELEASE},
    {PAD_B, 0, REQ_CONNECT, 1, OFFLINE | RELEASE},
    {PAD_X, 0, REQ_CONNECT, 2, OFFLINE | RELEASE},
    {PAD_Y, 0, REQ_CONNECT, 3, OFFLINE | RELEASE},
    {PAD_UP, 0, REQ_CONNECT, 4, OFFLINE | RELEASE},
    {PAD_DOWN, 0, REQ_CONNECT, 5, OFFLINE | RELEASE},
    {PAD_LEFT, 0, REQ_CONNECT, 6, OFFLINE | RELEASE},
    {PAD_RIGHT, 0, REQ_CONNECT, 7, OFFLINE | RELEASE},
    {PAD_L3, 0, REQ_CALIBRATE, SLOT_NONE, OFFLINE},
    {PAD_R3, 0, REQ_CALIBRATE, SLOT_NONE, OFFLINE},
    {PAD_A, 5000, REQ_PAIR, 0, OFFLINE},
    {PAD_B, 5000, REQ_PAIR, 1, OFFLINE},
    {PAD_X, 5000, REQ_PAIR, 2, OFFLINE},
    {PAD_Y, 5000, REQ_PAIR, 3, OFFLINE},
    {PAD_UP, 5000, REQ_PAIR, 4, OFFLINE},
    {PAD_DOWN, 5000, REQ_PAIR, 5, OFFLINE},
    {PAD_LEFT, 5000, REQ_PAIR, 6, OFFLINE},
    {PAD_RIGHT, 5000, REQ_PAIR, 7, OFFLINE},
    {PAD_M, 0, REQ_CONFIG, SLOT_NONE, CONNECTED | RELEASE},
};
#define GESTURES (sizeof(gestures) / sizeof(gestures[0]))

/* keys: the chord, without Home. before: keys already down when Home went down. */
static struct {
    u32 started, chord_at, keys, captured, before, tap, tap_release_at, done;
    u8 active, live, chorded, offline, fired, mixed, draining, tap_reported;
    u8 config;
    u32 config_since, config_at, config_held, config_down; /* config_at: its last press */
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

static int allowed(const struct gesture *g, u32 keys)
{
    if (g->keys != keys) return 0;
    if (!(g->flags & (ux.offline ? OFFLINE : CONNECTED))) return 0;
    return ux.offline || !(g->keys & ux.before);
}

static struct ctl_request released(u32 chord)
{
    u32 i;
    if (ux.chorded && !ux.fired && !(chord & ~ux.keys)) for (i = 0; i < GESTURES; i++) {
        const struct gesture *g = &gestures[i];
        if (!(g->flags & RELEASE) || !allowed(g, ux.keys)) continue;
        ux.fired = 1;
        return request(g->request, g->slot);
    }
    return request(REQ_NONE, SLOT_NONE);
}

/* Config mode: keys count on release, and only ones pressed in it. */
static struct ctl_request config_step(struct pad *p, u32 now)
{
    struct ctl_request r = request(REQ_NONE, SLOT_NONE);
    u32 keys = p->buttons & PAD_DIGITAL, up, i;
    int done = 0;

    if (keys & ~ux.config_held) ux.config_at = now;
    ux.config_down |= keys & ~ux.config_held;
    up = ux.config_down & ~keys;
    ux.config_held = keys;
    ux.config_down &= keys;
    if (up & (PAD_HOME | PAD_M)) {
        r = request(REQ_REMAP, SLOT_NONE);
        done = 1;
    } else if (up & (PAD_L1 | PAD_R1)) {
        r = request(REQ_RUMBLE_STEP, (up & PAD_R1) ? 1 : 0);
    } else for (i = 0; i < GESTURES && up; i++) {
        if (gestures[i].request != REQ_CONNECT || !(up & gestures[i].keys)) continue;
        r = request(REQ_REMAP, gestures[i].slot);
        done = 1;
        break;
    }
    if (!done && now - ux.config_at >= ux_timeouts.config_ms) done = 1;
    if (done) ux.config = 0;
    memset(p, 0, PAD_CONTROLS_SIZE);
    return r;
}

struct ctl_request ux_step(struct pad *p, u32 now, u8 state)
{
    struct ctl_request r = request(REQ_NONE, SLOT_NONE);
    u32 keys = p->buttons, chord = keys & ~PAD_HOME, output = keys, i;
    int offline = state == CTL_BOOT || state == CTL_IDLE || state == CTL_CONNECTING ||
                  state == CTL_PAIRING || state == CTL_STOPPING;

    if (ux.config) {
        if (state == CTL_CONNECTED) return config_step(p, now);
        ux.config = 0;
    }
    if (state != CTL_CONNECTED) ux.tap = 0;
    if (ux.tap && ux.tap_reported && (s32)(now - ux.tap_release_at) >= 0) ux.tap = 0;
    if (ux.draining) {
        output &= ~ux.captured;
        if (!(keys & ux.captured)) ux.draining = 0;
    } else {
        if (!ux.active && (keys & PAD_HOME)) {
            ux.active = 1;
            ux.live = ux.chorded = ux.fired = ux.mixed = 0;
            ux.done = 0;
            ux.started = ux.chord_at = now;
            ux.keys = 0;
            ux.captured = PAD_HOME;
            ux.before = chord;
        }
        if (ux.active) {
            ux.before &= chord;
            if (chord) ux.mixed = 1;
            if (!(keys & PAD_HOME)) {
                int lone = !ux.chorded && !ux.fired;
                if (lone && state == CTL_CONNECTED && !ux.live) {
                    ux.tap |= PAD_HOME;
                    ux.tap_reported = 0;
                } else if (lone && !ux.mixed && (state == CTL_IDLE || state == CTL_BOOT)) {
                    r = request(REQ_CONNECT, SLOT_LAST);
                } else {
                    r = released(chord);
                }
                ux.active = 0;
                ux.draining = (keys & ux.captured) != 0;
            } else if (ux.chorded && !ux.offline && !ux.fired && chord != ux.keys &&
                       (chord & ux.keys) == ux.keys) {
                /* Connected, another key joined before the gesture fired: they are all the host's. */
                ux.chorded = 0;
                ux.fired = ux.live = 1;
                ux.captured = 0;
            } else if (ux.chorded && chord != ux.keys) {
                r = released(chord);
                ux.active = 0;
                ux.draining = 1;
                ux.captured |= keys;
            } else {
                if (!ux.chorded && !ux.fired) {
                    ux.offline = (u8)offline;
                    for (i = 0; i < GESTURES; i++) {
                        if (!allowed(&gestures[i], chord)) continue;
                        ux.chorded = 1;
                        ux.live = 0;
                        ux.keys = chord;
                        ux.chord_at = now;
                        ux.captured |= chord;
                        break;
                    }
                }
                if (!ux.chorded && !ux.fired && now - ux.started >= ux_timeouts.home_grace_ms)
                    ux.live = 1;
                if (ux.chorded) for (i = 0; i < GESTURES; i++) {
                    const struct gesture *g = &gestures[i];
                    if ((ux.done & (1u << i)) || (g->flags & RELEASE) || !allowed(g, chord) ||
                        now - ux.chord_at < g->hold_ms) continue;
                    ux.done |= 1u << i;
                    ux.fired = 1;
                    r = request(g->request, g->slot);
                    r.generate_new_address = g->request == REQ_PAIR;
                    break;
                }
            }
            if (!ux.live) output &= ~ux.captured;
        }
    }
    if (r.kind == REQ_CONFIG) {
        r = request(REQ_NONE, SLOT_NONE);
        if (state == CTL_CONNECTED) {
            ux.config = 1;
            ux.config_since = ux.config_at = now;
            ux.config_held = keys & PAD_DIGITAL;   /* Home, maybe M: not presses in it */
            ux.config_down = 0;
            ux.draining = 0;                       /* config_held keeps them out instead */
            ux.tap = 0;
            memset(p, 0, PAD_CONTROLS_SIZE);
            return r;
        }
    }
    p->buttons = output | ux.tap;
    /* Until the down report leaves, keep the release deadline armed so the loop still runs. */
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
    if (ux.config) return ux.config_at + ux_timeouts.config_ms;
    if (ux.tap) next = ux.tap_release_at;
    if (!ux.active) return next;
    if (!ux.live && !ux.chorded && !ux.fired)
        next = earlier(next, ux.started + ux_timeouts.home_grace_ms);
    if (ux.chorded) for (i = 0; i < GESTURES; i++) {
        const struct gesture *g = &gestures[i];
        if (!(ux.done & (1u << i)) && !(g->flags & RELEASE) && allowed(g, ux.keys))
            next = earlier(next, ux.chord_at + g->hold_ms);
    }
    return next;
}

u8 ux_armed(void)
{
    u32 i;
    if (!ux.active || ux.fired || !ux.chorded) return SLOT_NONE;
    for (i = 0; i < GESTURES; i++)
        if ((gestures[i].flags & RELEASE) && allowed(&gestures[i], ux.keys)) return gestures[i].slot;
    return SLOT_NONE;
}

void ux_ignore_held(void)
{
    ux.active = ux.live = ux.chorded = ux.fired = 0;
    ux.draining = 1;
    ux.captured = PAD_DIGITAL;
    ux.tap = 0;
}

int ux_config(u32 *since)
{
    if (ux.config && since) *since = ux.config_since;
    return ux.config;
}

/* home_wake with Home already up: the wake was a tap. */
struct ctl_request ux_boot(u32 buttons, int home_wake, u32 now)
{
    struct pad p = {0};
    if (home_wake && !(buttons & PAD_HOME)) return request(REQ_CONNECT, SLOT_LAST);
    p.buttons = buttons;
    return ux_step(&p, now, CTL_BOOT);
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
    state = next_state;
    now = 100;
    run(1);
}
static void config(void)
{
    reset(CTL_CONNECTED);
    pad.buttons = PAD_HOME; run(10); pad.buttons |= PAD_M; run(10); pad.buttons = 0; run(10);
}
int main(void)
{
    u32 i;
    /* Long USB idle: no expired sleep deadline while attached; after unplug
     * wait the complete five seconds, even if offline wait already expired. */
    CHECK(ux_idle_deadline(1000, 1000, 100000, 1, 0) == 0);
    CHECK(ux_idle_deadline(1000, 1000, 100000, 0, 0) == 105000);
    CHECK(ux_idle_deadline(100000, 100000, 100000, 0, 0) == 115000);
    CHECK(ux_idle_deadline(1000, 20000, 100000, 0, 1) == 320000);
    CHECK(ux_idle_deadline(1000, 20000, 100000, 1, 1) == 0);
    CHECK(ux_idle_deadline(0xfffffff0u, 0, 0xfffffff0u, 0, 0) == 14984);
    CHECK(ux_idle_deadline(0xffff0000u, 0, 0xfffffff0u, 0, 0) == 4984);
    /* Connected: Home after its grace. */
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(ux_timeouts.home_grace_ms);
    CHECK(!seen.buttons); run(1); CHECK(seen.buttons == PAD_HOME);
    run(10000); CHECK(last.kind == REQ_NONE && seen.buttons == PAD_HOME); /* no hold-to-sleep */
    pad.buttons = 0; run(1); CHECK(!seen.buttons && !ux.tap);

    /* Connected: a short tap. */
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(10); pad.buttons = 0; run(1);
    CHECK(seen.buttons == PAD_HOME && last.kind == REQ_NONE);
    run(1000); CHECK(seen.buttons == PAD_HOME); /* a blocked/not-ready host cannot erase down */
    ux_reported(now); CHECK(ux_deadline() == now + ux_timeouts.tap_ms);
    run(ux_timeouts.tap_ms - 1); CHECK(seen.buttons == PAD_HOME);
    ux_reported(now); /* more reports cannot indefinitely extend the pulse */
    run(1); CHECK(!seen.buttons);
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(10); pad.buttons = 0; run(1);
    state = CTL_IDLE; run(1); CHECK(!seen.buttons && !ux.tap);
    state = CTL_CONNECTED; run(1); CHECK(!seen.buttons);

    /* Connected: Home then Minus, on release. */
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(10); pad.buttons |= PAD_SELECT; run(3000);
    CHECK(last.kind == REQ_NONE && !seen.buttons && !ux_deadline());
    pad.buttons = PAD_SELECT; run(1); CHECK(last.kind == REQ_DISCONNECT && !seen.buttons);
    run(10); CHECK(last.kind == REQ_NONE && !seen.buttons);
    pad.buttons = 0; run(1); CHECK(!seen.buttons && !ux.tap);
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(10); pad.buttons |= PAD_SELECT; run(10);
    pad.buttons = PAD_HOME; run(1); CHECK(last.kind == REQ_DISCONNECT && !seen.buttons);
    pad.buttons = 0; run(1); CHECK(!seen.buttons && !ux.tap);
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(3000); CHECK(seen.buttons == PAD_HOME);
    pad.buttons |= PAD_SELECT; run(1); CHECK(!seen.buttons);
    pad.buttons = 0; run(1); CHECK(last.kind == REQ_DISCONNECT && !seen.buttons);
    reset(CTL_CONNECTED); pad.buttons = PAD_SELECT; run(100); pad.buttons |= PAD_HOME; run(3000);
    CHECK(last.kind == REQ_NONE && (seen.buttons & PAD_SELECT));
    pad.buttons = PAD_HOME; run(1); pad.buttons |= PAD_SELECT; run(1); /* Minus again: now Home was first */
    pad.buttons = 0; run(1); CHECK(last.kind == REQ_DISCONNECT);
    /* Home+Minus+Plus: all three are the host's, with any keys that follow. */
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(1); pad.buttons |= PAD_SELECT; run(2000);
    CHECK(!seen.buttons);
    pad.buttons |= PAD_START; run(1); CHECK(seen.buttons == pad.buttons);
    run(6000); CHECK(last.kind == REQ_NONE && seen.buttons == pad.buttons && !ux_deadline());
    pad.buttons &= ~PAD_START; run(1000); CHECK(last.kind == REQ_NONE && seen.buttons == pad.buttons);
    pad.buttons = PAD_SELECT; run(1); CHECK(last.kind == REQ_NONE && seen.buttons == PAD_SELECT && !ux.tap);
    pad.buttons = 0; run(1); CHECK(last.kind == REQ_NONE && !seen.buttons);
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(500); pad.buttons |= PAD_SELECT; run(100);
    pad.buttons |= PAD_START; run(1); CHECK(seen.buttons == pad.buttons);
    pad.buttons = 0; run(1); CHECK(last.kind == REQ_NONE && !seen.buttons && !ux.tap);

    /* Connected: slots, pairing and calibration are host keys. */
    for (i = 1; i < GESTURES; i++) {
        if (gestures[i].flags & CONNECTED) continue;
        reset(CTL_CONNECTED); pad.buttons = PAD_HOME | gestures[i].keys; run(6000);
        CHECK(last.kind == REQ_NONE && !ux.chorded && seen.buttons == (PAD_HOME | gestures[i].keys));
    }

    /* Connecting or pairing: Home then Minus cancels it. */
    reset(CTL_PAIRING); pad.buttons = PAD_HOME; run(10); pad.buttons |= PAD_SELECT; run(10);
    pad.buttons = PAD_HOME; run(1); CHECK(last.kind == REQ_DISCONNECT);

    /* Offline: connect on release, or pair at 5 s. */
    for (i = CTL_BOOT; i <= CTL_STOPPING; i++) {
        if (i == CTL_CONNECTED) continue;
        reset((u8)i); run(5000);
        pad.buttons = PAD_HOME; run(ux_timeouts.home_grace_ms + 100);
        pad.buttons |= PAD_B; run(5000);
        CHECK(last.kind == REQ_NONE && !seen.buttons && ux_deadline() == now + 1);
        run(1); CHECK(last.kind == REQ_PAIR && last.slot == 1 && last.generate_new_address);
        pad.buttons = PAD_HOME; run(1); CHECK(last.kind == REQ_NONE);
        pad.buttons = 0; run(1); CHECK(last.kind == REQ_NONE && !seen.buttons && !ux.tap);
        reset((u8)i); pad.buttons = PAD_HOME | PAD_B; run(5000); CHECK(last.kind == REQ_NONE);
        pad.buttons = PAD_HOME; run(1); CHECK(last.kind == REQ_CONNECT && last.slot == 1 && !last.generate_new_address);
        run(10); pad.buttons = 0; run(1); CHECK(last.kind == REQ_NONE && !ux.tap);
        reset((u8)i); pad.buttons = PAD_HOME | PAD_B; run(100);
        pad.buttons = PAD_B; run(1); CHECK(last.kind == REQ_CONNECT && last.slot == 1);
        run(10); CHECK(!seen.buttons); pad.buttons = 0; run(1);
    }
    /* Armed. */
    reset(CTL_IDLE); CHECK(ux_armed() == SLOT_NONE);
    pad.buttons = PAD_HOME; run(1); CHECK(ux_armed() == SLOT_NONE);
    pad.buttons |= PAD_Y; run(1); CHECK(ux_armed() == 3);
    run(5000); CHECK(ux_armed() == SLOT_NONE); /* now pairing */
    reset(CTL_IDLE); pad.buttons = PAD_HOME | PAD_L1; run(1); CHECK(ux_armed() == SLOT_NONE);
    reset(CTL_CONNECTING); pad.buttons = PAD_HOME; run(1); CHECK(ux_armed() == SLOT_NONE);
    pad.buttons |= PAD_A; run(1); CHECK(ux_armed() == 0);
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(1); CHECK(ux_armed() == SLOT_NONE);
    pad.buttons |= PAD_A; run(1); CHECK(ux_armed() == SLOT_NONE);
    /* A slot started offline keeps going once connected. */
    reset(CTL_IDLE); pad.buttons = PAD_HOME | PAD_B; run(100); state = CTL_CONNECTED;
    run(4901); CHECK(last.kind == REQ_PAIR && last.slot == 1);
    reset(CTL_IDLE); pad.buttons = PAD_L3; run(100); pad.buttons |= PAD_HOME; run(1);
    CHECK(last.kind == REQ_CALIBRATE);
    reset(CTL_IDLE); pad.buttons = PAD_HOME | PAD_RIGHT; run(1);
    CHECK(!seen.buttons); pad.buttons = PAD_HOME; run(1);
    CHECK(last.kind == REQ_CONNECT && last.slot == 7);
    reset(CTL_IDLE); pad.buttons = PAD_HOME | PAD_B; run(100); pad.buttons |= PAD_A; run(6000);
    CHECK(last.kind == REQ_NONE && !seen.buttons);
    pad.buttons = 0; run(100); CHECK(last.kind == REQ_NONE && !seen.buttons && !ux.tap);
    reset(CTL_IDLE); pad.buttons = PAD_HOME | PAD_B; run(100); pad.buttons = PAD_A; run(1);
    CHECK(last.kind == REQ_NONE); /* let go into another key: a cancel */
    reset(CTL_IDLE); pad.buttons = PAD_HOME | PAD_L1; run(100); pad.buttons = 0; run(1);
    CHECK(last.kind == REQ_NONE); /* not a lone tap */

    /* Offline lone tap. */
    reset(CTL_IDLE); pad.buttons = PAD_HOME; run(3000); CHECK(last.kind == REQ_NONE);
    pad.buttons = 0; run(1); CHECK(last.kind == REQ_CONNECT && last.slot == SLOT_LAST);
    reset(CTL_CONNECTING); pad.buttons = PAD_HOME; run(10); pad.buttons = 0; run(1);
    CHECK(last.kind == REQ_NONE && !ux.tap);

    /* Waking with a slot. */
    for (i = 0; i < 8; i++) {
        reset(CTL_BOOT); pad.buttons = PAD_HOME | gestures[i + 1].keys;
        last = ux_boot(pad.buttons, 1, now);
        CHECK(last.kind == REQ_NONE);
        state = CTL_IDLE; run(1); CHECK(last.kind == REQ_NONE && !seen.buttons);
        run(4999); CHECK(last.kind == REQ_PAIR && last.slot == i);
        reset(CTL_BOOT); pad.buttons = PAD_HOME | gestures[i + 1].keys; ux_boot(pad.buttons, 1, now);
        state = CTL_IDLE; run(300); pad.buttons = 0; run(1);
        CHECK(last.kind == REQ_CONNECT && last.slot == i);
    }
    reset(CTL_BOOT); last = ux_boot(PAD_HOME | PAD_R3, 1, now);
    CHECK(last.kind == REQ_CALIBRATE);
    /* Waking with Home alone. */
    reset(CTL_BOOT); last = ux_boot(PAD_HOME, 1, now);
    CHECK(last.kind == REQ_NONE && ux_armed() == SLOT_NONE);
    pad.buttons = PAD_HOME; run(300); CHECK(last.kind == REQ_NONE);
    pad.buttons = 0; run(1); CHECK(last.kind == REQ_CONNECT && last.slot == SLOT_LAST);
    CHECK(ux_armed() == SLOT_NONE);
    reset(CTL_BOOT); ux_boot(PAD_HOME, 1, now);
    pad.buttons = PAD_HOME; state = CTL_IDLE; run(1000);
    pad.buttons |= PAD_X; run(1); CHECK(ux_armed() == 2);
    pad.buttons = PAD_HOME; run(1); CHECK(last.kind == REQ_CONNECT && last.slot == 2);
    pad.buttons = 0; run(1); CHECK(last.kind == REQ_NONE);
    reset(CTL_BOOT); CHECK(ux_boot(PAD_M | PAD_B, 0, now).kind == REQ_NONE);
    reset(CTL_BOOT); last = ux_boot(0, 1, now);
    CHECK(last.kind == REQ_CONNECT && last.slot == SLOT_LAST);
    reset(CTL_BOOT); CHECK(ux_boot(0, 0, now).kind == REQ_NONE);

    /* M is a plain key everywhere. */
    reset(CTL_CONNECTED); pad.buttons = PAD_M; run(1); CHECK(seen.buttons == PAD_M);
    pad.buttons |= PAD_A; run(2000); CHECK(last.kind == REQ_NONE && seen.buttons == pad.buttons);
    reset(CTL_IDLE); pad.buttons = PAD_M | PAD_A; run(100); CHECK(last.kind == REQ_NONE);

    /* Remap config mode: Home+M on release, whichever goes first. */
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(10); pad.buttons |= PAD_M; run(3000);
    CHECK(!ux_config(NULL) && !seen.buttons);
    pad.buttons = PAD_HOME; run(1); CHECK(ux_config(NULL) && last.kind == REQ_NONE && !seen.buttons);
    pad.buttons = 0; run(100); CHECK(ux_config(NULL) && last.kind == REQ_NONE); /* Home from the chord */
    pad.buttons = PAD_L4; pad.lx = 9000; pad.r2 = 30000; run(100);
    CHECK(ux_config(NULL) && !seen.buttons && !seen.lx && !seen.r2); /* nothing for the host */
    pad.buttons = 0; run(1); CHECK(ux_config(NULL) && last.kind == REQ_NONE);
    pad.buttons = PAD_X; run(100); CHECK(last.kind == REQ_NONE && !seen.buttons);
    pad.buttons = 0; run(1); CHECK(last.kind == REQ_REMAP && last.slot == 2 && !ux_config(NULL));
    run(1); CHECK(seen.lx == 9000 && seen.r2 == 30000);
    pad.buttons = PAD_A; run(1); CHECK(seen.buttons == PAD_A && last.kind == REQ_NONE);
    reset(CTL_CONNECTED); pad.buttons = PAD_HOME; run(10); pad.buttons |= PAD_M; run(100);
    pad.buttons = PAD_M; run(1); CHECK(ux_config(NULL) && !seen.buttons);
    pad.buttons = 0; run(1); CHECK(ux_config(NULL) && last.kind == REQ_NONE); /* M from the chord */
    pad.buttons = PAD_LEFT; run(10); pad.buttons = 0; run(1);
    CHECK(last.kind == REQ_REMAP && last.slot == 6);
    /* Home or M picks the base; Home does not reach the host. */
    for (i = 0; i < 2; i++) {
        config();
        pad.buttons = i ? PAD_M : PAD_HOME; run(1000); CHECK(ux_config(NULL) && !seen.buttons);
        pad.buttons = 0; run(1); CHECK(last.kind == REQ_REMAP && last.slot == SLOT_NONE);
        run(1000); CHECK(!seen.buttons && !ux.tap && !ux_config(NULL));
    }
    /* After it, held keys are the host's as usual. */
    config();
    pad.buttons = PAD_L1; run(10); pad.buttons |= PAD_B; run(10); pad.buttons = PAD_L1; run(1);
    CHECK(last.kind == REQ_REMAP && last.slot == 1);
    run(1); CHECK(seen.buttons == PAD_L1);
    config(); pad.buttons = PAD_HOME; run(ux_timeouts.config_ms + 1); CHECK(!ux_config(NULL)); run(10);
    pad.buttons |= PAD_SELECT; run(10); pad.buttons = 0; run(1); CHECK(last.kind == REQ_DISCONNECT);
    /* The timeout runs from the last press and changes nothing. */
    config();
    CHECK(ux_config(NULL) && ux_deadline() == now + ux_timeouts.config_ms - 9);
    run(ux_timeouts.config_ms - 1000); pad.buttons = PAD_R1; run(1); pad.buttons = 0;
    run(ux_timeouts.config_ms - 1); CHECK(ux_config(NULL));
    run(1); CHECK(!ux_config(NULL) && last.kind == REQ_NONE);
    /* L1 and R1 step the rumble level and stay in it. */
    config();
    pad.buttons = PAD_L1; run(10); pad.buttons = 0; run(1);
    CHECK(last.kind == REQ_RUMBLE_STEP && last.slot == 0 && ux_config(NULL) && !seen.buttons);
    pad.buttons = PAD_R1; run(10); pad.buttons = 0; run(1);
    CHECK(last.kind == REQ_RUMBLE_STEP && last.slot == 1 && ux_config(NULL));
    run(1); CHECK(last.kind == REQ_NONE);
    /* The session going ends it. */
    config();
    state = CTL_CONNECTING; run(1); CHECK(!ux_config(NULL) && last.kind == REQ_NONE);
    /* Offline, or M down before Home: no config mode. */
    reset(CTL_IDLE); pad.buttons = PAD_HOME | PAD_M; run(100); pad.buttons = 0; run(1);
    CHECK(!ux_config(NULL) && last.kind == REQ_NONE);
    reset(CTL_CONNECTED); pad.buttons = PAD_M; run(100); pad.buttons |= PAD_HOME; run(1000);
    CHECK(seen.buttons == (PAD_HOME | PAD_M));
    pad.buttons = 0; run(1); CHECK(!ux_config(NULL) && last.kind == REQ_NONE);

    /* Drained after ux_ignore_held. */
    reset(CTL_BOOT); ux_ignore_held(); pad.buttons = PAD_HOME | PAD_B; run(6000);
    CHECK(last.kind == REQ_NONE && !seen.buttons);
    pad.buttons = 0; run(1); CHECK(last.kind == REQ_NONE);
    reset(CTL_CONNECTED); now = 0xffffffc0u; pad.buttons = PAD_HOME; run(ux_timeouts.home_grace_ms + 1);
    CHECK(seen.buttons == PAD_HOME);
    return TEST_DONE("settings_ux");
}
#endif

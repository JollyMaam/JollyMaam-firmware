#include "pad.h"

/* Fixed timeouts and LED scenes. Shortcuts and remapping are runtime settings. */

const struct ux_timeouts ux_timeouts = {
    .tap_retry_ms = 60,         /* retry a deferred tap when no report has been accepted */
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

/* A captured physical chord is never reinterpreted when keys change: drain
 * it until release. Runtime edits similarly drain held keys. */
static struct {
    struct set_shortcuts config;
    u32 started, chord_at, keys, captured, tap, tap_release_at, done;
    u32 home_at, now;
    u8 have_config, active, live, chorded, fired, draining;
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
    const struct set_shortcuts *config = set_shortcuts();
    struct ctl_request r = request(REQ_NONE, SLOT_NONE);
    u32 keys = in_keys(p), modifier, i, output = keys;
    int home = (keys & PAD_HOME) != 0;

    if (!ux.have_config || memcmp(&ux.config, config, sizeof(*config))) {
        int changed = ux.have_config;
        ux.config = *config;
        ux.have_config = 1;
        if (changed) {
            ux.active = ux.live = ux.chorded = ux.fired = 0;
            ux.draining = keys != 0;
            ux.captured = keys;
            ux.tap = 0;
        }
    }
    modifier = ux.config.modifier;
    ux.now = now;

    /* Home remains a normal host key. Offline, a tap connects the last slot;
     * the configured hold powers off. Boot wake is handled by ux_boot(). */
    if (ux.home_ignore) {
        output &= ~PAD_HOME;
        if (!home) ux.home_ignore = 0;
    } else if (!(modifier & PAD_HOME)) {
        if (home && !ux.home_down) { ux.home_at = now; ux.home_fired = 0; }
        if (home && keys != PAD_HOME) ux.home_fired = 1;
        if (home && !ux.home_fired && ux.config.home_sleep_ms && now - ux.home_at >= ux.config.home_sleep_ms) {
            ux.home_fired = 1;
            r = request(REQ_SLEEP, SLOT_NONE);
        }
        if (!home && ux.home_down && !ux.home_fired && (state == CTL_IDLE || state == CTL_BOOT))
            r = request(REQ_CONNECT, SLOT_LAST);
    }
    ux.home_down = home;

    if (ux.tap && ux.tap_reported) ux.tap = 0;
    if (ux.draining) {
        if (!ux.config.passthrough) output &= ~ux.captured;
        if (!(keys & ux.captured)) ux.draining = 0;
    } else if (modifier) {
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
                if (!ux.live && !ux.chorded && !ux.fired && !ux.config.passthrough) {
                    ux.tap |= modifier;
                    ux.tap_reported = 0;
                }
                ux.active = 0;
                ux.draining = (keys & ux.captured) != 0;
            } else if (!ux.live || ux.config.passthrough) {
                if (ux.chorded && keys != ux.keys) {
                    ux.active = 0;
                    ux.draining = 1;
                    ux.captured |= keys;
                } else {
                    if (!ux.chorded && (ux.config.passthrough || now == ux.started ||
                                        now - ux.started < ux.config.grace_ms)) {
                        for (i = 0; i < SHORTCUT_RULES; i++) {
                            const struct shortcut_rule *g = &ux.config.rule[i];
                            if (!g->request || g->keys != keys) continue;
                            ux.chorded = 1;
                            ux.keys = keys;
                            ux.chord_at = now;
                            ux.captured |= keys;
                            break;
                        }
                    }
                    if (!ux.chorded && !ux.config.passthrough && now - ux.started >= ux.config.grace_ms)
                        ux.live = 1;
                    if (ux.chorded) for (i = 0; i < SHORTCUT_RULES; i++) {
                        const struct shortcut_rule *g = &ux.config.rule[i];
                        if ((ux.done & (1u << i)) || !g->request || g->keys != keys || now - ux.chord_at < g->hold_ms) continue;
                        ux.done |= 1u << i;
                        ux.fired = 1;
                        r = request(g->request, g->slot);
                        break;
                    }
                }
            }
            if (!ux.config.passthrough && !ux.live) output &= ~ux.captured;
        }
    }
    in_set_keys(p, output | ux.tap);
    /* A tap is held through at least one accepted report. Keep its release
     * deadline armed until acknowledgement, so a sleeping loop still runs. */
    if (ux.tap && !ux.tap_reported) ux.tap_release_at = now + ux_timeouts.tap_retry_ms;
    return r;
}

void ux_reported(void)
{
    if (ux.tap) { ux.tap_reported = 1; ux.tap_release_at = ux.now + 1; }
}

u32 ux_deadline(void)
{
    u32 next = 0, i;
    if (ux.tap) next = ux.tap_release_at;
    if (ux.home_down && !ux.home_fired && !ux.home_ignore && ux.config.home_sleep_ms && !(ux.config.modifier & PAD_HOME))
        next = earlier(next, ux.home_at + ux.config.home_sleep_ms);
    if (!ux.active) return next;
    if (!ux.live && !ux.chorded && !ux.fired && !ux.config.passthrough)
        next = earlier(next, ux.started + ux.config.grace_ms);
    if (ux.chorded && (!ux.live || ux.config.passthrough)) for (i = 0; i < SHORTCUT_RULES; i++) {
        const struct shortcut_rule *g = &ux.config.rule[i];
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

struct ctl_request ux_boot(u16 buttons, u8 hat, u8 state)
{
    (void)hat;
    (void)state;
    if (!(buttons & PAD_HOME)) return request(REQ_NONE, SLOT_NONE);
    ux.home_ignore = 1;
    return request(REQ_CONNECT, SLOT_LAST);
}

#ifdef UNITTEST
static struct set_shortcuts config;
const struct set_shortcuts *set_shortcuts(void) { return &config; }
/* Keep the input helpers exercised independently in mapping.c. */
u32 in_keys(const struct pad *p)
{
    static const u8 hats[] = {1,3,2,6,4,12,8,9,0};
    return p->buttons | (u32)hats[p->hat] << 16;
}
void in_set_keys(struct pad *p, u32 k)
{
    static const u8 hats[] = {8,0,2,1,4,8,3,2,6,7,8,0,5,6,4,8};
    p->buttons = (u16)k;
    p->hat = hats[(k >> 16) & 15];
}
static u32 now;
static struct pad pad, seen;
static struct ctl_request last;
static u8 state;
static void run(u32 ms)
{
    while (ms--) { now++; seen = pad; last = ux_step(&seen, now, state); }
}
static void reset(void)
{
    memset(&ux, 0, sizeof(ux));
    memset(&config, 0, sizeof(config));
    memset(&pad, 0, sizeof(pad));
    pad.hat = HAT_NEUTRAL;
    config.modifier = PAD_M;
    config.grace_ms = 150;
    config.home_sleep_ms = 5000;
    config.rule[0] = (struct shortcut_rule){PAD_M | PAD_B, 1500, REQ_CONNECT, 1};
    config.rule[4] = (struct shortcut_rule){PAD_M | PAD_B, 5000, REQ_PAIR, 1};
    config.rule[1] = (struct shortcut_rule){PAD_M | PAD_SELECT, 1500, REQ_DISCONNECT, SLOT_NONE};
    config.rule[2] = (struct shortcut_rule){PAD_M | PAD_L3, 3000, REQ_CALIBRATE, SLOT_NONE};
    config.rule[3] = (struct shortcut_rule){PAD_M | PAD_R3, 3000, REQ_CALIBRATE, SLOT_NONE};
    state = CTL_CONNECTED;
    now = 100;
    run(1);
}
int main(void)
{
    u32 i;
    reset(); pad.buttons = PAD_M; run(1);
    CHECK(!seen.buttons && ux_deadline() == now + 150);
    run(149); CHECK(!seen.buttons);
    run(1); CHECK(seen.buttons == PAD_M);
    pad.buttons |= PAD_B; run(2000);
    CHECK(last.kind == REQ_NONE && seen.buttons == (PAD_M | PAD_B));

    reset(); pad.buttons = PAD_M; run(10); pad.buttons = 0; run(1);
    CHECK(seen.buttons == PAD_M);
    run(1000); CHECK(seen.buttons == PAD_M); /* no host report: do not lose the tap */
    ux_reported(); run(1);
    CHECK(!seen.buttons);

    for (i = 0; i < 4; i++) {
        u32 keys, hold;
        reset(); keys = config.rule[i].keys; hold = config.rule[i].hold_ms;
        pad.buttons = (u16)keys; run(1);
        CHECK(!seen.buttons && ux_deadline() == now + hold);
        run(hold - 1); CHECK(last.kind == REQ_NONE);
        run(1); CHECK(last.kind == config.rule[i].request && last.slot == config.rule[i].slot);
        run(10); CHECK(last.kind == REQ_NONE);
        pad.buttons = (u16)(keys & ~PAD_M); run(1); CHECK(!seen.buttons);
        run(100); CHECK(!seen.buttons);
        pad.buttons = 0; run(1); CHECK(!seen.buttons);
    }
    reset(); pad.buttons = PAD_M | PAD_B; run(1501); CHECK(last.kind == REQ_CONNECT);
    run(3500); CHECK(last.kind == REQ_PAIR && last.slot == 1);
    run(1); CHECK(last.kind == REQ_NONE);
    reset(); pad.buttons = PAD_M; run(100); pad.buttons |= PAD_B; run(1500);
    CHECK(last.kind == REQ_NONE); run(1); CHECK(last.kind == REQ_CONNECT);
    reset(); pad.buttons = PAD_M | PAD_B; run(100); pad.buttons |= PAD_A; run(2000);
    CHECK(last.kind == REQ_NONE && !seen.buttons);
    pad.buttons = 0; run(100); CHECK(!seen.buttons); /* cancelled chord never taps */
    reset(); pad.buttons = PAD_M | PAD_B; run(100); pad.buttons = PAD_B; run(1);
    CHECK(!seen.buttons && !ux.tap);
    reset(); config.passthrough = 1; run(1); pad.buttons = PAD_M | PAD_B; run(1);
    CHECK(seen.buttons == pad.buttons); run(1500); CHECK(last.kind == REQ_CONNECT);
    reset(); pad.buttons = PAD_M | PAD_B; run(100); config.grace_ms = 250; run(2000);
    CHECK(last.kind == REQ_NONE && !seen.buttons);
    pad.buttons = 0; run(1); pad.buttons = PAD_M; run(250); CHECK(!seen.buttons);
    run(1); CHECK(seen.buttons == PAD_M);
    reset(); pad.buttons = PAD_M | PAD_L1; run(150);
    CHECK(seen.buttons == PAD_L1 && !ux.chorded);
    run(1); CHECK(seen.buttons == (PAD_M | PAD_L1));
    reset(); pad.buttons = PAD_M; run(1);
    now += 150; pad.buttons |= PAD_B; run(1); /* late event cannot extend grace */
    CHECK(seen.buttons == (PAD_M | PAD_B) && !ux.chorded);
    reset(); config.grace_ms = 0; run(1); pad.buttons = PAD_M; run(1);
    CHECK(seen.buttons == PAD_M);
    reset(); config.grace_ms = 0; run(1); pad.buttons = PAD_M | PAD_B; run(1501);
    CHECK(last.kind == REQ_CONNECT && !seen.buttons);
    reset(); now = 0xffffffc0u; pad.buttons = PAD_M; run(151); CHECK(seen.buttons == PAD_M);
    reset(); ux_ignore_held(); pad.buttons = PAD_HOME | PAD_M | PAD_B; run(6000);
    CHECK(last.kind == REQ_NONE && !seen.buttons);
    pad.buttons = 0; run(1);
    reset(); last = ux_boot(PAD_HOME | PAD_X, HAT_NEUTRAL, CTL_BOOT);
    CHECK(last.kind == REQ_CONNECT && last.slot == SLOT_LAST);
    pad.buttons = PAD_HOME; run(6000); CHECK(last.kind == REQ_NONE && !seen.buttons);
    pad.buttons = 0; run(1); state = CTL_IDLE; pad.buttons = PAD_HOME; run(1);
    pad.buttons = 0; run(1); CHECK(last.kind == REQ_CONNECT && last.slot == SLOT_LAST);
    reset(); pad.buttons = PAD_HOME; run(5001); CHECK(last.kind == REQ_SLEEP);
    reset(); pad.buttons = PAD_HOME | PAD_A; run(6000); CHECK(last.kind == REQ_NONE);
    reset(); config.home_sleep_ms = 0; run(1); pad.buttons = PAD_HOME;
    run(6000); CHECK(last.kind == REQ_NONE && !ux_deadline());
    reset(); config.modifier = PAD_L4;
    config.rule[0] = (struct shortcut_rule){PAD_L4 | PAD_A, 100, REQ_CONNECT, 3};
    run(1); pad.buttons = PAD_L4 | PAD_A; run(101);
    CHECK(last.kind == REQ_CONNECT && last.slot == 3 && !seen.buttons);
    for (i = 0; i < 4; i++) {
        static const u8 hats[] = {HAT_UP, HAT_DOWN, HAT_LEFT, HAT_RIGHT};
        static const u32 masks[] = {1u << 16, 1u << 18, 1u << 19, 1u << 17};
        reset(); config.rule[0] = (struct shortcut_rule){PAD_M | masks[i], 1500, REQ_CONNECT, 4 + i};
        run(1); pad.hat = hats[i]; pad.buttons = PAD_M; run(1501);
        CHECK(last.kind == REQ_CONNECT && last.slot == 4 + i && !seen.buttons && seen.hat == HAT_NEUTRAL);
        pad.buttons = 0; run(1); CHECK(seen.hat == HAT_NEUTRAL);
        pad.hat = HAT_NEUTRAL; run(1);
    }
    return TEST_DONE("settings_ux");
}
#endif

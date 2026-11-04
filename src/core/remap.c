#include <string.h>
#include "pad.h"

/* A remap slot on the buttons ux_step leaves, so the Home gestures keep
 * working on the physical keys.  Each key's map entry is its base row, "held:
 * hold KEY" (itself by default) or nothing; its rules add to that.  Rules
 * read inputs, never each other's outputs, and their outputs OR together.
 * Nothing waits to see what comes next: held for fires at its time, tapped on
 * a quick release, double tapped on the second press.  Timed output follows
 * the clock and never catches up after a stall.  A turbo modifier, while on,
 * pulses what its keys send while held (their base rows and hold rules), each
 * from the later of its press and the modifier coming on; the first modifier
 * on for a key wins. */

static struct {
    u32 prev;                        /* the input last pass */
    u32 down_at[REMAP_INPUTS], up_at[REMAP_INPUTS];
    u32 armed;                       /* released: a quick press is a double tap */
    u32 second;                      /* pressed as a double tap: its release arms nothing */
    u32 active, latched, pulsing;    /* a bit per rule */
    u32 since[REMAP_RULES];          /* level active, or latched on: the turbo phase */
    u32 pulse_until[REMAP_RULES];
    u32 next;
} rm;

static u32 outputs(const struct remap_rule *r)
{
    return r->outputs[0] | r->outputs[1] << 8 | (u32)r->outputs[2] << 16 | (u32)r->outputs[3] << 24;
}

static void later(u32 at)
{
    if (!rm.next || (s32)(at - rm.next) < 0) rm.next = at;
}

/* Square wave from since, starting down. */
static int turbo_on(u8 rate, u32 since, u32 now)
{
    u32 half = 500u / rate, n = (now - since) / half;

    later(since + (n + 1) * half);
    return !(n & 1);
}

u32 remap_step(const struct set_remap *m, u32 in, u32 now, u32 *every_report)
{
    u32 pressed = in & ~rm.prev, released = rm.prev & ~in, out = 0, max = 0, doubled = 0, i;
    u32 sends[REMAP_INPUTS];         /* what each held key sends: its base row and hold rules */
    u8 modifier[REMAP_INPUTS];       /* the turbo modifier on for it, rule + 1; 0 none */

    rm.next = 0;
    memset(modifier, 0, sizeof(modifier));
    for (i = 0; i < REMAP_INPUTS; i++)
        if (pressed & (1u << i)) rm.down_at[i] = now;
    for (i = 0; i < REMAP_INPUTS; i++) {
        u8 to = m->map[i];

        sends[i] = (in & (1u << i)) && to != REMAP_OFF ? 1u << (to ? to - 1u : i) : 0;
    }
    for (i = 0; i < REMAP_RULES; i++) {
        const struct remap_rule *r = &m->rule[i];
        u32 key = 1u << r->input, bit = 1u << i, ms = r->ms10 * 10u, o = outputs(r);
        u32 down = rm.down_at[r->input];
        int active = 0, event = 0;

        switch (r->trigger) {
        case RT_NONE: continue;
        case RT_HELD: active = (in & key) != 0; break;
        case RT_HELD_FOR:
            if (!(in & key)) break;
            if (now - down >= ms) active = 1;
            else later(down + ms);
            break;
        case RT_PRESSED: event = (pressed & key) != 0; break;
        case RT_RELEASED: event = (released & key) != 0; break;
        case RT_TAPPED: event = (released & key) && now - down < ms; break;
        case RT_DOUBLE:
            event = (pressed & key) && (rm.armed & key) && now - rm.up_at[r->input] < ms;
            if (event) doubled |= key;
            break;
        }
        if (active && !(rm.active & bit)) rm.since[i] = now;
        rm.active = active ? rm.active | bit : rm.active & ~bit;
        if (event && r->effect == RE_TAP) {
            rm.pulse_until[i] = now + ux_timeouts.tap_ms;
            rm.pulsing |= bit;
        } else if (event) {
            rm.latched ^= bit;
            rm.since[i] = now;
        }
        if (rm.pulsing & bit) {
            if ((s32)(now - rm.pulse_until[i]) < 0) {
                out |= o;
                later(rm.pulse_until[i]);
            } else {
                rm.pulsing &= ~bit;
            }
        }
        if (!(remap_level_effect(r->effect) ? active : (rm.latched & bit) != 0)) continue;
        if (r->effect == RE_HOLD) sends[r->input] |= o;
        else if (r->effect == RE_TOGGLE_HOLD) out |= o;
        else if (r->effect == RE_TURBO || r->effect == RE_TOGGLE_TURBO) {
            if (!r->rate) max |= o;
            else if (turbo_on(r->rate, rm.since[i], now)) out |= o;
        } else if (r->effect == RE_TURBO_MODIFIER || r->effect == RE_TOGGLE_TURBO_MODIFIER) {
            u32 k;

            for (k = 0; k < REMAP_INPUTS; k++)
                if ((o & (1u << k)) && !modifier[k]) modifier[k] = (u8)(i + 1);
        }
    }
    for (i = 0; i < REMAP_INPUTS; i++) {
        const struct remap_rule *r;
        u32 since;

        if (!sends[i]) continue;
        if (!modifier[i]) {
            out |= sends[i];
            continue;
        }
        r = &m->rule[modifier[i] - 1];
        since = rm.since[modifier[i] - 1];
        if ((s32)(rm.down_at[i] - since) > 0) since = rm.down_at[i];
        if (!r->rate) max |= sends[i];
        else if (turbo_on(r->rate, since, now)) out |= sends[i];
    }
    rm.second |= doubled;
    rm.armed &= ~pressed;
    for (i = 0; i < REMAP_INPUTS; i++) {
        u32 key = 1u << i;

        if (!(released & key)) continue;
        if (rm.second & key) {
            rm.second &= ~key;
        } else {
            rm.armed |= key;
            rm.up_at[i] = now;
        }
    }
    rm.prev = in;
    *every_report = max;
    return in_dpad(out);
}

void remap_reset(void)
{
    memset(&rm, 0, sizeof(rm));
}

u32 remap_deadline(void)
{
    return rm.next;
}

#ifdef UNITTEST
const struct ux_timeouts ux_timeouts = { .tap_ms = 60 };
static struct set_remap m;
static u32 now, out, every;

static void rule(u32 n, u8 trigger, u8 input, u32 ms, u8 effect, u32 o, u8 rate)
{
    struct remap_rule r = { trigger, (u8)input, (u8)(ms / 10), effect,
                            { (u8)o, (u8)(o >> 8), (u8)(o >> 16), (u8)(o >> 24) }, rate, 0 };
    m.rule[n] = r;
}

static void at(u32 in, u32 t)
{
    now = t;
    out = remap_step(&m, in, now, &every);
}

int main(void)
{
    u32 t;

    /* Identity, a swap, a key turned off. */
    at(PAD_SOUTH | PAD_UP, 1000); CHECK(out == (PAD_SOUTH | PAD_UP) && !every && !remap_deadline());
    m.map[0] = 1 + 1; m.map[1] = 0 + 1; m.map[2] = REMAP_OFF;
    at(PAD_SOUTH | PAD_SCREENSHOT, 1001); CHECK(out == PAD_EAST);
    at(PAD_SOUTH | PAD_EAST, 1002); CHECK(out == (PAD_SOUTH | PAD_EAST));
    /* Mapped onto opposite d-pad directions: they cancel. */
    m.map[3] = 16 + 1; m.map[4] = 18 + 1;
    at(PAD_WEST | PAD_NORTH, 1003); CHECK(out == 0);
    at(PAD_WEST, 1004); CHECK(out == PAD_UP);
    memset(&m, 0, sizeof(m)); remap_reset();

    /* Rules add to the base row; a chord from one key; base off: only the rules. */
    rule(0, RT_HELD, 9, 0, RE_HOLD, PAD_L1 | PAD_R1, 0);
    at(PAD_R4 | PAD_SOUTH, 2000); CHECK(out == (PAD_L1 | PAD_R1 | PAD_R4 | PAD_SOUTH));
    at(PAD_SOUTH, 2001); CHECK(out == PAD_SOUTH);
    m.map[9] = 0 + 1;
    at(PAD_R4, 2002); CHECK(out == (PAD_L1 | PAD_R1 | PAD_SOUTH));
    m.map[9] = REMAP_OFF;
    at(PAD_R4, 2003); CHECK(out == (PAD_L1 | PAD_R1));
    m.map[9] = 0;
    rule(0, RT_HELD, 2, 0, RE_NOTHING, 0, 0);       /* adds nothing; the base row stays */
    at(PAD_SCREENSHOT, 2004); CHECK(out == PAD_SCREENSHOT);
    memset(&m, 0, sizeof(m)); remap_reset();

    /* Turbo at 10 Hz: 50 ms down, 50 ms up, from the press; held keeps south too. */
    rule(0, RT_HELD, 0, 0, RE_TURBO, PAD_SOUTH, 10);
    rule(1, RT_HELD, 0, 0, RE_HOLD, PAD_EAST, 0);
    m.map[0] = REMAP_OFF;
    at(PAD_SOUTH, 3000); CHECK(out == (PAD_SOUTH | PAD_EAST) && remap_deadline() == 3050);
    at(PAD_SOUTH, 3049); CHECK(out == (PAD_SOUTH | PAD_EAST));
    at(PAD_SOUTH, 3050); CHECK(out == PAD_EAST && remap_deadline() == 3100);
    at(PAD_SOUTH, 3100); CHECK(out == (PAD_SOUTH | PAD_EAST));
    at(PAD_SOUTH, 3420); CHECK(out == (PAD_SOUTH | PAD_EAST)); /* a stall: the phase from the clock */
    at(0, 3421); CHECK(out == 0 && !remap_deadline());
    /* Rate 0: the report builder flips it. */
    rule(0, RT_HELD, 0, 0, RE_TURBO, PAD_SOUTH, 0);
    at(PAD_SOUTH, 3500); CHECK(out == PAD_EAST && every == PAD_SOUTH && !remap_deadline());
    at(0, 3501); CHECK(!every);
    memset(&m, 0, sizeof(m)); remap_reset();

    /* Tap on press, tap on release: one pulse of tap_ms each (base off). */
    memset(m.map, REMAP_OFF, sizeof(m.map));
    rule(0, RT_PRESSED, 5, 0, RE_TAP, PAD_HOME, 0);
    rule(1, RT_RELEASED, 5, 0, RE_TAP, PAD_SCREENSHOT, 0);
    at(PAD_M, 4000); CHECK(out == PAD_HOME && remap_deadline() == 4060);
    at(PAD_M, 4059); CHECK(out == PAD_HOME);
    at(PAD_M, 4060); CHECK(out == 0);
    at(0, 5000); CHECK(out == PAD_SCREENSHOT);
    at(0, 5060); CHECK(out == 0 && !remap_deadline());
    memset(&m, 0, sizeof(m)); remap_reset();

    /* Tap and long press on one key: tap = south on a quick release, hold = L1. */
    memset(m.map, REMAP_OFF, sizeof(m.map));
    rule(0, RT_TAPPED, 9, 300, RE_TAP, PAD_SOUTH, 0);
    rule(1, RT_HELD_FOR, 9, 300, RE_HOLD, PAD_L1, 0);
    at(PAD_R4, 6000); CHECK(out == 0 && remap_deadline() == 6300);
    at(0, 6100); CHECK(out == PAD_SOUTH);
    at(0, 6160); CHECK(out == 0);
    at(PAD_R4, 7000); at(PAD_R4, 7299); CHECK(out == 0);
    at(PAD_R4, 7300); CHECK(out == PAD_L1);
    at(0, 7500); CHECK(out == 0);
    memset(&m, 0, sizeof(m)); remap_reset();

    /* Toggle hold, toggle turbo. */
    memset(m.map, REMAP_OFF, sizeof(m.map));
    rule(0, RT_PRESSED, 13, 0, RE_TOGGLE_HOLD, PAD_UP, 0);
    rule(1, RT_PRESSED, 8, 0, RE_TOGGLE_TURBO, PAD_SOUTH, 10);
    at(PAD_L3, 8000); CHECK(out == PAD_UP);
    at(0, 8100); CHECK(out == PAD_UP);
    at(PAD_L3, 8200); CHECK(out == 0);
    at(0, 8300); CHECK(out == 0);
    at(PAD_L4, 9000); CHECK(out == PAD_SOUTH);
    at(0, 9010); at(0, 9050); CHECK(out == 0);
    at(0, 9100); CHECK(out == PAD_SOUTH);
    at(PAD_L4, 9120); at(0, 9200); CHECK(out == 0 && !remap_deadline());
    memset(&m, 0, sizeof(m)); remap_reset();

    /* Double tap within 300 ms: on the second press; a third does not count. */
    memset(m.map, REMAP_OFF, sizeof(m.map));
    rule(0, RT_DOUBLE, 5, 300, RE_TAP, PAD_HOME, 0);
    at(PAD_M, 10000); at(0, 10050); CHECK(out == 0);
    at(PAD_M, 10200); CHECK(out == PAD_HOME);
    at(0, 10260); at(PAD_M, 10300); CHECK(out == 0);
    at(0, 10350); at(PAD_M, 10400); CHECK(out == PAD_HOME); /* after a plain press again */
    at(0, 11000); at(PAD_M, 11400); at(0, 11450); CHECK(out == 0); /* too slow */
    memset(&m, 0, sizeof(m)); remap_reset();

    /* Turbo modifier: while R4 is held, south and east (here east sends
     * west) pulse at 10 Hz from the later of their press and R4's; north is
     * not chosen. */
    m.map[9] = REMAP_OFF; m.map[1] = 3 + 1;
    rule(0, RT_HELD, 9, 0, RE_TURBO_MODIFIER, PAD_SOUTH | PAD_EAST, 10);
    rule(1, RT_HELD, 0, 0, RE_HOLD, PAD_L1, 0);     /* south's hold rule pulses with it */
    at(PAD_SOUTH | PAD_NORTH, 13000); CHECK(out == (PAD_SOUTH | PAD_L1 | PAD_NORTH));
    at(PAD_SOUTH | PAD_NORTH | PAD_R4, 13100);
    CHECK(out == (PAD_SOUTH | PAD_L1 | PAD_NORTH) && remap_deadline() == 13150);
    at(PAD_SOUTH | PAD_NORTH | PAD_R4, 13150); CHECK(out == PAD_NORTH);
    at(PAD_SOUTH | PAD_EAST | PAD_NORTH | PAD_R4, 13170);
    CHECK(out == (PAD_WEST | PAD_NORTH));                       /* east's own phase */
    at(PAD_SOUTH | PAD_EAST | PAD_NORTH | PAD_R4, 13200);
    CHECK(out == (PAD_SOUTH | PAD_L1 | PAD_WEST | PAD_NORTH));
    at(PAD_SOUTH | PAD_EAST | PAD_NORTH | PAD_R4, 13220); CHECK(out == (PAD_SOUTH | PAD_L1 | PAD_NORTH));
    at(PAD_SOUTH | PAD_EAST | PAD_NORTH, 13230);
    CHECK(out == (PAD_SOUTH | PAD_L1 | PAD_WEST | PAD_NORTH));  /* off: held again */
    at(PAD_R4, 13240); CHECK(out == 0 && !remap_deadline());
    /* At max the report builder flips them; toggled on by a press. */
    rule(0, RT_PRESSED, 9, 0, RE_TOGGLE_TURBO_MODIFIER, PAD_SOUTH, 0);
    at(0, 13300); at(PAD_R4, 13310); at(0, 13320);
    at(PAD_SOUTH, 13330); CHECK(out == 0 && every == (PAD_SOUTH | PAD_L1));
    at(PAD_SOUTH | PAD_R4, 13340); at(PAD_SOUTH, 13350); CHECK(out == (PAD_SOUTH | PAD_L1) && !every);
    memset(&m, 0, sizeof(m)); remap_reset();

    /* A held key counts as pressed after a reset. */
    memset(m.map, REMAP_OFF, sizeof(m.map));
    rule(0, RT_PRESSED, 0, 0, RE_TAP, PAD_EAST, 0);
    at(PAD_SOUTH, 12000); remap_reset(); at(PAD_SOUTH, 12001); CHECK(out == PAD_EAST);
    /* Clock wrap. */
    memset(&m, 0, sizeof(m)); remap_reset();
    rule(0, RT_HELD, 0, 0, RE_TURBO, PAD_SOUTH, 10);
    m.map[0] = REMAP_OFF;
    t = 0xffffffe0u;
    at(PAD_SOUTH, t); CHECK(out == PAD_SOUTH);
    at(PAD_SOUTH, t + 50); CHECK(out == 0 && remap_deadline() == t + 100);
    return TEST_DONE("remap");
}
#endif

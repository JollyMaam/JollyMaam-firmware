#include <string.h>
#include "pad.h"

/* A remap slot on the buttons ux_step leaves, so the Home gestures keep
 * working on the physical keys.  Each key's map entry is its base row, "held:
 * hold KEY" (itself by default) or nothing; its rules add to that.  Rules
 * read inputs, never
 * each other's outputs, and their outputs OR together.  Nothing waits to see
 * what comes next: held for fires at its time, tapped on a quick release,
 * double tapped on the second press.  Timed output follows the clock and never
 * catches up after a stall. */

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

static int level(u8 effect)
{
    return effect == RE_HOLD || effect == RE_TURBO || effect == RE_NOTHING;
}

u32 remap_step(const struct set_remap *m, u32 in, u32 now, u32 *every_report)
{
    u32 pressed = in & ~rm.prev, released = rm.prev & ~in, out = 0, max = 0, doubled = 0, i;

    rm.next = 0;
    for (i = 0; i < REMAP_INPUTS; i++)
        if (pressed & (1u << i)) rm.down_at[i] = now;
    for (i = 0; i < REMAP_INPUTS; i++) {
        u8 to = m->map[i];

        if ((in & (1u << i)) && to != REMAP_OFF) out |= 1u << (to ? to - 1u : i);
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
        if (!(level(r->effect) ? active : (rm.latched & bit) != 0)) continue;
        if (r->effect == RE_HOLD || r->effect == RE_TOGGLE_HOLD) out |= o;
        else if (r->effect == RE_TURBO || r->effect == RE_TOGGLE_TURBO) {
            if (!r->rate) max |= o;
            else if (turbo_on(r->rate, rm.since[i], now)) out |= o;
        }
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
    at(PAD_A | PAD_UP, 1000); CHECK(out == (PAD_A | PAD_UP) && !every && !remap_deadline());
    m.map[0] = 1 + 1; m.map[1] = 0 + 1; m.map[2] = REMAP_OFF;
    at(PAD_A | PAD_SCREENSHOT, 1001); CHECK(out == PAD_B);
    at(PAD_A | PAD_B, 1002); CHECK(out == (PAD_A | PAD_B));
    /* Mapped onto opposite d-pad directions: they cancel. */
    m.map[3] = 16 + 1; m.map[4] = 18 + 1;
    at(PAD_X | PAD_Y, 1003); CHECK(out == 0);
    at(PAD_X, 1004); CHECK(out == PAD_UP);
    memset(&m, 0, sizeof(m)); remap_reset();

    /* Rules add to the base row; a chord from one key; base off: only the rules. */
    rule(0, RT_HELD, 9, 0, RE_HOLD, PAD_L1 | PAD_R1, 0);
    at(PAD_R4 | PAD_A, 2000); CHECK(out == (PAD_L1 | PAD_R1 | PAD_R4 | PAD_A));
    at(PAD_A, 2001); CHECK(out == PAD_A);
    m.map[9] = 0 + 1;
    at(PAD_R4, 2002); CHECK(out == (PAD_L1 | PAD_R1 | PAD_A));
    m.map[9] = REMAP_OFF;
    at(PAD_R4, 2003); CHECK(out == (PAD_L1 | PAD_R1));
    m.map[9] = 0;
    rule(0, RT_HELD, 2, 0, RE_NOTHING, 0, 0);       /* adds nothing; the base row stays */
    at(PAD_SCREENSHOT, 2004); CHECK(out == PAD_SCREENSHOT);
    memset(&m, 0, sizeof(m)); remap_reset();

    /* Turbo at 10 Hz: 50 ms down, 50 ms up, from the press; held keeps A too. */
    rule(0, RT_HELD, 0, 0, RE_TURBO, PAD_A, 10);
    rule(1, RT_HELD, 0, 0, RE_HOLD, PAD_B, 0);
    m.map[0] = REMAP_OFF;
    at(PAD_A, 3000); CHECK(out == (PAD_A | PAD_B) && remap_deadline() == 3050);
    at(PAD_A, 3049); CHECK(out == (PAD_A | PAD_B));
    at(PAD_A, 3050); CHECK(out == PAD_B && remap_deadline() == 3100);
    at(PAD_A, 3100); CHECK(out == (PAD_A | PAD_B));
    at(PAD_A, 3420); CHECK(out == (PAD_A | PAD_B)); /* a stall: the phase from the clock */
    at(0, 3421); CHECK(out == 0 && !remap_deadline());
    /* Rate 0: the report builder flips it. */
    rule(0, RT_HELD, 0, 0, RE_TURBO, PAD_A, 0);
    at(PAD_A, 3500); CHECK(out == PAD_B && every == PAD_A && !remap_deadline());
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

    /* Tap and long press on one key: tap = A on a quick release, hold = L1. */
    memset(m.map, REMAP_OFF, sizeof(m.map));
    rule(0, RT_TAPPED, 9, 300, RE_TAP, PAD_A, 0);
    rule(1, RT_HELD_FOR, 9, 300, RE_HOLD, PAD_L1, 0);
    at(PAD_R4, 6000); CHECK(out == 0 && remap_deadline() == 6300);
    at(0, 6100); CHECK(out == PAD_A);
    at(0, 6160); CHECK(out == 0);
    at(PAD_R4, 7000); at(PAD_R4, 7299); CHECK(out == 0);
    at(PAD_R4, 7300); CHECK(out == PAD_L1);
    at(0, 7500); CHECK(out == 0);
    memset(&m, 0, sizeof(m)); remap_reset();

    /* Toggle hold, toggle turbo. */
    memset(m.map, REMAP_OFF, sizeof(m.map));
    rule(0, RT_PRESSED, 13, 0, RE_TOGGLE_HOLD, PAD_UP, 0);
    rule(1, RT_PRESSED, 8, 0, RE_TOGGLE_TURBO, PAD_A, 10);
    at(PAD_L3, 8000); CHECK(out == PAD_UP);
    at(0, 8100); CHECK(out == PAD_UP);
    at(PAD_L3, 8200); CHECK(out == 0);
    at(0, 8300); CHECK(out == 0);
    at(PAD_L4, 9000); CHECK(out == PAD_A);
    at(0, 9010); at(0, 9050); CHECK(out == 0);
    at(0, 9100); CHECK(out == PAD_A);
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

    /* A held key counts as pressed after a reset. */
    memset(m.map, REMAP_OFF, sizeof(m.map));
    rule(0, RT_PRESSED, 0, 0, RE_TAP, PAD_B, 0);
    at(PAD_A, 12000); remap_reset(); at(PAD_A, 12001); CHECK(out == PAD_B);
    /* Clock wrap. */
    memset(&m, 0, sizeof(m)); remap_reset();
    rule(0, RT_HELD, 0, 0, RE_TURBO, PAD_A, 10);
    m.map[0] = REMAP_OFF;
    t = 0xffffffe0u;
    at(PAD_A, t); CHECK(out == PAD_A);
    at(PAD_A, t + 50); CHECK(out == 0 && remap_deadline() == t + 100);
    return TEST_DONE("remap");
}
#endif

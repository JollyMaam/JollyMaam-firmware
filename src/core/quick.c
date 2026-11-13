#include <string.h>
#include "pad.h"

/* Quick settings' layer over the remap (docs/firmware/remapping.md): per key
 * a turbo step, a mapping or the pad's one macro, all in RAM, never in
 * flash.  It lasts as a picked remap does: the slot's own reconnects keep
 * it; another slot, a disconnect, sleep or holding M in quick settings clear
 * it (quick_clear).
 *
 * A recording takes the keys as ux_step leaves them, the remap suspended so
 * the host sees what will play: every key but M, whose press ends it, and
 * the triggers as pressed (in_triggers), which play all the way down.  Keys
 * held when it starts count from their next press.  A recording that is a
 * single chord (every press within QUICK_CHORD_MS, none released before the
 * last) becomes a mapping: the key sends those keys while held.  Nothing
 * pressed clears the key.  A stop by the timeout, or the remap stopping
 * (quick settings, a disconnect, calibration), discards it.
 *
 * A macro plays after the remap, so it triggers no rule and no Home gesture;
 * while it runs, the keys it uses are its own (the d-pad as one).  Its key's
 * press starts it from the beginning; held when a run ends, the key starts
 * the next.  A looping macro's key starts or stops the loop.  Steps follow
 * each other from when the last one went out: a stall moves the rest later
 * rather than squeezing them together.
 *
 * Steps are two bytes: the wait since the previous one in QUICK_UNIT_MS,
 * then QUICK_PRESS | key, key alone for a release, or QUICK_WAIT_ONLY. */

#define QUICK_UNIT_MS     4
#define QUICK_PRESS       0x20
#define QUICK_WAIT_ONLY   0xff
#define QUICK_CHORD_MS    50        /* presses this close together are one chord */
#define QUICK_MIN_PRESS   40        /* a recorded press lasts at least this: hosts poll every 7.5-15 ms */
#define QUICK_KEYS        (PAD_DIGITAL & ~(PAD_HOME | PAD_M))   /* keys that take a turbo, a mapping or the macro */
#ifndef QUICK_MACRO_BYTES
#define QUICK_MACRO_BYTES 254       /* the most a macro holds; a lent buffer may give less */
#endif

enum { IDLE, RECORDING, PLAYING };

static struct remap_quick keys;
static struct {
    u8 *buf;                     /* the macro's steps; NULL: none (a lent buffer is given back) */
    u8 len, room, pos;
    u8 loop;                     /* the macro loops: its key starts and stops it */
    u8 mode;
    u8 rec_key;                  /* recording for this key */
    u8 eat_m;                    /* M ended a recording: kept from the host until released */
    u8 signal;
    u16 signal_at;               /* plat_ms, low bits */
    u8 turbo_key;                /* the last turbo step's key + 1: shown QUICK_TURBO_SHOW_MS */
    u16 turbo_at;                /* plat_ms, low bits */
    u32 uses;                    /* the keys the macro presses */
    u32 prev;                    /* the keys last pass */
    u32 at;                      /* recording: the last step's time; playing: when the next is due */
    u32 held;                    /* recording: keys pressed since it began, still down; playing: the macro's */
} q;

#ifndef QUICK_LENT_BUFFER
static u8 own[QUICK_MACRO_BYTES];
#endif

static int take_buffer(void)
{
#ifdef QUICK_LENT_BUFFER         /* the tool device's: its commands are refused while held */
    u32 n;

    if (!q.buf && (q.buf = plat_tool_lend(&n)) != NULL) q.room = (u8)(n < QUICK_MACRO_BYTES ? n : QUICK_MACRO_BYTES);
#else
    q.buf = own;
    q.room = QUICK_MACRO_BYTES;
#endif
    q.room &= (u8)~1u;
    return q.buf != NULL;
}

static void drop_macro(void)
{
#ifdef QUICK_LENT_BUFFER
    if (q.buf) plat_tool_return();
#endif
    q.buf = NULL;
    q.len = q.pos = 0;
    q.mode = IDLE;
    q.held = q.uses = 0;
    keys.macro = 0;
}

static u32 bit(u8 key)
{
    return 1u << key;
}

static void drop_map(u8 key)
{
    u32 i;

    for (i = 0; i < QUICK_MAPS; i++)
        if (keys.map[i] >> 24 == key + 1u) keys.map[i] = 0;
}

/* Nothing on key: no turbo, mapping or macro. */
static void plain(u8 key)
{
    keys.slow &= ~bit(key);
    keys.fast &= ~bit(key);
    drop_map(key);
    if (keys.macro == key + 1u) drop_macro();
}

const struct remap_quick *quick_keys(void)
{
    return &keys;
}

void quick_signal(u8 sig, u32 now)
{
    q.signal = sig;
    q.signal_at = (u16)now;
}

void quick_clear(void)
{
    drop_macro();
    memset(&keys, 0, sizeof(keys));
    q.eat_m = q.turbo_key = 0;
}

/* The key's next turbo step (quick_led shows it, its rate or dark for off,
 * while the key is held and for QUICK_TURBO_SHOW_MS from the step); a key holding the macro: cleared
 * first. */
void quick_turbo(u8 key, u32 now)
{
    u32 b = bit(key);

    if (key >= 32 || !(QUICK_KEYS & b)) return;
    q.turbo_key = (u8)(key + 1);
    q.turbo_at = (u16)now;
    if (keys.macro == key + 1u) {
        drop_macro();
    } else if (keys.slow & b) {
        keys.slow &= ~b;
        keys.fast |= b;
    } else if (keys.fast & b) {
        keys.fast &= ~b;
    } else {
        keys.slow |= b;
    }
}

void quick_record(u8 key, int loop, u32 now)
{
    if (key >= 32 || !(QUICK_KEYS & bit(key))) return;
    drop_macro();                /* one buffer: the old macro goes, whatever this becomes */
    q.turbo_key = 0;
    if (!take_buffer()) {
        quick_signal(SIG_DISCARDED, now);
        return;
    }
    q.mode = RECORDING;
    q.rec_key = key;
    q.loop = (u8)loop;
    q.len = 0;
    q.held = 0;
    q.at = now;
    q.signal = SIG_NONE;
}

int quick_recording(void)
{
    return q.mode == RECORDING;
}

/* Room for a step and, after it, the releases of everything held and the
 * closing pause. */
static int fits(u32 held)
{
    u32 n = 0;

    for (; held; held &= held - 1) n++;
    return q.len + 4u + 2u * n <= q.room;
}

/* A step after the wait from q.at to at (waits over a byte go first, as wait
 * steps); 0: no room, nothing added. */
static int put(u32 at, u8 action, u32 held_after)
{
    u32 units = (s32)(at - q.at) > 0 && q.len ? (at - q.at) / QUICK_UNIT_MS : 0;

    if (!q.len) q.at = at;       /* the first step: no silence before it */
    while (units >= 0xff) {
        if (!fits(held_after | q.held)) return 0;
        q.buf[q.len++] = 0xff;
        q.buf[q.len++] = QUICK_WAIT_ONLY;
        units -= 0xff;
        q.at += 0xffu * QUICK_UNIT_MS;
    }
    if (action != QUICK_WAIT_ONLY || units) {
        if (q.len + 2u > q.room) return 0;
        q.buf[q.len++] = (u8)units;
        q.buf[q.len++] = action;
    }
    q.at += units * QUICK_UNIT_MS;
    return 1;
}

/* How long ago key was pressed. */
static u32 pressed_for(u8 key, u32 now)
{
    u32 ms = (s32)(now - q.at) > 0 ? now - q.at : 0, i = q.len;

    while (i >= 2) {
        i -= 2;
        if (q.buf[i + 1] == (QUICK_PRESS | key)) return ms;
        ms += q.buf[i] * QUICK_UNIT_MS;
    }
    return ms;
}

/* A single chord: the keys it presses, 0 if it is not one. */
static u32 chord(void)
{
    u32 i, ms = 0, pressed = 0, released = 0;

    for (i = 0; i < q.len; i += 2) {
        u8 a = q.buf[i + 1];

        if (pressed) ms += q.buf[i] * QUICK_UNIT_MS;
        if (a == QUICK_WAIT_ONLY) continue;
        if (a & QUICK_PRESS) {
            if (released || (pressed & bit(a & 0x1f)) || ms > QUICK_CHORD_MS) return 0;
            pressed |= bit(a & 0x1f);
        } else {
            released |= bit(a);
        }
    }
    return pressed;
}

/* Recording over at now: saved as a macro or a mapping, or the key cleared. */
static void finish(u32 now)
{
    u8 key = q.rec_key;
    u32 i, held = q.held, map;

    for (i = 0; held; i++, held >>= 1)
        if (held & 1) put(now, (u8)i, 0);
    q.held = 0;
    put(now, QUICK_WAIT_ONLY, 0);          /* the pause before the stop: the gap between runs */
    q.mode = IDLE;
    q.uses = 0;
    for (i = 0; i < q.len; i += 2)
        if (q.buf[i + 1] != QUICK_WAIT_ONLY) q.uses |= bit(q.buf[i + 1] & 0x1f);
    map = q.loop ? 0 : chord();
    plain(key);
    if (!q.uses) {
        drop_macro();
        quick_signal(SIG_CLEARED, now);
    } else if (map) {
        drop_macro();
        for (i = 0; i < QUICK_MAPS && keys.map[i]; i++) {}
        if (i == QUICK_MAPS) {             /* full: the oldest goes */
            memmove(keys.map, keys.map + 1, sizeof(keys.map) - sizeof(keys.map[0]));
            i = QUICK_MAPS - 1;
        }
        keys.map[i] = map | (u32)(key + 1u) << 24;
        quick_signal(SIG_SAVED_MAP, now);
    } else {
        keys.macro = (u8)(key + 1u);
        q.pos = 0;
        quick_signal(q.loop ? SIG_SAVED_LOOP : SIG_SAVED, now);
    }
}

static void discard(u32 now)
{
    drop_macro();
    quick_signal(SIG_DISCARDED, now);
}

u32 quick_record_step(u32 in, u32 now)
{
    u32 pressed = in & ~q.prev, released = q.prev & ~in, i;

    q.prev = in;
    if (q.mode != RECORDING) return in;
    if (pressed & PAD_M) {
        finish(now);
        q.eat_m = 1;
        return 0;
    }
    in &= ~PAD_M;
    pressed &= QUICK_KEYS | PAD_HOME | PAD_L2 | PAD_R2;   /* the triggers from their press points */
    released &= q.held;
    for (i = 0; i < 32 && (pressed | released); i++) {
        u32 b = bit((u8)i);

        if (released & b) {
            u32 ms = pressed_for((u8)i, now);

            /* Too short for a host to see: the release goes later. */
            if (!put(now + (ms < QUICK_MIN_PRESS ? QUICK_MIN_PRESS - ms : 0), (u8)i, q.held & ~b)) break;
            q.held &= ~b;
            released &= ~b;
        }
        if (pressed & b) {
            if (!fits(q.held | b) || !put(now, (u8)(QUICK_PRESS | i), q.held | b)) {
                finish(now);             /* full: what fits is the macro */
                return in;
            }
            q.held |= b;
            pressed &= ~b;
        }
    }
    if ((s32)(now - q.at) >= (s32)ux_timeouts.quick_ms && !q.held) discard(now);
    return in;
}

/* The keys a macro using these takes from the host: the d-pad as one. */
static u32 owned(u32 uses)
{
    return uses & PAD_DPAD ? uses | PAD_DPAD : uses;
}

static void start(u32 now)
{
    q.mode = PLAYING;
    q.pos = 0;
    q.held = 0;
    q.at = now + q.buf[0] * QUICK_UNIT_MS;
}

u32 quick_play(u32 in, u32 out, u32 now, u32 *every)
{
    u32 pressed = in & ~q.prev, k = keys.macro ? bit((u8)(keys.macro - 1)) : 0, steps = 0;

    q.prev = in;
    if (q.eat_m) {
        if (in & PAD_M) out &= ~PAD_M;
        else q.eat_m = 0;
    }
    if (pressed & k) {
        if (q.loop && q.mode == PLAYING) q.mode = IDLE;
        else start(now);
    }
    while (q.mode == PLAYING && (s32)(now - q.at) >= 0) {
        u8 a;

        if (q.pos >= q.len) {    /* a run over: again while its key is held, or looping */
            q.held = 0;
            if (!(q.loop || (in & k)) || steps) {
                if (!(q.loop || (in & k))) q.mode = IDLE;
                break;           /* no run of no time in one pass */
            }
            start(now);
            continue;
        }
        a = q.buf[q.pos + 1];
        if (a != QUICK_WAIT_ONLY) {
            if (a & QUICK_PRESS) q.held |= bit(a & 0x1f);
            else q.held &= ~bit(a);
        }
        q.pos = (u8)(q.pos + 2);
        steps++;
        /* The next from now, not from when this was due: no catching up. */
        q.at = now + (q.pos < q.len ? q.buf[q.pos] * QUICK_UNIT_MS : 0);
    }
    if (q.mode != PLAYING) return out;
    *every &= ~owned(q.uses);
    return in_dpad((out & ~owned(q.uses)) | q.held);
}

void quick_stop(u32 now)
{
    if (q.mode == RECORDING) discard(now);
    if (q.mode == PLAYING) q.mode = IDLE;
    q.held = 0;
    q.prev = 0;
}

u32 quick_deadline(void)
{
    if (q.mode == PLAYING) return q.at ? q.at : 1;
    if (q.mode == RECORDING) return q.at + ux_timeouts.quick_ms;
    return 0;
}

/* LED feedback: [off, on] ms, pulses. */
static const struct { u16 off, on; u8 n; } signals[] = {
    [SIG_REMAP_ON] = { 150, 500, 1 },
    [SIG_REMAP_OFF] = { 100, 100, 2 },
    [SIG_CLEARED] = { 50, 50, 6 },
    [SIG_SAVED] = { 150, 300, 2 },
    [SIG_SAVED_LOOP] = { 150, 300, 3 },
    [SIG_SAVED_MAP] = { 150, 700, 1 },
    [SIG_DISCARDED] = { 300, 80, 3 },
};
#define RECORD_BLINK_MS 100
#define QUICK_TURBO_SHOW_MS 400 /* a whole number of either rate's blinks */
#define TURBO_OFF_POLL_MS 50     /* off, held past it: till its release */
#define LOOP_BLINK_MS   500

u8 quick_led(u32 now, u32 *next, u32 held)
{
    u32 t = (u16)((u16)now - q.turbo_at), turbo, step = q.turbo_key ? bit(q.turbo_key - 1u) : 0;

    if (step && t >= QUICK_TURBO_SHOW_MS && !(held & step)) q.turbo_key = (u8)(step = 0);
    held |= step;
    turbo = held & (keys.slow | keys.fast);

    *next = 0;
    if (q.signal) {
        u32 t = (u16)((u16)now - q.signal_at), period = signals[q.signal].off + signals[q.signal].on;
        u32 phase = t % period;

        if (t / period < signals[q.signal].n) {
            *next = now - phase + (phase < signals[q.signal].off ? signals[q.signal].off : period);
            return phase < signals[q.signal].off ? LED_SIGNAL_DARK : LED_SIGNAL_LIT;
        }
        q.signal = SIG_NONE;
    }
    if (turbo) {                 /* quick settings: a held or just stepped key's turbo, at its rate */
        u32 half = 500u / (turbo & keys.slow ? QUICK_SLOW_HZ : QUICK_FAST_HZ), phase = t % (2 * half);

        *next = now - phase + (phase < half ? half : 2 * half);
        return phase < half ? LED_SIGNAL_LIT : LED_SIGNAL_DARK;
    }
    if (step) {                  /* stepped to off: dark */
        *next = t < QUICK_TURBO_SHOW_MS ? now + QUICK_TURBO_SHOW_MS - t : now + TURBO_OFF_POLL_MS;
        return LED_SIGNAL_DARK;
    }
    if (q.mode == RECORDING || (q.mode == PLAYING && q.loop)) {
        u32 half = q.mode == RECORDING ? RECORD_BLINK_MS : LOOP_BLINK_MS, phase = now % (2 * half);

        *next = now - phase + (phase < half ? half : 2 * half);
        return phase < half ? LED_SIGNAL_DARK : LED_SIGNAL_LIT;
    }
    return LED_SIGNAL_NONE;
}

#ifdef UNITTEST
const struct ux_timeouts ux_timeouts = { .quick_ms = 30000 };
#ifdef QUICK_LENT_BUFFER
static u8 lent[360];
static int lend_ok = 1, lent_out;
u8 *plat_tool_lend(u32 *size) { if (!lend_ok || lent_out) return NULL; lent_out = 1; *size = sizeof(lent); return lent; }
void plat_tool_return(void) { lent_out = 0; }
#endif

static u32 now, every;

static u32 rec(u32 in, u32 ms)
{
    u32 out = 0;

    while (ms--) out = quick_record_step(in, ++now);
    return out;
}

static u32 play(u32 in, u32 out, u32 ms)
{
    u32 o = 0;

    while (ms--) { every = 0; o = quick_play(in, out, ++now, &every); }
    return o;
}

static u32 map_of(u8 key)
{
    u32 i;

    for (i = 0; i < QUICK_MAPS; i++)
        if (keys.map[i] >> 24 == key + 1u) return keys.map[i] & 0xffffff;
    return 0;
}

int main(void)
{
    u32 t, next, o;

    /* Turbo steps: slow, fast, none. */
    now = 1000;
    quick_turbo(IN_L4, 1000); CHECK(keys.slow == PAD_L4 && !keys.fast);
    CHECK(quick_led(1000, &next, PAD_L4) == LED_SIGNAL_LIT && next == 1050);     /* 10 Hz, from the step */
    CHECK(quick_led(1060, &next, 0) == LED_SIGNAL_DARK && next == 1100);         /* released: still shown */
    CHECK(quick_led(1399, &next, 0) == LED_SIGNAL_DARK && next == 1400);
    CHECK(quick_led(1400, &next, 0) == LED_SIGNAL_NONE && !next);                /* 400 ms: four blinks */
    CHECK(quick_led(1400, &next, PAD_L4) == LED_SIGNAL_LIT);                    /* held: as long as it is */
    quick_turbo(IN_L4, 2000); CHECK(!keys.slow && keys.fast == PAD_L4);
    CHECK(quick_led(2000, &next, 0) == LED_SIGNAL_LIT && next == 2025);
    quick_turbo(IN_L4, 3000); CHECK(!keys.slow && !keys.fast);
    CHECK(quick_led(3000, &next, 0) == LED_SIGNAL_DARK && next == 3400);          /* off: dark */
    CHECK(quick_led(3500, &next, PAD_L4) == LED_SIGNAL_DARK && next == 3550);     /* held: still */
    CHECK(quick_led(3600, &next, 0) == LED_SIGNAL_NONE && !next);
    quick_turbo(IN_HOME, 3000); quick_turbo(IN_M, 3000); CHECK(!keys.slow);

    /* A tap of south, recorded: a mapping, not a macro; the buffer given back. */
    quick_turbo(IN_R4, now);
    quick_record(IN_R4, 0, now); CHECK(quick_recording() && quick_led(now, &next, 0) == LED_SIGNAL_DARK);
    CHECK(rec(0, 500) == 0);
    CHECK(rec(PAD_SOUTH, 80) == PAD_SOUTH);              /* the host sees what is pressed */
    rec(0, 100);
    CHECK(rec(PAD_M, 1) == 0 && !quick_recording());
    CHECK(map_of(IN_R4) == PAD_SOUTH && !keys.macro && !q.buf && !(keys.slow & PAD_R4));
    CHECK(q.signal == SIG_SAVED_MAP);
    CHECK(play(PAD_M, PAD_M, 10) == 0);                   /* the stopping M is not the host's */
    CHECK(play(0, 0, 1) == 0 && play(PAD_M, PAD_M, 1) == PAD_M);
    play(0, 0, 1);

    /* A chord, pressed 30 ms apart: one mapping. */
    quick_record(IN_L4, 0, now);
    rec(PAD_L1, 30); rec(PAD_L1 | PAD_EAST, 200); rec(PAD_EAST, 20); rec(0, 20); rec(PAD_M, 1);
    CHECK(map_of(IN_L4) == (PAD_L1 | PAD_EAST) && map_of(IN_R4) == PAD_SOUTH);

    /* Hold L1, then press A after 200 ms: a macro.  Its key starts it; it
     * owns its keys; a run ends with everything released. */
    quick_record(IN_R4, 0, now);
    rec(0, 20); rec(PAD_L1, 200); rec(PAD_L1 | PAD_SOUTH, 100); rec(PAD_L1, 100); rec(0, 100);
    t = now;
    rec(PAD_M, 1);
    CHECK(keys.macro == IN_R4 + 1 && !map_of(IN_R4) && q.signal == SIG_SAVED);
    CHECK(q.uses == (PAD_L1 | PAD_SOUTH) && q.len == 10);   /* 4 edges and the trailing pause */
    (void)t;
    play(0, 0, 10);
    o = play(PAD_R4, PAD_L1 | PAD_WEST, 1); CHECK(o == (PAD_L1 | PAD_WEST));   /* the host's L1 is the macro's */
    o = play(PAD_R4, PAD_WEST, 1); CHECK(o == (PAD_L1 | PAD_WEST));
    o = play(0, 0, 150); CHECK(o == PAD_L1);              /* released: the run goes on */
    o = play(0, 0, 100); CHECK(o == (PAD_L1 | PAD_SOUTH));
    o = play(0, PAD_SOUTH, 100); CHECK(o == PAD_L1);
    o = play(0, PAD_SOUTH, 200); CHECK(o == PAD_SOUTH);  /* over: the host's own again */
    CHECK(!quick_deadline() && q.mode == IDLE);
    /* Held through the end: the next run.  A press starts afresh. */
    o = play(PAD_R4, 0, 520); CHECK(q.mode == PLAYING);
    o = play(PAD_R4, 0, 200); CHECK(q.mode == PLAYING && (o & PAD_L1));
    play(0, 0, 50); play(PAD_R4, 0, 1); CHECK(q.pos == 2);
    play(0, 0, 1000); CHECK(q.mode == IDLE);

    /* A stall: the steps after it keep their spacing. */
    play(PAD_R4, 0, 1); play(0, 0, 1);
    now += 400; every = 0; o = quick_play(0, 0, now, &every);
    CHECK(o == (PAD_L1 | PAD_SOUTH) && q.mode == PLAYING);  /* south late; its release 100 ms after */
    o = play(0, 0, 99); CHECK(o == (PAD_L1 | PAD_SOUTH));
    o = play(0, 0, 1); CHECK(o == PAD_L1);
    quick_stop(now); CHECK(q.mode == IDLE && keys.macro);

    /* Looping: the key starts and stops it. */
    quick_record(IN_L3, 1, now);
    rec(PAD_WEST, 60); rec(0, 40); rec(PAD_M, 1);
    CHECK(keys.macro == IN_L3 + 1 && q.loop && q.signal == SIG_SAVED_LOOP);
    play(PAD_L3, 0, 1); play(0, 0, 500); CHECK(q.mode == PLAYING);
    CHECK(quick_led(now, &next, 0) != LED_SIGNAL_NONE);
    play(PAD_L3, 0, 1); CHECK(q.mode == IDLE && play(0, 0, 1) == 0);

    /* The triggers' presses are recorded like keys. */
    quick_record(IN_L3, 0, now);
    rec(PAD_R2, 60); rec(0, 60); rec(PAD_L2, 60); rec(0, 60); rec(PAD_M, 1);
    CHECK(keys.macro == IN_L3 + 1 && q.uses == (PAD_L2 | PAD_R2));
    play(0, 0, 1); play(PAD_L3, 0, 1); o = play(0, 0, 10); CHECK(o == PAD_R2);
    play(0, 0, 500);

    /* Short presses go out for QUICK_MIN_PRESS. */
    quick_record(IN_L3, 0, now);
    rec(PAD_NORTH, 8); rec(0, 100); rec(PAD_EAST, 60); rec(0, 50); rec(PAD_M, 1);
    CHECK(keys.macro == IN_L3 + 1);
    play(0, 0, 1); play(PAD_L3, 0, 1);
    o = play(0, 0, 30); CHECK(o == PAD_NORTH);
    o = play(0, 0, 15); CHECK(o == 0);
    o = play(0, 0, 70); CHECK(o == PAD_EAST);
    play(0, 0, 500);

    /* Nothing pressed clears the key; a timeout or the remap stopping discards. */
    quick_record(IN_L3, 0, now); rec(0, 100); rec(PAD_M, 1);
    CHECK(!keys.macro && q.signal == SIG_CLEARED);
    quick_record(IN_L3, 0, now); rec(PAD_NORTH, 50); rec(0, 30001);
    CHECK(!quick_recording() && !keys.macro && q.signal == SIG_DISCARDED);
    quick_record(IN_L3, 0, now); rec(PAD_NORTH, 50); quick_stop(now);
    CHECK(!quick_recording() && !keys.macro && q.signal == SIG_DISCARDED);
    /* Keys held at the start count from their next press. */
    rec(PAD_EAST, 10); quick_record(IN_L3, 0, now); rec(PAD_EAST, 100); rec(0, 10); rec(PAD_M, 1);
    CHECK(!keys.macro && q.signal == SIG_CLEARED);

    /* Full: what fits is the macro, everything released. */
    quick_record(IN_L3, 0, now);
    for (t = 0; t < 200 && quick_recording(); t++) { rec(PAD_SOUTH, 50); rec(0, 50); }
    CHECK(!quick_recording() && keys.macro && q.len <= q.room);
    CHECK(q.buf[q.len - 1] == QUICK_WAIT_ONLY || !(q.buf[q.len - 1] & QUICK_PRESS));

    /* The oldest mapping goes when they are all taken; clearing ends it all. */
    for (t = 0; t < QUICK_MAPS + 1; t++) {
        quick_record((u8)(IN_SOUTH + t), 0, now); rec(PAD_UP, 60); rec(0, 10); rec(PAD_M, 1); play(0, 0, 1);
    }
    CHECK(!map_of(IN_SOUTH) && map_of((u8)(IN_SOUTH + QUICK_MAPS)) == PAD_UP);
    quick_clear(); CHECK(!keys.macro && !map_of((u8)(IN_SOUTH + QUICK_MAPS)) && !q.buf);

    /* Signals: dark then lit pulses, then nothing. */
    quick_signal(SIG_REMAP_OFF, 5000);
    CHECK(quick_led(5000, &next, 0) == LED_SIGNAL_DARK && next == 5100);
    CHECK(quick_led(5100, &next, 0) == LED_SIGNAL_LIT && next == 5200);
    CHECK(quick_led(5300, &next, 0) == LED_SIGNAL_LIT && next == 5400);
    CHECK(quick_led(5400, &next, 0) == LED_SIGNAL_NONE && !next);
#ifdef QUICK_LENT_BUFFER
    lend_ok = 0; quick_record(IN_L3, 0, now); CHECK(!quick_recording() && q.signal == SIG_DISCARDED);
#endif
    return TEST_DONE("quick");
}
#endif

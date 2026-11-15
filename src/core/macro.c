#include <string.h>
#include "pad.h"

/* The remap slot's saved macros (pad.h, settings.c's macros target), played
 * from flash a step at a time: nothing of a macro is in RAM, so a quick
 * settings macro (quick.c) keeps its buffer and both can play at once.
 *
 * One plays at a time; a new start replaces it.  A level rule (RE_MACRO)
 * starts it as it comes on and, still on when a run ends, starts the next;
 * going off lets the run finish.  RE_PLAY_MACRO plays one run from the
 * start.  RE_TOGGLE_MACRO starts it looping, or stops the loop at once.
 * While it plays it owns the keys it uses (the d-pad as one): the remap's
 * own sends of them are dropped.  Each step follows the previous one by
 * its wait from when that one went out: a stall moves the rest later. */

#define LOOPING 0x80              /* in p.n: the macro loops */

static struct {
    u8 n;                        /* the macro playing, 1-based, | LOOPING; 0 none */
    u8 key, to, pos;             /* its record, the end of its steps, the next step */
    u32 at;                      /* when the next step is due */
    u32 held, uses;              /* the keys it holds now, every key it presses */
} p;                             /* the 8BitDo Wireless has RAM for little more */

static u32 owned(u32 uses)
{
    return uses & PAD_DPAD ? uses | PAD_DPAD : uses;
}

static u8 lowest(u8 bits)
{
    u8 n = 1;

    while (!(bits & 1)) bits >>= 1, n++;
    return n;
}

/* The wait before the step at pos, from now (none past the last). */
static void due_from(u32 now)
{
    u8 w = 0;

    if (p.pos < p.to) plat_store_read_at(p.key, p.pos, &w, 1);
    p.at = now + (u32)w * MACRO_UNIT_MS;
}

/* Macro n (| LOOPING) from its start; not there: none plays. */
static void begin(u8 remap, u8 n, u32 now)
{
    u8 b[8];
    u16 key, from, to, o;
    u32 i, k;

    p.n = 0;
    p.held = p.uses = 0;
    if (!set_macro(remap, n & ~LOOPING, &key, &from, &to) || to > 0xff) return;
    for (o = from; o < to; o = (u16)(o + k)) {   /* the keys it uses */
        k = to - o < sizeof(b) ? to - o : sizeof(b);
        if (plat_store_read_at(key, o, b, (u16)k) != (int)k) return;
        for (i = 1; i < k; i += 2)
            if (b[i] != MACRO_WAIT_ONLY) p.uses |= 1u << (b[i] & ~MACRO_PRESS & 0x1f);
    }
    p.n = n;
    p.key = (u8)key;
    p.to = (u8)to;
    p.pos = (u8)from;
    due_from(now);
}

u32 macro_play(u8 remap, const struct remap_macros *r, u32 out, u32 now, u32 *every)
{
    u8 steps = 0;

    if (r->toggle) {
        u8 n = lowest(r->toggle);

        if (p.n == (n | LOOPING)) p.n = 0;
        else begin(remap, n | LOOPING, now);
    } else if (r->start) {
        begin(remap, lowest(r->start), now);
    }
    while (p.n && (s32)(now - p.at) >= 0) {
        u8 a;

        if (p.pos >= p.to) {     /* a run over: again while its rule is on, or looping */
            u8 n = p.n;
            int again = (n & LOOPING) || (r->held >> (n - 1) & 1);

            p.held = 0;
            if (!again) p.n = 0;
            if (!again || steps) break;   /* no run of no time in one pass */
            begin(remap, n, now);
            continue;
        }
        if (plat_store_read_at(p.key, (u16)(p.pos + 1), &a, 1) != 1) {
            p.n = 0;             /* gone from flash */
            break;
        }
        if (a != MACRO_WAIT_ONLY) {
            if (a & MACRO_PRESS) p.held |= 1u << (a & 0x1f);
            else p.held &= ~(1u << a);
        }
        p.pos = (u8)(p.pos + 2);
        steps++;
        due_from(now);           /* the next from now, not from when this was due */
    }
    if (!p.n) {
        p.held = 0;
        return out;
    }
    *every &= ~owned(p.uses);
    return in_dpad((out & ~owned(p.uses)) | p.held);
}

void macro_stop(void)
{
    memset(&p, 0, sizeof(p));
}

u32 macro_deadline(void)
{
    return p.n ? (p.at ? p.at : 1) : 0;
}

#ifdef UNITTEST
/* Remap 0's macros in one record (key 5): 1 holds south 40 ms then up for
 * 20 ms; 2 taps L1 after a wait. */
static const u8 rec[] = {
    0, 0, 0, 0, 0, 0,
    9, 0, 0, MACRO_PRESS | IN_SOUTH, 10, IN_SOUTH, 0, MACRO_PRESS | IN_UP, 5, IN_UP,
    5, 0, 5, MACRO_PRESS | IN_L1, 5, IN_L1, 0,
};
static int gone;

int plat_store_read_at(u16 key, u16 offset, void *buf, u16 len)
{
    if (key != 5 || gone || offset >= sizeof(rec)) return 0;
    if (len > sizeof(rec) - offset) len = (u16)(sizeof(rec) - offset);
    memcpy(buf, rec + offset, len);
    return len;
}

int set_macro(u8 remap, u8 n, u16 *key, u16 *from, u16 *to)
{
    if (remap || (n != 1 && n != 2)) return 0;
    *key = 5;
    *from = n == 1 ? 8 : 18;
    *to = n == 1 ? 16 : 22;
    return 1;
}

static struct remap_macros req;
static u32 now, every;

static u32 at(u32 out, u32 t)
{
    now = t;
    every = PAD_SOUTH | PAD_EAST;
    out = macro_play(0, &req, out, now, &every);
    memset(&req.start, 0, 2);    /* events last one pass */
    return out;
}

int main(void)
{
    u32 o;

    /* Played once: south now, released 40 ms on, up at once, up released
     * 20 ms on; the remap's south and every-report south dropped meanwhile. */
    req.start = 1;
    o = at(PAD_SOUTH | PAD_EAST, 1000);
    CHECK(o == (PAD_SOUTH | PAD_EAST) && every == PAD_EAST && macro_deadline() == 1040);
    o = at(PAD_EAST, 1039); CHECK(o == (PAD_SOUTH | PAD_EAST));
    o = at(PAD_EAST, 1040); CHECK(o == (PAD_UP | PAD_EAST) && macro_deadline() == 1060);
    o = at(PAD_DOWN, 1041); CHECK(o == PAD_UP);              /* the d-pad is its own */
    o = at(0, 1060); CHECK(o == 0 && !macro_deadline());
    o = at(PAD_SOUTH, 1061); CHECK(o == PAD_SOUTH && every == (PAD_SOUTH | PAD_EAST));
    /* A stall: the rest moves later. */
    req.start = 1; at(0, 2000);
    o = at(0, 2100); CHECK(o == PAD_UP && macro_deadline() == 2120);
    at(0, 2120); CHECK(!macro_deadline());
    /* A level rule: on (remap.c starts it), a run, again while on; off lets
     * the run finish. */
    req.held = 2; req.start = 2;
    o = at(0, 3000); CHECK(o == 0 && macro_deadline() == 3020);
    o = at(0, 3020); CHECK(o == PAD_L1);
    o = at(0, 3040); CHECK(o == 0);
    o = at(0, 3040 + 1); CHECK(o == 0 && macro_deadline() == 3041 + 20);   /* the next run */
    req.held = 0;
    o = at(0, 3061); CHECK(o == PAD_L1);
    o = at(0, 3081); CHECK(o == 0 && !macro_deadline());
    /* A new start replaces the one playing. */
    req.start = 1; at(0, 4000);
    req.start = 2; o = at(0, 4010); CHECK(o == 0 && macro_deadline() == 4030);
    o = at(PAD_SOUTH, 4030); CHECK(o == (PAD_L1 | PAD_SOUTH));
    at(0, 4050);
    /* A loop: toggled on, runs again and again, toggled off at once. */
    req.toggle = 2; at(0, 5000);
    at(0, 5020); at(0, 5040); o = at(0, 5041); CHECK(macro_deadline() == 5061);
    o = at(0, 5061); CHECK(o == PAD_L1);
    req.toggle = 2; o = at(0, 5062); CHECK(o == 0 && !macro_deadline());
    /* No such macro, or gone from flash: nothing plays. */
    req.start = 4; o = at(PAD_SOUTH, 6000); CHECK(o == PAD_SOUTH && !macro_deadline());
    req.start = 1; at(0, 6100); gone = 1;
    o = at(PAD_EAST, 6140); CHECK(o == PAD_EAST && !macro_deadline());
    gone = 0;
    req.start = 1; at(0, 7000); macro_stop(); CHECK(!macro_deadline());
    return TEST_DONE("macro");
}
#endif

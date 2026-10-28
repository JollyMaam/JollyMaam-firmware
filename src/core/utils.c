#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pad.h"

/* A board short of RAM sets smaller rings (make/<product>.mk). */
#ifndef LOG_LINES
#define LOG_LINES 64
#endif
#ifndef LOG_TEXT
#define LOG_TEXT  120          /* a line is 128 bytes with seq and ms */
#endif
#define LOG_MAGIC 0x474f4c44   /* changes with the layout below */

static struct {
    u32 magic;
    u32 head;
    struct { u32 seq, ms; char text[LOG_TEXT]; } line[LOG_LINES];
} ring RETAINED;

/* Kept over a reset when it is whole: another image (a dongle's, after a
 * wrong flash) may have its ring a few bytes off, under the same magic. */
void util_log_init(void)
{
    if (ring.magic == LOG_MAGIC && (!ring.head || ring.line[(ring.head - 1) % LOG_LINES].seq == ring.head)) return;
    memset(&ring, 0, sizeof(ring));
    ring.magic = LOG_MAGIC;
}

/* The line is claimed under the lock and formatted in place, unlocked:
 * no copy of it on the stack. An interrupt's line takes the next slot;
 * the line stays terminated if formatting never finishes. */
void util_log(const char *fmt, ...)
{
    va_list ap;
    u32 i;

    plat_lock();
    i = ring.head++ % LOG_LINES;
    ring.line[i].seq = ring.head;
    ring.line[i].ms = plat_ms();
    ring.line[i].text[0] = ring.line[i].text[LOG_TEXT - 1] = 0;
    plat_unlock();
    va_start(ap, fmt);
    vsnprintf(ring.line[i].text, LOG_TEXT, fmt, ap);
    va_end(ap);
    plat_debug_line(ring.line[i].text);
}

/* Lines from sequence number *seq on, "seconds.ms text\n" each (time since
 * the boot that wrote the line), as many as fit.  *seq
 * becomes the number of the first line returned (later than asked if the
 * older ones were overwritten); the return is the byte count, and the next
 * read starts at *seq plus the lines returned, which the caller counts. */
u32 util_log_read(u32 *seq, char *out, u32 max)
{
    u32 oldest = ring.head > LOG_LINES ? ring.head - LOG_LINES + 1 : 1;
    u32 s = *seq < oldest ? oldest : *seq;
    u32 used = 0;

    *seq = s;
    for (; s <= ring.head; s++) {
        const char *t = ring.line[(s - 1) % LOG_LINES].text;
        u32 ms = ring.line[(s - 1) % LOG_LINES].ms;
        char stamp[16];
        u32 k = (u32)snprintf(stamp, sizeof(stamp), "%5u.%03u ", ms / 1000, ms % 1000);
        u32 n = strlen(t);

        if (used + k + n + 1 > max) break;
        memcpy(out + used, stamp, k);
        memcpy(out + used + k, t, n);
        used += k + n;
        out[used++] = '\n';
    }
    return used;
}

void util_put16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

void util_put32(u8 *p, u32 v)
{
    util_put16(p, (u16)v);
    util_put16(p + 2, (u16)(v >> 16));
}

#ifndef TRACE
#define TRACE 1024
#endif
static struct { u32 us; u16 arg; u8 marker, pad; } trace[TRACE];
static u32 trace_n;
static volatile u8 trace_on;

void util_trace(u8 marker, u16 arg)
{
    u32 i;

    if (!trace_on) return;
    plat_lock();
    i = trace_n++ % TRACE;
    trace[i].us = plat_us();
    trace[i].marker = marker;
    trace[i].arg = arg;
    plat_unlock();
}

void util_trace_enable(int on)
{
    plat_lock();
    if (on) trace_n = 0;
    trace_on = (u8)on;
    plat_unlock();
}

/* u32 count, then the entries in ring order (the oldest at count % TRACE once full). */
u32 util_trace_read(u32 offset, u8 *out, u32 max)
{
    u32 n = 0, k;

    if (offset < 4) {
        u8 head[4];

        util_put32(head, trace_n);
        k = 4 - offset < max ? 4 - offset : max;
        memcpy(out, head + offset, k);
        n = k;
        offset = 0;
    } else {
        offset -= 4;
    }
    if (offset < sizeof(trace)) {
        k = sizeof(trace) - offset;
        if (k > max - n) k = max - n;
        memcpy(out + n, (const u8 *)trace + offset, k);
        n += k;
    }
    return n;
}

/* ---- the latency probe ------------------------------------------------------
 * A presses and releases made inside the pad, each at a random time within
 * its period, applied where the real buttons come in (util_probe_apply) and
 * stamped twice: when it happened and when the first report carrying it was
 * sent (util_probe_sent).  An edge undone before any report carried it is
 * stamped sent 0.  The tool device reads the records (util_probe_take) and
 * the PC matches them with its own input events.  Period 0: A flips in
 * every report instead, as soon as the last one went out (the most reports
 * the link carries).  Nothing is saved. */
#ifndef PROBE_EDGES
#ifdef BOARD_DONGLE
#define PROBE_EDGES 0            /* no buttons of its own to probe */
#else
#define PROBE_EDGES 64
#endif
#endif
#if PROBE_EDGES
static struct {
    u32 left;                    /* edges still to make; the last one releases */
    u32 period_us, slot_us, next_us;
    u32 rng;
    u8 level;                    /* A as the probe holds it */
    u8 every_report;             /* period 0: flip after each report sent */
    u8 flip;                     /* every_report: a report went out, flip at the next input */
    u32 head, unsent, tail, lost;   /* records [tail, head); [unsent, head) not sent yet */
    struct util_probe_edge ring[PROBE_EDGES];
} probe;

static u32 probe_random(void)
{
    probe.rng ^= probe.rng << 13;
    probe.rng ^= probe.rng >> 17;
    probe.rng ^= probe.rng << 5;
    return probe.rng;
}

void util_probe_start(u32 edges, u32 period_ms)
{
    plat_lock();
    probe.left = (edges + 1) & ~1u;      /* even: it ends released */
    probe.every_report = !period_ms;
    probe.flip = 1;
    probe.period_us = (period_ms ? period_ms : 1) * 1000;
    probe.slot_us = plat_us();
    probe.rng = probe.slot_us | 1;
    probe.next_us = probe.slot_us + probe_random() % probe.period_us;
    probe.level = 0;
    probe.head = probe.unsent = probe.tail = probe.lost = 0;
    plat_unlock();
}

/* Makes the edges that are due and puts A as the probe holds it into the
 * buttons.  Returns when the next edge is due (plat_us), 0 when stopped. */
u32 util_probe_apply(u32 *buttons)
{
    u32 now = plat_us();

    if (!probe.left && !probe.level) return 0;
    plat_lock();
    if (probe.every_report) probe.next_us = probe.flip ? now : now + 1;
    while (probe.left && (s32)(now - probe.next_us) >= 0) {
        struct util_probe_edge *e;

        if (probe.head - probe.tail == PROBE_EDGES) {
            probe.tail++;
            probe.lost++;
            if ((s32)(probe.unsent - probe.tail) < 0) probe.unsent = probe.tail;
        }
        e = &probe.ring[probe.head++ % PROBE_EDGES];
        probe.level ^= 1;
        e->edge_us = probe.next_us;
        e->sent_us = 0;
        e->level = probe.level;
        probe.left--;
        probe.flip = 0;
        if (probe.every_report) break;
        probe.slot_us += probe.period_us;
        probe.next_us = probe.slot_us + probe_random() % probe.period_us;
    }
    plat_unlock();
    if (probe.level) *buttons |= PAD_A;
    else *buttons &= ~(u32)PAD_A;
    return probe.left && !probe.every_report ? probe.next_us : 0;
}

/* A report built from the newest input went out at us: the newest edge is in
 * it, the ones before it never were. */
void util_probe_sent(u32 us)
{
    if (probe.unsent == probe.head) return;
    plat_lock();
    probe.ring[(probe.head - 1) % PROBE_EDGES].sent_us = us ? us : 1;
    probe.unsent = probe.head;
    probe.flip = 1;
    plat_unlock();
}

void util_probe_stop(void)
{
    plat_lock();
    probe.left = 0;
    probe.level = 0;
    plat_unlock();
}

/* Up to max sent-or-superseded records, oldest first; *lost counts the
 * records overwritten before they were taken, *active whether edges remain. */
u32 util_probe_take(struct util_probe_edge *out, u32 max, u32 *lost, u8 *active)
{
    u32 n = 0;

    plat_lock();
    while (n < max && probe.tail != probe.unsent) out[n++] = probe.ring[probe.tail++ % PROBE_EDGES];
    *lost = probe.lost;
    *active = (u8)(probe.left || probe.level);
    plat_unlock();
    return n;
}
#else
void util_probe_start(u32 edges, u32 period_ms) { (void)edges; (void)period_ms; }
u32 util_probe_apply(u32 *buttons) { (void)buttons; return 0; }
void util_probe_sent(u32 us) { (void)us; }
void util_probe_stop(void) {}
u32 util_probe_take(struct util_probe_edge *out, u32 max, u32 *lost, u8 *active)
{
    (void)out; (void)max;
    *lost = 0;
    *active = 0;
    return 0;
}
#endif

/* Into a power-of-two histogram: bucket i holds values below 32 << i µs. */
void util_stat_add(struct stat_hist *h, u32 us)
{
    u32 b = 0, v = us >> 5;

    while (v && b < STAT_BUCKETS - 1) {
        v >>= 1;
        b++;
    }
    h->sum += us;
    h->count++;
    if (us > h->max) h->max = us;
    if (h->bucket[b] < 0xffff) h->bucket[b]++;
}

u32 util_get32(const u8 *p)
{
    return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* ---- AES-128 (encrypt only), FIPS-197: Switch 2 pairing, LE key derivation ---- */
static const u8 sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static u8 xtime(u8 x)
{
    return (u8)((x << 1) ^ ((x & 0x80U) ? 0x1bU : 0U));
}

void util_aes128(const u8 key[16], const u8 in[16], u8 out[16])
{
    u8 k[16], s[16], t[16];
    u8 rcon = 1;

    memcpy(k, key, 16);
    for (u32 i = 0; i < 16; i++) s[i] = in[i] ^ k[i];
    for (int round = 1; round <= 10; round++) {
        for (u32 i = 0; i < 16; i++) t[i] = sbox[s[(i + 4 * (i & 3)) & 15]];
        if (round < 10) {
            for (int c = 0; c < 16; c += 4) {
                u8 a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3];
                u8 all = a0 ^ a1 ^ a2 ^ a3;
                t[c] ^= all ^ xtime(a0 ^ a1);
                t[c + 1] ^= all ^ xtime(a1 ^ a2);
                t[c + 2] ^= all ^ xtime(a2 ^ a3);
                t[c + 3] ^= all ^ xtime(a3 ^ a0);
            }
        }
        k[0] ^= sbox[k[13]] ^ rcon;
        k[1] ^= sbox[k[14]];
        k[2] ^= sbox[k[15]];
        k[3] ^= sbox[k[12]];
        for (int i = 4; i < 16; i++) k[i] ^= k[i - 4];
        rcon = xtime(rcon);
        for (u32 i = 0; i < 16; i++) s[i] = t[i] ^ k[i];
    }
    memcpy(out, s, 16);
}

#ifdef UNITTEST
void plat_lock(void) {}
void plat_unlock(void) {}
static u32 fake_ms = 1234;
u32 plat_ms(void) { return fake_ms; }
u32 plat_us(void) { return fake_ms * 1000; }

int main(void)
{
    char buf[4096];
    u32 seq = 1, n, i;

    util_log_init();
    CHECK(util_log_read(&seq, buf, sizeof(buf)) == 0);
    util_log("hello %d", 1);
    util_log("world");
    n = util_log_read(&seq, buf, sizeof(buf));
    CHECK(seq == 1 && n == 34 && !memcmp(buf, "    1.234 hello 1\n    1.234 world\n", 34));
    seq = 2;
    n = util_log_read(&seq, buf, sizeof(buf));
    CHECK(seq == 2 && n == 16);
    fake_ms = 0;
    for (i = 0; i < 100; i++) util_log("line %u", i);
    seq = 1;
    n = util_log_read(&seq, buf, sizeof(buf));
    CHECK(seq == 102 - LOG_LINES + 1);
    {
        char first[32];

        snprintf(first, sizeof(first), "    0.000 line %u\n", 100 - LOG_LINES);
        CHECK(!memcmp(buf, first, strlen(first)));
    }
    seq = 1;
    n = util_log_read(&seq, buf, 40);
    CHECK(n == 36);
    util_log_init();
    seq = 102;
    CHECK(util_log_read(&seq, buf, sizeof(buf)) == 18);
    /* Another image's ring, 4 bytes off: our head is its magic. */
    ring.head = LOG_MAGIC;
    util_log_init();
    seq = 1;
    CHECK(ring.head == 0 && util_log_read(&seq, buf, sizeof(buf)) == 0);
    {
        static const u8 key[16] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
        static const u8 in[16] = { 0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d, 0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34 };
        static const u8 want[16] = { 0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb, 0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32 };
        u8 out[16];
        util_aes128(key, in, out);
        CHECK(!memcmp(out, want, 16));
    }
#if PROBE_EDGES
    {   /* the probe: edges one per period, A follows, a report stamps the newest */
        struct util_probe_edge e[8];
        u32 b = PAD_B, at, lost;
        u8 active;

        fake_ms = 100;
        util_probe_start(3, 10);                 /* rounded up to 4: ends released */
        at = util_probe_apply(&b);
        CHECK(at >= 100000 && at < 110000 && b == PAD_B);
        fake_ms = 110;                           /* past the first edge */
        at = util_probe_apply(&b);
        CHECK(b == (PAD_B | PAD_A) && at >= 110000 && at < 120000);
        CHECK(util_probe_take(e, 8, &lost, &active) == 0 && active);   /* not sent yet */
        util_probe_sent(111000);
        CHECK(util_probe_take(e, 8, &lost, &active) == 1 && e[0].level == 1 &&
              e[0].edge_us >= 100000 && e[0].edge_us < 110000 && e[0].sent_us == 111000);
        fake_ms = 130;                           /* two edges before a report */
        util_probe_apply(&b);
        CHECK(b == (PAD_B | PAD_A));             /* released and pressed again */
        util_probe_sent(131000);
        CHECK(util_probe_take(e, 8, &lost, &active) == 2 && e[0].sent_us == 0 && e[1].sent_us == 131000);
        fake_ms = 140;
        CHECK(util_probe_apply(&b) == 0 && b == PAD_B);   /* the last edge releases */
        util_probe_sent(141000);
        CHECK(util_probe_take(e, 8, &lost, &active) == 1 && e[0].level == 0 && !active && lost == 0);
        b = PAD_A;
        CHECK(util_probe_apply(&b) == 0 && b == PAD_A);   /* stopped: the real A again */

        util_probe_start(4, 0);                  /* every report */
        b = 0;
        CHECK(util_probe_apply(&b) == 0 && b == PAD_A);   /* flipped at once, no deadline */
        CHECK(util_probe_apply(&b) == 0 && b == PAD_A);   /* not again before a report */
        util_probe_sent(150000);
        util_probe_apply(&b);
        CHECK(b == 0);
        util_probe_sent(151000);
        CHECK(util_probe_take(e, 8, &lost, &active) == 2 && e[0].sent_us == 150000 &&
              e[1].level == 0 && e[1].sent_us == 151000);
    }
#endif
    return TEST_DONE("utils");
}
#endif

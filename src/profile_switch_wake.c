#include <string.h>
#include "pad.h"

/* Waking a sleeping Switch 1: not a link, but raw packets on one channel for
 * some seconds, carrying the console's wake-up code.  The baseband whitens
 * every packet and its whitening state is not measured, so the body is
 * pre-cancelled for each of the 32 states in turn.  The access code has been
 * seen on the air; a console actually waking has not. */

#define CHANNEL      0
#define DURATION_MS  1000
#define DWELL_MS     30          /* per whitening state (8 bursts): all 32 fit in 1 s */
#define INTERVAL     6           /* slots between bursts (3.75 ms) */
#define BODY         339
#define SKIP_BITS    34          /* multi-slot header bits before the body */
#define STATES       32

/* ---- the wake-up code ------------------------------------------------------
 * The Baseband sync word of the console's LAP, with its low 32 bits inverted.
 * PN and the (63,30) BCH generator times (D + 1) are from the Core spec. */

#define PN        0x83848D96BBCC54FCull
#define GENERATOR 0x585713DA9ull

static u64 sync_word(const u8 a[6])
{
    u32 lap = a[0] | (u32)a[1] << 8 | (u32)a[2] << 16;
    u32 info = ((lap & 0x800000) ? 0x13000000u : 0x2C000000u) | lap;
    u64 v = (u64)(info ^ (u32)(PN >> 34)) << 34, r = v;
    int bit;

    for (bit = 63; bit >= 34; bit--)
        if (r & (1ull << bit)) r ^= GENERATOR << (bit - 34);
    return (v ^ r) ^ PN;
}

/* The parity field the baseband is given; its low four bytes are the wake-up
 * code in transmission order. */
static void wake_parity(const u8 a[6], u8 out[5])
{
    u64 v = sync_word(a) & 0x3FFFFFFFFull;
    int i;

    for (i = 0; i < 4; i++) out[i] = (u8)(v >> (8 * i)) ^ 0xff;
    out[4] = (u8)(v >> 32);
}

/* The order the reference walks the whitening states in. */
static const u8 seeds[STATES] = {
    0x69, 0x01, 0x31, 0x7d, 0x05, 0x5b, 0x6b, 0x03, 0x5f, 0x37, 0x07, 0x59, 0x21, 0x6d, 0x5d, 0x35,
    0x7b, 0x13, 0x23, 0x6f, 0x17, 0x49, 0x79, 0x11, 0x4d, 0x25, 0x15, 0x4b, 0x33, 0x7f, 0x4f, 0x27,
};

/* XOR the whitening sequence (7-bit LFSR, taps 0 and 4, low bit out) over
 * the body, `skip` bits in. */
static void unwhiten(u8 *body, u16 len, u8 seed, u8 skip)
{
    u8 reg = seed & 0x7f;
    u32 bit, n = (u32)len * 8 + skip;

    for (bit = 0; bit < n; bit++) {
        if (bit >= skip && (reg & 1)) body[(bit - skip) / 8] ^= (u8)(1u << ((bit - skip) % 8));
        reg = (u8)((reg >> 1) | (((reg ^ (reg >> 4)) & 1) << 6));
    }
}

/* The wake-up code rotated by 10 bits, repeated; the last three bytes zero,
 * as the reference sends it. */
static void build_body(const u8 a[6], u8 *body)
{
    u32 w = ~(u32)sync_word(a);
    u8 rep[4];
    u16 i;

    util_put32(rep, (w >> 10) | (w << 22));
    for (i = 0; i < BODY; i++) body[i] = rep[i % 4];
    memset(body + BODY - 3, 0, 3);
}

#ifndef UNITTEST
/* ---- the radio (BR23) ------------------------------------------------------
 * The register sequence of the controller library's own fixed-frequency
 * transmit (__fix_txrx_enable() in bredr_link.c.o), with our parity, payload
 * and interval.  Frame offsets are from the archive's DWARF for ctrl_frame. */

struct ctrl_frame;
struct link_task;
struct bredr_tx_bulk;
struct link_task_ops { int (*resume)(struct link_task *); int (*suspend)(struct link_task *); };

extern struct ctrl_frame *BTCTRLER_BIN_bredr_bd_get_frame(void) __asm__("bredr_bd_get_frame");
extern void BTCTRLER_BIN_bredr_bd_put_frame(struct ctrl_frame *f) __asm__("bredr_bd_put_frame");
extern void BTCTRLER_BIN_bredr_bd_frame_enable(struct ctrl_frame *f, u8 flag, u8 set_en) __asm__("bredr_bd_frame_enable");
extern void BTCTRLER_BIN_bredr_bd_frame_disable(struct ctrl_frame *f) __asm__("bredr_bd_frame_disable");
extern void BTCTRLER_BIN___write_reg_bdaddr(struct ctrl_frame *f, const u8 *address) __asm__("__write_reg_bdaddr");
extern void BTCTRLER_BIN___write_reg_bch(struct ctrl_frame *f, const u8 *bch) __asm__("__write_reg_bch");
extern void BTCTRLER_BIN___write_reg_format(struct ctrl_frame *f, u8 train, u8 format) __asm__("__write_reg_format");
extern void BTCTRLER_BIN___write_reg_miscntl(struct ctrl_frame *f, u8 ptt, u8 lt_addr) __asm__("__write_reg_miscntl");
extern void BTCTRLER_BIN___write_reg_linkcntl(struct ctrl_frame *f, int index, int value) __asm__("__write_reg_linkcntl");
extern void BTCTRLER_BIN___write_reg_clkoffset(struct ctrl_frame *f, int clk_offset) __asm__("__write_reg_clkoffset");
extern void BTCTRLER_BIN___write_reg_txinfo(struct ctrl_frame *f, int index, u8 *data, int len, int llid, int flow, int packet) __asm__("__write_reg_txinfo");
extern void BTCTRLER_BIN___clean_reg_txempty(struct ctrl_frame *f, int index) __asm__("__clean_reg_txempty");
extern u16 *BTCTRLER_BIN_get_packet_type(int len, u8 edr, u8 slot) __asm__("get_packet_type");
extern struct bredr_tx_bulk *BTCTRLER_BIN_bredr_tx_bulk_alloc(int size) __asm__("bredr_tx_bulk_alloc");
extern void BTCTRLER_BIN_bredr_tx_bulk_free(struct bredr_tx_bulk *b) __asm__("bredr_tx_bulk_free");
extern int BTCTRLER_BIN_link_task_idle_disable(void) __asm__("link_task_idle_disable");
extern void BTCTRLER_BIN_link_task_add(struct link_task *t) __asm__("link_task_add");
extern void BTCTRLER_BIN_link_task_del(struct link_task *t) __asm__("link_task_del");
extern int hci_jieli_open(void);
extern void hci_jieli_close(void);

#define F_WINCNTL 16             /* u16: whitening */
#define F_TXFREQ  21
#define F_RXFREQ  23
#define F_BUFCTL  88             /* u16 */
#define BULK_HEAD 28
#define BB_BASE   (*(volatile u32 *)0x1cfc44u)   /* payloads must lie within 32 KB above */
#define REG16(f, o) (*(volatile u16 *)((u8 *)(f) + (o)))
#define REG8(f, o)  (*(volatile u8 *)((u8 *)(f) + (o)))

static struct ctrl_frame *frame;
static struct bredr_tx_bulk *bulk;
static u8 task[28] __attribute__((aligned(4)));
static u8 running;

static int resume(struct link_task *t)
{
    (void)t;
    if (frame) BTCTRLER_BIN_bredr_bd_frame_enable(frame, 0, 1);
    running = 1;
    return 0;
}

static int suspend(struct link_task *t)
{
    (void)t;
    if (frame) BTCTRLER_BIN_bredr_bd_frame_disable(frame);
    running = 0;
    return 0;
}

static const struct link_task_ops task_ops = { resume, suspend };

static int radio_start(const u8 address[6], const u8 parity[5], u8 seed, const u8 *body)
{
    u8 freq = (u8)((CHANNEL >> 1) + (CHANNEL & 1 ? 40 : 0));
    u16 *type, len = BODY;
    u8 *payload;

    if (!(frame = BTCTRLER_BIN_bredr_bd_get_frame())) return -1;
    if (!(bulk = BTCTRLER_BIN_bredr_tx_bulk_alloc(BODY))) {
        BTCTRLER_BIN_bredr_bd_put_frame(frame);
        frame = NULL;
        return -1;
    }
    payload = (u8 *)bulk + BULK_HEAD;
    memcpy(payload, body, BODY);
    if ((u32)payload - BB_BASE >= 32767) {   /* txinfo would reset the chip */
        util_log("wake: buffer out of the baseband window");
        BTCTRLER_BIN_bredr_tx_bulk_free(bulk);
        bulk = NULL;
        BTCTRLER_BIN_bredr_bd_put_frame(frame);
        frame = NULL;
        return -1;
    }
    BTCTRLER_BIN___write_reg_bdaddr(frame, address);
    BTCTRLER_BIN___write_reg_bch(frame, parity);
    BTCTRLER_BIN___write_reg_miscntl(frame, 0, 7);
    BTCTRLER_BIN___write_reg_clkoffset(frame, 0);
    BTCTRLER_BIN___write_reg_format(frame, 0, 2);
    BTCTRLER_BIN___write_reg_linkcntl(frame, 0, 16401);
    BTCTRLER_BIN___write_reg_linkcntl(frame, 1, 16385);
    REG16(frame, F_BUFCTL) = (u16)((REG16(frame, F_BUFCTL) & ~0x0f00) | 0x0100);
    type = BTCTRLER_BIN_get_packet_type(BODY, 0, 5);
    if (len > type[0]) len = type[0];
    BTCTRLER_BIN___write_reg_txinfo(frame, 0, payload, len, 2, 1, type[1]);
    BTCTRLER_BIN___clean_reg_txempty(frame, 0);
    BTCTRLER_BIN___clean_reg_txempty(frame, 1);
    REG16(frame, F_BUFCTL) = (u16)((REG16(frame, F_BUFCTL) & ~0x0022) | 0x0001);
    REG16(frame, F_WINCNTL) = seed;
    REG8(frame, F_TXFREQ) = freq;
    REG8(frame, F_RXFREQ) = freq;
    memset(task, 0, sizeof(task));
    task[0] = 1;                 /* attr */
    task[3] = 1;                 /* num */
    task[4] = 1;                 /* prio */
    *(u16 *)(task + 6) = 4096;   /* type */
    *(u16 *)(task + 8) = INTERVAL;
    *(const struct link_task_ops **)(task + 24) = &task_ops;
    BTCTRLER_BIN_link_task_idle_disable();
    BTCTRLER_BIN_link_task_add((struct link_task *)task);
    return 0;
}

static void radio_stop(void)
{
    if (!frame) return;
    BTCTRLER_BIN_link_task_del((struct link_task *)task);
    if (running) BTCTRLER_BIN_bredr_bd_frame_disable(frame);
    running = 0;
    BTCTRLER_BIN_bredr_bd_put_frame(frame);
    frame = NULL;
    if (bulk) BTCTRLER_BIN_bredr_tx_bulk_free(bulk);
    bulk = NULL;
}

/* ---- the step ------------------------------------------------------------- */

static struct {
    u8 active, state;
    u8 target[6], parity[5];
    u32 started, state_ms;
} w;

static int start_state(void)
{
    static u8 body[BODY];

    build_body(w.target, body);
    unwhiten(body, BODY, seeds[w.state], SKIP_BITS);
    return radio_start(w.target, w.parity, seeds[w.state], body);
}

static void stop(void)
{
    radio_stop();
    if (w.active) hci_jieli_close();
    w.active = 0;
}

static int start(const struct profile_session *s)
{
    static const u8 zero[6];

    if (!s->bond || !memcmp(s->bond->peer, zero, 6)) return -1;
    memset(&w, 0, sizeof(w));
    memcpy(w.target, s->bond->peer, 6);
    wake_parity(w.target, w.parity);
    if (hci_jieli_open()) return -1;
    w.active = 1;
    w.started = w.state_ms = plat_ms();
    if (start_state()) {
        stop();
        return -1;
    }
    util_log("wake: %02x:%02x:%02x:%02x:%02x:%02x", w.target[5], w.target[4], w.target[3],
             w.target[2], w.target[1], w.target[0]);
    return 0;
}

/* The controller stays open when the wake finishes: the session takes it over. */
static int step(u32 now, u32 *next)
{
    if (!w.active) return 1;
    if (now - w.started >= DURATION_MS) {
        radio_stop();
        w.active = 0;
        util_log("wake: done");
        return 1;
    }
    if (now - w.state_ms >= DWELL_MS) {
        radio_stop();
        w.state = (u8)((w.state + 1) % STATES);
        w.state_ms = now;
        if (start_state()) {
            stop();
            return 1;
        }
    }
    *next = w.state_ms + DWELL_MS;
    return 0;
}

const struct profile_wake profile_switch_wake = { start, step, stop };
#endif

#ifdef UNITTEST
#include "test.h"

void util_put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/* The reference's worked examples (NS1-Wakeup-Demo), printed address order. */
static const struct { u8 printed[6]; u32 wake32; } vectors[] = {
    { { 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 }, 0x79F98210u },
    { { 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa }, 0xfad69689u },
    { { 0x54, 0xf7, 0x08, 0x90, 0x34, 0x25 }, 0x0cb13e3fu },
    { { 0xef, 0x4a, 0xc5, 0x1a, 0x00, 0xee }, 0xaf3bc02du },
};

int main(void)
{
    u8 a[6], parity[5], code[4], body[BODY], again[BODY];
    u32 i, j;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        for (j = 0; j < 6; j++) a[j] = vectors[i].printed[5 - j];
        CHECK(~(u32)sync_word(a) == vectors[i].wake32);
        wake_parity(a, parity);
        util_put32(code, vectors[i].wake32);
        CHECK(!memcmp(parity, code, 4));
    }
    CHECK((sync_word(a) ^ 0xffffffffu) == 0xB06803B9AF3BC02Dull);   /* inside the access code */
    build_body(a, body);
    for (j = 4; j < BODY - 3; j++) CHECK(body[j] == body[j % 4]);
    CHECK(!body[BODY - 1] && !body[BODY - 3]);
    memcpy(again, body, BODY);
    unwhiten(again, BODY, seeds[0], SKIP_BITS);
    CHECK(memcmp(again, body, BODY));
    unwhiten(again, BODY, seeds[0], SKIP_BITS);
    CHECK(!memcmp(again, body, BODY));
    return TEST_DONE("profile_switch_wake");
}
#endif

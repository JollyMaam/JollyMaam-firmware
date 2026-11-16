#include <string.h>
#include "pad.h"

/* Waking a sleeping Switch 1: raw packets on one channel, with the
 * body pre-cancelled for each whitening state.  The access code has been
 * seen on the air; a console actually waking has not. */

#define CHANNEL      0
#define DURATION_MS  1000
#define DWELL_MS     30          /* per whitening state (8 bursts): all 32 fit in 1 s */
#define INTERVAL     6           /* slots between bursts (3.75 ms) */
#define BODY         SWITCH1_WAKE_BODY
#define STATES       SWITCH1_WAKE_STATES

/* ---- the radio (BR23) ------------------------------------------------------
 * The register sequence of the controller library's own fixed-frequency
 * transmit (__fix_txrx_enable() in bredr_link.c.o), with our parity, payload
 * and interval.  Frame offsets are from the archive's DWARF for ctrl_frame. */

struct ctrl_frame;
struct link_task;
struct bredr_tx_bulk;
struct link_task_ops { int (*resume)(struct link_task *); int (*suspend)(struct link_task *); };

extern struct ctrl_frame *bredr_bd_get_frame(void);
extern void bredr_bd_put_frame(struct ctrl_frame *f);
extern void bredr_bd_frame_enable(struct ctrl_frame *f, u8 flag, u8 set_en);
extern void bredr_bd_frame_disable(struct ctrl_frame *f);
extern void __write_reg_bdaddr(struct ctrl_frame *f, const u8 *address);
extern void __write_reg_bch(struct ctrl_frame *f, const u8 *bch);
extern void __write_reg_format(struct ctrl_frame *f, u8 train, u8 format);
extern void __write_reg_miscntl(struct ctrl_frame *f, u8 ptt, u8 lt_addr);
extern void __write_reg_linkcntl(struct ctrl_frame *f, int index, int value);
extern void __write_reg_clkoffset(struct ctrl_frame *f, int clk_offset);
extern void __write_reg_txinfo(struct ctrl_frame *f, int index, u8 *data, int len, int llid, int flow, int packet);
extern void __clean_reg_txempty(struct ctrl_frame *f, int index);
extern u16 *get_packet_type(int len, u8 edr, u8 slot);
extern struct bredr_tx_bulk *bredr_tx_bulk_alloc(int size);
extern void bredr_tx_bulk_free(struct bredr_tx_bulk *b);
extern int link_task_idle_disable(void);
extern void link_task_add(struct link_task *t);
extern void link_task_del(struct link_task *t);
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
    if (frame) bredr_bd_frame_enable(frame, 0, 1);
    running = 1;
    return 0;
}

static int suspend(struct link_task *t)
{
    (void)t;
    if (frame) bredr_bd_frame_disable(frame);
    running = 0;
    return 0;
}

static const struct link_task_ops task_ops = { resume, suspend };

static int radio_start(const u8 address[6], const u8 parity[5], u8 seed, const u8 *body)
{
    u8 freq = (u8)((CHANNEL >> 1) + (CHANNEL & 1 ? 40 : 0));
    u16 *type, len = BODY;
    u8 *payload;

    if (!(frame = bredr_bd_get_frame())) return -1;
    if (!(bulk = bredr_tx_bulk_alloc(BODY))) {
        bredr_bd_put_frame(frame);
        frame = NULL;
        return -1;
    }
    payload = (u8 *)bulk + BULK_HEAD;
    memcpy(payload, body, BODY);
    if ((u32)payload - BB_BASE >= 32767) {   /* txinfo would reset the chip */
        util_log("wake: buffer out of the baseband window");
        bredr_tx_bulk_free(bulk);
        bulk = NULL;
        bredr_bd_put_frame(frame);
        frame = NULL;
        return -1;
    }
    __write_reg_bdaddr(frame, address);
    __write_reg_bch(frame, parity);
    __write_reg_miscntl(frame, 0, 7);
    __write_reg_clkoffset(frame, 0);
    __write_reg_format(frame, 0, 2);
    __write_reg_linkcntl(frame, 0, 16401);
    __write_reg_linkcntl(frame, 1, 16385);
    REG16(frame, F_BUFCTL) = (u16)((REG16(frame, F_BUFCTL) & ~0x0f00) | 0x0100);
    type = get_packet_type(BODY, 0, 5);
    if (len > type[0]) len = type[0];
    __write_reg_txinfo(frame, 0, payload, len, 2, 1, type[1]);
    __clean_reg_txempty(frame, 0);
    __clean_reg_txempty(frame, 1);
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
    link_task_idle_disable();
    link_task_add((struct link_task *)task);
    return 0;
}

static void radio_stop(void)
{
    if (!frame) return;
    link_task_del((struct link_task *)task);
    if (running) bredr_bd_frame_disable(frame);
    running = 0;
    bredr_bd_put_frame(frame);
    frame = NULL;
    if (bulk) bredr_tx_bulk_free(bulk);
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
    u8 seed = switch1_wake_packet(w.target, w.state, w.parity, body);

    return radio_start(w.target, w.parity, seed, body);
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

const struct profile_wake plat_switch_wake = { start, step, stop };

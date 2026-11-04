#include <string.h>
#include "pad.h"
#include "yc3121.h"
#include "usb_yc3121.h"

/* USB on the YC3121: the tool device (a vendor HID interface, commands as
 * feature reports, answers as platform_telink_usb.c's: on input reports to
 * the command's client, so programs share the device), served from the USB
 * interrupt.  It has to be: the controller takes the
 * host's next OUT packet while the last is still in its FIFO, and a polled
 * driver lost the tail of multi-packet SET_REPORTs (single packets never).
 *
 * The controller as stock v1.01 drives it (its USB core, 0x01024e78 and the
 * functions under it); none of this is in Yichip's published SDK:
 *   0xf6000 CONFIG: 0, then 0x3c to run
 *   0xf6001..3 interrupt masks (0x70, 0xf0, 0xff); 0xf6004 the address, bit 7
 *   set on the EP0 event after SET_ADDRESS's status stage
 *   0xf6010 TRIGGER: 1 << ep sends the IN packet written to the FIFO;
 *   0x10 << ep: EP n's zero-length packet / the OUT packet taken (stock
 *   gives it after each SET_REPORT data packet, nothing at that SETUP)
 *   0xf6011 STALL: 1 stalls EP0 (with TRIGGER 0x10); it stays until written
 *   0, so each SETUP clears it (stock never does: not established)
 *   0xf6012 CLEAR: stock's and the loader's init write 0x40 (meaning not
 *   established)
 *   0xf6018 + ep: the endpoint's FIFO, a byte per access
 *   0xf6020 + ep: the length of the OUT packet in the FIFO
 *   0xf6026 STATUS (write 1 to clear): bit 0 EP0 OUT or SETUP (bit 4: it is
 *   a SETUP; set while the packet is still arriving), bits 1-3 EP1-3 OUT,
 *   bit 6 a transaction done (stock waits for it before an IN packet, and
 *   before reading a SETUP whose FIFO count is not 8), bit 7 bus reset
 *   0xf6027 (write 1 to clear): bit n, EP n's IN packet went out
 * Its clock: CLK_CLS bits 13 and 6 cleared (on), 0xc8905 = 0xff,
 * HCLK_CON |= 0x800. */

#define USB_CONFIG     (*(volatile uint8_t *)0xf6000)
#define USB_MASK(n)    (*(volatile uint8_t *)(0xf6001 + (n)))
#define USB_ADDR       (*(volatile uint8_t *)0xf6004)
#define USB_TRIGGER    (*(volatile uint8_t *)0xf6010)
#define USB_STALL      (*(volatile uint8_t *)0xf6011)
#define USB_CLEAR      (*(volatile uint8_t *)0xf6012)
#define USB_FIFO(ep)   (*(volatile uint8_t *)(0xf6018 + (ep)))
#define USB_LEN(ep)    (*(volatile uint8_t *)(0xf6020 + (ep)))
#define USB_STATUS     (*(volatile uint8_t *)0xf6026)
#define USB_SENT       (*(volatile uint8_t *)0xf6027)
#define YC_CLK_CLS     (*(volatile uint32_t *)0xf856c)

#define EP0_SIZE   64
/* The report is one EP0 packet: multi-packet SET_REPORTs lost their tail
 * (see the top), single packets never.  A command (flags bit 1 set: the
 * answer on input reports; without it, dropped) takes up to DATA_MAX bytes
 * in; its answer (up to ANSWER_MAX) goes out on input reports (IN_HEAD, then
 * IN_DATA of it each).  A command while another's answer is waiting or going
 * out is refused to its client.  GET_REPORT is refused (stalled). */
#define REPORT     64
#define HEAD_IN    JM_HID_CLIENT_HEAD_SIZE
#define DATA_MAX   (REPORT - HEAD_IN)
#define ANSWER_MAX 600
#define IN_HEAD    JM_HID_INPUT_HEAD_SIZE
#define IN_DATA    (REPORT - IN_HEAD)
#define IN_WAIT_MS 1000          /* a piece no one takes: no one reads, the answer is dropped */

enum { TOOL_MORE = JM_HID_MORE, TOOL_LAST = JM_HID_LAST, TOOL_REFUSED = JM_HID_REFUSED };

static const u8 device_descriptor[] = {
    18, 1, 0x10, 0x01, 0, 0, 0, EP0_SIZE,
    (u8)USB_OWN_VID, USB_OWN_VID >> 8, (u8)USB_OWN_HID_PID, USB_OWN_HID_PID >> 8,
    0x00, 0x01,                          /* bcdDevice 1.00 */
    1, 2, 0, 1,                          /* manufacturer 1, product 2, no serial */
};

static const u8 report_descriptor[] = {
    0x06, 0xc2, 0xff,                    /* usage page 0xffc2 */
    0x09, 0x01,                          /* usage 1 */
    0xa1, 0x01,                          /* application collection */
    0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x96, (u8)REPORT, REPORT >> 8,
    0x09, 0x01, 0xb1, 0x02,              /* feature: data, variable */
    0x95, REPORT,
    0x09, 0x01, 0x81, 0x02,              /* input: data, variable */
    0xc0,
};

static const u8 configuration_descriptor[] = {
    9, 2, 34, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 3, 0, 0, 0,           /* HID, no boot protocol */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(report_descriptor), 0,
    7, 5, 0x81, 3, 64, 0, 1,             /* interrupt IN: the answers on input reports */
};
#define HID_AT 18

static struct usb_events events;


static struct {
    u8 setup[8];
    u8 address_pending, configuration;
    u8 receiving;                /* a SET_REPORT's data stage */
    u16 got, want;               /* its bytes so far, of wLength */
    u8 sending;                  /* a control read's data stage */
    u16 at, total;               /* of src */
    u8 zlp;                      /* it ends with a zero-length packet */
    const u8 *src;               /* what goes out on EP0: buf, or a constant */
    u8 buf[REPORT];
} u;

/* ---- a game controller personality (plat_usb_mode) --------------------------
 * One HID interface from the profile's usb_identity: interrupt IN 0x81 and,
 * with out_size, interrupt OUT 0x02, as stock's own personalities have
 * them.  Input is newest-wins, messages queue and go first; the host's
 * writes (EP2, SET_REPORT) wait for plat_usb_take(). */
#define MESSAGES 4
#define WRITES   4
#define EP_IN    1
#define EP_OUT   2
#define MS_OS_STRING 0xee        /* Xbox 360: Microsoft OS 1.0 descriptors */
#define MS_OS_VENDOR 0x90        /* the stock dongle's vendor code */

static struct {
    const struct usb_identity *id;
    int (*read)(u8 kind, u16 id, u16 offset, u8 *out, u16 max);
    u8 dev[18];
    u8 cfg[9 + 9 + 17 + 7 + 7];
    u8 cfg_len;
    u8 set_kind, set_id;         /* the SET_REPORT whose data comes */
    u8 rx[REPORT];
    volatile u8 in_busy;         /* EP1's packet not taken yet */
    u8 input[REPORT], input_len;
    struct { u8 len, data[REPORT]; } msg[MESSAGES];
    u8 m_head, m_count;
    struct bt_write writes[WRITES];
    u8 w_head, w_count;
    volatile u8 event;           /* EV_USB: the host took a report or wrote one */
} pz;

static struct {
    u8 report[REPORT];           /* the command taken (a SET_REPORT comes into pz.rx) */
    u8 tag, flags;
    u32 client;                  /* with JM_HID_INPUT: whose answer it is */
    u16 n;                       /* its data's length */
    u16 answer, off;
    volatile u8 pending;         /* taken, for plat_usb_service() */
    u8 sending, last_out;        /* the answer on input reports; its last piece queued */
    u32 sent_at;
    u8 data[ANSWER_MAX];
} t;

/* A refused command's sender (one: a second waits for its timeout). */
static struct {
    u8 tag, waiting;
    u32 client;
} refused;

/* Bring-up: the last EP0 events, kept over a restart (.noinit). */
struct usb_trace usb_trace __attribute__((section(".noinit")));

static void trace(u8 status, u8 sent, u8 len, const u8 *setup)
{
    struct usb_trace_entry *e;

    if (usb_trace.stop && usb_trace.next >= usb_trace.stop) return;
    e = &usb_trace.e[usb_trace.next++ % USB_TRACE];

    e->ms = (u16)plat_ms();
    e->status = status, e->sent = sent, e->len = len;
    e->type = setup ? setup[0] : 0xff;
    e->request = setup ? setup[1] : 0xff;
    e->state = (u8)(u.receiving | u.sending << 1 | (u.got / 4 > 63 ? 63 : u.got / 4) << 2);
}

/* ---- the controller -------------------------------------------------------- */

/* Bytes left in EP0's FIFO (a restart through the loader keeps them, and
 * its first SETUP would then be read out of step). */
void plat_usb_drain(void)
{
    for (int i = 0; i < 1024 && USB_LEN(0); i++)
        (void)USB_FIFO(0);
}

void plat_usb_init(void)
{
    YC_CLK_CLS &= ~(1u << 13);
    *(volatile uint8_t *)0xc8905 = 0xff;
    YC_CLK_CLS &= ~(1u << 6);
    YC_HCLK_CON |= 0x800;
    USB_CONFIG = 0;
    plat_delay_ms(20);                   /* the host sees the loader's device go */
    USB_CLEAR = 0x40;
    plat_usb_drain();
    USB_TRIGGER = 0xc0;
    USB_CONFIG = 0x3c;
    USB_STATUS = 0xff;
    USB_SENT = 0xff;
    USB_ADDR = 0;
    USB_MASK(0) = 0x70;
    USB_MASK(1) = 0xf0;
    USB_MASK(2) = 0xff;
    memset(&u, 0, sizeof(u));
    *(volatile u32 *)0xe000e100 = 1;    /* NVIC: IRQ 0 on */
}

void plat_usb_irq(void)
{
    plat_usb_poll();
}

/* After bit 6 of STATUS comes round again, as stock does before each IN. */
static void send_in(u8 ep)
{
    USB_STATUS = 0x40;
    for (int i = 0; i < 10000 && !(USB_STATUS & 0x40); i++)
        ;
    USB_SENT = 1u << ep;
    USB_TRIGGER = 1u << ep;
}

static void ep0_zlp(void)
{
    USB_TRIGGER = 0x10;
}

static void ep0_stall(void)
{
    USB_STALL = 1;
    USB_TRIGGER = 0x10;
    events.stalls++;
    memcpy(events.stalled, u.setup, 8);
}

/* The next packet of the control read, or its closing zero-length one. */
static void ep0_next(void)
{
    u16 n = u.total - u.at < EP0_SIZE ? u.total - u.at : EP0_SIZE;

    if (!n) {
        u.sending = 0;
        if (u.zlp) ep0_zlp();
        u.zlp = 0;
        return;
    }
    for (u16 i = 0; i < n; i++)
        USB_FIFO(0) = u.src[u.at + i];
    u.at += n;
    if (u.at == u.total && n < EP0_SIZE) u.sending = 0;
    send_in(0);
}

static void ep0_answer(const u8 *data, u16 len)
{
    u16 want = (u16)(u.setup[6] | u.setup[7] << 8);

    if (len > want) len = want;
    u.src = data;                       /* buf, or constant: it stays */
    u.at = 0;
    u.total = len;
    u.zlp = len < want && len % EP0_SIZE == 0;
    u.sending = 1;
    ep0_next();
}

static u16 string(u8 index, u8 *out)
{
    const char *s = index == 1 ? USB_MANUFACTURER : board_name;
    u16 n = 2;

    if (pz.id) s = index == 1 ? pz.id->manufacturer : index == 2 ? pz.id->product : pz.id->serial;
    if (!s) s = "";
    if (index == 0) {
        out[2] = 0x09, out[3] = 0x04;
        n = 4;
    } else {
        for (; *s && n < REPORT - 2; s++) {
            out[n++] = (u8)*s;
            out[n++] = 0;
        }
    }
    out[0] = (u8)n;
    out[1] = 3;
    return n;
}

/* ---- the tool device ------------------------------------------------------- */

static void tool_pump(void);

/* A SET_REPORT in pz.rx (free: the tool device has no personality). */
static void tool_take(u16 len)
{
    const u8 *r = pz.rx;
    u16 n = (u16)(r[16] | r[17] << 8);

    if (len < HEAD_IN || !(r[18] & JM_HID_INPUT)) return;
    if (t.pending || t.sending) {
        refused.tag = r[19];
        refused.client = util_get32(r + 20);
        refused.waiting = 1;
        tool_pump();
        return;
    }
    memcpy(t.report, r, HEAD_IN);
    t.flags = r[18];
    t.tag = r[19];
    t.client = util_get32(r + 20);
    if (n > len - HEAD_IN) n = len - HEAD_IN;
    memcpy(t.data, r + HEAD_IN, n);
    t.n = n;
    t.answer = t.off = 0;
    t.pending = 1;
    events.commands++;
}

/* On EP1 when it is free: a refusal, else the answer's next piece: the
 * client, the tag, the state, u16 offset, IN_DATA bytes. */
static void tool_pump(void)
{
    u8 head[IN_HEAD];
    u16 n = 0, i;

    if (pz.id || pz.in_busy || !u.configuration) return;
    if (refused.waiting) {
        refused.waiting = 0;
        util_put32(head, refused.client);
        head[4] = refused.tag;
        head[5] = TOOL_REFUSED;
        head[6] = head[7] = 0;
    } else if (t.sending && !t.last_out) {
        n = t.answer - t.off < IN_DATA ? (u16)(t.answer - t.off) : IN_DATA;
        util_put32(head, t.client);
        head[4] = t.tag;
        head[5] = t.off + n < t.answer ? TOOL_MORE : TOOL_LAST;
        util_put16(head + 6, t.off);
        t.last_out = head[5] == TOOL_LAST;
    } else {
        return;
    }
    for (i = 0; i < IN_HEAD; i++) USB_FIFO(EP_IN) = head[i];
    for (i = 0; i < IN_DATA; i++) USB_FIFO(EP_IN) = i < n ? t.data[t.off + i] : 0;
    t.off = (u16)(t.off + n);
    t.sent_at = plat_ms();
    pz.in_busy = 1;
    send_in(EP_IN);
}

int plat_usb_tool_waiting(void)
{
    return t.pending;
}

/* The command taken, answered here on the main loop (the interrupt only
 * takes it): what the tools then read. */
void plat_usb_service(void)
{
    u16 answer;

    if (t.sending && pz.in_busy && plat_ms() - t.sent_at > IN_WAIT_MS) {
        plat_lock();
        t.sending = 0;                  /* a piece no one takes: no one is reading */
        plat_unlock();
    }
    if (!t.pending) return;
    answer = (u16)usb_tool_command(t.report, t.data, t.n, ANSWER_MAX);
    plat_lock();
    t.pending = 0;
    t.answer = answer;
    t.off = 0;
    t.sending = 1;
    t.last_out = 0;
    tool_pump();
    plat_unlock();
}

/* ---- the personality's endpoints ------------------------------------------------ */

static void write_take(u8 kind, const u8 *data, u8 n)
{
    struct bt_write *w;
    u8 at = 0;

    if (pz.w_count >= WRITES) return;    /* the loop is behind: dropped */
    w = &pz.writes[(pz.w_head + pz.w_count) % WRITES];
    w->kind = kind;
    w->id = 0;
    if (pz.id->report_ids && n) w->id = data[0], at = 1;
    w->len = (u16)(n - at < BT_WRITE_MAX ? n - at : BT_WRITE_MAX);
    memcpy(w->data, data + at, w->len);
    pz.w_count++;
    pz.event = 1;
}

/* EP1: a message first, else the newest input, when the last went out. */
static void pump(void)
{
    const u8 *d;
    u8 n;

    if (pz.in_busy || !u.configuration) return;
    if (pz.m_count) {
        d = pz.msg[pz.m_head].data;
        n = pz.msg[pz.m_head].len;
        pz.m_head = (u8)((pz.m_head + 1) % MESSAGES);
        pz.m_count--;
    } else if (pz.input_len) {
        d = pz.input;
        n = pz.input_len;
        pz.input_len = 0;
    } else {
        return;
    }
    for (u8 i = 0; i < n; i++) USB_FIFO(EP_IN) = d[i];
    pz.in_busy = 1;
    send_in(EP_IN);
}

/* EP2: the host's output report. */
static void ep_out(void)
{
    u8 n = USB_LEN(EP_OUT), i;

    if (n > REPORT) n = REPORT;
    for (i = 0; i < n; i++) pz.rx[i] = USB_FIFO(EP_OUT);
    USB_TRIGGER = 0x10 << EP_OUT;        /* taken: the next may come */
    if (pz.id) write_take(BT_WRITE_OUTPUT, pz.rx, n);
}

/* The class and standard requests of a personality; 0: the tool device's
 * handling (addresses, status, features) applies. */
static int personality_setup(u8 type, u8 request, u8 value_lo, u8 value_hi, u16 length)
{
    const struct usb_identity *id = pz.id;
    u8 at = id->report_ids ? 1 : 0;
    int n;

    if (type == 0x80 && request == 6) {                  /* GET_DESCRIPTOR */
        if (value_hi == 1) ep0_answer(pz.dev, sizeof(pz.dev));
        else if (value_hi == 2) ep0_answer(pz.cfg, pz.cfg_len);
        else if (value_hi == 3 && value_lo <= 3) ep0_answer(u.buf, string(value_lo, u.buf));
        else if (value_hi == 3 && value_lo == MS_OS_STRING && id->kind == USB_XINPUT) {
            /* Microsoft OS 1.0: "MSFT100" and our vendor request, so Windows
             * asks for the compatible ID that loads its Xbox 360 driver. */
            static const u8 msft[18] = { 18, 3, 'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0,
                                         '0', 0, MS_OS_VENDOR, 0 };
            ep0_answer(msft, sizeof(msft));
        } else ep0_stall();
        return 1;
    }
    if ((type & 0xe0) == 0xc0 && id->kind == USB_XINPUT) {   /* vendor, to the host */
        static const u8 compat[40] = {
            40, 0, 0, 0, 0x00, 0x01, 0x04, 0x00, 1, 0, 0, 0, 0, 0, 0, 0,
            0, 1, 'X', 'U', 'S', 'B', '1', '0', 0, 0,
        };
        static const u8 zero[20];

        /* The compatible ID "XUSB10" (interface 0), as the stock dongle sends
         * it; anything else Windows asks of a wired pad: 20 zero bytes. */
        if (request == MS_OS_VENDOR && u.setup[4] == 4) ep0_answer(compat, sizeof(compat));
        else ep0_answer(zero, sizeof(zero));
        return 1;
    }
    if (type == 0x81 && request == 6 && id->kind != USB_XINPUT) {   /* the interface's */
        if (value_hi == 0x22) ep0_answer(id->hid_descriptor, id->hid_descriptor_len);
        else if (value_hi == 0x21) ep0_answer(pz.cfg + 18, 9);
        else ep0_stall();
        return 1;
    }
    if (type == 0x00 && request == 9) {                  /* SET_CONFIGURATION */
        u.configuration = value_lo;
        pz.in_busy = 0;
        ep0_zlp();
        events.configured = value_lo != 0;
        pz.event = 1;
        return 1;
    }
    if ((type & 0x60) != 0x20 || (type & 0x1f) != 1) return 0;   /* class, interface */
    switch (request) {
    case 1:                                              /* GET_REPORT */
        n = -1;
        if (pz.read && value_hi == 3 && length > at)
            n = pz.read(BT_WRITE_FEATURE, value_lo, 0, u.buf + at,
                        (u16)((length < REPORT ? length : REPORT) - at));
        if (n < 0) {
            ep0_stall();
            return 1;
        }
        if (at) u.buf[0] = value_lo;
        ep0_answer(u.buf, (u16)(n + at));
        return 1;
    case 9:                                              /* SET_REPORT */
        if (!length) {
            ep0_zlp();
            return 1;
        }
        pz.set_kind = value_hi == 3 ? BT_WRITE_FEATURE : BT_WRITE_OUTPUT;
        pz.set_id = value_lo;
        u.receiving = 1;
        u.got = 0;
        u.want = length < REPORT ? length : REPORT;
        return 1;
    case 2:                                              /* GET_IDLE */
        u.buf[0] = 0;
        ep0_answer(u.buf, 1);
        return 1;
    case 0x0a:                                           /* SET_IDLE */
    case 0x0b:                                           /* SET_PROTOCOL */
        ep0_zlp();
        return 1;
    }
    ep0_stall();
    return 1;
}

/* ---- control requests ------------------------------------------------------ */

static void setup(void)
{
    u8 type = u.setup[0], request = u.setup[1];
    u8 value_lo = u.setup[2], value_hi = u.setup[3];
    u16 length = (u16)(u.setup[6] | u.setup[7] << 8);

    if (u.receiving) {
        if (!usb_trace.stop) usb_trace.stop = usb_trace.next + 4;   /* the trace keeps it */
        events.incomplete++;
        events.incomplete_got = u.got;
        events.incomplete_want = u.want;
    }
    u.sending = u.receiving = 0;
    events.setups++;
    if (pz.id && personality_setup(type, request, value_lo, value_hi, length)) return;
    if (type == 0x80 && request == 6) {                  /* GET_DESCRIPTOR */
        if (value_hi == 1) ep0_answer(device_descriptor, sizeof(device_descriptor));
        else if (value_hi == 2) ep0_answer(configuration_descriptor, sizeof(configuration_descriptor));
        else if (value_hi == 3 && value_lo <= 2) ep0_answer(u.buf, string(value_lo, u.buf));
        else ep0_stall();
    } else if (type == 0x81 && request == 6) {           /* the interface's */
        if (value_hi == 0x22) ep0_answer(report_descriptor, sizeof(report_descriptor));
        else if (value_hi == 0x21) ep0_answer(configuration_descriptor + HID_AT, 9);
        else ep0_stall();
    } else if (type == 0x00 && request == 5) {           /* SET_ADDRESS */
        USB_ADDR = value_lo & 0x7f;
        u.address_pending = 1;
        ep0_zlp();
        events.addressed = 1;
    } else if (type == 0x00 && request == 9) {           /* SET_CONFIGURATION */
        u.configuration = value_lo;
        pz.in_busy = 0;                  /* EP1 starts empty */
        ep0_zlp();
        events.configured = value_lo != 0;
    } else if (type == 0x80 && request == 8) {           /* GET_CONFIGURATION */
        ep0_answer(&u.configuration, 1);
    } else if ((type & 0xe0) == 0x80 && request == 0) {  /* GET_STATUS */
        static const u8 zero[2];
        ep0_answer(zero, 2);
    } else if ((type & 0x60) == 0 && (request == 1 || request == 3)) {   /* features */
        ep0_zlp();
    } else if (type == 0x21 && request == 0x0a) {        /* SET_IDLE */
        ep0_zlp();
    } else if (type == 0x21 && request == 9 && value_hi == 3 && length) {   /* SET_REPORT feature */
        u.receiving = 1;
        u.got = 0;
        u.want = length < REPORT ? length : REPORT;   /* stock triggers nothing here */
        events.set_reports++;
    } else {
        ep0_stall();
    }
}

/* An EP0 OUT packet: SET_REPORT data, or a control read's status stage. */
static void ep0_out(u8 n, u8 skip)
{
    if (!n && u.receiving) return;      /* its event came after we took the packet */
    events.out_packets++;
    events.out_last = n;
    if (!u.receiving) events.out_idle++;
    for (u8 i = 0; i < n; i++) {
        u8 b = USB_FIFO(0);
        if (i >= skip && u.receiving && u.got < REPORT) pz.rx[u.got++] = b;
    }
    if (!u.receiving) return;
    if (u.got >= u.want) {
        u.receiving = 0;
        if (!pz.id) tool_take(u.got);
        else if (pz.id->report_ids || pz.set_kind == BT_WRITE_FEATURE)
            write_take(pz.set_kind, pz.rx, (u8)u.got);
        else {                           /* no report ids: the id from the request */
            struct bt_write *w;

            write_take(pz.set_kind, pz.rx, (u8)u.got);
            w = &pz.writes[(pz.w_head + pz.w_count + WRITES - 1) % WRITES];
            if (pz.w_count) w->id = pz.set_id;
        }
    }
    ep0_zlp();
}

/* EP0's FIFO count once the packet is all in: bit 0 of STATUS is set while
 * it is still arriving (a 64-byte packet takes ~43 us at full speed), and a
 * poll that read it then took a part as the packet and lost the rest.
 * Stock reads from its interrupt, late enough not to see this. */
static u8 ep0_settled(void)
{
    u8 n = USB_LEN(0);

    for (int i = 0; i < 40; i++) {      /* up to ~120 us */
        yc_delay_us(3);
        u8 m = USB_LEN(0);
        if (m == n && (n == EP0_SIZE || i >= 2)) break;
        n = m;
    }
    return n;
}

void plat_usb_poll(void)
{
    u8 status = USB_STATUS, sent = USB_SENT;

    if (!(status | (sent & 0x0f))) return;         /* sent bit 4 stays set */
    if (status & 0x80) {
        USB_ADDR = 0;
        memset(&u, 0, sizeof(u));
        events.resets++;
    }
    USB_STATUS = 0xa0;                  /* bit 6 stays: it ends an EP0 packet */
    if (status & 0x0e) USB_STATUS = status & 0x0e;      /* EP1-3 OUT */
    if (status & (1u << EP_OUT)) ep_out();
    if (sent & (1u << EP_IN)) {
        USB_SENT = 1u << EP_IN;
        pz.in_busy = 0;
        if (pz.id) {
            pz.event = 1;
            pump();
        } else {
            if (t.sending && t.last_out) t.sending = 0;   /* the last piece is out */
            tool_pump();
        }
    }
    if (status & 1) {
        u8 n;

        /* Bit 0 comes while the packet is still arriving (the FIFO count
         * growing), bit 6 when it is in.  Then the event is cleared before
         * the FIFO is read: the next packet sets the same bits, and a
         * SET_REPORT's first data packet follows its SETUP unasked. */
        for (u32 t0 = plat_us(); !(status & 0x40) && plat_us() - t0 < 300;)
            status |= USB_STATUS & 0x40;
        USB_STATUS = (status & 0x10 ? 0x11 : 0x01) | 0x40;
        n = ep0_settled();
        if (u.address_pending) {
            USB_ADDR |= 0x80;
            u.address_pending = 0;
        }
        if (status & 0x10) {
            USB_STALL = 0;
            if (n != 8) events.odd_setups++, events.odd_length = n;
            for (int i = 0; i < 8; i++) u.setup[i] = USB_FIFO(0);
            trace(status, sent, n, u.setup);
            setup();
            if (n > 8) ep0_out((u8)(n - 8), 0);         /* the data stage's first packet with it */
        } else {
            trace(status, sent, n, NULL);
            ep0_out(n, 0);
        }
    }
    if (!(status & 1) && (status & 0x8e || sent & 0x0f)) trace(status, sent, 0, NULL);
    if (sent & 0x0f) {
        USB_SENT = sent & 0x0f;
        if ((sent & 1) && u.sending) ep0_next();
    }
}

const struct usb_events *plat_usb_events(void)
{
    return &events;
}

/* ---- the personality's interface ----------------------------------------------- */

static void build(const struct usb_identity *id)
{
    u8 *c = pz.cfg, eps = id->out_size ? 2 : 1, n = 0;
    /* The wired Xbox 360 controller's first interface, as xpad and Windows'
     * xusb22 know it; its 0x21 descriptor names the endpoints. */
    static const u8 xinput[17] = {
        0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x80 | EP_IN, 0x14, 0, 0, 0, 0, 0x13,
        EP_OUT, 0x08, 0, 0,
    };

    memcpy(pz.dev, (const u8[]){ 18, 1, 0x00, 0x02, 0, 0, 0, EP0_SIZE }, 8);
    if (id->kind == USB_XINPUT) pz.dev[4] = pz.dev[5] = pz.dev[6] = 0xff;
    pz.dev[8] = (u8)id->vendor_id, pz.dev[9] = (u8)(id->vendor_id >> 8);
    pz.dev[10] = (u8)id->product_id, pz.dev[11] = (u8)(id->product_id >> 8);
    pz.dev[12] = (u8)id->bcd_device, pz.dev[13] = (u8)(id->bcd_device >> 8);
    pz.dev[14] = id->manufacturer ? 1 : 0;
    pz.dev[15] = id->product ? 2 : 0;
    pz.dev[16] = id->serial ? 3 : 0;
    pz.dev[17] = 1;
    memcpy(c + n, (const u8[]){ 9, 2, 0, 0, 1, 1, 0, 0x80, 0 }, 9), n += 9;
    c[8] = id->max_power ? id->max_power : 50;
    if (id->kind == USB_XINPUT) {
        memcpy(c + n, (const u8[]){ 9, 4, 0, 0, 2, 0xff, 0x5d, 0x01, 0 }, 9), n += 9;
        memcpy(c + n, xinput, sizeof(xinput)), n += sizeof(xinput);
    } else {
        memcpy(c + n, (const u8[]){ 9, 4, 0, 0, 0, 3, 0, 0, 0 }, 9), c[n + 4] = eps, n += 9;
        memcpy(c + n, (const u8[]){ 9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0, 0 }, 9);
        c[n + 7] = (u8)id->hid_descriptor_len, c[n + 8] = (u8)(id->hid_descriptor_len >> 8), n += 9;
    }
    memcpy(c + n, (const u8[]){ 7, 5, 0x80 | EP_IN, 3, 0, 0, 0 }, 7);
    c[n + 4] = id->in_size, c[n + 6] = id->interval ? id->interval : 1, n += 7;
    if (id->out_size) {
        memcpy(c + n, (const u8[]){ 7, 5, EP_OUT, 3, 0, 0, 0 }, 7);
        c[n + 4] = id->out_size, c[n + 6] = id->kind == USB_XINPUT ? 8 : id->interval ? id->interval : 1, n += 7;
    }
    c[2] = n;
    pz.cfg_len = n;
}

/* A personality: off the bus, then back as it (NULL: the tool device).
 * HID and Xbox 360; a Switch 2 one (bulk) stays the tool device. */
void plat_usb_mode(const struct usb_identity *id,
                   int (*read)(u8 kind, u16 id, u16 offset, u8 *out, u16 max))
{
    if (id == pz.id) {
        pz.read = read;
        return;
    }
    if (id && id->kind == USB_HID_BULK) {
        util_log("usb: no bulk (Switch 2) personality on this pad");
        id = NULL;
        if (!pz.id) return;
    }
    __asm__ volatile ("cpsid i" ::: "memory");
    memset(&pz, 0, sizeof(pz));
    if (id) build(id);
    pz.id = id;
    pz.read = read;
    __asm__ volatile ("cpsie i" ::: "memory");
    util_log("usb: %s", id ? (id->product ? id->product : "a personality") : "the tool device");
    plat_usb_init();                     /* off the bus 20 ms: the host enumerates again */
}

int plat_usb_ready(void)
{
    return pz.id && u.configuration;
}

int plat_usb_input_wanted(void)
{
    return plat_usb_ready() && !pz.in_busy && !pz.input_len && !pz.m_count;
}

int plat_usb_input_staged(void)
{
    return pz.input_len != 0;
}

int plat_usb_send_input(u8 id, const u8 *data, u16 len)
{
    u8 at = pz.id && pz.id->report_ids ? 1 : 0;

    if (!plat_usb_ready() || len + at > pz.id->in_size || len + at > REPORT) return 0;
    plat_lock();
    if (at) pz.input[0] = id;
    memcpy(pz.input + at, data, len);
    pz.input_len = (u8)(len + at);
    pump();
    plat_unlock();
    return 1;
}

int plat_usb_send_message(u16 id, const u8 *data, u16 len)
{
    u8 at = pz.id && pz.id->report_ids ? 1 : 0, *m;
    int sent = 0;

    if (!plat_usb_ready() || (id & HOST_BULK) || len + at > pz.id->in_size || len + at > REPORT) return 0;
    plat_lock();
    if (pz.m_count < MESSAGES) {
        u8 k = (u8)((pz.m_head + pz.m_count) % MESSAGES);

        m = pz.msg[k].data;
        if (at) m[0] = (u8)id;
        memcpy(m + at, data, len);
        pz.msg[k].len = (u8)(len + at);
        pz.m_count++;
        pump();
        sent = 1;
    }
    plat_unlock();
    return sent;
}

int plat_usb_take(struct bt_write *w)
{
    int got = 0;

    plat_lock();
    if (pz.w_count) {
        *w = pz.writes[pz.w_head];
        pz.w_head = (u8)((pz.w_head + 1) % WRITES);
        pz.w_count--;
        got = 1;
    }
    plat_unlock();
    return got;
}

/* For the wait loop: a tool command to answer, or a personality's event. */
int plat_usb_wake(void)
{
    int e = t.pending || pz.event;

    pz.event = 0;
    return e;
}

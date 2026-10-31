#include <string.h>
#include "pad.h"
#include "yc3121.h"
#include "usb_yc3121.h"

/* USB on the YC3121: the tool device (a vendor HID interface, commands as
 * feature reports, as platform_telink_usb.c's without input reports), polled
 * from the main loop with the USB interrupt off.
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
 *   0xf6018 + ep: the endpoint's FIFO, a byte per access
 *   0xf6020 + ep: the length of the OUT packet in the FIFO
 *   0xf6026 STATUS (write 1 to clear): bit 0 EP0 OUT or SETUP (bit 4: it is
 *   a SETUP), bits 1-3 EP1-3 OUT, bit 6 toggles while the bus runs (an IN
 *   packet is triggered after it is seen again), bit 7 bus reset
 *   0xf6027 (write 1 to clear): bit n, EP n's IN packet went out
 * Its clock: CLK_CLS bits 13 and 6 cleared (on), 0xc8905 = 0xff,
 * HCLK_CON |= 0x800. */

#define USB_CONFIG     (*(volatile uint8_t *)0xf6000)
#define USB_MASK(n)    (*(volatile uint8_t *)(0xf6001 + (n)))
#define USB_ADDR       (*(volatile uint8_t *)0xf6004)
#define USB_TRIGGER    (*(volatile uint8_t *)0xf6010)
#define USB_STALL      (*(volatile uint8_t *)0xf6011)
#define USB_FIFO(ep)   (*(volatile uint8_t *)(0xf6018 + (ep)))
#define USB_LEN(ep)    (*(volatile uint8_t *)(0xf6020 + (ep)))
#define USB_STATUS     (*(volatile uint8_t *)0xf6026)
#define USB_SENT       (*(volatile uint8_t *)0xf6027)
#define YC_CLK_CLS     (*(volatile uint32_t *)0xf856c)

#define EP0_SIZE   64
#define DATA_MAX   300           /* a report of 320: five EP0 packets */
#define HEAD       20
#define REPORT     (HEAD + DATA_MAX)

enum { TOOL_IDLE, TOOL_BUSY, TOOL_MORE, TOOL_LAST };

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
    0xc0,
};

static const u8 configuration_descriptor[] = {
    9, 2, 34, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 3, 0, 0, 0,           /* HID, no boot protocol */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(report_descriptor), 0,
    7, 5, 0x81, 3, 64, 0, 10,            /* interrupt IN, never sent on */
};
#define HID_AT 18

static struct usb_events events;

static struct {
    u8 setup[8];
    u8 address_pending, configuration;
    u8 receiving;                /* a SET_REPORT's data stage */
    u16 got, want;               /* its bytes so far, of wLength */
    u8 sending;                  /* a control read's data stage */
    u16 at, total;               /* of buf */
    u8 zlp;                      /* it ends with a zero-length packet */
    u8 buf[REPORT];              /* what goes out on EP0 */
} u;

static struct {
    u8 report[REPORT];           /* the host's last SET_REPORT */
    u8 tag, state;
    u16 answer;
    u8 data[DATA_MAX];
} t;

/* ---- the controller -------------------------------------------------------- */

void plat_usb_init(void)
{
    YC_CLK_CLS &= ~(1u << 13);
    *(volatile uint8_t *)0xc8905 = 0xff;
    YC_CLK_CLS &= ~(1u << 6);
    YC_HCLK_CON |= 0x800;
    USB_CONFIG = 0;
    plat_delay_ms(20);                   /* the host sees the loader's device go */
    USB_TRIGGER = 0xc0;
    USB_CONFIG = 0x3c;
    USB_STATUS = 0xff;
    USB_SENT = 0xff;
    USB_ADDR = 0;
    USB_MASK(0) = 0x70;
    USB_MASK(1) = 0xf0;
    USB_MASK(2) = 0xff;
    memset(&u, 0, sizeof(u));
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
        USB_FIFO(0) = u.buf[u.at + i];
    u.at += n;
    if (u.at == u.total && n < EP0_SIZE) u.sending = 0;
    send_in(0);
}

static void ep0_answer(const u8 *data, u16 len)
{
    u16 want = (u16)(u.setup[6] | u.setup[7] << 8);

    if (len > want) len = want;
    if (data != u.buf) memcpy(u.buf, data, len);
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

static void tool_take(u16 len)
{
    u16 n = (u16)(t.report[16] | t.report[17] << 8);

    if (len < HEAD) return;
    t.tag = t.report[19];
    if (n > len - HEAD) n = len - HEAD;
    memcpy(t.data, t.report + HEAD, n);
    t.answer = (u16)usb_tool_command(t.report, t.data, n, DATA_MAX);
    t.state = TOOL_LAST;
    events.commands++;
}

static u16 tool_report(u8 *out)
{
    u16 n = t.state == TOOL_LAST ? t.answer : 0;

    memset(out, 0, REPORT);
    out[0] = t.tag;
    out[1] = t.state;
    out[2] = (u8)n;
    out[3] = (u8)(n >> 8);
    memcpy(out + 4, t.data, n);
    return REPORT;
}

/* ---- control requests ------------------------------------------------------ */

static void setup(void)
{
    u8 type = u.setup[0], request = u.setup[1];
    u8 value_lo = u.setup[2], value_hi = u.setup[3];
    u16 length = (u16)(u.setup[6] | u.setup[7] << 8);

    u.sending = u.receiving = 0;
    events.setups++;
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
    } else if (type == 0xa1 && request == 1 && value_hi == 3) {   /* GET_REPORT feature */
        ep0_answer(u.buf, tool_report(u.buf));
    } else if (type == 0x21 && request == 9 && value_hi == 3 && length) {   /* SET_REPORT feature */
        u.receiving = 1;
        u.got = 0;
        u.want = length < REPORT ? length : REPORT;   /* stock triggers nothing here */
    } else {
        ep0_stall();
    }
}

/* An EP0 OUT packet: SET_REPORT data, or a control read's status stage. */
static void ep0_out(u8 n, u8 skip)
{
    for (u8 i = 0; i < n; i++) {
        u8 b = USB_FIFO(0);
        if (i >= skip && u.receiving && u.got < REPORT) t.report[u.got++] = b;
    }
    if (!u.receiving) return;
    if (u.got >= u.want) {
        u.receiving = 0;
        tool_take(u.got);
    }
    ep0_zlp();
}

void plat_usb_poll(void)
{
    u8 status = USB_STATUS, sent = USB_SENT;

    if (!(status | sent)) return;
    if (status & 0x80) {
        USB_ADDR = 0;
        memset(&u, 0, sizeof(u));
        events.resets++;
    }
    USB_STATUS = 0xe0;
    if (status & 0x0e) USB_STATUS = status & 0x0e;      /* EP1-3 OUT: not used */
    if (status & 1) {
        u8 n = USB_LEN(0);

        if (u.address_pending) {
            USB_ADDR |= 0x80;
            u.address_pending = 0;
        }
        if (status & 0x10) {
            USB_STATUS = 0x11;
            USB_STALL = 0;
            for (int i = 0; i < 8; i++) u.setup[i] = USB_FIFO(0);
            if (n != 8) events.odd_setups++;
            setup();
            if (n > 8) ep0_out((u8)(n - 8), 0);         /* data with it */
        } else {
            USB_STATUS = 1;
            ep0_out(n, 0);
        }
    }
    if (sent & 0x0f) {
        USB_SENT = sent & 0x0f;
        if ((sent & 1) && u.sending) ep0_next();
    }
}

const struct usb_events *plat_usb_events(void)
{
    return &events;
}

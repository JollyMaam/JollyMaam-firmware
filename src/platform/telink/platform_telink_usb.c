#include "sdk.h"
#include "board.h"

/* USB on the B80: the tool device, a vendor HID interface whose commands go
 * to usb.c, as on the Jieli chips (platform_jieli_usb.c): a command is a
 * feature report the host sets, its answer one it gets.  Polled from the
 * wait loop; the USB interrupts stay off so the radio's shared handler never
 * sees them.  Control transfers follow the Telink platform SDK's USB_Demo,
 * with the B80's own sequencing:
 *   - SETUP and STATUS of a no-data OUT request can both be pending at one
 *     poll: SETUP is decoded first, then that STATUS answered.
 *   - The first IN packet of a control read is written at SETUP; DATA or
 *     STATUS flags seen in that same poll must not advance it again.
 *   - A short control read of a multiple of 8 bytes ends with a zero-length
 *     packet (the SDK's reg_usb_sups_cyc_cali workaround).
 * EP0 moves 8 bytes a packet: the report goes in and out a byte at a time,
 * with no buffer of its own; a command's data lands in data[].
 *
 * The report: 16 bytes of command (usb.c's CDB), u16 length, u8 flags (bit
 * 0: the data goes to the pad; bit 1: the answer on input reports, to the
 * u32 client that follows), u8 tag, then the data.  The report is 20 +
 * DATA_MAX bytes: the tools take a device's limit from its report size.
 * With bit 1, the answer goes out on interrupt IN in 64-byte input reports,
 * which every program that has the device open receives: the client, the
 * tag, the state (TOOL_*), u16 offset, 56 bytes of the answer.  REFUSED:
 * a command came while another's answer was waiting or going out.  Without
 * bit 1, the answer is a feature report: the tag, the state, u16 length,
 * the data (as platform_jieli_usb.c's tool device).
 *
 * The device: 16c0:05df, the V-USB shared ID for generic HID, with our
 * manufacturer (pad.h).  The tools tell it apart by the usage page and
 * IDENTIFY.  Its serial number is the flash UID in hex (what IDENTIFY calls
 * the flash ID). */

#define EP_IN     4              /* interrupt IN 0x84 */
#define DATA_MAX  336            /* the largest answer: FIELD (328); a remap slot READ is 283 with 8 rules */
#define HEAD      20
#define HEAD_IN   24             /* with the client */
#define REPORT    (HEAD + DATA_MAX)
#define IN_HEAD   8
#define IN_DATA   (64 - IN_HEAD)
#define IN_WAIT_MS 1000          /* a piece no one takes: the answer is dropped */

enum { TOOL_IDLE, TOOL_BUSY, TOOL_MORE, TOOL_LAST, TOOL_REFUSED };
enum { TOOL_OUT = 1, TOOL_INPUT = 2 };

static const u8 device_descriptor[] = {
    18, 1, 0x10, 0x01, 0, 0, 0, 8,
    (u8)USB_OWN_VID, USB_OWN_VID >> 8, (u8)USB_OWN_HID_PID, USB_OWN_HID_PID >> 8,
    0x00, 0x01,                          /* bcdDevice 1.00 */
    1, 2, 3, 1,                          /* manufacturer 1, product 2, serial 3 */
};

static const u8 report_descriptor[] = {
    0x06, 0xc2, 0xff,                    /* usage page 0xffc2 */
    0x09, 0x01,                          /* usage 1 */
    0xa1, 0x01,                          /* application collection */
    0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x96, (u8)REPORT, REPORT >> 8,
    0x09, 0x01, 0xb1, 0x02,              /* feature: data, variable */
    0x95, 64,
    0x09, 0x01, 0x81, 0x02,              /* input: data, variable */
    0xc0,
};

static const u8 configuration_descriptor[] = {
    9, 2, 34, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 3, 0, 0, 0,           /* HID, no boot protocol */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(report_descriptor), 0,
    7, 5, 0x80 | EP_IN, 3, 64, 0, 1,
};
#define HID_AT 18                        /* the HID descriptor in it */

static const u8 language[] = { 4, 3, 0x09, 0x04 };

/* ---- the command ----------------------------------------------------------- */

static struct {
    u8 cdb[16];
    u16 length;                  /* the host's: of its data, or of the answer it wants */
    u8 flags, tag;
    u32 client;
    u8 state;
    u8 pending;                  /* for usb.c, on the control loop */
    u8 taken;                    /* the host read the answer */
    u8 busy;                     /* a command taken, its answer not all out */
    u8 sending;                  /* on input reports, from off */
    u8 drop;                     /* the SET_REPORT coming in is refused */
    u16 answer;                  /* its length */
    u16 off;
    u32 sent_at;
    u8 data[DATA_MAX];
} t;

/* A refused command's sender (one: a second waits for its timeout). */
static struct {
    u8 flags, tag, waiting;
    u32 client;
} refused;

#define T_HEAD ((t.flags & TOOL_INPUT) ? HEAD_IN : HEAD)

/* A byte of a SET_REPORT, the k-th; while a command is taken, only its
 * sender is kept. */
static void tool_put(u16 k, u8 b)
{
    if (k == 0) t.drop = t.busy;
    if (t.drop) {
        if (k == 18) refused.flags = b;
        else if (k == 19) refused.tag = b;
        else if (k >= 20 && k < 24) refused.client = (refused.client & ~(0xffu << 8 * (k - 20))) | (u32)b << 8 * (k - 20);
        return;
    }
    if (k < 16) t.cdb[k] = b;
    else if (k == 16) t.length = b;
    else if (k == 17) t.length |= (u16)(b << 8);
    else if (k == 18) t.flags = b;
    else if (k == 19) t.tag = b;
    else if (k < T_HEAD) t.client = (t.client & ~(0xffu << 8 * (k - 20))) | (u32)b << 8 * (k - 20);
    else if (k - T_HEAD < DATA_MAX) t.data[k - T_HEAD] = b;
}

/* On interrupt IN, when it is free: a refusal, else the answer's next piece. */
static void tool_pump(void)
{
    u8 head[IN_HEAD];
    u16 n = 0, i;

    if (usbhw_is_ep_busy(EP_IN)) return;
    if (t.sending && t.off >= t.answer && t.off) {
        t.sending = t.busy = 0;  /* the last piece is out */
        t.taken = 1;
    }
    if (refused.waiting) {
        refused.waiting = 0;
        util_put32(head, refused.client);
        head[4] = refused.tag;
        head[5] = TOOL_REFUSED;
        head[6] = head[7] = 0;
    } else if (t.sending && (t.off < t.answer || !t.off)) {
        n = t.answer - t.off < IN_DATA ? t.answer - t.off : IN_DATA;
        util_put32(head, t.client);
        head[4] = t.tag;
        head[5] = t.off + n < t.answer ? TOOL_MORE : TOOL_LAST;
        util_put16(head + 6, t.off);
        if (!t.answer) t.off = 1;        /* nothing to send but LAST */
    } else {
        return;
    }
    usbhw_reset_ep_ptr(EP_IN);
    for (i = 0; i < IN_HEAD; i++) usbhw_write_ep_data(EP_IN, head[i]);
    for (i = 0; i < IN_DATA; i++) usbhw_write_ep_data(EP_IN, i < n ? t.data[t.off + i] : 0);
    if (head[5] != TOOL_REFUSED && t.answer) t.off = (u16)(t.off + n);
    if (head[5] != TOOL_REFUSED) t.sent_at = plat_ms();
    usbhw_data_ep_ack(EP_IN);
}

/* The k-th byte of the answer the host gets. */
static u8 tool_byte(u16 k)
{
    u16 n = t.state == TOOL_LAST ? t.answer : 0;

    if (k == 0) return t.tag;
    if (k == 1) return t.state;
    if (k == 2) return (u8)n;
    if (k == 3) return (u8)(n >> 8);
    return k - 4 < n ? t.data[k - 4] : 0;
}

/* ---- the controller -------------------------------------------------------- */

enum { FROM_POINTER, FROM_STRING, FROM_TOOL };

static struct {
    u8 configuration;
    u8 stall;                    /* the control request is refused */
    u8 no_data_out;
    u8 receiving;                /* a SET_REPORT's data stage */
    u8 from, string;             /* what an answer's bytes come from */
    const u8 *response;
    u16 at;                      /* the next byte, of a string or the report */
    u16 remaining, total, host_length;
    u8 small[2];
} u;

static struct {
    u8 present, raw;
    u32 changed;
} power;
#define USB_POWER_DEBOUNCE_MS 20

/* String descriptors, a byte at a time as they go out: manufacturer,
 * product (board_name) or serial (the flash UID in hex). */
static u16 string_chars(u8 index)
{
    return index == 1 ? (u16)strlen(USB_MANUFACTURER)
         : index == 2 ? (u16)(strlen(board_name) + sizeof(USB_PRODUCT_TAIL) - 1) : 32;
}

static u8 string_byte(u8 index, u16 k)
{
    static const char hex[] = "0123456789abcdef";
    u16 c = (u16)((k - 2) / 2);
    u8 b;

    if (k == 0) return (u8)(2 + 2 * string_chars(index));
    if (k == 1) return 3;
    if (k & 1) return 0;
    if (index == 1) return (u8)USB_MANUFACTURER[c];
    if (index == 2) return (u8)(c < strlen(board_name) ? board_name[c] : USB_PRODUCT_TAIL[c - strlen(board_name)]);
    b = plat_unique_id()[c / 2];
    return (u8)hex[c & 1 ? b & 15 : b >> 4];
}

static void ep0_packet(void)
{
    u16 n = u.remaining < 8 ? u.remaining : 8, i;

    usbhw_reset_ctrl_ep_ptr();
    for (i = 0; i < n; i++)
        usbhw_write_ctrl_ep_data(u.from == FROM_STRING ? string_byte(u.string, u.at++) :
                                 u.from == FROM_TOOL ? tool_byte(u.at++) : *u.response++);
    u.remaining -= n;
    if (u.from == FROM_TOOL && !u.remaining && t.state == TOOL_LAST) t.taken = 1;
    reg_usb_sups_cyc_cali = !n && !(u.total & 7) && u.total < u.host_length ? 0x18 : 0x38;
}

static void answer_clock(void);

/* A packet of a SET_REPORT's data; past the header, a command too long for
 * data[] is refused. */
static void ep0_receive(void)
{
    u16 n = u.remaining < 8 ? u.remaining : 8, i;

    usbhw_reset_ctrl_ep_ptr();
    for (i = 0; i < n; i++) tool_put(u.at++, usbhw_read_ctrl_ep_data());
    u.remaining -= n;
    if (!t.drop && u.at >= T_HEAD && (t.flags & TOOL_OUT) && t.length > DATA_MAX) u.stall = 1;
    if (!u.remaining && !u.stall) {
        u.receiving = 0;
        if (t.drop) {
            if (u.at >= HEAD_IN && (refused.flags & TOOL_INPUT)) refused.waiting = 1;
        } else if (u.at >= T_HEAD) {
            t.state = TOOL_BUSY;
            t.busy = t.pending = 1;
            /* the clock at once: padtool maps the pad's clock onto its own
             * by round trips, which a wait for the control loop skews */
            if (t.cdb[0] == 0xf5 && t.cdb[1] == 0x05 && !(t.flags & TOOL_OUT)) answer_clock();
        }
    }
}

static void ep0_setup(void)
{
    u8 type, request;
    u16 value, index, length;

    reg_usb_sups_cyc_cali = 0x38;
    usbhw_reset_ctrl_ep_ptr();
    type = usbhw_read_ctrl_ep_data();
    request = usbhw_read_ctrl_ep_data();
    value = usbhw_read_ctrl_ep_u16();
    index = usbhw_read_ctrl_ep_u16();
    length = usbhw_read_ctrl_ep_u16();
    u.stall = 0;
    u.receiving = 0;
    u.response = 0;
    u.from = FROM_POINTER;
    u.at = 0;
    u.remaining = 0;
    if (type == 0x80 && request == 6) {          /* GET_DESCRIPTOR */
        switch (value >> 8) {
        case 1:
            u.response = device_descriptor;
            u.remaining = sizeof(device_descriptor);
            break;
        case 2:
            u.response = configuration_descriptor;
            u.remaining = sizeof(configuration_descriptor);
            break;
        case 3:
            if ((value & 255) == 0) {
                u.response = language;
                u.remaining = sizeof(language);
            } else if ((value & 255) >= 1 && (value & 255) <= 3 && index == 0x409) {
                u.from = FROM_STRING;
                u.string = (u8)value;
                u.remaining = (u16)(2 + 2 * string_chars(u.string));
            } else {
                u.stall = 1;
            }
            break;
        default:
            u.stall = 1;
            break;
        }
        if ((value >> 8) < 3 && ((value & 255) || index)) u.stall = 1;
    } else if (type == 0x81 && request == 6 && !index && (value == 0x2100 || value == 0x2200)) {
        u.response = value == 0x2100 ? configuration_descriptor + HID_AT : report_descriptor;
        u.remaining = value == 0x2100 ? 9 : sizeof(report_descriptor);
    } else if (type == 0 && request == 5 && value < 128 && !index && !length) {
        /* SET_ADDRESS: the hardware takes it after the status stage. */
    } else if (type == 0 && request == 9 && value <= 1 && !index && !length) {
        u.configuration = (u8)value;
    } else if (type == 0x80 && request == 8 && !value && !index && length == 1) {
        u.small[0] = u.configuration;
        u.response = u.small;
        u.remaining = 1;
    } else if ((type == 0x80 || type == 0x81 || type == 0x82) && request == 0 && !value && length == 2) {
        u8 valid = type == 0x80 ? !index : type == 0x81 ? (!index && u.configuration) :
                   (index == 0 || index == 0x80 || (u.configuration && index == (0x80 | EP_IN)));

        if (!valid) {
            u.stall = 1;
        } else {
            u.small[0] = u.small[1] = 0;
            u.response = u.small;
            u.remaining = 2;
        }
    } else if (type == 0x81 && request == 10 && !value && !index && length == 1 && u.configuration) {
        u.small[0] = 0;
        u.response = u.small;
        u.remaining = 1;
    } else if (type == 1 && request == 11 && !value && !index && !length && u.configuration) {
        /* SET_INTERFACE 0 */
    } else if (type == 2 && request == 1 && !value && !length && u.configuration && index == (0x80 | EP_IN)) {
        reg_usb_ep_ctrl(EP_IN) = 0;              /* CLEAR_FEATURE halt: it never halts */
    } else if (type == 0xa1 && request == 1 && value == 0x0300 && !index && u.configuration) {
        u.from = FROM_TOOL;                      /* GET_REPORT, feature */
        u.remaining = REPORT;
    } else if (type == 0x21 && request == 9 && value == 0x0300 && !index && length && length <= REPORT &&
               u.configuration) {
        u.receiving = 1;                         /* SET_REPORT, feature: its data next */
        u.remaining = length;
    } else if (type == 0x21 && (request == 10 || request == 11) && !index && !length && u.configuration) {
        /* SET_IDLE, SET_PROTOCOL */
    } else if (type == 0xa1 && request == 2 && !index && length == 1 && u.configuration) {
        u.small[0] = 0;                          /* GET_IDLE */
        u.response = u.small;
        u.remaining = 1;
    } else {
        u.stall = 1;
    }
    if (!u.receiving && u.remaining > length) u.remaining = length;
    u.total = u.remaining;
    u.host_length = length;
    if (!u.stall && (type & 0x80)) ep0_packet();
    else usbhw_reset_ctrl_ep_ptr();      /* reading SETUP moved the FIFO pointer */
    u.no_data_out = !(type & 0x80) && !length;
    usbhw_write_ctrl_ep_ctrl(u.stall ? FLD_EP_DAT_STALL : FLD_EP_DAT_ACK);
}

static void ep0_status(void)
{
    reg_usb_sups_cyc_cali = 0x38;
    usbhw_write_ctrl_ep_ctrl(u.stall ? FLD_EP_STA_STALL : FLD_EP_STA_ACK);
}

void plat_usb_init(void)
{
    memset(&u, 0, sizeof(u));
    power.present = power.raw = (u8)board_usb_present();
    power.changed = plat_ms();
    usb_dp_pullup_en(0);
    reg_clk_en0 |= FLD_CLK0_USB_EN;
    reg_rst0 |= FLD_RST0_USB;
    reg_rst0 &= ~FLD_RST0_USB;
    /* Standard requests by hand; address, interface, status and sync by the hardware. */
    reg_ctrl_ep_irq_mode = FLD_CTRL_EP_AUTO_ADDR | FLD_CTRL_EP_AUTO_INTF | FLD_CTRL_EP_AUTO_STA |
                           FLD_CTRL_EP_AUTO_SYN;
    reg_usb_ep_en = BIT(EP_IN);
    reg_usb_ep_buf_addr(EP_IN) = 0x40;
    reg_usb_ep_max_size = 64 >> 3;
    reg_usb_ep_ctrl(EP_IN) = 0;
    irq_disable_type(FLD_IRQ_USB_RST_EN | FLD_IRQ_USB_PWDN_EN | FLD_IRQ_EP0_SETUP_EN |
                     FLD_IRQ_EP0_DAT_EN | FLD_IRQ_EP0_STA_EN | FLD_IRQ_EP_DATA_EN);
    usb_set_pin(1);              /* attaches: the D+ pull-up */
    if (!power.present) usb_dp_pullup_en(0);
}

/* USB power on the connector, debounced: what attaches and detaches the
 * tool device, and what the controller sees.  The raw pin is only for the
 * power-off path's last look (board_usb_present). */
int plat_usb_present(void)
{
    return power.present;
}

int plat_usb_configured(void)
{
    return u.configuration != 0;
}

static void control(void)
{
    u8 irq;

    if (irq_get_src() & FLD_IRQ_USB_RST_EN) {
        irq_clr_sel_src(FLD_IRQ_USB_RST_EN);
        memset(&u, 0, sizeof(u));
        reg_usb_ep_ctrl(EP_IN) = 0;
        t.sending = t.busy = t.pending = refused.waiting = 0;
    }
    irq = usbhw_get_ctrl_ep_irq();
    if (irq & FLD_CTRL_EP_IRQ_SETUP) {
        usbhw_clr_ctrl_ep_irq(irq & (FLD_CTRL_EP_IRQ_SETUP | FLD_CTRL_EP_IRQ_DATA));
        ep0_setup();
        if (irq & FLD_CTRL_EP_IRQ_STA) {
            usbhw_clr_ctrl_ep_irq(FLD_CTRL_EP_IRQ_STA);
            if (u.no_data_out) ep0_status();
        }
        return;
    }
    if (irq & FLD_CTRL_EP_IRQ_DATA) {
        usbhw_clr_ctrl_ep_irq(FLD_CTRL_EP_IRQ_DATA);
        if (u.receiving && !u.stall) ep0_receive();
        else if (!u.stall) ep0_packet();
        usbhw_write_ctrl_ep_ctrl(u.stall ? FLD_EP_DAT_STALL : FLD_EP_DAT_ACK);
    }
    if (irq & FLD_CTRL_EP_IRQ_STA) {
        usbhw_clr_ctrl_ep_irq(FLD_CTRL_EP_IRQ_STA);
        ep0_status();
    }
}

u32 plat_usb_poll(void)
{
    u8 raw = (u8)board_usb_present();
    u32 event = 0;

    if (raw != power.raw) {
        power.raw = raw;
        power.changed = plat_ms();
    }
    if (raw != power.present && plat_ms() - power.changed >= USB_POWER_DEBOUNCE_MS) {
        if (raw) plat_usb_init();
        else {
            power.present = 0;
            usb_dp_pullup_en(0);
            memset(&u, 0, sizeof(u));
        }
        util_log("usb: power %s", raw ? "present" : "gone");
        event = EV_USB;
    }
    if (!power.present) return event;
    control();
    if (u.configuration) tool_pump();
    if (t.sending && usbhw_is_ep_busy(EP_IN) && plat_ms() - t.sent_at > IN_WAIT_MS)
        t.sending = t.busy = 0;  /* a piece no one takes: no one is reading */
    return event | (t.pending ? EV_USB : 0);
}

static void answered(void);

/* A command waiting: usb.c answers it here, on the control loop.  An
 * answer is what the host asked for (at most DATA_MAX), zero-filled. */
void plat_usb_service(void)
{
    u16 max;

    if (!t.pending) return;
    t.pending = 0;
    t.taken = 0;
    if (t.flags & TOOL_OUT) {
        usb_command(t.cdb, t.data, t.length, 1);
        t.answer = 0;
    } else {
        max = t.length < DATA_MAX ? t.length : DATA_MAX;
        memset(t.data, 0, sizeof(t.data));
        usb_command(t.cdb, t.data, max, 0);
        t.answer = max;
    }
    answered();
}

/* CLOCK (usb.c) without the control loop: the microsecond clock. */
static void answer_clock(void)
{
    t.pending = 0;
    t.taken = 0;
    t.answer = t.length < DATA_MAX ? t.length : DATA_MAX;
    memset(t.data, 0, sizeof(t.data));
    if (t.answer >= 4) util_put32(t.data, plat_us());
    answered();
}

static void answered(void)
{
    if (t.flags & TOOL_INPUT) {
        t.state = TOOL_IDLE;     /* GET_REPORT shows none of it */
        t.off = 0;
        t.sending = 1;
        t.sent_at = plat_ms();
    } else {
        t.state = TOOL_LAST;
        t.busy = 0;
    }
}

/* The answer to the last command out (read by the host, 250 ms at most),
 * then ms more of answering the host. */
void plat_usb_settle(u32 ms)
{
    u32 start = plat_ms();

    while (u.configuration && ((t.state == TOOL_LAST && !t.taken) || t.sending) && plat_ms() - start < 250)
        wd_clear(), plat_usb_poll();
    start = plat_ms();
    while (plat_ms() - start < ms) wd_clear(), plat_usb_poll();
}

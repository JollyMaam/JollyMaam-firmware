#include "sdk.h"
#include "board.h"

/* USB on the B80: the tool device, mass storage (bulk only) with no medium,
 * whose vendor commands go to usb.c.  Polled from the wait loop; the USB
 * interrupts stay off so the radio's shared handler never sees them.
 * Control transfers follow the Telink platform SDK's USB_Demo, with the
 * B80's own sequencing:
 *   - SETUP and STATUS of a no-data OUT request can both be pending at one
 *     poll: SETUP is decoded first, then that STATUS answered.
 *   - The first IN packet of a control read is written at SETUP; DATA or
 *     STATUS flags seen in that same poll must not advance it again.
 *   - A short control read of a multiple of 8 bytes ends with a zero-length
 *     packet (the SDK's reg_usb_sups_cyc_cali workaround).
 * A vendor command may bring data (WRITE): it is collected first, up to
 * DATA_MAX, then handed to usb.c with the command.
 * Bulk IN never stalls for a short answer: it is filled with zeros up to the
 * host's length, and the CSW residue counts the fill.  A CLEAR_FEATURE on a
 * really stalled bulk IN endpoint is known to fail on this controller
 * (EPROTO on the host), so nothing here relies on it.
 *
 * The device: 16c0:27df, the V-USB shared ID for mass storage.  The tools
 * tell it apart by INQUIRY's C2PD and IDENTIFY, not by the ID.  Its serial
 * number is the flash UID in hex (what IDENTIFY calls the flash ID), with
 * no prefix: V-USB's rules want one starting the serial on this ID; we
 * chose a bare UID. */

#define VENDOR_OP 0xf5
#define EP_IN     4              /* bulk IN 0x84 */
#define EP_OUT    5              /* bulk OUT 0x05 */
#define DATA_MAX  336            /* the largest answer: FIELD's longest (328), LOG, IDENTIFY */

static const u8 device_descriptor[] = {
    18, 1, 0x10, 0x01, 0, 0, 0, 8,
    0xc0, 0x16, 0xdf, 0x27,              /* 16c0:27df */
    0x00, 0x01,                          /* bcdDevice 1.00 */
    0, 2, 3, 1,                          /* no manufacturer, product 2, serial 3 */
};

static const u8 configuration_descriptor[] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,        /* mass storage, SCSI, bulk only */
    7, 5, 0x80 | EP_IN, 2, 64, 0, 0,
    7, 5, EP_OUT, 2, 64, 0, 0,
};

static const u8 language[] = { 4, 3, 0x09, 0x04 };

/* ---- SCSI and bulk-only transport ---------------------------------------- */

enum { MSC_CBW, MSC_COMMAND, MSC_DATA_IN, MSC_DATA_OUT, MSC_CSW, MSC_CSW_ACK, MSC_LOCKED };

static struct {
    u8 state;
    u8 halt_in, halt_out;
    u8 recovery;                 /* after a bad CBW: until a class reset and both clears */
    u8 reset_seen;
    u8 tag[4];
    u8 status;
    u8 cdb[16];
    u32 host;                    /* the host's length */
    u32 relevant;                /* what the answer has; the rest of host is fill */
    u32 sent;
    u32 got;                     /* host to pad: bytes received */
    u8 from_host;                /* the vendor command's data came from the host */
    u8 key, asc, ascq;
    u8 data[DATA_MAX];
} m;

static void sense(u8 key, u8 asc, u8 ascq)
{
    m.key = key;
    m.asc = asc;
    m.ascq = ascq;
    m.status = 1;
}

/* The standard commands, answered at once; 0xff: a vendor command for usb.c. */
static u32 scsi(void)
{
    const u8 *c = m.cdb;
    u32 n = 0, i;

    memset(m.data, 0, sizeof(m.data));
    switch (c[0]) {
    case 0x12:                           /* INQUIRY: the model, and C2PD for the tools */
        if (c[1] & ~1 || ((c[1] & 1) == 0 && c[2])) {
            sense(5, 0x24, 0);
            break;
        }
        if (c[1] & 1) {
            m.data[1] = c[2];
            if (c[2] == 0) {
                m.data[3] = 2;
                m.data[5] = 0x80;
                n = 6;
            } else if (c[2] == 0x80) {
                m.data[3] = 16;
                for (i = 0; i < 8; i++) {
                    m.data[4 + 2 * i] = "0123456789abcdef"[plat_unique_id()[i] >> 4];
                    m.data[5 + 2 * i] = "0123456789abcdef"[plat_unique_id()[i] & 15];
                }
                n = 20;
            } else {
                sense(5, 0x24, 0);
            }
        } else {
            m.data[1] = 0x80;
            m.data[2] = 2;
            m.data[3] = 2;
            m.data[4] = 35;
            memset(m.data + 8, ' ', 24);
            for (i = 0; i < 16 && board_name[i]; i++) m.data[16 + i] = (u8)board_name[i];
            memcpy(m.data + 32, "1.00C2PD", 8);
            n = 40;
        }
        if (n > c[4]) n = c[4];
        break;
    case 0x03:                           /* REQUEST SENSE, fixed format */
        m.data[0] = 0x70;
        m.data[2] = m.key;
        m.data[7] = 10;
        m.data[12] = m.asc;
        m.data[13] = m.ascq;
        n = c[4] < 18 ? c[4] : 18;
        if (n) m.key = m.asc = m.ascq = 0;
        break;
    case 0x00: case 0x25: case 0x23: case 0x28: case 0x08:
        sense(2, 0x3a, 0);               /* NOT READY: medium not present */
        break;
    case 0x1b: case 0x1e:                /* START STOP UNIT, PREVENT ALLOW: nothing to do */
        break;
    case 0x1a:                           /* MODE SENSE (6): write protected */
        if ((c[2] & 0x3f) != 0x3f && (c[2] & 0x3f) != 0) {
            sense(5, 0x24, 0);
            break;
        }
        m.data[0] = 3;
        m.data[2] = 0x80;
        n = c[4] < 4 ? c[4] : 4;
        break;
    case 0x5a:                           /* MODE SENSE (10) */
        m.data[1] = 6;
        m.data[3] = 0x80;
        n = 8;
        if (n > ((u32)c[7] << 8 | c[8])) n = (u32)c[7] << 8 | c[8];
        break;
    case VENDOR_OP:
        return 0xff;
    default:
        sense(5, 0x20, 0);               /* invalid command */
        break;
    }
    return n;
}

static void bot_bus_reset(void)
{
    memset(&m, 0, sizeof(m) - sizeof(m.data));
}

static void bot_invalid(void)
{
    m.state = MSC_LOCKED;
    m.halt_in = m.halt_out = m.recovery = 1;
    m.reset_seen = 0;
}

static void bot_class_reset(void)
{
    m.state = MSC_CBW;
    m.sent = m.relevant = m.host = 0;
    m.reset_seen = 1;            /* halts and toggles stay until CLEAR_FEATURE */
}

static void bot_clear_halt(int in)
{
    if (m.recovery && !m.reset_seen) return;
    if (in) m.halt_in = 0;
    else m.halt_out = 0;
    if (m.reset_seen && !m.halt_in && !m.halt_out) m.recovery = 0;
}

/* The data phase of an answer of n bytes. */
static void bot_answer(u32 n)
{
    if (!m.host) {
        if (n) m.status = 2;
        m.state = MSC_CSW;
        return;
    }
    if (n > m.host) {
        n = m.host;
        m.status = 2;
    }
    m.relevant = n;
    m.sent = 0;
    m.state = MSC_DATA_IN;
}

static void bot_receive(const u8 *p, u8 len)
{
    u32 n;

    if (m.state != MSC_CBW || m.recovery || len != 31 || p[0] != 'U' || p[1] != 'S' ||
        p[2] != 'B' || p[3] != 'C' || p[12] & 0x7f || p[13] || !p[14] || p[14] > 16) {
        bot_invalid();
        return;
    }
    memcpy(m.tag, p + 4, 4);
    m.host = p[8] | (u32)p[9] << 8 | (u32)p[10] << 16 | (u32)p[11] << 24;
    m.status = 0;
    memset(m.cdb, 0, sizeof(m.cdb));
    memcpy(m.cdb, p + 15, p[14]);
    m.from_host = 0;
    if (m.host && !(p[12] & 0x80) && m.cdb[0] == VENDOR_OP && m.host <= DATA_MAX) {
        m.got = 0;                       /* a vendor command's data first (WRITE) */
        m.from_host = 1;
        memset(m.data, 0, sizeof(m.data));
        m.state = MSC_DATA_OUT;
        return;
    }
    if (m.host && !(p[12] & 0x80)) {
        /* Host to pad: only vendor commands take data.  Stall it. */
        sense(5, 0x24, 0);
        m.status = 2;
        m.halt_out = 1;
        m.state = MSC_CSW;
        return;
    }
    n = scsi();
    if (n == 0xff) {
        m.state = MSC_COMMAND;           /* for usb.c, on the control loop */
        return;
    }
    bot_answer(n);
}

/* A packet of a vendor command's data from the host. */
static void bot_data_out(const u8 *p, u8 len)
{
    if (m.got + len > m.host) {
        bot_invalid();
        return;
    }
    memcpy(m.data + m.got, p, len);
    m.got += len;
    if (m.got == m.host || len < 64) m.state = MSC_COMMAND;
}

/* The next bulk IN packet, when the endpoint is idle; 0: none. */
static u8 bot_next_in(u8 *packet)
{
    u32 n, i;

    if (m.halt_in || m.recovery || m.state == MSC_LOCKED) return 0;
    if (m.state == MSC_DATA_IN) {
        if (m.sent < m.host) {
            n = m.host - m.sent;
            if (n > 64) n = 64;
            for (i = 0; i < n; i++, m.sent++)
                packet[i] = m.sent < m.relevant && m.sent < DATA_MAX ? m.data[m.sent] : 0;
            return (u8)n;
        }
        m.state = MSC_CSW;
    }
    if (m.state == MSC_CSW) {
        u32 residue = m.host - m.relevant;

        memcpy(packet, "USBS", 4);
        memcpy(packet + 4, m.tag, 4);
        util_put32(packet + 8, residue);
        packet[12] = m.status;
        m.state = MSC_CSW_ACK;
        return 13;
    }
    return 0;
}

static void bot_in_acked(void)
{
    if (m.state != MSC_CSW_ACK) return;
    if (m.status == 2) {
        m.state = MSC_LOCKED;
        m.recovery = 1;
        m.reset_seen = 0;
    } else {
        m.state = MSC_CBW;
    }
}

/* ---- the controller -------------------------------------------------------- */

static struct {
    u8 configuration;
    u8 in_flight, in_toggle, out_armed;
    u8 stall;                    /* the control request is refused */
    u8 no_data_out;
    u8 resume_bulk;              /* bulk IN restarts after CLEAR_FEATURE's status stage */
    const u8 *response;
    u8 string;                   /* instead of response: string descriptor 2 or 3 */
    u16 at;                      /* its next byte */
    u16 remaining, total, host_length;
    u8 small[2];
} u;

static struct {
    u8 present, raw;
    u32 changed;
} power;
#define USB_POWER_DEBOUNCE_MS 20

static void bulk_reset(int bus)
{
    u.in_flight = 0;
    u.resume_bulk = 0;
    u.out_armed = 0;
    if (bus) {
        bot_bus_reset();
        u.in_toggle = 0;
        reg_usb_ep_ctrl(EP_IN) = 0;
        reg_usb_ep_ctrl(EP_OUT) = 0;
    } else {
        bot_class_reset();
        reg_usb_ep_ctrl(EP_IN) &= ~FLD_USB_EP_BUSY;
        reg_usb_ep_ctrl(EP_OUT) &= ~FLD_USB_EP_BUSY;
    }
    reg_usb_irq = BIT(EP_IN) | BIT(EP_OUT);
}

/* String descriptors, a byte at a time as they go out: product (board_name)
 * or serial (the flash UID in hex). */
static u16 string_chars(u8 index)
{
    return index == 2 ? (u16)strlen(board_name) : 32;
}

static u8 string_byte(u8 index, u16 k)
{
    static const char hex[] = "0123456789abcdef";
    u16 c = (u16)((k - 2) / 2);
    u8 b;

    if (k == 0) return (u8)(2 + 2 * string_chars(index));
    if (k == 1) return 3;
    if (k & 1) return 0;
    if (index == 2) return (u8)board_name[c];
    b = plat_unique_id()[c / 2];
    return (u8)hex[c & 1 ? b & 15 : b >> 4];
}

static void ep0_packet(void)
{
    u16 n = u.remaining < 8 ? u.remaining : 8, i;

    usbhw_reset_ctrl_ep_ptr();
    for (i = 0; i < n; i++) usbhw_write_ctrl_ep_data(u.string ? string_byte(u.string, u.at++) : *u.response++);
    u.remaining -= n;
    reg_usb_sups_cyc_cali = !n && !(u.total & 7) && u.total < u.host_length ? 0x18 : 0x38;
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
    u.response = 0;
    u.string = 0;
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
            } else if (((value & 255) == 2 || (value & 255) == 3) && index == 0x409) {
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
    } else if (type == 0 && request == 5 && value < 128 && !index && !length) {
        /* SET_ADDRESS: the hardware takes it after the status stage. */
    } else if (type == 0 && request == 9 && value <= 1 && !index && !length) {
        u.configuration = (u8)value;
        bulk_reset(1);
    } else if (type == 0x80 && request == 8 && !value && !index && length == 1) {
        u.small[0] = u.configuration;
        u.response = u.small;
        u.remaining = 1;
    } else if ((type == 0x80 || type == 0x81 || type == 0x82) && request == 0 && !value && length == 2) {
        u8 valid = type == 0x80 ? !index : type == 0x81 ? (!index && u.configuration) :
                   (index == 0 || index == 0x80 ||
                    (u.configuration && (index == (0x80 | EP_IN) || index == EP_OUT)));

        if (!valid) {
            u.stall = 1;
        } else {
            u.small[0] = type == 0x82 ? (index == (0x80 | EP_IN) ? m.halt_in : index == EP_OUT ? m.halt_out : 0) : 0;
            u.small[1] = 0;
            u.response = u.small;
            u.remaining = 2;
        }
    } else if (type == 0x81 && request == 10 && !value && !index && length == 1 && u.configuration) {
        u.small[0] = 0;
        u.response = u.small;
        u.remaining = 1;
    } else if (type == 1 && request == 11 && !value && !index && !length && u.configuration) {
        bulk_reset(1);                           /* SET_INTERFACE 0 */
    } else if (type == 2 && (request == 1 || request == 3) && !value && !length && u.configuration &&
               (index == (0x80 | EP_IN) || index == EP_OUT)) {
        if (request == 3) {                      /* SET_FEATURE halt */
            if (index == EP_OUT) {
                m.halt_out = 1;
                u.out_armed = 0;
            } else {
                m.halt_in = 1;
                u.in_flight = 0;
                reg_usb_ep_ctrl(EP_IN) = FLD_USB_EP_STALL;
            }
        } else if (index == EP_OUT) {            /* CLEAR_FEATURE halt */
            bot_clear_halt(0);
            u.out_armed = 0;
            reg_usb_irq = BIT(EP_OUT);
            reg_usb_ep_ctrl(EP_OUT) = m.halt_out ? FLD_USB_EP_STALL : 0;
        } else {
            bot_clear_halt(1);
            u.in_toggle = 0;
            u.in_flight = 0;
            reg_usb_ep_ctrl(EP_IN) = m.halt_in ? FLD_USB_EP_STALL : 0;
            u.resume_bulk = 1;
        }
    } else if (type == 0xa1 && request == 0xfe && !value && !index && length == 1 && u.configuration) {
        u.small[0] = 0;                          /* GET_MAX_LUN */
        u.response = u.small;
        u.remaining = 1;
    } else if (type == 0x21 && request == 0xff && !value && !index && !length && u.configuration) {
        bulk_reset(0);                           /* bulk-only mass storage reset */
    } else {
        u.stall = 1;
    }
    if (u.remaining > length) u.remaining = length;
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
    u.resume_bulk = 0;
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
    reg_usb_ep_en = BIT(EP_IN) | BIT(EP_OUT);
    reg_usb_ep_buf_addr(EP_IN) = 0x40;
    reg_usb_ep_buf_addr(EP_OUT) = 0x80;
    reg_usb_ep_max_size = 64 >> 3;
    bulk_reset(1);
    irq_disable_type(FLD_IRQ_USB_RST_EN | FLD_IRQ_USB_PWDN_EN | FLD_IRQ_EP0_SETUP_EN |
                     FLD_IRQ_EP0_DAT_EN | FLD_IRQ_EP0_STA_EN | FLD_IRQ_EP_DATA_EN);
    usb_set_pin(1);              /* attaches: the D+ pull-up */
    if (!power.present) usb_dp_pullup_en(0);
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
        u.configuration = 0;
        u.response = 0;
        u.remaining = u.total = u.host_length = 0;
        u.stall = 0;
        bulk_reset(1);
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
        if (!u.stall) ep0_packet();
        usbhw_write_ctrl_ep_ctrl(u.stall ? FLD_EP_DAT_STALL : FLD_EP_DAT_ACK);
    }
    if (irq & FLD_CTRL_EP_IRQ_STA) {
        usbhw_clr_ctrl_ep_irq(FLD_CTRL_EP_IRQ_STA);
        ep0_status();
    }
}

static void bulk(void)
{
    u8 packet[64], len, i;

    if (u.in_flight && !usbhw_is_ep_busy(EP_IN)) {
        u.in_flight = 0;
        bot_in_acked();
    }
    if (usbhw_get_eps_irq() & BIT(EP_OUT)) {
        len = reg_usb_ep_ptr(EP_OUT);
        u.out_armed = 0;
        reg_usb_irq = BIT(EP_OUT);
        usbhw_reset_ep_ptr(EP_OUT);
        if (len <= 64) {
            for (i = 0; i < len; i++) packet[i] = usbhw_read_ep_data(EP_OUT);
            if (m.state == MSC_DATA_OUT) bot_data_out(packet, len);
            else bot_receive(packet, len);
        } else {
            bot_invalid();
        }
        reg_usb_ep_ctrl(EP_OUT) = 0;
    }
    if (!u.in_flight && !m.halt_in && (len = bot_next_in(packet))) {
        usbhw_reset_ep_ptr(EP_IN);
        for (i = 0; i < len; i++) usbhw_write_ep_data(EP_IN, packet[i]);
        reg_usb_ep_ctrl(EP_IN) = FLD_USB_EP_BUSY | (u.in_toggle ? FLD_USB_EP_DAT1 : FLD_USB_EP_DAT0);
        u.in_toggle ^= 1;
        u.in_flight = 1;
    }
    if (m.halt_in && !u.in_flight && !(reg_usb_ep_ctrl(EP_IN) & FLD_USB_EP_STALL))
        usbhw_data_ep_stall(EP_IN);
    if (m.halt_out) {
        if (!(reg_usb_ep_ctrl(EP_OUT) & FLD_USB_EP_STALL)) usbhw_data_ep_stall(EP_OUT);
    } else if ((m.state == MSC_CBW || m.state == MSC_DATA_OUT) && !m.recovery && !u.out_armed) {
        /* Track our own arming, not BUSY: a packet arriving after the IRQ
         * sample clears BUSY, and re-arming would drop it. */
        u.out_armed = 1;
        usbhw_reset_ep_ptr(EP_OUT);
        usbhw_data_ep_ack(EP_OUT);
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
            bulk_reset(1);
        }
        util_log("usb: power %s", raw ? "present" : "gone");
        event = EV_USB;
    }
    if (!power.present) return event;
    control();
    if (u.configuration && !u.resume_bulk) bulk();
    return event | (m.state == MSC_COMMAND ? EV_USB : 0);
}

/* A vendor command waiting: usb.c answers it here, on the control loop. */
void plat_usb_service(void)
{
    u32 max;

    if (m.state != MSC_COMMAND) return;
    if (m.from_host) {                   /* its data in: done with it, then the status */
        usb_command(m.cdb, m.data, m.got, 1);
        m.relevant = m.got;
        m.state = MSC_CSW;
        return;
    }
    max = m.host < DATA_MAX ? m.host : DATA_MAX;
    memset(m.data, 0, sizeof(m.data));
    bot_answer(usb_command(m.cdb, m.data, max, 0));
}

/* The answer to the last command out (its CSW taken, 250 ms at most),
 * then ms more of answering the host. */
void plat_usb_settle(u32 ms)
{
    u32 start = plat_ms();

    while (u.configuration && m.state != MSC_CBW && m.state != MSC_LOCKED && plat_ms() - start < 250)
        wd_clear(), plat_usb_poll();
    start = plat_ms();
    while (plat_ms() - start < ms) wd_clear(), plat_usb_poll();
}

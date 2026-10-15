#include <system/includes.h>
#include <usb/device/usb_stack.h>
#include <usb/device/msd.h>
#include "usb_config.h"
#include "board.h"

/* USB as a game controller: the device a profile's struct usb_identity
 * describes, on the SDK's device core.  The core runs EP0; our setup hook
 * sees every request first and answers the descriptors, the HID class
 * requests and the vendor ones.  Interrupt IN carries reports, interrupt OUT
 * (or SET_REPORT) the host's writes; a Switch 2 also has a bulk pair.
 * With audio, a USB speaker follows: the stock pad's 48 kHz stereo 16-bit on
 * isochronous OUT 0x03, a feature unit with mute and volume in dB (-60 to 0
 * in steps of 0.5, so hosts get a real dB scale), its packets straight to the DAC (platform_jieli_audio.c).
 *
 * plat_usb_mode(NULL) is the tool device: the SDK's mass storage, whose
 * vendor SCSI commands platform_jieli.c hands to usb.c. */

#define EP_IN        1           /* 0x81: reports */
#define EP_OUT       1           /* 0x01: the host's reports (XInput: 0x02) */
#define EP_XOUT      2
#define EP_BULK      2           /* 0x02 and 0x82: Switch 2's commands */
#define EP_AUDIO     3           /* 0x03: isochronous, the speaker */
#define PACKET       64
#define AUDIO_PACKET 192         /* 1 ms of 48 kHz stereo 16-bit */
#define MESSAGES     4
#define MESSAGE_MAX  128         /* sent in packets of the endpoint's size */
#define WRITES       6
#define DESC_MAX     256
#define REQ_GET_REPORT 0x01
#define REQ_GET_IDLE   0x02
#define REQ_SET_REPORT 0x09
#define REQ_SET_IDLE   0x0a
#define REQ_SET_PROTOCOL 0x0b
#define DT_HID       0x21
#define DT_REPORT    0x22
#define DT_CS_INTERFACE 0x24
#define DT_CS_ENDPOINT  0x25
#define UAC_SET_CUR  0x01
#define UAC_GET_CUR  0x81
#define UAC_GET_MIN  0x82
#define UAC_GET_MAX  0x83
#define UAC_GET_RES  0x84
#define UAC_MUTE     1           /* feature unit control selectors */
#define UAC_VOLUME   2
#define UNIT_IN      1           /* the USB streaming input terminal */
#define UNIT_FEATURE 2
#define UNIT_OUT     3           /* the headphones */
#define VOLUME_MIN   (-60 * 256) /* UAC volume: 1/256 dB */
#define VOLUME_RES   128
#define VOLUME_START (-10 * 256)

extern u32 usb_read_txcsr(const usb_dev usb_id, const u32 ep);
extern void usb_memory_init(void);
void plat_usb_tool_mode(void);           /* platform_jieli.c: back to mass storage */
void plat_usb_hold_present(u32 ms);

/* One IN endpoint: the input waiting (newest wins) and messages (queued). */
struct pipe {
    u8 ep;                       /* with USB_DIR_IN */
    u8 size;
    volatile u8 busy;
    volatile u8 input_len;
    u8 input[PACKET];
    struct { u8 len; u8 data[MESSAGE_MAX]; } msg[MESSAGES];
    volatile u8 head, count;
    u8 off;                      /* of the head message, sent so far */
};

static const struct usb_identity *volatile ident;
static int (*host_read)(u8 kind, u16 id, u16 offset, u8 *out, u16 max);
static u8 device_desc[18];
static u8 config_desc[DESC_MAX];
static u16 config_len;
static u8 hid_at;                /* the HID descriptor's offset in config_desc */
static u8 out_ep;
static u8 *dma_in, *dma_out, *dma_bulk_in, *dma_bulk_out;
static volatile u8 configured;
static struct pipe intr, bulk;
static struct bt_write writes[WRITES];
static volatile u8 w_head, w_count;
static u8 set_report_kind, set_report_id;
static u8 rx[PACKET] __attribute__((aligned(4)));
static u8 audio_if;              /* the audio control interface; streaming is the next; 0: none */
static u8 *dma_audio;
static volatile u8 audio_alt;
static s16 volume[2] = { VOLUME_START, VOLUME_START };
static u8 mute;
static u8 audio_cs, audio_ch;    /* the SET_CUR waiting for its data */

static void reply(struct usb_device_t *dev, struct usb_ctrlrequest *req, const u8 *d, u32 n);
static void stall(struct usb_device_t *dev);

static u8 *put(u8 *p, const u8 *d, u32 n)
{
    memcpy(p, d, n);
    return p + n;
}

static u8 *endpoint(u8 *p, u8 address, u8 type, u16 size, u8 interval)
{
    u8 d[7] = { 7, USB_DT_ENDPOINT, address, type, (u8)size, (u8)(size >> 8), interval };
    return put(p, d, 7);
}

static u8 *interface(u8 *p, u8 number, u8 endpoints, u8 cls, u8 sub, u8 proto)
{
    u8 d[9] = { 9, USB_DT_INTERFACE, number, 0, endpoints, cls, sub, proto, 0 };
    return put(p, d, 9);
}

static u8 *speaker(u8 *p, u8 first)
{
    const u8 d[] = {
        /* Audio control: a header, the streaming input terminal, a feature
         * unit (mute on the master channel, volume on each), the
         * headphones. */
        9, USB_DT_INTERFACE, first, 0, 0, 1, 1, 0, 0,
        9, DT_CS_INTERFACE, 1, 0x00, 0x01, 9 + 12 + 10 + 9, 0, 1, (u8)(first + 1),
        12, DT_CS_INTERFACE, 2, UNIT_IN, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0,
        10, DT_CS_INTERFACE, 6, UNIT_FEATURE, UNIT_IN, 1, 0x01, 0x02, 0x02, 0,
        9, DT_CS_INTERFACE, 3, UNIT_OUT, 0x02, 0x03, 0, UNIT_FEATURE, 0,
        /* Streaming: alternate 0 is idle, 1 carries PCM. */
        9, USB_DT_INTERFACE, (u8)(first + 1), 0, 0, 1, 2, 0, 0,
        9, USB_DT_INTERFACE, (u8)(first + 1), 1, 1, 1, 2, 0, 0,
        7, DT_CS_INTERFACE, 1, UNIT_IN, 1, 0x01, 0x00,
        11, DT_CS_INTERFACE, 2, 1, 2, 2, 16, 1, 0x80, 0xbb, 0x00,
        9, USB_DT_ENDPOINT, EP_AUDIO, 0x09, (u8)AUDIO_PACKET, AUDIO_PACKET >> 8, 1, 0, 0,
        7, DT_CS_ENDPOINT, 1, 0, 0, 0, 0,
    };

    return put(p, d, sizeof(d));
}

static void build(const struct usb_identity *id)
{
    /* The Xbox 360 controller's vendor device class, but not beside a
     * speaker: Linux matches no class driver to the interfaces of a vendor
     * class device.  The stock pad's Xbox 360 mode has class 0 too. */
    const u8 vendor = id->kind == USB_XINPUT && !(id->audio && board_audio) ? 0xff : 0;
    const u8 dev[18] = {
        18, USB_DT_DEVICE, 0x00, 0x02, vendor, vendor, vendor, EP0_SETUP_LEN,
        (u8)id->vendor_id, (u8)(id->vendor_id >> 8), (u8)id->product_id, (u8)(id->product_id >> 8),
        (u8)id->bcd_device, (u8)(id->bcd_device >> 8),
        (u8)(id->manufacturer ? 1 : 0), (u8)(id->product ? 2 : 0), (u8)(id->serial ? 3 : 0), 1,
    };
    u8 *p = config_desc;

    memcpy(device_desc, dev, sizeof(dev));
    p += 9;
    if (id->kind == USB_XINPUT) {
        /* The wired Xbox 360 controller's first interface, as xpad and
         * Windows' xusb22 know it; its 0x21 descriptor names the endpoints. */
        static const u8 xinput[17] = {
            0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x80 | EP_IN, 0x14, 0, 0, 0, 0, 0x13,
            EP_XOUT, 0x08, 0, 0,
        };

        out_ep = EP_XOUT;
        p = interface(p, 0, 2, 0xff, 0x5d, 0x01);
        p = put(p, xinput, sizeof(xinput));
        p = endpoint(p, 0x80 | EP_IN, USB_ENDPOINT_XFER_INT, id->in_size, id->interval);
        p = endpoint(p, EP_XOUT, USB_ENDPOINT_XFER_INT, id->out_size, 8);
    } else {
        u8 hid[9] = { 9, DT_HID, 0x11, 0x01, 0, 1, DT_REPORT,
                      (u8)id->hid_descriptor_len, (u8)(id->hid_descriptor_len >> 8) };

        out_ep = EP_OUT;
        p = interface(p, 0, id->out_size ? 2 : 1, USB_CLASS_HID, 0, 0);
        hid_at = (u8)(p - config_desc);
        p = put(p, hid, sizeof(hid));
        p = endpoint(p, 0x80 | EP_IN, USB_ENDPOINT_XFER_INT, id->in_size, id->interval);
        if (id->out_size) p = endpoint(p, EP_OUT, USB_ENDPOINT_XFER_INT, id->out_size, id->interval);
        if (id->kind == USB_HID_BULK) {
            p = interface(p, 1, 2, 0xff, 0, 0);
            p = endpoint(p, EP_BULK, USB_ENDPOINT_XFER_BULK, PACKET, 0);
            p = endpoint(p, 0x80 | EP_BULK, USB_ENDPOINT_XFER_BULK, PACKET, 0);
        }
    }
    {
        u8 n = id->kind == USB_HID_BULK ? 2 : 1;

        audio_if = id->audio && board_audio ? n : 0;
        if (audio_if) p = speaker(p, audio_if);
        config_len = (u16)(p - config_desc);
        u8 head[9] = { 9, USB_DT_CONFIG, (u8)config_len, (u8)(config_len >> 8),
                       (u8)(audio_if ? n + 2 : n), 1, 0, 0xa0, id->max_power };
        memcpy(config_desc, head, 9);
    }
}

static u32 string(u8 index, u8 *out)
{
    const struct usb_identity *id = ident;
    const char *s = index == 1 ? id->manufacturer : index == 2 ? id->product :
                    index == 3 ? id->serial : NULL;
    u32 n = 0;

    if (!index) {
        out[0] = 4; out[1] = USB_DT_STRING; out[2] = 0x09; out[3] = 0x04;
        return 4;
    }
    if (!s) return 0;
    while (s[n] && n < 62) {
        out[2 + 2 * n] = (u8)s[n];
        out[3 + 2 * n] = 0;
        n++;
    }
    out[0] = (u8)(2 + 2 * n);
    out[1] = USB_DT_STRING;
    return out[0];
}

/* ---- the IN pipes: whatever is next, when the endpoint is free ----------- */

static void pump(struct pipe *q)
{
    if (q->busy || !configured) return;
    if (q->count) {
        u8 i = q->head, n = (u8)(q->msg[i].len - q->off);

        if (n > q->size) n = q->size;
        q->busy = 1;
        usb_g_intr_write(0, q->ep & 0x0f, q->msg[i].data + q->off, n);
        q->off = (u8)(q->off + n);
        if (q->off >= q->msg[i].len) {
            q->off = 0;
            q->head = (u8)((i + 1) % MESSAGES);
            q->count--;
        }
    } else if (q->input_len) {
        u8 n = q->input_len;

        q->input_len = 0;
        q->busy = 1;
        usb_g_intr_write(0, q->ep & 0x0f, q->input, n);
    }
}

static void in_done(struct usb_device_t *dev, u32 ep)
{
    struct pipe *q = (ep & 0x0f) == (intr.ep & 0x0f) ? &intr : &bulk;

    (void)dev;
    q->busy = 0;
    pump(q);
    plat_raise(EV_USB);
}

static void push_write(u8 kind, u16 id, const u8 *d, u32 n)
{
    struct bt_write *w;

    if (w_count == WRITES) return;     /* the host outruns us: the newest is lost */
    w = &writes[(w_head + w_count) % WRITES];
    if (n > BT_WRITE_MAX) n = BT_WRITE_MAX;
    w->kind = kind;
    w->id = id;
    w->len = (u16)n;
    memcpy(w->data, d, n);
    w_count++;
    plat_raise(EV_USB);
}

/* An OUT packet: the report id first when the descriptor uses them. */
static void host_wrote(u8 kind, const u8 *d, u32 n)
{
    if (ident->report_ids && n) push_write(kind, d[0], d + 1, n - 1);
    else push_write(kind, 0, d, n);
}

static void out_done(struct usb_device_t *dev, u32 ep)
{
    u32 n = usb_g_intr_read(0, ep, rx, sizeof(rx), 0);

    (void)dev;
    if (ep == EP_BULK && ident->kind == USB_HID_BULK) push_write(BT_WRITE_BULK, 0, rx, n);  /* no report id */
    else host_wrote(BT_WRITE_OUTPUT, rx, n);
}

static void endpoints(void)
{
    const struct usb_identity *id = ident;

    intr.busy = bulk.busy = 0;
    usb_g_ep_config(0, EP_IN | USB_DIR_IN, USB_ENDPOINT_XFER_INT, 1, dma_in, PACKET);
    usb_g_set_intr_hander(0, EP_IN | USB_DIR_IN, in_done);
    usb_enable_ep(0, EP_IN);
    if (id->out_size) {
        usb_g_set_intr_hander(0, out_ep, out_done);
        usb_g_ep_config(0, out_ep, USB_ENDPOINT_XFER_INT, 1, dma_out, PACKET);
        usb_enable_ep(0, out_ep);
    }
    if (id->kind == USB_HID_BULK) {
        usb_g_ep_config(0, EP_BULK | USB_DIR_IN, USB_ENDPOINT_XFER_BULK, 1, dma_bulk_in, PACKET);
        usb_g_set_intr_hander(0, EP_BULK | USB_DIR_IN, in_done);
        usb_g_set_intr_hander(0, EP_BULK, out_done);
        usb_g_ep_config(0, EP_BULK, USB_ENDPOINT_XFER_BULK, 1, dma_bulk_out, PACKET);
        usb_enable_ep(0, EP_BULK);
    }
}

/* ---- the speaker ------------------------------------------------------------ */

static void audio_rx(struct usb_device_t *dev, u32 ep)
{
    u32 n = usb_g_iso_read(0, EP_AUDIO, NULL, AUDIO_PACKET, 0);

    (void)dev;
    (void)ep;
    plat_audio_write(dma_audio, n);
}

static void stream(u8 alt)
{
    if (!audio_if || alt == audio_alt) return;
    audio_alt = alt;
    if (alt) {
        usb_g_set_intr_hander(0, EP_AUDIO, audio_rx);
        usb_g_ep_config(0, EP_AUDIO, USB_ENDPOINT_XFER_ISOC, 1, dma_audio, AUDIO_PACKET);
    } else {
        usb_clr_intr_rxe(0, EP_AUDIO);
        usb_g_set_intr_hander(0, EP_AUDIO, NULL);
    }
    plat_audio_stream(alt);
}

static u32 audio_set_data(struct usb_device_t *dev, struct usb_ctrlrequest *req)
{
    u32 n = req->wLength < sizeof(rx) ? req->wLength : sizeof(rx);

    (void)dev;
    usb_read_ep0(0, rx, n);
    if (audio_cs == UAC_MUTE && n >= 1) mute = rx[0] != 0;
    if (audio_cs == UAC_VOLUME && n >= 2) {
        s16 v = (s16)(rx[0] | rx[1] << 8);

        v = v > 0 ? 0 : v < VOLUME_MIN ? VOLUME_MIN : v;
        if (audio_ch == 0 || audio_ch == 1) volume[0] = v;
        if (audio_ch == 0 || audio_ch == 2) volume[1] = v;
    }
    plat_audio_volume(volume[0], volume[1], mute);
    return USB_EP0_STAGE_SETUP;
}

/* Class requests to the audio control interface: the feature unit only. */
static u32 audio_request(struct usb_device_t *dev, struct usb_ctrlrequest *req)
{
    u8 *buf = usb_get_setup_buffer(dev);
    u8 cs = HIBYTE(req->wValue), ch = LOBYTE(req->wValue);
    s16 v;

    if (HIBYTE(req->wIndex) != UNIT_FEATURE || (cs != UAC_MUTE && cs != UAC_VOLUME)) {
        stall(dev);
        return 1;
    }
    if (req->bRequest == UAC_SET_CUR) {
        audio_cs = cs;
        audio_ch = ch;
        usb_set_setup_recv(dev, audio_set_data);
        return 1;
    }
    if (cs == UAC_MUTE) {
        if (req->bRequest != UAC_GET_CUR) {
            stall(dev);
            return 1;
        }
        buf[0] = mute;
        reply(dev, req, buf, 1);
        return 1;
    }
    switch (req->bRequest) {
    case UAC_GET_CUR: v = volume[ch == 2 ? 1 : 0]; break;
    case UAC_GET_MIN: v = VOLUME_MIN; break;
    case UAC_GET_MAX: v = 0; break;
    case UAC_GET_RES: v = VOLUME_RES; break;
    default:
        stall(dev);
        return 1;
    }
    buf[0] = (u8)v;
    buf[1] = (u8)((u16)v >> 8);
    reply(dev, req, buf, req->wLength < 2 ? req->wLength : 2);
    return 1;
}

static void on_reset(struct usb_device_t *dev, u32 itf)
{
    (void)dev;
    (void)itf;
    configured = 0;
    stream(0);
    endpoints();
}

/* ---- EP0 ------------------------------------------------------------------ */

static u32 set_report_data(struct usb_device_t *dev, struct usb_ctrlrequest *req)
{
    u32 n = req->wLength < sizeof(rx) ? req->wLength : sizeof(rx);

    usb_read_ep0(0, rx, n);
    if (ident->report_ids && n && rx[0] == set_report_id) push_write(set_report_kind, rx[0], rx + 1, n - 1);
    else push_write(set_report_kind, set_report_id, rx, n);
    (void)dev;
    return USB_EP0_STAGE_SETUP;
}

static void reply(struct usb_device_t *dev, struct usb_ctrlrequest *req, const u8 *d, u32 n)
{
    usb_set_data_payload(dev, req, d, n);
}

static void stall(struct usb_device_t *dev)
{
    usb_set_setup_phase(dev, USB_EP0_SET_STALL);
}

static void ok(struct usb_device_t *dev)
{
    usb_set_setup_phase(dev, USB_EP0_STAGE_SETUP);
}

static u32 get_report(struct usb_device_t *dev, struct usb_ctrlrequest *req)
{
    u8 *buf = usb_get_setup_buffer(dev);
    u8 type = HIBYTE(req->wValue), id = LOBYTE(req->wValue);
    u8 at = ident->report_ids ? 1 : 0;
    u16 max = req->wLength < USB_SETUP_SIZE ? req->wLength : USB_SETUP_SIZE;
    int n = -1;

    if (host_read && type == 3 && max > at)
        n = host_read(BT_WRITE_FEATURE, id, 0, buf + at, (u16)(max - at));
    if (n < 0) {
        stall(dev);
        return 1;
    }
    buf[0] = at ? id : buf[0];
    reply(dev, req, buf, (u32)n + at);
    return 1;
}

static u32 hook(struct usb_device_t *dev, struct usb_ctrlrequest *req)
{
    const struct usb_identity *id = ident;
    u8 *buf = usb_get_setup_buffer(dev);
    u32 type = req->bRequestType & USB_TYPE_MASK;
    u32 recip = req->bRequestType & USB_RECIP_MASK;
    u32 n;

    if (!id) return 0;
    if (type == USB_TYPE_STANDARD) {
        switch (req->bRequest) {
        case USB_REQ_GET_DESCRIPTOR:
            switch (HIBYTE(req->wValue)) {
            case USB_DT_DEVICE: reply(dev, req, device_desc, sizeof(device_desc)); return 1;
            case USB_DT_CONFIG: reply(dev, req, config_desc, config_len); return 1;
            case USB_DT_STRING:
                n = string(LOBYTE(req->wValue), buf);
                if (n) reply(dev, req, buf, n);
                else stall(dev);
                return 1;
            case DT_HID:
                if (id->kind == USB_XINPUT) break;
                reply(dev, req, config_desc + hid_at, 9);
                return 1;
            case DT_REPORT:
                if (!id->hid_descriptor) break;
                reply(dev, req, id->hid_descriptor, id->hid_descriptor_len);
                return 1;
            }
            stall(dev);
            return 1;
        case USB_REQ_SET_CONFIGURATION:
            stream(0);
            endpoints();
            configured = LOBYTE(req->wValue) != 0;
            plat_raise(EV_USB);
            return 0;            /* the core answers it */
        case USB_REQ_GET_INTERFACE:
            if (recip != USB_RECIP_INTERFACE) return 0;
            buf[0] = audio_if && LOBYTE(req->wIndex) == audio_if + 1 ? audio_alt : 0;
            reply(dev, req, buf, 1);
            return 1;
        case USB_REQ_SET_INTERFACE:
            if (recip != USB_RECIP_INTERFACE) return 0;
            if (audio_if && LOBYTE(req->wIndex) == audio_if + 1 && req->wValue <= 1) {
                ok(dev);
                stream((u8)req->wValue);
            } else if (req->wValue) stall(dev);
            else ok(dev);
            return 1;
        }
        return 0;
    }
    if (type == USB_TYPE_CLASS && recip == USB_RECIP_INTERFACE && audio_if &&
        LOBYTE(req->wIndex) == audio_if)
        return audio_request(dev, req);
    if (type == USB_TYPE_CLASS && recip == USB_RECIP_INTERFACE) {
        switch (req->bRequest) {
        case REQ_GET_REPORT:
            return get_report(dev, req);
        case REQ_SET_REPORT:
            set_report_kind = HIBYTE(req->wValue) == 3 ? BT_WRITE_FEATURE : BT_WRITE_OUTPUT;
            set_report_id = LOBYTE(req->wValue);
            usb_set_setup_recv(dev, set_report_data);
            return 1;
        case REQ_GET_IDLE:
            buf[0] = 0;
            reply(dev, req, buf, 1);
            return 1;
        case REQ_SET_IDLE:
        case REQ_SET_PROTOCOL:
            ok(dev);
            return 1;
        }
        stall(dev);
        return 1;
    }
    /* Vendor requests: Windows asks a wired Xbox 360 pad for 20 bytes it
     * does not need to make sense of; anything else is not ours. */
    if (type == USB_TYPE_VENDOR && (req->bRequestType & USB_DIR_IN) && id->kind == USB_XINPUT) {
        n = req->wLength < 20 ? req->wLength : 20;
        memset(buf, 0, n);
        reply(dev, req, buf, n);
        return 1;
    }
    stall(dev);
    return 1;
}

/* ---- the interface ---------------------------------------------------------- */

void plat_usb_mode(const struct usb_identity *id,
                   int (*read)(u8 kind, u16 id, u16 offset, u8 *out, u16 max))
{
    if (id == ident) {
        host_read = read;
        return;
    }
    plat_usb_hold_present(1500);         /* the re-enumeration is not an unplug */
    local_irq_disable();
    stream(0);
    ident = NULL;
    configured = 0;
    local_irq_enable();
    plat_audio_off();
    audio_if = 0;
    usb_device_mode(0, 0);               /* off the bus: the host sees a new device */
    memset(&intr, 0, sizeof(intr));
    memset(&bulk, 0, sizeof(bulk));
    w_head = w_count = 0;
    if (!id) {
        plat_usb_tool_mode();
        return;
    }
    host_read = read;
    build(id);
    intr.ep = 0x80 | EP_IN;
    intr.size = id->in_size;
    bulk.ep = 0x80 | EP_BULK;
    bulk.size = PACKET;
    ident = id;
    usb_device_mode(0, 0x80000000);      /* no SDK class: the core and its EP0 only */
    dma_in = usb_alloc_ep_dmabuffer(0, EP_IN | USB_DIR_IN, PACKET);
    dma_out = id->out_size ? usb_alloc_ep_dmabuffer(0, out_ep, PACKET) : NULL;
    if (id->kind == USB_HID_BULK) {
        dma_bulk_in = usb_alloc_ep_dmabuffer(0, EP_BULK | USB_DIR_IN, PACKET);
        dma_bulk_out = usb_alloc_ep_dmabuffer(0, EP_BULK, PACKET);
    }
    if (audio_if) {
        dma_audio = usb_alloc_ep_dmabuffer(0, EP_AUDIO, AUDIO_PACKET + 4);
        plat_audio_on();
        plat_audio_volume(volume[0], volume[1], mute);
    }
    usb_set_reset_hander(0, 0, on_reset);
    usb_set_setup_hook(usb_id2device(0), hook);
}

int plat_usb_ready(void)
{
    return ident && configured;
}

int plat_usb_input_wanted(void)
{
    return plat_usb_ready() && !intr.busy && !intr.input_len && !intr.count;
}

int plat_usb_send_input(u8 id, const u8 *data, u16 len)
{
    u8 at = ident && ident->report_ids ? 1 : 0;

    if (!plat_usb_ready() || len + at > intr.size) return 0;
    local_irq_disable();
    if (at) intr.input[0] = id;
    memcpy(intr.input + at, data, len);
    intr.input_len = (u8)(len + at);
    pump(&intr);
    local_irq_enable();
    return 1;
}

int plat_usb_send_message(u16 id, const u8 *data, u16 len)
{
    struct pipe *q = id & HOST_BULK ? &bulk : &intr;
    u8 at = !(id & HOST_BULK) && ident && ident->report_ids ? 1 : 0;
    u8 *m;
    int sent = 0;

    if (!plat_usb_ready() || len + at > (q == &bulk ? MESSAGE_MAX : q->size)) return 0;
    local_irq_disable();
    if (q->count < MESSAGES) {
        m = q->msg[(q->head + q->count) % MESSAGES].data;
        if (at) m[0] = (u8)id;
        memcpy(m + at, data, len);
        q->msg[(q->head + q->count) % MESSAGES].len = (u8)(len + at);
        q->count++;
        pump(q);
        sent = 1;
    }
    local_irq_enable();
    return sent;
}

int plat_usb_take(struct bt_write *w)
{
    int got = 0;

    local_irq_disable();
    if (w_count) {
        *w = writes[w_head];
        w_head = (u8)((w_head + 1) % WRITES);
        w_count--;
        got = 1;
    }
    local_irq_enable();
    return got;
}

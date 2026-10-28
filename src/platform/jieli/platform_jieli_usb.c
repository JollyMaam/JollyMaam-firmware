#include <system/includes.h>
#include <usb/device/usb_stack.h>
#include <usb/device/descriptor.h>
#include "usb_config.h"
#include "board.h"

/* USB as a game controller: the device a profile's struct usb_identity
 * describes, on the SDK's device core.  The core runs EP0; our setup hook
 * sees every request first and answers the descriptors, the HID class
 * requests and the vendor ones.  Interrupt IN carries reports, interrupt OUT
 * (or SET_REPORT) the host's writes; a Switch 2 also has a bulk pair.
 * With audio, a USB speaker follows: the stock pad's 48 kHz stereo 16-bit on
 * isochronous OUT 0x03, a feature unit with mute and volume in dB (-60 to 0
 * in steps of 0.5, so hosts get a real dB scale), its packets straight to
 * the DAC (platform_jieli_audio.c).  As on the stock pad the speaker is there
 * only while a plug is in the jack: a plug going in or out re-enumerates.
 * As a Switch 2 Pro Controller, the real one's headset instead, always there:
 * every function behind an interface association, headphones on 0x03 and a
 * 48 kHz stereo microphone on 0x83, which sends silence for now.
 *
 * plat_usb_mode(NULL) is the tool device: a vendor HID interface whose
 * feature reports carry usb.c's commands (below). */

#define EP_IN        1           /* 0x81: reports */
#define EP_OUT       1           /* 0x01: the host's reports (XInput: 0x02) */
#define EP_XOUT      2
#define EP_BULK      2           /* 0x02 and 0x82: Switch 2's commands */
#define EP_AUDIO     3           /* 0x03: isochronous, the speaker */
#define PACKET       64
#define AUDIO_PACKET 192         /* 1 ms of 48 kHz stereo 16-bit, both ways */
#define MESSAGES     4
#define MESSAGE_MAX  128         /* sent in packets of the endpoint's size */
#define WRITES       6
#define DESC_MAX     320
#define REQ_GET_REPORT 0x01
#define REQ_GET_IDLE   0x02
#define REQ_SET_REPORT 0x09
#define REQ_SET_IDLE   0x0a
#define REQ_SET_PROTOCOL 0x0b
#define DT_HID       0x21
#define DT_REPORT    0x22
#define DT_CS_INTERFACE 0x24
#define DT_CS_ENDPOINT  0x25
#define DT_IAD       0x0b
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
#define UNIT_MIC_FEATURE 5       /* headset: the microphone's, after its input terminal 4 */
#define VOLUME_MIN   (-60 * 256) /* UAC volume: 1/256 dB */
#define VOLUME_RES   128
#define VOLUME_START (-10 * 256)
#define JACK_SETTLE_MS 200
#define AGAIN_MS     2000        /* a host that has not configured us by then */
#define AGAIN_MAX    3

extern u32 usb_read_txcsr(const usb_dev usb_id, const u32 ep);
extern void usb_memory_init(void);
void plat_usb_frames_rebase(void);       /* platform_jieli.c: after a core re-init */
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
static u8 jack;                  /* debounced: a plug in the jack */
static u8 audio_if;              /* the audio control interface; streaming is the next; 0: none */
static u8 headset;               /* audio_if is a headset: the microphone's streaming follows */
static u8 *dma_audio, *dma_mic;
static volatile u8 audio_alt, mic_alt;
static s16 volume[2] = { VOLUME_START, VOLUME_START };
static u8 mute;
static s16 mic_volume = VOLUME_START;    /* kept for the host, the microphone is silent */
static u8 mic_mute;
static u8 audio_unit, audio_cs, audio_ch;    /* the SET_CUR waiting for its data */

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

static u8 *association(u8 *p, u8 first, u8 count, u8 cls, u8 sub)
{
    u8 d[8] = { 8, DT_IAD, first, count, cls, sub, 0, 0 };
    return put(p, d, 8);
}

/* The Switch 2 Pro Controller's, as the real one describes it: master mute
 * and volume on both feature units, synchronous endpoints of 7 bytes. */
static u8 *headset_audio(u8 *p, u8 first)
{
    const u8 d[] = {
        9, USB_DT_INTERFACE, first, 0, 0, 1, 1, 0, 0,
        10, DT_CS_INTERFACE, 1, 0x00, 0x01, 71, 0, 2, (u8)(first + 1), (u8)(first + 2),
        12, DT_CS_INTERFACE, 2, UNIT_IN, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0,
        10, DT_CS_INTERFACE, 6, UNIT_FEATURE, UNIT_IN, 1, 0x03, 0x00, 0x00, 0,
        9, DT_CS_INTERFACE, 3, UNIT_OUT, 0x02, 0x03, 0, UNIT_FEATURE, 0,
        12, DT_CS_INTERFACE, 2, 4, 0x01, 0x02, 0, 1, 0x00, 0x00, 0, 0,
        9, DT_CS_INTERFACE, 6, UNIT_MIC_FEATURE, 4, 1, 0x03, 0x00, 0,
        9, DT_CS_INTERFACE, 3, 6, 0x01, 0x01, 0, UNIT_MIC_FEATURE, 0,
        9, USB_DT_INTERFACE, (u8)(first + 1), 0, 0, 1, 2, 0, 0,
        9, USB_DT_INTERFACE, (u8)(first + 1), 1, 1, 1, 2, 0, 0,
        7, DT_CS_INTERFACE, 1, UNIT_IN, 0, 0x01, 0x00,
        11, DT_CS_INTERFACE, 2, 1, 2, 2, 16, 1, 0x80, 0xbb, 0x00,
        7, USB_DT_ENDPOINT, EP_AUDIO, 0x0d, (u8)AUDIO_PACKET, AUDIO_PACKET >> 8, 1,
        7, DT_CS_ENDPOINT, 1, 0, 0, 0, 0,
        9, USB_DT_INTERFACE, (u8)(first + 2), 0, 0, 1, 2, 0, 0,
        9, USB_DT_INTERFACE, (u8)(first + 2), 1, 1, 1, 2, 0, 0,
        7, DT_CS_INTERFACE, 1, 6, 0, 0x01, 0x00,
        11, DT_CS_INTERFACE, 2, 1, 2, 2, 16, 1, 0x80, 0xbb, 0x00,
        7, USB_DT_ENDPOINT, 0x80 | EP_AUDIO, 0x0d, (u8)AUDIO_PACKET, AUDIO_PACKET >> 8, 1,
        7, DT_CS_ENDPOINT, 1, 0, 0, 0, 0,
    };

    p = association(p, first, 3, 1, 1);
    return put(p, d, sizeof(d));
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
    /* Audio: the speaker with a plug in the jack, the headset always. */
    const u8 audio = board_audio && (id->audio == USB_AUDIO_HEADSET ||
                                     (id->audio == USB_AUDIO_SPEAKER && jack));
    const u8 iad = audio && id->audio == USB_AUDIO_HEADSET;
    /* The Xbox 360 controller's vendor device class, but not beside a
     * speaker: Linux matches no class driver to the interfaces of a vendor
     * class device.  The stock pad's Xbox 360 mode has class 0 too.  With
     * interface associations: the class that says so. */
    const u8 vendor = id->kind == USB_XINPUT && !audio ? 0xff : 0;
    const u8 dev[18] = {
        18, USB_DT_DEVICE, 0x00, 0x02, (u8)(iad ? 0xef : vendor), (u8)(iad ? 0x02 : vendor),
        (u8)(iad ? 0x01 : vendor), EP0_SETUP_LEN,
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
        if (iad) p = association(p, 0, 1, USB_CLASS_HID, 0);
        p = interface(p, 0, id->out_size ? 2 : 1, USB_CLASS_HID, 0, 0);
        hid_at = (u8)(p - config_desc);
        p = put(p, hid, sizeof(hid));
        p = endpoint(p, 0x80 | EP_IN, USB_ENDPOINT_XFER_INT, id->in_size, id->interval);
        if (id->out_size) p = endpoint(p, EP_OUT, USB_ENDPOINT_XFER_INT, id->out_size, id->interval);
        if (id->kind == USB_HID_BULK) {
            if (iad) p = association(p, 1, 1, 0xff, 0);
            p = interface(p, 1, 2, 0xff, 0, 0);
            p = endpoint(p, EP_BULK, USB_ENDPOINT_XFER_BULK, PACKET, 0);
            p = endpoint(p, 0x80 | EP_BULK, USB_ENDPOINT_XFER_BULK, PACKET, 0);
        }
    }
    {
        u8 n = id->kind == USB_HID_BULK ? 2 : 1;

        audio_if = audio ? n : 0;
        headset = iad;
        if (headset) p = headset_audio(p, audio_if);
        else if (audio_if) p = speaker(p, audio_if);
        config_len = (u16)(p - config_desc);
        u8 head[9] = { 9, USB_DT_CONFIG, (u8)config_len, (u8)(config_len >> 8),
                       (u8)(n + (headset ? 3 : audio_if ? 2 : 0)), 1, 0, 0xa0, id->max_power };
        memcpy(config_desc, head, 9);
    }
}

#define MS_OS_STRING 0xee
#define MS_OS_VENDOR 0x90        /* the stock dongle's vendor code */

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
    if (index == MS_OS_STRING && id->kind == USB_XINPUT) {
        /* Microsoft OS 1.0: "MSFT100" and our vendor request, so Windows
         * asks for the compatible ID that loads its Xbox 360 driver for an
         * ID it does not list (the stock dongle does the same). */
        static const char sig[] = "MSFT100";

        for (n = 0; n < 7; n++) {
            out[2 + 2 * n] = (u8)sig[n];
            out[3 + 2 * n] = 0;
        }
        out[16] = MS_OS_VENDOR;
        out[17] = 0;
        out[0] = 18;
        out[1] = USB_DT_STRING;
        return 18;
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


/* The headset's microphone: a packet of silence every frame while the host
 * listens.  Only with none still waiting: the SDK's write flushes the FIFO,
 * and a flush interrupts again at once, a storm that starves everything
 * else while the host is not taking packets. */

static void mic_tx(struct usb_device_t *dev, u32 ep)
{
    (void)dev;
    (void)ep;
    if (usb_read_txcsr(0, EP_AUDIO) & 1) return;     /* TxPktRdy */
    memset(dma_mic, 0, AUDIO_PACKET);
    usb_g_iso_write(0, EP_AUDIO, NULL, AUDIO_PACKET);
}

static void mic(u8 alt)
{
    if (!headset || alt == mic_alt) return;
    mic_alt = alt;
    if (alt) {
        usb_enable_ep(0, EP_AUDIO);       /* the number: the SDK's mic does so too */
        usb_g_set_intr_hander(0, EP_AUDIO | USB_DIR_IN, mic_tx);
        usb_g_ep_config(0, EP_AUDIO | USB_DIR_IN, USB_ENDPOINT_XFER_ISOC, 1, dma_mic, AUDIO_PACKET);
        mic_tx(NULL, 0);
    } else {
        usb_clr_intr_txe(0, EP_AUDIO);
        usb_g_set_intr_hander(0, EP_AUDIO | USB_DIR_IN, NULL);
    }
}

static u32 audio_set_data(struct usb_device_t *dev, struct usb_ctrlrequest *req)
{
    u32 n = req->wLength < sizeof(rx) ? req->wLength : sizeof(rx);

    (void)dev;
    usb_read_ep0(0, rx, n);
    if (audio_unit == UNIT_MIC_FEATURE) {
        if (audio_cs == UAC_MUTE && n >= 1) mic_mute = rx[0] != 0;
        if (audio_cs == UAC_VOLUME && n >= 2) mic_volume = (s16)(rx[0] | rx[1] << 8);
        return USB_EP0_STAGE_SETUP;
    }
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
    u8 unit = HIBYTE(req->wIndex), is_mic = unit == UNIT_MIC_FEATURE && headset;
    s16 v;

    if ((unit != UNIT_FEATURE && !is_mic) || (cs != UAC_MUTE && cs != UAC_VOLUME)) {
        stall(dev);
        return 1;
    }
    if (req->bRequest == UAC_SET_CUR) {
        audio_unit = unit;
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
        buf[0] = is_mic ? mic_mute : mute;
        reply(dev, req, buf, 1);
        return 1;
    }
    switch (req->bRequest) {
    case UAC_GET_CUR: v = is_mic ? mic_volume : volume[ch == 2 ? 1 : 0]; break;
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
    mic(0);
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

/* Debug: the host's control requests after a personality starts, kept here
 * (the hook runs in the interrupt) and logged from the control thread,
 * "type/request value index length" each.  Shows what an enumeration asked. */
#define SETUP_LOG 24
static struct { u8 type, request; u16 value, index, length; } setup_log[SETUP_LOG];
static volatile u8 setup_count, setup_logged;

static void setup_log_flush(void)
{
    char line[120];
    u32 n = 0, end = setup_count < SETUP_LOG ? setup_count : SETUP_LOG;

    while (setup_logged < end) {
        n += (u32)snprintf(line + n, sizeof(line) - n, " %02x/%02x %04x %04x %u",
                           setup_log[setup_logged].type, setup_log[setup_logged].request,
                           setup_log[setup_logged].value, setup_log[setup_logged].index,
                           setup_log[setup_logged].length);
        setup_logged++;
        if (n > sizeof(line) - 24 || setup_logged == end) {
            util_log("usb setup:%s", line);
            n = 0;
        }
    }
}

static u32 hook(struct usb_device_t *dev, struct usb_ctrlrequest *req)
{
    const struct usb_identity *id = ident;
    u8 *buf = usb_get_setup_buffer(dev);
    u32 type = req->bRequestType & USB_TYPE_MASK;
    u32 recip = req->bRequestType & USB_RECIP_MASK;
    u32 n;

    if (!id) return 0;
    if (setup_count < SETUP_LOG) {
        setup_log[setup_count].type = req->bRequestType;
        setup_log[setup_count].request = req->bRequest;
        setup_log[setup_count].value = req->wValue;
        setup_log[setup_count].index = req->wIndex;
        setup_log[setup_count].length = req->wLength;
    }
    if (setup_count < 255) setup_count++;
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
            mic(0);
            endpoints();
            configured = LOBYTE(req->wValue) != 0;
            plat_raise(EV_USB);
            return 0;            /* the core answers it */
        case USB_REQ_GET_INTERFACE:
            if (recip != USB_RECIP_INTERFACE) return 0;
            buf[0] = audio_if && LOBYTE(req->wIndex) == audio_if + 1 ? audio_alt :
                     headset && LOBYTE(req->wIndex) == audio_if + 2 ? mic_alt : 0;
            reply(dev, req, buf, 1);
            return 1;
        case USB_REQ_SET_INTERFACE:
            if (recip != USB_RECIP_INTERFACE) return 0;
            if (audio_if && LOBYTE(req->wIndex) == audio_if + 1 && req->wValue <= 1) {
                ok(dev);
                stream((u8)req->wValue);
            } else if (headset && LOBYTE(req->wIndex) == audio_if + 2 && req->wValue <= 1) {
                ok(dev);
                mic((u8)req->wValue);
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
    /* Vendor requests: an Xbox 360 pad's compatible ID "XUSB10" for
     * Windows (interface 0, 40 bytes, as the stock dongle sends it); Windows
     * also asks a wired Xbox 360 pad for 20 bytes it does not need to make
     * sense of; others go to the profile (a Switch 2 reads its device info
     * this way). */
    if (type == USB_TYPE_VENDOR && (req->bRequestType & USB_DIR_IN) && id->kind == USB_XINPUT &&
        req->bRequest == MS_OS_VENDOR && req->wIndex == 4) {
        static const u8 compat[40] = {
            40, 0, 0, 0, 0x00, 0x01, 0x04, 0x00, 1, 0, 0, 0, 0, 0, 0, 0,
            0, 1, 'X', 'U', 'S', 'B', '1', '0', 0, 0,
        };

        reply(dev, req, compat, req->wLength < 40 ? req->wLength : 40);
        return 1;
    }
    if (type == USB_TYPE_VENDOR && (req->bRequestType & USB_DIR_IN) && id->kind == USB_XINPUT) {
        n = req->wLength < 20 ? req->wLength : 20;
        memset(buf, 0, n);
        reply(dev, req, buf, n);
        return 1;
    }
    if (type == USB_TYPE_VENDOR && host_read) {
        u16 max = req->wLength < USB_SETUP_SIZE ? req->wLength : USB_SETUP_SIZE;
        int got;

        if (!(req->bRequestType & USB_DIR_IN)) max = 0;
        if (!(req->bRequestType & USB_DIR_IN) && req->wLength) got = -1;   /* none with data yet */
        else got = host_read(BT_WRITE_VENDOR, req->bRequest, req->wValue, buf, max);
        if (got < 0) stall(dev);
        else if (req->bRequestType & USB_DIR_IN) reply(dev, req, buf, (u32)got);
        else ok(dev);
        return 1;
    }
    stall(dev);
    return 1;
}

/* ---- the interface ---------------------------------------------------------- */

static void start(const struct usb_identity *id,
                  int (*read)(u8 kind, u16 id, u16 offset, u8 *out, u16 max))
{
    plat_usb_hold_present(1500);         /* the re-enumeration is not an unplug */
    local_irq_disable();
    setup_count = setup_logged = 0;
    stream(0);
    mic(0);
    ident = NULL;
    configured = 0;
    local_irq_enable();
    plat_audio_off();
    audio_if = 0;
    headset = 0;
    usb_device_mode(0, 0);               /* off the bus: the host sees a new device */
    memset(&intr, 0, sizeof(intr));
    memset(&bulk, 0, sizeof(bulk));
    w_head = w_count = 0;
    if (!id) {
        plat_usb_tool_mode();
        plat_usb_frames_rebase();
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
        if (headset) dma_mic = usb_alloc_ep_dmabuffer(0, EP_AUDIO | USB_DIR_IN, AUDIO_PACKET);
        plat_audio_on();
        plat_audio_volume(volume[0], volume[1], mute);
    }
    usb_set_reset_hander(0, 0, on_reset);
    usb_set_setup_hook(usb_id2device(0), hook);
    plat_usb_frames_rebase();
}

void plat_usb_mode(const struct usb_identity *id,
                   int (*read)(u8 kind, u16 id, u16 offset, u8 *out, u16 max))
{
    if (id == ident) {
        host_read = read;
        return;
    }
    start(id, read);
}

/* Control thread.  A plug in or out for JACK_SETTLE_MS changes the device.
 * Now and then the host does not see us come back after such a change: the
 * port stays empty until the next.  Not seeing a SET_CONFIGURATION within
 * AGAIN_MS, we go off the bus and on again. */
void plat_usb_jack(void)
{
    static u32 since, again_ms;
    static u8 again;
    u8 now = (u8)board_jack();

    setup_log_flush();

    if (again && (configured || !ident)) again = 0;
    if (again && plat_now_ms - again_ms >= AGAIN_MS) {
        util_log("usb: not configured, again");
        again_ms = plat_now_ms;
        again--;
        start(ident, host_read);
    }

#ifdef JACK_FLIP_TEST
    now = (u8)(plat_now_ms / 6000 & 1);
#endif

    if (!board_audio || now == jack) {
        since = plat_now_ms;
        return;
    }
    if (plat_now_ms - since < JACK_SETTLE_MS) return;
    jack = now;
    util_log("usb: jack %s", jack ? "in" : "out");
    if (ident && ident->audio == USB_AUDIO_SPEAKER) {
        start(ident, host_read);
        again = AGAIN_MAX;
        again_ms = plat_now_ms;
    }
}

int plat_usb_jack_in(void)
{
    return jack;
}

int plat_usb_ready(void)
{
    return ident && configured;
}

int plat_usb_input_wanted(void)
{
    return plat_usb_ready() && !intr.busy && !intr.input_len && !intr.count;
}

int plat_usb_input_staged(void)
{
    return intr.input_len != 0;
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

/* ---- the tool device ---------------------------------------------------------
 *
 * One HID interface, usage page 0xffc2, with one 512-byte feature report, a
 * 64-byte input report and no report ids.  A command is a SET_REPORT: the
 * CDB (16 bytes: 0xf5, the sub-command, its arguments, or a loader opcode),
 * u16 length, u8 flags (bit 0: data to the pad; bit 1: the answer on input
 * reports, to the u32 client that follows), u8 tag, then that many bytes of
 * data to the pad; without bit 0, length is the answer's size, as a SCSI
 * command's transfer length (zero-padded).  The control thread runs it
 * (usb_command()).
 *
 * With bit 1 the answer goes out on interrupt IN, which every program that
 * has the device open receives: the client, the tag, a state, u16 offset,
 * then 56 bytes of the answer.  MORE: a piece, more follow; LAST: the last
 * piece (or none); REFUSED: another command is running, send it again.  So
 * several programs share the device, each taking only its own answers.
 *
 * Without bit 1 each GET_REPORT answers: the tag, a state, u16 length, that
 * many bytes of the answer.  BUSY: not run yet, ask again; MORE: a piece,
 * the rest follows in the next GET_REPORTs; LAST: the last piece (or none).
 * Whoever reads first takes a piece: one program at a time. */

#define TOOL_REPORT     512
#define TOOL_HEAD       20
#define TOOL_HEAD_IN    24       /* with the client */
#define TOOL_REPLY_HEAD 4
#define TOOL_IN_HEAD    8
#define TOOL_IN_DATA    (PACKET - TOOL_IN_HEAD)
#define TOOL_IN_WAIT_MS 1000     /* a piece no one takes: the answer is dropped */
#define TOOL_REFUSALS   4
#define TOOL_SERIAL     33
enum { TOOL_IDLE, TOOL_BUSY, TOOL_MORE, TOOL_LAST, TOOL_REFUSED };
enum { TOOL_OUT = 1, TOOL_INPUT = 2 };

static const u8 tool_report_desc[] = {
    0x06, 0xc2, 0xff,            /* usage page 0xffc2 */
    0x09, 0x01,                  /* usage 1 */
    0xa1, 0x01,                  /* application collection */
    0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x96, (u8)TOOL_REPORT, (u8)(TOOL_REPORT >> 8),
    0x09, 0x01, 0xb1, 0x02,      /* feature: data, variable */
    0x95, PACKET,
    0x09, 0x01, 0x81, 0x02,      /* input: data, variable */
    0xc0,
};
static u8 tool_req[TOOL_REPORT] __attribute__((aligned(4)));
static u8 tool_answer[TOOL_REPORT] __attribute__((aligned(4)));
static u8 tool_in[PACKET] __attribute__((aligned(4)));
static volatile u8 tool_pending, tool_state, tool_tag;
static volatile u8 tool_busy;    /* a command accepted, its answer not all out */
static volatile u8 tool_sending, tool_in_busy, tool_drop;
static u32 tool_client, tool_in_at;
static struct { u32 client; u8 tag; } tool_refusal[TOOL_REFUSALS];
static volatile u8 tool_refusals;
static u16 tool_len, tool_off, tool_rx;
static char tool_serial[TOOL_SERIAL];

static u32 tool_string(u8 index, u8 *out)
{
    const char *s = index == 1 ? USB_MANUFACTURER : index == 2 ? board_name : index == 3 ? tool_serial : NULL;
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

/* The core hands over the data stage a packet at a time: STAGE_OUT asks
 * for the next one. */
static u32 get32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* On interrupt IN, whatever is next: a refusal, else the answer's next
 * piece.  From the USB interrupt, or with it off. */
static void tool_pump(void)
{
    u8 *p = tool_in;

    if (tool_in_busy) return;
    memset(p, 0, PACKET);
    if (tool_refusals) {
        tool_refusals--;
        util_put32(p, tool_refusal[tool_refusals].client);
        p[4] = tool_refusal[tool_refusals].tag;
        p[5] = TOOL_REFUSED;
    } else if (tool_sending) {
        u16 n = tool_len - tool_off;

        if (n > TOOL_IN_DATA) n = TOOL_IN_DATA;
        util_put32(p, tool_client);
        p[4] = tool_tag;
        p[5] = tool_off + n < tool_len ? TOOL_MORE : TOOL_LAST;
        util_put16(p + 6, tool_off);
        memcpy(p + TOOL_IN_HEAD, tool_answer + tool_off, n);
        tool_off = (u16)(tool_off + n);
        if (p[5] == TOOL_LAST) tool_sending = tool_busy = 0;
    } else {
        return;
    }
    tool_in_busy = 1;
    tool_in_at = plat_ms();
    usb_g_intr_write(0, EP_IN, p, PACKET);
}

static void tool_in_done(struct usb_device_t *dev, u32 ep)
{
    (void)dev;
    (void)ep;
    tool_in_busy = 0;
    tool_pump();
}

/* The first packet decides: while a command runs, a new one is drained
 * and dropped, and its client told so when it gave one. */
static u32 tool_set_data(struct usb_device_t *dev, struct usb_ctrlrequest *req)
{
    u32 want = req->wLength < TOOL_REPORT ? req->wLength : TOOL_REPORT;
    u32 n = usb_read_count0(0);
    u8 head[PACKET];

    (void)dev;
    if (n > want - tool_rx) n = want - tool_rx;
    if (!tool_rx) {
        usb_read_ep0(0, head, n);
        tool_drop = tool_busy;
        if (!tool_drop) memcpy(tool_req, head, n);
        else if (n >= TOOL_HEAD_IN && (head[18] & TOOL_INPUT) && tool_refusals < TOOL_REFUSALS) {
            tool_refusal[tool_refusals].client = get32(head + 20);
            tool_refusal[tool_refusals].tag = head[19];
            tool_refusals++;
            tool_pump();
        }
    } else {
        usb_read_ep0(0, tool_drop ? head : tool_req + tool_rx, n);
    }
    tool_rx = (u16)(tool_rx + n);
    if (n == PACKET && tool_rx < want) return USB_EP0_STAGE_OUT;
    if (!tool_drop && tool_rx >= ((tool_req[18] & TOOL_INPUT) ? TOOL_HEAD_IN : TOOL_HEAD)) {
        tool_tag = tool_req[19];
        tool_state = TOOL_BUSY;
        tool_busy = 1;
        tool_pending = 1;
        plat_raise(EV_USB);
    }
    return USB_EP0_STAGE_SETUP;
}

static void tool_get(struct usb_device_t *dev, struct usb_ctrlrequest *req)
{
    u8 *buf = usb_get_setup_buffer(dev);
    u16 n = 0;

    memset(buf, 0, TOOL_REPORT);
    if (tool_state == TOOL_MORE || tool_state == TOOL_LAST) {
        n = tool_len - tool_off;
        if (n > TOOL_REPORT - TOOL_REPLY_HEAD) n = TOOL_REPORT - TOOL_REPLY_HEAD;
        memcpy(buf + TOOL_REPLY_HEAD, tool_answer + tool_off, n);
        tool_off = (u16)(tool_off + n);
        tool_state = tool_off < tool_len ? TOOL_MORE : TOOL_LAST;
    }
    buf[0] = tool_tag;
    buf[1] = tool_state;
    buf[2] = (u8)n;
    buf[3] = (u8)(n >> 8);
    reply(dev, req, buf, req->wLength < TOOL_REPORT ? req->wLength : TOOL_REPORT);
}

static void tool_reset(struct usb_device_t *dev, u32 itf)
{
    (void)dev;
    (void)itf;
    tool_in_busy = tool_sending = tool_busy = tool_refusals = 0;
    usb_g_ep_config(0, EP_IN | USB_DIR_IN, USB_ENDPOINT_XFER_INT, 1, dma_in, PACKET);
    usb_g_set_intr_hander(0, EP_IN | USB_DIR_IN, tool_in_done);
    usb_enable_ep(0, EP_IN);
}

static u32 tool_hook(struct usb_device_t *dev, struct usb_ctrlrequest *req)
{
    u8 *buf = usb_get_setup_buffer(dev);
    u32 type = req->bRequestType & USB_TYPE_MASK;
    u32 n;

    if (type == USB_TYPE_STANDARD) {
        if (req->bRequest == USB_REQ_SET_CONFIGURATION) {
            tool_reset(dev, 0);
            return 0;            /* the core answers it */
        }
        if (req->bRequest != USB_REQ_GET_DESCRIPTOR) return 0;
        switch (HIBYTE(req->wValue)) {
        case USB_DT_DEVICE: reply(dev, req, device_desc, sizeof(device_desc)); return 1;
        case USB_DT_CONFIG: reply(dev, req, config_desc, config_len); return 1;
        case USB_DT_STRING:
            n = tool_string(LOBYTE(req->wValue), buf);
            if (n) reply(dev, req, buf, n);
            else stall(dev);
            return 1;
        case DT_HID: reply(dev, req, config_desc + hid_at, 9); return 1;
        case DT_REPORT: reply(dev, req, tool_report_desc, sizeof(tool_report_desc)); return 1;
        }
        stall(dev);
        return 1;
    }
    if (type == USB_TYPE_CLASS && (req->bRequestType & USB_RECIP_MASK) == USB_RECIP_INTERFACE) {
        switch (req->bRequest) {
        case REQ_GET_REPORT:
            if (HIBYTE(req->wValue) != 3) break;
            tool_get(dev, req);
            return 1;
        case REQ_SET_REPORT:
            if (HIBYTE(req->wValue) != 3) break;
            tool_rx = 0;
            usb_set_setup_recv(dev, tool_set_data);
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
    }
    stall(dev);
    return 1;
}

/* The descriptors: V-USB's generic HID ID (pad.h), our manufacturer, the
 * board as product, the flash UID as serial; the HID interface as a
 * personality's, with its IN endpoint. */
void plat_usb_tool_mode(void)
{
    const u8 *uid = plat_unique_id();
    u8 hid[9] = { 9, DT_HID, 0x11, 0x01, 0, 1, DT_REPORT, sizeof(tool_report_desc), 0 };
    u8 *p = config_desc + 9;
    u32 i;

    for (i = 0; i < 16; i++) {
        static const char hex[] = "0123456789ABCDEF";

        tool_serial[2 * i] = uid ? hex[uid[i] >> 4] : '0';
        tool_serial[2 * i + 1] = uid ? hex[uid[i] & 15] : '0';
    }
    get_device_descriptor(device_desc);
    device_desc[4] = device_desc[5] = device_desc[6] = 0;   /* the class is the interface's */
    device_desc[8] = (u8)USB_OWN_VID;
    device_desc[9] = (u8)(USB_OWN_VID >> 8);
    device_desc[10] = (u8)USB_OWN_HID_PID;
    device_desc[11] = (u8)(USB_OWN_HID_PID >> 8);
    device_desc[14] = 1;
    device_desc[15] = 2;
    device_desc[16] = 3;
    p = interface(p, 0, 1, USB_CLASS_HID, 0, 0);
    hid_at = (u8)(p - config_desc);
    p = put(p, hid, sizeof(hid));
    p = endpoint(p, 0x80 | EP_IN, USB_ENDPOINT_XFER_INT, PACKET, 1);
    config_len = (u16)(p - config_desc);
    {
        u8 head[9] = { 9, USB_DT_CONFIG, (u8)config_len, (u8)(config_len >> 8), 1, 1, 0, 0x80, 50 };

        memcpy(config_desc, head, 9);
    }
    tool_pending = tool_busy = tool_sending = tool_in_busy = tool_refusals = 0;
    tool_state = TOOL_IDLE;
    usb_device_mode(0, 0x80000000);      /* no SDK class: the core and its EP0 only */
    dma_in = usb_alloc_ep_dmabuffer(0, EP_IN | USB_DIR_IN, PACKET);
    usb_set_reset_hander(0, 0, tool_reset);
    usb_set_setup_hook(usb_id2device(0), tool_hook);
}

void plat_usb_tool_service(void)
{
    u8 cdb[16];
    u16 len, head;
    u8 out, input;
    u32 client;

    if (!tool_pending) {
        local_irq_disable();     /* a piece no one takes: no one is reading */
        if (tool_sending && tool_in_busy && plat_ms() - tool_in_at > TOOL_IN_WAIT_MS)
            tool_sending = tool_busy = 0;
        local_irq_enable();
        return;
    }
    local_irq_disable();
    memcpy(cdb, tool_req, 16);
    len = (u16)(tool_req[16] | tool_req[17] << 8);
    out = tool_req[18] & TOOL_OUT;
    input = tool_req[18] & TOOL_INPUT;
    client = get32(tool_req + 20);
    head = input ? TOOL_HEAD_IN : TOOL_HEAD;
    if (len > (out ? TOOL_REPORT - head : TOOL_REPORT)) len = out ? TOOL_REPORT - head : TOOL_REPORT;
    if (out) memcpy(tool_answer, tool_req + head, len);
    tool_pending = 0;
    local_irq_enable();
    if (out) {
        usb_command(cdb, tool_answer, len, 1);
        len = 0;
    } else {
        memset(tool_answer, 0, len);
        usb_command(cdb, tool_answer, len, 0);
    }
    local_irq_disable();
    tool_len = len;
    tool_off = 0;
    tool_state = input ? TOOL_IDLE : TOOL_LAST;     /* GET_REPORT takes no pieces of it */
    if (input) {
        tool_client = client;
        tool_sending = 1;
        tool_in_at = plat_ms();
        tool_pump();
    } else {
        tool_busy = 0;
    }
    local_irq_enable();
}

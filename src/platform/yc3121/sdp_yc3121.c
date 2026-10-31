#include <string.h>
#include "pad.h"
#include "sdp_yc3121.h"

/* The SDP records the Bluetooth core serves, in the form its command 0xa0
 * takes (seen in stock v1.01's records): per record a count of UUID16s and
 * the UUIDs (big-endian; what a service search matches), the u32 record
 * handle (big-endian), then the record as a data element sequence of
 * attribute ID / value pairs.  Ours: the HID device (as BTstack's
 * hid_create_sdp_record() lays it out) and Device ID (PnP). */

struct w {
    u8 *out;
    u16 at, max;
    u8 bad;
};

static void put(struct w *w, u8 b)
{
    if (w->at < w->max) w->out[w->at++] = b;
    else w->bad = 1;
}

static void be16(struct w *w, u16 v) { put(w, (u8)(v >> 8)); put(w, (u8)v); }
static void be32(struct w *w, u32 v) { be16(w, (u16)(v >> 16)); be16(w, (u16)v); }

static void uint8(struct w *w, u8 v) { put(w, 0x08); put(w, v); }
static void uint16(struct w *w, u16 v) { put(w, 0x09); be16(w, v); }
static void uint32(struct w *w, u32 v) { put(w, 0x0a); be32(w, v); }
static void uuid16(struct w *w, u16 v) { put(w, 0x19); be16(w, v); }
static void boolean(struct w *w, u8 v) { put(w, 0x28); put(w, v); }
static void attr(struct w *w, u16 id) { uint16(w, id); }

static void bytes(struct w *w, u8 type8, const u8 *p, u16 n)
{
    if (n < 256) {
        put(w, type8), put(w, (u8)n);
    } else {
        put(w, (u8)(type8 + 1)), be16(w, n);
    }
    while (n--) put(w, *p++);
}

/* A sequence: open() leaves room for a u16 length, close() fills it in. */
static u16 open(struct w *w) { put(w, 0x36); be16(w, 0); return w->at; }

static void close(struct w *w, u16 from)
{
    u16 n = (u16)(w->at - from);

    if (from >= 2 && from <= w->max) {
        w->out[from - 2] = (u8)(n >> 8);
        w->out[from - 1] = (u8)n;
    }
}

static void header(struct w *w, const u16 *uuids, u8 count, u32 handle)
{
    put(w, count);
    for (u8 i = 0; i < count; i++) be16(w, uuids[i]);
    be32(w, handle);
}

static void protocol(struct w *w, u16 psm, u16 upper)
{
    u16 a = open(w), b = open(w);
    uuid16(w, 0x0100);                   /* L2CAP */
    uint16(w, psm);
    close(w, b);
    b = open(w);
    uuid16(w, upper);
    close(w, b);
    close(w, a);
}

static void browse_and_language(struct w *w)
{
    u16 s;

    attr(w, 0x0005);                     /* BrowseGroupList: public */
    s = open(w), uuid16(w, 0x1002), close(w, s);
    attr(w, 0x0006);                     /* LanguageBaseAttributeIDList */
    s = open(w), uint16(w, 0x656e), uint16(w, 0x006a), uint16(w, 0x0100), close(w, s);
}

static void profile(struct w *w, u16 uuid, u16 version)
{
    u16 a, b;

    attr(w, 0x0009);                     /* BluetoothProfileDescriptorList */
    a = open(w), b = open(w);
    uuid16(w, uuid), uint16(w, version);
    close(w, b), close(w, a);
}

u16 yc_sdp_records(u8 *out, u16 max, const struct yc_sdp_hid *h)
{
    static const u16 hid_uuids[] = { 0x1124, 0x0100, 0x0011, 0x1002 };
    static const u16 pnp_uuids[] = { 0x1200, 0x0100, 0x0001, 0x1002 };
    struct w w = { out, 0, max, 0 };
    u16 rec, s, t, u;

    header(&w, hid_uuids, 4, 0x00010000);
    rec = open(&w);
    attr(&w, 0x0000), uint32(&w, 0x00010000);
    attr(&w, 0x0001);                    /* ServiceClassIDList: HID */
    s = open(&w), uuid16(&w, 0x1124), close(&w, s);
    attr(&w, 0x0004);                    /* control channel */
    protocol(&w, 0x0011, 0x0011);
    browse_and_language(&w);
    profile(&w, 0x1124, 0x0101);
    attr(&w, 0x000d);                    /* AdditionalProtocolDescriptorLists: interrupt */
    s = open(&w), protocol(&w, 0x0013, 0x0011), close(&w, s);
    attr(&w, 0x0100), bytes(&w, 0x25, (const u8 *)h->name, (u16)strlen(h->name));
    attr(&w, 0x0201), uint16(&w, 0x0111);           /* HIDParserVersion */
    attr(&w, 0x0202), uint8(&w, h->subclass);       /* HIDDeviceSubclass */
    attr(&w, 0x0203), uint8(&w, 0);                 /* HIDCountryCode */
    attr(&w, 0x0204), boolean(&w, 1);               /* HIDVirtualCable */
    attr(&w, 0x0205), boolean(&w, 1);               /* HIDReconnectInitiate */
    attr(&w, 0x0206);                               /* HIDDescriptorList */
    s = open(&w), t = open(&w);
    uint8(&w, 0x22), bytes(&w, 0x25, h->descriptor, h->descriptor_len);
    close(&w, t), close(&w, s);
    attr(&w, 0x0207);                               /* HIDLANGIDBaseList */
    s = open(&w), t = open(&w), uint16(&w, 0x0409), uint16(&w, 0x0100), close(&w, t), close(&w, s);
    attr(&w, 0x020b), uint16(&w, 0x0100);           /* HIDProfileVersion */
    attr(&w, 0x020c), uint16(&w, 0x0c80);           /* HIDSupervisionTimeout */
    attr(&w, 0x020d), boolean(&w, 0);               /* HIDNormallyConnectable */
    attr(&w, 0x020e), boolean(&w, 0);               /* HIDBootDevice */
    close(&w, rec);

    header(&w, pnp_uuids, 4, 0x00010001);
    rec = open(&w);
    attr(&w, 0x0000), uint32(&w, 0x00010001);
    attr(&w, 0x0001);                    /* ServiceClassIDList: Device ID */
    u = open(&w), uuid16(&w, 0x1200), close(&w, u);
    attr(&w, 0x0004);
    protocol(&w, 0x0001, 0x0001);        /* L2CAP PSM 1: SDP */
    browse_and_language(&w);
    profile(&w, 0x1200, 0x0103);
    attr(&w, 0x0200), uint16(&w, 0x0103);           /* SpecificationID */
    attr(&w, 0x0201), uint16(&w, h->vendor);
    attr(&w, 0x0202), uint16(&w, h->product);
    attr(&w, 0x0203), uint16(&w, h->version);
    attr(&w, 0x0204), boolean(&w, 1);               /* PrimaryRecord */
    attr(&w, 0x0205), uint16(&w, 0x0002);           /* VendorIDSource: USB */
    close(&w, rec);
    return w.bad ? 0 : w.at;
}

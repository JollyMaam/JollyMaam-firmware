#ifndef SDP_YC3121_H
#define SDP_YC3121_H

#include "pad.h"

struct yc_sdp_hid {
    const char *name;
    const u8 *descriptor;
    u16 descriptor_len;
    u8 subclass;                 /* HIDDeviceSubclass: 0x08 gamepad */
    u16 vendor, product, version;    /* Device ID (USB vendor IDs) */
};

/* The HID and Device ID records, as the core's command 0xa0 takes them:
 * the length, or 0 if they do not fit in max. */
u16 yc_sdp_records(u8 *out, u16 max, const struct yc_sdp_hid *h);

#endif

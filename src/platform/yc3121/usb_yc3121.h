#ifndef USB_YC3121_H
#define USB_YC3121_H

#include "pad.h"

/* What the controller has seen since start, for bring-up. */
struct usb_events {
    u32 resets, setups, odd_setups, stalls, commands;
    u8 addressed, configured, odd_length;
    u8 stalled[8];               /* the last refused SETUP */
};

void plat_usb_init(void);
void plat_usb_poll(void);
const struct usb_events *plat_usb_events(void);

/* The app's: a tool command (usb.c's CDB at report[0..15]) with n bytes of
 * data in data[]; its answer goes in data[] (at most max), its length
 * returned. */
u32 usb_tool_command(const u8 *report, u8 *data, u32 n, u32 max);

#endif

#ifndef USB_YC3121_H
#define USB_YC3121_H

#include "pad.h"

/* What the controller has seen since start, for bring-up. */
struct usb_events {
    u32 resets, setups, odd_setups, stalls, commands;
    u32 set_reports, get_reports, out_packets, out_idle;   /* out_idle: no SET_REPORT open */
    u8 out_last;
    u32 incomplete;              /* a SETUP came before the SET_REPORT's data was all in */
    u16 incomplete_got, incomplete_want;
    u32 loop_max_us;             /* the app's: its longest pass between polls */
    u8 addressed, configured, odd_length;
    u8 stalled[8];               /* the last refused SETUP */
};

/* Bring-up: the last EP0 events (state: bit 0 receiving, 1 sending, the
 * rest the SET_REPORT bytes so far / 4). */
#define USB_TRACE 64
struct usb_trace_entry {
    u16 ms;
    u8 status, sent, len, type, request, state;
};
struct usb_trace {
    u32 next;
    u32 stop;                    /* nonzero: frozen there (after a lost SET_REPORT) */
    struct usb_trace_entry e[USB_TRACE];
};
extern struct usb_trace usb_trace;

void plat_usb_init(void);
void plat_usb_drain(void);
void plat_usb_poll(void);       /* the interrupt's (plat_usb_irq) */
void plat_usb_irq(void);
const struct usb_events *plat_usb_events(void);

/* The app's, called from plat_usb_service() on the main loop: a tool
 * command (usb.c's CDB at report[0..15], the report's flags at report[18])
 * with n bytes of data in data[]; its answer goes in data[] (at most max),
 * its length returned.  The interrupt takes a command; until it is answered
 * the tools read TOOL_BUSY. */
u32 usb_tool_command(const u8 *report, u8 *data, u32 n, u32 max);
void plat_usb_service(void);
int plat_usb_tool_waiting(void);
int plat_usb_wake(void);         /* a tool command, or a personality's event (cleared) */

#endif

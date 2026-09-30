#ifndef BTSTACK_JL_HID_EXT_H
#define BTSTACK_JL_HID_EXT_H

#include "generic/typedef.h"

void btstack_jl_hid_set_device_id(u16 vendor, u16 product, u16 version);

/* GET_REPORT from the host, on the BTstack run loop.  report_type is the HIDP
 * one (1 input, 2 output, 3 feature).  Fill exactly `size` bytes of report
 * body (no report id) and return 1, or return 0 to refuse. */
void btstack_jl_hid_set_get_report_handler(
    int (*handler)(u8 report_type, u8 report_id, u8 *report, u16 size));

/* The controller's input state on the interrupt channel.  It replaces an
 * input report the link has not taken yet, so the host always gets the newest
 * state, never a backlog; replies and events still go through
 * edr_hid_data_send(), queued and ahead of it.  0 on success. */
int edr_hid_input_send(u8 report_id, u8 *data, u16 len);

#endif

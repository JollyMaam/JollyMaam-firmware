#ifndef BTSTACK_JL_HID_EXT_H
#define BTSTACK_JL_HID_EXT_H

/*
 * Classic HID settings the SDK's edr_init_cfg_t has no field for.  Set them
 * before btstack_init(), which uses them for that session and then puts back
 * the Switch profile's (Device ID 057e:2009 version 0x0001, no GET_REPORT
 * handler).
 */

#include "generic/typedef.h"

/* Device ID (PnP) SDP record, USB vendor-id source. */
void btstack_jl_hid_set_device_id(u16 vendor, u16 product, u16 version);

/* GET_REPORT from the host, on the BTstack run loop.  report_type is the HIDP
 * one (1 input, 2 output, 3 feature).  Fill exactly `size` bytes of report
 * body (no report id) and return 1, or return 0 to refuse. */
void btstack_jl_hid_set_get_report_handler(
    int (*handler)(u8 report_type, u8 report_id, u8 *report, u16 size));

#endif

#ifndef CYCLONE2_PROFILE_CLASSIC_H
#define CYCLONE2_PROFILE_CLASSIC_H

/*
 * Bluetooth Classic HID device session, shared by the profiles that differ
 * only in what they say over HID (Switch Pro Controller, Xbox, DualSense
 * Edge): pairing, reconnecting to the slot's host by page, bond cleanup, and
 * opening the HID channels once the link is encrypted.
 */

#include "generic/typedef.h"
#include "system/event.h"
#include "app_comm_bt.h"
#include "profile.h"

struct cyclone2_classic_profile {
    const char *local_name;
    /* Class of device, SSP settings, report descriptor, sniff parameters. */
    const edr_init_cfg_t *edr;
    /* Device ID (PnP) record: which host driver binds. */
    u16 vendor_id;
    u16 product_id;
    u16 version;
    /* Output reports from the host: HIDP header (0xa2 interrupt, 0x52
     * SET_REPORT), report id, body. */
    void (*output)(u8 *packet, u16 size, u16 channel);
    /* Optional GET_REPORT handler, see btstack_jl_hid_ext.h. */
    int (*get_report)(u8 report_type, u8 report_id, u8 *report, u16 size);
    /* Optional: reset the profile's HID state as a session starts. */
    void (*session_start)(void);
    /* Optional: an ACL link to the host came up. */
    void (*link_up)(void);
    /* Optional: the session is being torn down. */
    void (*session_exit)(void);
};

void cyclone2_classic_start(const struct cyclone2_classic_profile *profile,
                            u8 slot, u8 pairing);
void cyclone2_classic_disconnect(void);
int cyclone2_classic_exit(void);
int cyclone2_classic_is_connected(void);
int cyclone2_classic_reconnect(void);
enum cyclone2_profile_event cyclone2_classic_handle_event(
    const struct sys_event *event);
/* Drop the controller's stored bonds: one peer, or all of them. */
void cyclone2_classic_forget_peer(const u8 address[6]);
void cyclone2_classic_forget_all(void);

#endif

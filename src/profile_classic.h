#ifndef CYCLONE2_PROFILE_CLASSIC_H
#define CYCLONE2_PROFILE_CLASSIC_H

#include "generic/typedef.h"
#include "system/event.h"
#include "app_comm_bt.h"
#include "profile.h"

struct cyclone2_classic_profile {
    const char *local_name;

    const edr_init_cfg_t *edr;

    u16 vendor_id;
    u16 product_id;
    u16 version;

    void (*output)(u8 *packet, u16 size, u16 channel);

    int (*get_report)(u8 report_type, u8 report_id, u8 *report, u16 size);
    /* Optional: reset the profile's HID state as a session starts. */
    void (*session_start)(void);

    void (*link_up)(void);

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

void cyclone2_classic_forget_peer(const u8 address[6]);
void cyclone2_classic_forget_all(void);

#endif

#ifndef CYCLONE2_PROFILE_H
#define CYCLONE2_PROFILE_H

#include "generic/typedef.h"
#include "system/event.h"
#include "input.h"

enum cyclone2_profile_type {
    CYCLONE2_PROFILE_SWITCH,
    CYCLONE2_PROFILE_GENERIC,
    CYCLONE2_PROFILE_SWITCH2,
    CYCLONE2_PROFILE_TYPE_COUNT,
};

enum cyclone2_profile_event {
    CYCLONE2_PROFILE_EVENT_NONE         = 0,
    CYCLONE2_PROFILE_EVENT_CONNECTED    = 1U << 0,
    CYCLONE2_PROFILE_EVENT_DISCONNECTED = 1U << 1,
};

/* The application owns slot selection and policy.  This module hides the
 * SDK-specific differences between Classic HID and BLE HID. */
void cyclone2_profile_start(enum cyclone2_profile_type type, u8 slot,
                            u8 pairing);
void cyclone2_profile_disconnect(void);
void cyclone2_profile_exit(void);
int cyclone2_profile_is_connected(void);
int cyclone2_profile_send(const struct cyclone2_input_report *report);
enum cyclone2_profile_event cyclone2_profile_handle_event(
    const struct sys_event *event);
const char *cyclone2_profile_name(enum cyclone2_profile_type type);

void cyclone2_generic_prepare_stack(void);
void cyclone2_generic_start(u8 slot, u8 pairing);
void cyclone2_generic_disconnect(void);
void cyclone2_generic_exit(void);
int cyclone2_generic_is_connected(void);
int cyclone2_generic_send(const struct cyclone2_input_report *report);
enum cyclone2_profile_event cyclone2_generic_handle_event(
    const struct sys_event *event);

void cyclone2_switch2_prepare_stack(void);
void cyclone2_switch2_start(u8 slot, u8 pairing);
void cyclone2_switch2_disconnect(void);
void cyclone2_switch2_exit(void);
int cyclone2_switch2_is_connected(void);
int cyclone2_switch2_send(const struct cyclone2_input_report *report);
enum cyclone2_profile_event cyclone2_switch2_handle_event(
    const struct sys_event *event);

void cyclone2_switch_start(u8 slot, u8 pairing);
void cyclone2_switch_disconnect(void);
void cyclone2_switch_exit(void);
int cyclone2_switch_is_connected(void);
int cyclone2_switch_send(const struct cyclone2_input_report *report);
enum cyclone2_profile_event cyclone2_switch_handle_event(
    const struct sys_event *event);
void cyclone2_switch_forget_peer(const u8 address[6]);
void cyclone2_switch_forget_all(void);

#endif

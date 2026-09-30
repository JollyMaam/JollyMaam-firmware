#ifndef CYCLONE2_PROFILE_H
#define CYCLONE2_PROFILE_H

#include "generic/typedef.h"
#include "system/event.h"
#include "input.h"
#include "profile_types.h"

enum cyclone2_profile_event {
    CYCLONE2_PROFILE_EVENT_NONE         = 0,
    CYCLONE2_PROFILE_EVENT_CONNECTED    = 1U << 0,
    CYCLONE2_PROFILE_EVENT_DISCONNECTED = 1U << 1,
};

void cyclone2_profile_start(enum cyclone2_profile_type type, u8 slot,
                            u8 pairing);
void cyclone2_profile_disconnect(void);
/* Returns non-zero once the radio session really is down; the caller must not
 * start the next profile before that, or the two sessions overlap. */
int cyclone2_profile_exit(void);
int cyclone2_profile_is_connected(void);
int cyclone2_profile_reconnect(void);
int cyclone2_profile_send(const struct cyclone2_input_report *report);
enum cyclone2_profile_event cyclone2_profile_handle_event(
    const struct sys_event *event);
const char *cyclone2_profile_name(enum cyclone2_profile_type type);
u16 cyclone2_profile_default_hz(enum cyclone2_profile_type type);

void cyclone2_generic_prepare_stack(void);
void cyclone2_generic_start(u8 slot, u8 pairing);
void cyclone2_generic_disconnect(void);
int cyclone2_generic_exit(void);
int cyclone2_generic_is_connected(void);
int cyclone2_generic_send(const struct cyclone2_input_report *report);
enum cyclone2_profile_event cyclone2_generic_handle_event(
    const struct sys_event *event);

void cyclone2_switch2_prepare_stack(void);
void cyclone2_switch2_start(u8 slot, u8 pairing);
int cyclone2_switch2_wake_check(u8 slot, u8 channel, u8 seconds);
int cyclone2_switch2_send(const struct cyclone2_input_report *report);

void cyclone2_switch_start(u8 slot, u8 pairing);
int cyclone2_switch_send(const struct cyclone2_input_report *report);

void cyclone2_xbox_start(u8 slot, u8 pairing);
int cyclone2_xbox_send(const struct cyclone2_input_report *report);

void cyclone2_xbox_ble_prepare_stack(void);
void cyclone2_xbox_ble_start(u8 slot, u8 pairing);
int cyclone2_xbox_ble_send(const struct cyclone2_input_report *report);

void cyclone2_dualsense_start(u8 slot, u8 pairing);
int cyclone2_dualsense_send(const struct cyclone2_input_report *report);

void cyclone2_stickmouse_start(u8 slot, u8 pairing);
void cyclone2_gyromouse_start(u8 slot, u8 pairing);
int cyclone2_mouse_send(const struct cyclone2_input_report *report);

int cyclone2_profile_le_exit(void);
enum cyclone2_profile_event cyclone2_profile_le_handle_event(
    const struct sys_event *event);

int cyclone2_profile_wake_check(enum cyclone2_profile_type type, u8 slot,
                               u8 channel, u8 seconds);
/* Positive means reconnect immediately; zero means poll the wake action. */
int cyclone2_profile_wake_start(enum cyclone2_profile_type type, u8 slot,
                               u8 channel, u8 seconds);
void cyclone2_profile_wake_stop(void);
int cyclone2_profile_wake_step(u32 now_ms);

#endif

#ifndef CYCLONE2_PROFILE_INTERNAL_H
#define CYCLONE2_PROFILE_INTERNAL_H

#include "profile.h"

void cyclone2_generic_prepare_stack(void);
void cyclone2_generic_start(u8 slot, u8 pairing);
void cyclone2_generic_disconnect(void);
void cyclone2_generic_exit(void);
int cyclone2_generic_is_connected(void);
int cyclone2_generic_send(const struct cyclone2_input_report *report);
enum cyclone2_profile_event cyclone2_generic_handle_event(
    const struct sys_event *event);

void cyclone2_switch_start(u8 slot, u8 pairing);
void cyclone2_switch_disconnect(void);
void cyclone2_switch_exit(void);
int cyclone2_switch_is_connected(void);
int cyclone2_switch_send(const struct cyclone2_input_report *report);
enum cyclone2_profile_event cyclone2_switch_handle_event(
    const struct sys_event *event);

#endif

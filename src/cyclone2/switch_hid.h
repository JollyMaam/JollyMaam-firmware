#ifndef CYCLONE2_SWITCH_HID_H
#define CYCLONE2_SWITCH_HID_H

#include "generic/typedef.h"
#include "cyclone2/input.h"

#define CYCLONE2_SWITCH_REPORT_MAP_SIZE 170U

extern const u8 cyclone2_switch_report_map[CYCLONE2_SWITCH_REPORT_MAP_SIZE];
extern const u16 cyclone2_switch_report_map_size;

void cyclone2_switch_hid_initialize(void);
void cyclone2_switch_hid_send(const struct cyclone2_input_report *input);

#endif

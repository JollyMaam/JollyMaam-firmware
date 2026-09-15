#ifndef CYCLONE2_INPUT_H
#define CYCLONE2_INPUT_H

#include "generic/typedef.h"

enum cyclone2_button {
    /* Keep these bit positions aligned with Linux's mapping of HID Button
     * usages 1..16 to BTN_SOUTH..BTN_DEAD.  SDL can then construct the
     * standard GameController mapping without a device-specific database
     * entry.  BTN_C, BTN_Z, BTN_TL2 and BTN_TR2 have no matching standard
     * digital role here, so use those slots for the four extra controls. */
    CYCLONE2_REPORT_BUTTON_A          = 1U << 0,
    CYCLONE2_REPORT_BUTTON_B          = 1U << 1,
    CYCLONE2_REPORT_BUTTON_SCREENSHOT = 1U << 2,  /* BTN_C */
    CYCLONE2_REPORT_BUTTON_X          = 1U << 3,
    CYCLONE2_REPORT_BUTTON_Y          = 1U << 4,
    CYCLONE2_REPORT_BUTTON_M          = 1U << 5,  /* BTN_Z */
    CYCLONE2_REPORT_BUTTON_L1         = 1U << 6,
    CYCLONE2_REPORT_BUTTON_R1         = 1U << 7,
    CYCLONE2_REPORT_BUTTON_L4         = 1U << 8,  /* BTN_TL2 */
    CYCLONE2_REPORT_BUTTON_R4         = 1U << 9,  /* BTN_TR2 */
    CYCLONE2_REPORT_BUTTON_SELECT     = 1U << 10,
    CYCLONE2_REPORT_BUTTON_START      = 1U << 11,
    CYCLONE2_REPORT_BUTTON_HOME       = 1U << 12,
    CYCLONE2_REPORT_BUTTON_L3         = 1U << 13,
    CYCLONE2_REPORT_BUTTON_R3         = 1U << 14,
};

enum cyclone2_hat {
    CYCLONE2_HAT_UP = 0,
    CYCLONE2_HAT_UP_RIGHT,
    CYCLONE2_HAT_RIGHT,
    CYCLONE2_HAT_DOWN_RIGHT,
    CYCLONE2_HAT_DOWN,
    CYCLONE2_HAT_DOWN_LEFT,
    CYCLONE2_HAT_LEFT,
    CYCLONE2_HAT_UP_LEFT,
    CYCLONE2_HAT_NEUTRAL,
};

struct cyclone2_input_report {
    u16 buttons;
    u8 hat;
    u8 left_x;
    u8 left_y;
    u8 right_x;
    u8 right_y;
    u8 left_trigger;
    u8 right_trigger;
} __attribute__((packed));

void cyclone2_input_initialize(void);
void cyclone2_input_read(struct cyclone2_input_report *report);

#endif

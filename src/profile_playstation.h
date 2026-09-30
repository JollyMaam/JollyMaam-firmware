#ifndef CYCLONE2_PROFILE_PLAYSTATION_H
#define CYCLONE2_PROFILE_PLAYSTATION_H

#include "generic/typedef.h"
#include "input.h"

/* What the DualShock 4 and DualSense profiles share: both Bluetooth report
 * families carry the same CRC, button bytes, touch points and motion axes. */

void playstation_put_u16(u8 *output, u16 value);
void playstation_put_u32(u8 *output, u32 value);

/* CRC over the HIDP header byte (0xa1 input, 0xa3 feature), the report id and
 * the body before its last four bytes, stored little endian in those four. */
void playstation_seal(u8 header, u8 report_id, u8 *body, u16 size);

/* The three button bytes: hat and face buttons, shoulders and sticks, then
 * PS (Home) in bit 0 and the touchpad click (M) in bit 1.  Face buttons go by
 * position: the bottom one (A) is cross. */
void playstation_buttons(const struct cyclone2_input_report *input, u8 out[3]);

/* Gyro, then accelerometer, three little-endian axes each, in the
 * controllers' own frame.  Returns 0 and the sample time, or -1 with nothing
 * written when there is no sample yet. */
int playstation_motion(u8 out[12], u32 *timestamp_us);

/* M puts a finger on the touchpad, in the middle, and the gyro moves it
 * (profile.#.gyro_speed points per degree).  M is also the touchpad click;
 * only a tap of it clicks, so that a finger moved with M held is not a
 * click-and-drag.  The click is held a moment after the tap, with the finger
 * still down, as a real press would be. */
struct playstation_touch {
    u16 width;
    u16 height;
    u8 down;
    u8 id;
    u8 clicking;
    s32 x;
    s32 y;
    u32 start_ms;
    u32 click_ms;
};

void playstation_touch_reset(struct playstation_touch *touch, u16 width,
                             u16 height);
/* Every report: moves the finger and turns the report's M into the click. */
void playstation_touch_update(struct playstation_touch *touch,
                              struct cyclone2_input_report *input, u32 now);
/* One touch point: bit 7 of the first byte is "no finger", the rest its
 * tracking id; then 12-bit x and y. */
void playstation_touch_fill(const struct playstation_touch *touch, u8 point[4]);

#endif

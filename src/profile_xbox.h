#ifndef CYCLONE2_PROFILE_XBOX_H
#define CYCLONE2_PROFILE_XBOX_H

/* Shared by the Xbox One S (Classic, profile_xbox.c) and Xbox Series X|S
 * (LE, profile_xbox_ble.c) profiles; the report layouts are documented in
 * profile_xbox.c. */

#include "generic/typedef.h"
#include "input.h"

/* Input report 0x01's 16-byte body.  series: Series X|S layout (View as
 * button 11, capture as Share in byte 15) instead of the One S one. */
void cyclone2_xbox_fill_state(u8 state[16],
                              const struct cyclone2_input_report *input,
                              int series);
/* Output report 0x03's 8-byte body: apply the rumble it asks for. */
void cyclone2_xbox_rumble(const u8 body[8]);

#endif

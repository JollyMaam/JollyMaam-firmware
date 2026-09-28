#ifndef CYCLONE2_PROFILE_XBOX_H
#define CYCLONE2_PROFILE_XBOX_H

#include "generic/typedef.h"
#include "input.h"

/* Input report 0x01's 16-byte body.  series: Series X|S layout (View as
 * button 11, capture as Share in byte 15) instead of the One S one. */
void cyclone2_xbox_fill_state(u8 state[16],
                              const struct cyclone2_input_report *input,
                              int series);

void cyclone2_xbox_rumble(const u8 body[8]);

#endif

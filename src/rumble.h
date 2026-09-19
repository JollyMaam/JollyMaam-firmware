#ifndef CYCLONE2_RUMBLE_H
#define CYCLONE2_RUMBLE_H

#include "generic/typedef.h"

#define CYCLONE2_RUMBLE_MAX_DURATION_MS 2000U

struct cyclone2_rumble_status {
    u8 left;
    u8 right;
    u8 active;
    u16 duration_ms;
};

void cyclone2_rumble_initialize(void);
int cyclone2_rumble_set(u8 left, u8 right, u16 duration_ms);
void cyclone2_rumble_stop(void);
void cyclone2_rumble_get_status(struct cyclone2_rumble_status *status);

#endif

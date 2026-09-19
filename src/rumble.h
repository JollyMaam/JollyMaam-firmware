#ifndef CYCLONE2_RUMBLE_H
#define CYCLONE2_RUMBLE_H

#include "generic/typedef.h"

#define CYCLONE2_RUMBLE_MAX_DURATION_MS 2000U

struct cyclone2_rumble_status {
    u32 updates;
    u8 left;
    u8 right;
    u8 active;
    u16 duration_ms;
    u32 last_request_ms;
    u32 last_apply_ms;
    u32 last_apply_latency_ms;
    u32 max_apply_latency_ms;
};

void cyclone2_rumble_initialize(void);
int cyclone2_rumble_set(u8 left, u8 right, u16 duration_ms);
void cyclone2_rumble_set_latched(u8 left, u8 right);
void cyclone2_rumble_stop(void);
void cyclone2_rumble_poll(u32 now_ms);
void cyclone2_rumble_get_status(struct cyclone2_rumble_status *status);

#endif

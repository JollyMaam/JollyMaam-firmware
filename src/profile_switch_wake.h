#ifndef CYCLONE2_WAKE_H
#define CYCLONE2_WAKE_H

#include "generic/typedef.h"

#define WAKE_INTERVAL_SLOTS_DEFAULT  6U

/* How long one whitening state is held before the sweep moves on.  At the
 * 3.75 ms burst period this is about 66 bursts, which is far longer than the
 * ~40 ms of replies the reference measures, and short enough that the default
 * duration below covers all 32 states in one attempt. */
#define WAKE_STATE_DWELL_MS        250U

#define WAKE_SECONDS_MIN             1U
#define WAKE_SECONDS_MAX            30U
#define WAKE_CHANNEL_MAX            78U

#define WAKE_CHANNEL_DEFAULT         0U

#define WAKE_SECONDS_DEFAULT         8U

struct wake_status {
    u8  active;
    u8  slot;
    u8  channel;
    u8  states_visited;
    u16 interval_slots;
    u16 seconds;
    u32 starts;
    u32 elapsed_ms;
    int last_error;
};

int wake_start(u8 slot, u8 channel, u8 seconds);

void wake_stop(void);

int wake_active(void);

void wake_status_get(struct wake_status *status);

int wake_step(u32 now_ms);

#endif

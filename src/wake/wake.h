#ifndef CYCLONE2_WAKE_H
#define CYCLONE2_WAKE_H

/*
 * Session layer for waking a sleeping Switch 1.  See docs/ns1-wakeup.md for
 * the protocol and for what has and has not been measured.
 *
 * The session is aimed at one profile slot, because each slot is its own
 * console: a pad paired with a Switch 1 in slot 0 and a Switch 2 in slot 1 has
 * two different Bluetooth addresses, and the wake-up access code is derived
 * from the console's address, not the pad's.  The address comes from the
 * per-slot peer record that a working connection writes (src/settings.c), so
 * nothing new has to be stored for the wake to have a target.
 */

#include "generic/typedef.h"

/* Default burst period: 6 slots of 625 us, the 3.75 ms the reference uses. */
#define WAKE_INTERVAL_SLOTS_DEFAULT  6U

/* How long one whitening state is held before the sweep moves on.  At the
 * 3.75 ms burst period this is about 66 bursts, which is far longer than the
 * ~40 ms of replies the reference measures, and short enough that the default
 * duration below covers all 32 states in one attempt. */
#define WAKE_STATE_DWELL_MS        250U

#define WAKE_SECONDS_MIN             1U
#define WAKE_SECONDS_MAX            30U
#define WAKE_CHANNEL_MAX            78U
/* What `wake N` uses when the console command does not say.  Channel 0 is
 * 2402 MHz, which is where the reference's own connection cycle starts. */
#define WAKE_CHANNEL_DEFAULT         0U
/* 32 states at the dwell above, so one default attempt walks the whole sweep
 * at least once.  See docs/ns1-wakeup.md. */
#define WAKE_SECONDS_DEFAULT         8U

struct wake_status {
    u8  active;
    u8  slot;
    u8  channel;
    u8  states_visited;
    u16 interval_slots;
    u16 seconds;
    u32 starts;               /* burst trains started (one per state) */
    u32 elapsed_ms;
    int last_error;
};

/* Aim a wake at the console slot `slot` last connected to.  Returns 0 when
 * the bursts have started, -1 if that slot has no peer address yet, -2 if the
 * arguments are out of range, -3 if the baseband refused the frame. */
int wake_start(u8 slot, u8 channel, u8 seconds);

/* Stop early.  Safe when idle. */
void wake_stop(void);

int wake_active(void);

void wake_status_get(struct wake_status *status);

/* Drive the session from the application poll.  Calls back into the
 * controller when the attempt is over; see cyclone2_wake_finished(). */
void wake_step(u32 now_ms);

#endif

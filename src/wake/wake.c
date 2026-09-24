/*
 * Session layer for the Switch 1 wake sequence.  See wake.h.
 *
 * What is transmitted, in one sentence: for the console's address, the
 * standard BR/EDR access code with the lower 32 bits of its sync word
 * inverted, carrying a body of the wake-up code repeated.  The baseband
 * applies its own whitening to every packet, so the body is pre-cancelled
 * against the state the baseband is about to use.
 *
 * The one thing this has to guess is which whitening state the console will
 * see, because this port has not captured what the BR23 baseband does there.
 * Rather than pick one, successive bursts walk the reference's 32 states, each
 * pre-cancelled to match.  That makes one burst in 32 self-consistent; if the
 * baseband's whitening turns out to be clock-driven and understood, the sweep
 * collapses to a single state.  docs/ns1-wakeup.md carries the reasoning.
 */

#include "generic/typedef.h"
#include <system/timer.h>

#include "controller.h"
#include "settings.h"
#include "usb.h"
#include "wake.h"
#include "wake_payload.h"
#include "wake_radio.h"
#include "wakeup_code.h"

static struct wake_status status;
static struct wake_radio_config config;
static uint8_t body[WAKE_BODY_MAX];
static u8 wake_state_index;
static u32 wake_phase_started_ms;
static u32 wake_deadline_ms;
static u32 wake_state_started_ms;
static u8 wake_target[6];

/* Start one whitening state's worth of bursts.  The body is rebuilt for the
 * state every time: pre-cancelling is what makes the wake-up code, rather than
 * a whitened version of it, reach the air. */
static int wake_start_state(u8 index)
{
    const uint8_t seed = wake_whiten_seed(index);
    uint8_t parity[5];
    unsigned i;
    int result;

    wake_build_phase1(wake_target, body, (u16)WAKE_BODY_MAX);
    /* DH3 and DH5 share the multi-slot payload header; the sweep stays on
     * multi-slot packets so every burst in the attempt is the same shape the
     * reference's phase 1 uses. */
    wake_whiten_cancel(body, (u16)WAKE_BODY_MAX, seed, WAKE_WHITEN_SKIP_MULTISLOT);

    for (i = 0; i < 6U; ++i) {
        config.address[i] = wake_target[i];
    }
    /* The wake-up parity is the standard one with the lower 32 bits of the
     * sync word inverted; this is the only value in the frame that differs
     * from what a normal transmit of this address would use. */
    wake_parity_inverted(wake_target, parity);
    for (i = 0; i < 5U; ++i) {
        config.parity[i] = parity[i];
    }
    config.channel = status.channel;
    config.interval_slots = status.interval_slots;
    config.llid = 2U;
    config.flow = 1U;
    config.edr = 0U;
    config.whiten_seed = seed;

    result = wake_radio_start(&config, body, (u16)WAKE_BODY_MAX);
    if (result == 0) {
        status.starts++;
    }
    return result;
}

int wake_start(u8 slot, u8 channel, u8 seconds)
{
    int result;

    if (channel > WAKE_CHANNEL_MAX) return -2;
    if (seconds < WAKE_SECONDS_MIN || seconds > WAKE_SECONDS_MAX) return -2;
    if (wake_active()) return -2;

    /* The target is the console this slot last connected to.  A slot that has
     * never connected has no address, and neither has a slot whose pairing
     * was just restarted, so there is nothing to aim at. */
    if (!cyclone2_peer_address_read(slot, wake_target)) return -1;
    {
        static const u8 zero[6] = {0, 0, 0, 0, 0, 0};

        if (!memcmp(wake_target, zero, 6U)) return -1;
    }

    memset(&status, 0, sizeof(status));
    status.active = 1U;
    status.slot = slot;
    status.channel = channel;
    status.seconds = seconds;
    status.interval_slots = WAKE_INTERVAL_SLOTS_DEFAULT;
    wake_state_index = 0U;
    wake_phase_started_ms = sys_timer_get_ms();
    wake_deadline_ms = wake_phase_started_ms + (u32)seconds * 1000U;
    wake_state_started_ms = wake_phase_started_ms;

    result = wake_start_state(wake_state_index);
    if (result != 0) {
        status.active = 0U;
        status.last_error = result;
        return -3;
    }
    cyclone2_usb_debug("wake slot=%u channel=%u seconds=%u",
                       slot, channel, seconds);
    return 0;
}

void wake_stop(void)
{
    if (!wake_active()) return;
    wake_radio_stop();
    status.active = 0U;
}

int wake_active(void)
{
    return status.active != 0U;
}

void wake_status_get(struct wake_status *out)
{
    if (out) {
        *out = status;
    }
}

void wake_step(u32 now_ms)
{
    if (!wake_active()) return;

    status.elapsed_ms = now_ms - wake_phase_started_ms;
    if ((s32)(now_ms - wake_deadline_ms) >= 0) {
        wake_radio_stop();
        status.active = 0U;
        cyclone2_usb_debug("wake finished: states=%u elapsed=%u ms",
                           status.states_visited, status.elapsed_ms);
        /* Hand back to the controller, which reconnects this slot. */
        cyclone2_wake_finished();
        return;
    }

    if (now_ms - wake_state_started_ms < WAKE_STATE_DWELL_MS) return;

    wake_radio_stop();
    wake_state_index = (u8)((wake_state_index + 1U) % WAKE_WHITEN_STATES);
    status.states_visited++;
    wake_state_started_ms = now_ms;
    if (wake_start_state(wake_state_index) != 0) {
        status.active = 0U;
        status.last_error = -3;
        cyclone2_wake_finished();
    }
}

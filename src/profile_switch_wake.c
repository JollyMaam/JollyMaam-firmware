/* BR23 whitening control is unverified; sweep the reference's 32 seeds.
 * Only the access code has been verified over the air. */

#include <string.h>
#include "generic/typedef.h"
#include <system/timer.h>

#include "config.h"
#include "usb.h"
#include "profile_switch_wake.h"
#include "profile_switch_wake_payload.h"
#include "profile_switch_wake_radio.h"
#include "profile_switch_wake_code.h"

/* Closing the SDK clears its baseband buffer window; reopen before raw TX. */
extern int jieli_btctrler_open(const uint8_t *bd_addr);
extern void jieli_btctrler_close(void);
static u8 controller_open;
static struct wake_status status;
static struct wake_radio_config radio_config;
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

    wake_whiten_cancel(body, (u16)WAKE_BODY_MAX, seed, WAKE_WHITEN_SKIP_MULTISLOT);

    for (i = 0; i < 6U; ++i) {
        radio_config.address[i] = wake_target[i];
    }

    wake_parity_inverted(wake_target, parity);
    for (i = 0; i < 5U; ++i) {
        radio_config.parity[i] = parity[i];
    }
    radio_config.channel = status.channel;
    radio_config.interval_slots = status.interval_slots;
    radio_config.llid = 2U;
    radio_config.flow = 1U;
    radio_config.edr = 0U;
    radio_config.whiten_seed = seed;

    result = wake_radio_start(&radio_config, body, (u16)WAKE_BODY_MAX);
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
    memcpy(wake_target, config_peer(slot), sizeof(wake_target));
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

    result = jieli_btctrler_open(NULL);
    controller_open = 1U;
    if (result == 0) result = wake_start_state(wake_state_index);
    if (result != 0) {
        wake_stop();
        status.last_error = result;
        return -3;
    }
    cyclone2_usb_debug("wake slot=%u channel=%u seconds=%u",
                       slot, channel, seconds);
    return 0;
}

void wake_stop(void)
{
    if (wake_active()) wake_radio_stop();
    status.active = 0U;
    if (controller_open) jieli_btctrler_close();
    controller_open = 0U;
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

int wake_step(u32 now_ms)
{
    if (!wake_active()) return 1;

    status.elapsed_ms = now_ms - wake_phase_started_ms;
    if ((s32)(now_ms - wake_deadline_ms) >= 0) {
        wake_radio_stop();
        status.active = 0U;
        cyclone2_usb_debug("wake finished: states=%u elapsed=%u ms",
                           status.states_visited, status.elapsed_ms);

        return 1;
    }

    if (now_ms - wake_state_started_ms < WAKE_STATE_DWELL_MS) return 0;

    wake_radio_stop();
    wake_state_index = (u8)((wake_state_index + 1U) % WAKE_WHITEN_STATES);
    status.states_visited++;
    wake_state_started_ms = now_ms;
    if (wake_start_state(wake_state_index) != 0) {
        status.active = 0U;
        status.last_error = -3;
        return 1;
    }
    return 0;
}

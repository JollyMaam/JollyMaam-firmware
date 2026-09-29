/* The parts of the controller state that are plain data, kept free of SDK
 * code so the host checks (tools/lights_test.c) can use them. */
#include <stddef.h>

#include "controller_state.h"

#define OFFLINE_SLEEP_MS      15000U
#define USB_UNPLUG_SLEEP_MS     5000U

int cyclone2_offline_sleep_due(const struct cyclone2_controller_state *state,
                              u32 now_ms, u32 usb_absent_since_ms,
                              int usb_present)
{
    return (state->kind == CYCLONE2_CONTROLLER_IDLE ||
            state->kind == CYCLONE2_CONTROLLER_DISCONNECTED) && !usb_present &&
           now_ms - state->entered_ms >= OFFLINE_SLEEP_MS &&
           now_ms - usb_absent_since_ms >= USB_UNPLUG_SLEEP_MS;
}

const struct cyclone2_session *cyclone2_controller_session(
    const struct cyclone2_controller_state *s)
{
    switch (s->kind) {
    case CYCLONE2_CONTROLLER_CONNECTING: return &s->as.connecting.session;
    case CYCLONE2_CONTROLLER_PAIRING: return &s->as.pairing.session;
    case CYCLONE2_CONTROLLER_CONNECTED: return &s->as.connected.session;
    case CYCLONE2_CONTROLLER_DISCONNECTED: return &s->as.disconnected.session;
    case CYCLONE2_CONTROLLER_STOPPING:
        return s->as.stopping.has_session ? &s->as.stopping.session : NULL;
    default: return NULL;
    }
}

int cyclone2_controller_slot(const struct cyclone2_controller_state *s,
                             u8 *slot)
{
    const struct cyclone2_session *session = cyclone2_controller_session(s);

    if (s->kind == CYCLONE2_CONTROLLER_WAKING) {
        *slot = s->as.waking.slot;
        return 1;
    }
    if (s->kind == CYCLONE2_CONTROLLER_STOPPING) {
        if (s->as.stopping.after == CYCLONE2_AFTER_STOP_START) {
            *slot = s->as.stopping.then.start.slot;
            return 1;
        }
        if (s->as.stopping.after == CYCLONE2_AFTER_STOP_WAKE) {
            *slot = s->as.stopping.then.wake.slot;
            return 1;
        }
    }
    if (!session) return 0;
    *slot = session->slot;
    return 1;
}

const char *cyclone2_controller_kind_name(enum cyclone2_controller_kind kind)
{
    static const char *const names[CYCLONE2_CONTROLLER_KIND_COUNT] = {
        "idle", "connecting", "pairing", "connected", "disconnected",
        "waking", "stopping",
    };

    return kind < CYCLONE2_CONTROLLER_KIND_COUNT ? names[kind] : "?";
}

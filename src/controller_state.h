#ifndef CYCLONE2_CONTROLLER_STATE_H
#define CYCLONE2_CONTROLLER_STATE_H

#include "generic/typedef.h"
#include "profile_types.h"

/* What the controller is doing with the radio, as one tagged union: each kind
 * carries exactly the data that kind has, so there is no "pairing flag while
 * connected" or "wake slot while idle" to get out of step.  Only controller.c
 * changes it (from the application task); others read it through
 * cyclone2_controller_state(). */

/* A slot's radio session: a profile has been started on the stack and has
 * not been torn down.  Every kind with a session can take its events. */
struct cyclone2_session {
    u8 slot;
    enum cyclone2_profile_type type;
    u32 started_ms;
};

struct cyclone2_wake_target {
    u8 slot;
    enum cyclone2_profile_type type;
    u8 channel;
    u8 seconds;
};

/* Where a stop is headed once the radio is quiet. */
enum cyclone2_after_stop {
    /* Tear the session down in one call and keep it as disconnected: the
     * `disconnect` command.  A later connect restarts the stack from it. */
    CYCLONE2_AFTER_STOP_DISCONNECTED,
    /* Wait for the session to be down, then nothing runs. */
    CYCLONE2_AFTER_STOP_IDLE,
    CYCLONE2_AFTER_STOP_START,
    CYCLONE2_AFTER_STOP_WAKE,
    CYCLONE2_AFTER_STOP_REBOOT,
    CYCLONE2_AFTER_STOP_SLEEP,
};

struct cyclone2_stop_start {
    u8 slot;
    u8 pairing;
    enum cyclone2_profile_type type;
};

/* Power-off: USB is looked at several times before the pad goes dark. */
struct cyclone2_stop_sleep {
    u8 confirmations;
    u8 waited_for_flash;
};

struct cyclone2_stop {
    /* The session being torn down; none once it is down, or when the stop
     * began without one. */
    u8 has_session;
    struct cyclone2_session session;
    u32 since_ms;
    enum cyclone2_after_stop after;
    union {
        struct cyclone2_stop_start start;
        struct cyclone2_wake_target wake;
        struct cyclone2_stop_sleep sleep;
    } then;
};

enum cyclone2_controller_kind {
    /* No session: after boot, or after a session was taken down. */
    CYCLONE2_CONTROLLER_IDLE,
    CYCLONE2_CONTROLLER_CONNECTING,
    CYCLONE2_CONTROLLER_PAIRING,
    CYCLONE2_CONTROLLER_CONNECTED,
    /* The session is still up but the link is not, and the pad is not
     * trying: the host went away, or a connect timed out on USB power.  The
     * host may still reconnect on its own. */
    CYCLONE2_CONTROLLER_DISCONNECTED,
    CYCLONE2_CONTROLLER_WAKING,
    CYCLONE2_CONTROLLER_STOPPING,
    CYCLONE2_CONTROLLER_KIND_COUNT,
};

struct cyclone2_controller_state {
    enum cyclone2_controller_kind kind;
    /* When this kind was entered. */
    u32 entered_ms;
    union {
        struct {
            struct cyclone2_session session;
            /* Timeouts count from here; a retry moves it. */
            u32 since_ms;
            u8 attempts;
        } connecting;
        struct {
            struct cyclone2_session session;
            u32 since_ms;
        } pairing;
        struct {
            struct cyclone2_session session;
        } connected;
        struct {
            struct cyclone2_session session;
        } disconnected;
        struct cyclone2_wake_target waking;
        struct cyclone2_stop stopping;
    } as;
};

const struct cyclone2_controller_state *cyclone2_controller_state(void);

/* The session, if the state has one (NULL otherwise). */
const struct cyclone2_session *cyclone2_controller_session(
    const struct cyclone2_controller_state *state);

/* The slot the state is about: its session's, a wake's, or the one a stop
 * is switching to.  Zero if there is none. */
int cyclone2_controller_slot(const struct cyclone2_controller_state *state,
                             u8 *slot);

const char *cyclone2_controller_kind_name(enum cyclone2_controller_kind kind);

/* Idle and disconnected states get a short battery sleep grace period.
 * Activity on sticks and buttons does not reset it. */
int cyclone2_offline_sleep_due(const struct cyclone2_controller_state *state,
                              u32 now_ms, u32 usb_absent_since_ms,
                              int usb_present);

#endif

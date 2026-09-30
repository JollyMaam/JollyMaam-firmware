#include <system/app_core.h>
#include <system/includes.h>
#include <usb/otg.h>
#include <asm/power/p33.h>
#include <usb/usb_phy.h>
#include <system/user_cfg.h>

#include "app_config.h"
#include "btstack_jl.h"
#include "btstack_jl_le.h"
#include "calibration.h"
#include "controller.h"
#include "controller_state.h"
#include "timing.h"
#include "ps.h"
#include "main.h"
#include "imu.h"
#include "profile.h"
#include "profile_classic.h"
#include "rumble.h"
#include "config.h"
#include "led.h"
#include "lights.h"
#include "user_cfg_id.h"
#include "usb.h"

#define CYCLONE2_CONTROL_PERIOD_US        1000U
#define CYCLONE2_DEBOUNCE_US             2000U
#define CYCLONE2_PROFILE_COUNT              CONFIG_PROFILES
#define CYCLONE2_HOME_GRACE_MS              150U
/* A Home released within the grace time, with no chord, still reaches the
 * host: as a press this long, so a host sampling at its own rate sees it. */
#define CYCLONE2_HOME_TAP_MS                 60U
#define CYCLONE2_LONG_HOLD_MS              5000U

#define CYCLONE2_CALIBRATE_HOLD_MS         3000U
#define CYCLONE2_CONNECT_TIMEOUT_MS       30000U

#define CYCLONE2_PAIR_TIMEOUT_MS          180000U
/* How long the radio must have been in a quiet state before settings may be
 * written to flash (see radio_quiet()). */
#define CYCLONE2_RADIO_SETTLE_MS            500U
/* Extra outgoing connection attempts after one ends without a usable session.
 * Small and bounded: an unreachable host must not become a retry storm. */
#define CYCLONE2_CONNECT_RETRIES               2U
/* No button, stick or trigger moved for this long: disconnect and sleep
 * (USB power keeps the pad awake and connected). */
#define CYCLONE2_IDLE_SLEEP_MS          300000U
/* How far a stick or trigger must move to count as use rather than drift. */
#define CYCLONE2_ACTIVITY_STICK           2048
#define CYCLONE2_ACTIVITY_TRIGGER         4096
#define CYCLONE2_INJECT_MIN_PERIOD_MS       20U
#define CYCLONE2_INJECT_MAX_PERIOD_MS       1000U
#define CYCLONE2_INJECT_MAX_DURATION_MS    60000U
/* USB frame activity is checked repeatedly before power-off. CDC DTR/RTS can
 * remain latched after unplug and cannot veto power-off by itself. */
#define CYCLONE2_SLEEP_CONFIRM              4U
#define CYCLONE2_SLEEP_RECHECK_MS         250U
#define CYCLONE2_USB_SOF_STALE_MS         250U
/* Home+Start held at boot (pinhole reset, wake from sleep) enters UBOOT if
 * USB turns up this soon after; enumeration takes well under this. */
#define CYCLONE2_UBOOT_BOOT_WINDOW_MS     3000U

#define CYCLONE2_PROFILE_SWITCH_MS          100U
/* The stack's halt waits for the peer to confirm the disconnect, so the
 * teardown is polled from a timer instead of blocking the application task
 * (which also runs the USB console).  The profile's supervision timeout is
 * 8 s; past that we give up rather than wait forever. */
#define CYCLONE2_PROFILE_EXIT_POLL_MS       100U
#define CYCLONE2_PROFILE_EXIT_MAX_MS      12000U

#define CYCLONE2_STALE_EVENT_MS             400U

#define CYCLONE2_FACE_BUTTONS \
    (CYCLONE2_REPORT_BUTTON_A | CYCLONE2_REPORT_BUTTON_B | \
     CYCLONE2_REPORT_BUTTON_X | CYCLONE2_REPORT_BUTTON_Y)

enum cyclone2_home_state {
    CYCLONE2_HOME_IDLE,
    CYCLONE2_HOME_PENDING,
    CYCLONE2_HOME_REPORTED,
    CYCLONE2_HOME_CHORD,
    CYCLONE2_HOME_INVALID_CHORD,
    CYCLONE2_HOME_CALIBRATE_CHORD,
};

static struct cyclone2_input_report candidate_report;
static struct cyclone2_input_report current_report;
static enum cyclone2_home_state home_state;
static struct cyclone2_controller_state state;
static u32 home_start_ms;
static u32 chord_start_ms;
static u32 button_changed_us[16];
static u32 hat_changed_us;
static u32 next_report_us;
static u8 chord_key;
static u8 requested_profile;
static u32 home_tap_start_ms;
static u8 home_tap;
static struct cyclone2_input_report activity_reference;
static u32 last_activity_ms;
static u32 usb_absent_since_ms;
static u32 usb_sof_seen_ms;
static u16 usb_sof_frame;
static u8 usb_sof_initialized;
static u8 usb_was_present;
static u8 uboot_chord_at_boot;
static u32 started_ms;
static u8 calibrating;
static u8 ignore_chord_until_release;
static struct cyclone2_report_statistics report_statistics;
static struct cyclone2_injection_status injection_status;
static u32 injection_next_transition_ms;
static u32 injection_target_edges;
static u8 injection_last_emitted;
static u8 injection_release_pending;

extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);
extern void bt_max_pwr_set(u8 pwr, u8 pg_pwr, u8 iq_pwr, u8 ble_pwr);
extern void bt_production_test(u8 en);
extern void set_bt_data_rate_acl_3mbs_mode(u8 en);
static u8 tx_power_high;
static void start_session(u8 slot, u8 pairing, enum cyclone2_profile_type type);
static void profile_select(u8 profile, u8 pairing);
static void injection_cancel(void);
static u32 active_report_hz(void);
static void soft_reset_now(void *private_data);
static void start_wake(struct cyclone2_wake_target target);
static struct cyclone2_stop *request_transition(enum cyclone2_after_stop after);

const struct cyclone2_controller_state *cyclone2_controller_state(void)
{
    return &state;
}

static const struct cyclone2_session *session(void)
{
    return cyclone2_controller_session(&state);
}

/* Every change of state goes through here.  The time a kind was entered
 * survives a change within the kind (a connect retry). */
static void become(const struct cyclone2_controller_state *next)
{
    u32 entered = state.entered_ms;

    if (next->kind != state.kind) entered = sys_timer_get_ms();
    state = *next;
    state.entered_ms = entered;
}

static void become_idle(void)
{
    struct cyclone2_controller_state next = {
        .kind = CYCLONE2_CONTROLLER_IDLE,
    };

    become(&next);
}

static void become_connected(struct cyclone2_session s)
{
    struct cyclone2_controller_state next = {
        .kind = CYCLONE2_CONTROLLER_CONNECTED, .as.connected.session = s,
    };

    become(&next);
}

static void become_disconnected(struct cyclone2_session s)
{
    struct cyclone2_controller_state next = {
        .kind = CYCLONE2_CONTROLLER_DISCONNECTED,
        .as.disconnected.session = s,
    };

    become(&next);
}

/* Stop whatever runs, headed for `after`; the caller fills in `then`. */
static struct cyclone2_stop *begin_stop(enum cyclone2_after_stop after)
{
    const struct cyclone2_session *s = session();
    struct cyclone2_controller_state next = {
        .kind = CYCLONE2_CONTROLLER_STOPPING,
    };

    next.as.stopping.has_session = s != NULL;
    if (s) next.as.stopping.session = *s;
    next.as.stopping.since_ms = sys_timer_get_ms();
    next.as.stopping.after = after;
    become(&next);
    return &state.as.stopping;
}

static int stopping_for(enum cyclone2_after_stop after)
{
    return state.kind == CYCLONE2_CONTROLLER_STOPPING &&
           state.as.stopping.after == after;
}

static void radio_exit_only(void *private_data)
{
    struct cyclone2_stop *stop = &state.as.stopping;

    (void)private_data;
    cyclone2_rumble_stop();
    if (!stopping_for(CYCLONE2_AFTER_STOP_DISCONNECTED)) return;
    if (!stop->has_session) {
        become_idle();
        return;
    }
    cyclone2_profile_exit();
    become_disconnected(stop->session);
}

int cyclone2_usb_present(void)
{
    u32 now = sys_timer_get_ms();
    u16 frame = usb_read_sofframe(USB0);

    /* PC3 is shared with LED SPI and reads low even with a cable attached.
     * The SDK OTG state remains IDLE_MODE while this CDC device is live.
     * A changing SOF frame counter proves that a USB host is present. */
    if (!usb_sof_initialized) {
        usb_sof_initialized = 1U;
        usb_sof_frame = frame;
        usb_sof_seen_ms = now - CYCLONE2_USB_SOF_STALE_MS;
    } else if (frame != usb_sof_frame) {
        usb_sof_frame = frame;
        usb_sof_seen_ms = now;
    }
    return now - usb_sof_seen_ms < CYCLONE2_USB_SOF_STALE_MS;
}

u8 get_cur_bt_idx(void)
{
    const struct cyclone2_session *s = session();

    return s ? s->slot : 0U;
}

static enum cyclone2_profile_type slot_type(u8 slot)
{
    u8 type = config.profile[slot % CYCLONE2_PROFILE_COUNT].type;

    return type < CYCLONE2_PROFILE_TYPE_COUNT ?
           (enum cyclone2_profile_type)type : CYCLONE2_PROFILE_SWITCH;
}

/* Whether settings may be written to flash now: not while a pairing or a
 * radio transition is under way.  The resets that rule comes from were a
 * syscfg access inside the SSP exchange and during a profile start
 * (docs/power-reset.md); a settled link, an idle radio, or paging with no
 * link up take a write.  The state must have held for a moment. */
static int radio_quiet(u32 now)
{
    int quiet;

    quiet = state.kind == CYCLONE2_CONTROLLER_IDLE ||
            state.kind == CYCLONE2_CONTROLLER_DISCONNECTED ||
            state.kind == CYCLONE2_CONTROLLER_CONNECTED ||
            (state.kind == CYCLONE2_CONTROLLER_CONNECTING &&
             !btstack_jl_core_link_up() && !btstack_jl_le_connected());
    return quiet && now - state.entered_ms >= CYCLONE2_RADIO_SETTLE_MS;
}

int cyclone2_radio_quiet(void)
{
    return radio_quiet(sys_timer_get_ms());
}

void cyclone2_report_statistics_get(struct cyclone2_report_statistics *stats)
{
    struct cyclone2_timing timing;

    *stats = report_statistics;
    cyclone2_timing_get(&timing);
    stats->target_hz = active_report_hz();
    stats->hz = timing.last_second_hz;
}

int cyclone2_connect_request(u8 profile)
{
    if (profile >= CYCLONE2_PROFILE_COUNT) return -1;
    if (state.kind == CYCLONE2_CONTROLLER_STOPPING) return -1;
    if (state.kind == CYCLONE2_CONTROLLER_CONNECTED &&
        state.as.connected.session.slot == profile &&
        cyclone2_profile_is_connected()) return 0;
    injection_cancel();
    profile_select(profile, 0U);
    return 1;
}

int cyclone2_wake_request(u8 slot, u8 channel, u8 seconds)
{
    struct cyclone2_stop *stop;
    int result;

    if (slot >= CYCLONE2_PROFILE_COUNT) return -1;
    if (state.kind == CYCLONE2_CONTROLLER_STOPPING ||
        state.kind == CYCLONE2_CONTROLLER_WAKING) return -1;
    result = cyclone2_profile_wake_check(slot_type(slot), slot, channel, seconds);
    if (result) return result;

    injection_cancel();
    /* Always deferred, even with no session running: profile_transition_now()
     * runs from a timer, and going through it keeps the command parser from
     * carrying the profile-start path on its own stack - the same reason the
     * other commands queue their work. */
    stop = request_transition(CYCLONE2_AFTER_STOP_WAKE);
    if (!stop) return -1;
    stop->then.wake.slot = slot;
    stop->then.wake.type = slot_type(slot);
    stop->then.wake.channel = channel;
    stop->then.wake.seconds = seconds;
    return 1;
}

static void wake_finished(void)
{
    u8 slot;

    if (state.kind != CYCLONE2_CONTROLLER_WAKING) return;
    slot = state.as.waking.slot;
    become_idle();

    cyclone2_profile_wake_stop();
    cyclone2_usb_debug("wake done; reconnecting slot %s",
                       config_slot_name(slot));
    profile_select(slot, 0U);
}

static void start_wake(struct cyclone2_wake_target target)
{
    struct cyclone2_controller_state next = {
        .kind = CYCLONE2_CONTROLLER_WAKING, .as.waking = target,
    };
    int result;

    become(&next);
    result = cyclone2_profile_wake_start(target.type, target.slot,
                                         target.channel, target.seconds);
    if (result > 0) {
        wake_finished();
    } else if (result < 0) {
        cyclone2_usb_debug("wake not started (%d); reconnecting slot %s",
                           result, config_slot_name(target.slot));
        cyclone2_profile_wake_stop();
        become_idle();
        profile_select(target.slot, 0U);
    }
}

/* Same as a connect request except that the slot forgets its stored link key
 * and becomes pairable.  The physical chord is Home+face for five seconds;
 * this is the equivalent for someone driving the controller over USB, e.g.
 * while the console sits on its "Change Grip/Order" screen. */
int cyclone2_pair_request(u8 profile)
{
    if (profile >= CYCLONE2_PROFILE_COUNT) return -1;
    if (state.kind == CYCLONE2_CONTROLLER_STOPPING) return -1;
    injection_cancel();
    profile_select(profile, 1U);
    return 1;
}

/* Pairing request that also adopts a fresh Bluetooth address.  Use this when
 * the host still holds a link key that the controller can no longer match;
 * the host then sees a device it has never paired with. */
int cyclone2_pair_fresh_request(u8 profile)
{
    if (profile >= CYCLONE2_PROFILE_COUNT) return -1;
    if (state.kind == CYCLONE2_CONTROLLER_STOPPING) return -1;
    /* A new address: a host that still holds a key for the old one sees a
     * pad it has never met. */
    config_slot_address_fresh(profile);
    injection_cancel();
    profile_select(profile, 1U);
    return 1;
}

/* Zero the Bluetooth controller's whole bonding store.
 *
 * A chip dump shows pairing records (peer address followed by the key) in the
 * retention memory just below the MaskROM boot-state word, at 0x10760 and
 * nearby.  Neither reflashing nor erasing VM reaches them, and
 * delete_link_key() only works with the slot index the stack tracks for a live
 * connection, so a link that never authenticates cannot be deleted that way.
 * Zeroing the store and restarting is the factory reset of pairing state. */
#define CYCLONE2_BT_BOND_STORE      ((volatile u8 *)0x00010700U)
#define CYCLONE2_BT_BOND_STORE_SIZE 0x100U

int cyclone2_clear_keys_request(void)
{
    volatile u8 *store = CYCLONE2_BT_BOND_STORE;
    u32 index;

    if (state.kind == CYCLONE2_CONTROLLER_STOPPING) return -1;
    for (index = 0U; index < CYCLONE2_BT_BOND_STORE_SIZE; index++) {
        store[index] = 0U;
    }
    __asm__ volatile("csync");
    return cyclone2_reboot_request();
}

/* Reset the chip and come back up as this same application.
 *
 * The same teardown as clearing the keys, without touching anything else: stop
 * the radio, stop the rumble, then soft-reset.  Useful for putting the pad back
 * to a known boot state from a script - and for measuring what a reset does to
 * the host, since it is a controlled, repeatable one.
 */
int cyclone2_reboot_request(void)
{
    if (state.kind == CYCLONE2_CONTROLLER_STOPPING) return -1;
    injection_cancel();
    cyclone2_rumble_stop();
    if (begin_stop(CYCLONE2_AFTER_STOP_REBOOT)->has_session)
        cyclone2_profile_disconnect();
    sys_timeout_add(NULL, soft_reset_now, 100U);
    return 1;
}

void cyclone2_wake_cancel(void)
{
    /* A wake still waiting for the radio to go quiet: stop there instead. */
    if (stopping_for(CYCLONE2_AFTER_STOP_WAKE)) {
        state.as.stopping.after = CYCLONE2_AFTER_STOP_IDLE;
        return;
    }
    if (state.kind != CYCLONE2_CONTROLLER_WAKING) return;
    cyclone2_profile_wake_stop();
    become_idle();
}

int cyclone2_disconnect_request(void)
{
    if (state.kind == CYCLONE2_CONTROLLER_WAKING) {
        cyclone2_wake_cancel();
        return 1;
    }
    if (!session()) return 0;
    if (state.kind == CYCLONE2_CONTROLLER_STOPPING) return -1;

    injection_cancel();
    cyclone2_rumble_stop();
    begin_stop(CYCLONE2_AFTER_STOP_DISCONNECTED);
    if (cyclone2_profile_is_connected()) cyclone2_profile_disconnect();
    /* Let the profile emit its protocol-level disconnect before shutting down
     * the shared Bluetooth stack. This also stops discoverability after a
     * failed pairing attempt, where the state is disconnected but btstack is
     * active.
     * A later connect command uses the existing reset handoff to initialize
     * that stack from a clean state. */
    sys_timeout_add(NULL, radio_exit_only, 100U);
    return 1;
}

int cyclone2_radio_stopped(void)
{
    return (state.kind == CYCLONE2_CONTROLLER_IDLE ||
            state.kind == CYCLONE2_CONTROLLER_DISCONNECTED) &&
           btstack_jl_core_idle();
}

void cyclone2_connection_status_get(struct cyclone2_connection_status *status)
{
    const struct cyclone2_session *s = session();
    u8 slot = s ? s->slot : 0U;

    status->slot = slot;
    status->profile_type = s ? s->type : slot_type(slot);
    status->connection_state = (u8)state.kind;
    status->usb_present = (u8)cyclone2_usb_present();
    status->connect_attempts = state.kind == CYCLONE2_CONTROLLER_CONNECTING ?
                               state.as.connecting.attempts : 0U;
    memcpy(status->local_address, bt_get_mac_addr(), 6U);
    status->stale = 0U;
    if (s) {
        u8 address[6];

        config_slot_address(slot, address);
        status->stale = s->type != slot_type(slot) ||
                        memcmp(address, status->local_address, 6U) != 0;
    }
}

int cyclone2_inject_start(u32 period_ms, u32 duration_ms)
{
    if (!session() || !cyclone2_profile_is_connected() ||
        period_ms < CYCLONE2_INJECT_MIN_PERIOD_MS ||
        period_ms > CYCLONE2_INJECT_MAX_PERIOD_MS || duration_ms == 0U ||
        duration_ms > CYCLONE2_INJECT_MAX_DURATION_MS || (period_ms & 1U)) return -1;
    injection_status.active = 1U;
    injection_status.synthetic_a = 1U;
    injection_status.period_ms = period_ms;
    injection_status.duration_ms = duration_ms;
    injection_status.emitted_edges = 0U;
    injection_status.overruns = 0U;
    injection_last_emitted = 0U;
    injection_release_pending = 0U;

    injection_target_edges =
        ((duration_ms + period_ms - 1U) / period_ms) * 2U;
    injection_next_transition_ms = sys_timer_get_ms() + period_ms / 2U;
    return 0;
}

void cyclone2_inject_stop(void)
{
    if (injection_status.active) {
        injection_status.active = 0U;
        injection_status.synthetic_a = 0U;
        injection_release_pending = injection_last_emitted != 0U;
        return;
    }
    injection_status.active = 0U;
    injection_status.synthetic_a = 0U;
    injection_release_pending = 0U;
}

static void injection_cancel(void)
{
    injection_status.active = 0U;
    injection_status.synthetic_a = 0U;
    injection_release_pending = 0U;
}

void cyclone2_injection_status_get(struct cyclone2_injection_status *status)
{
    *status = injection_status;
}

static void update_injection(u32 now)
{
    u32 half_period;

    if (!injection_status.active) return;
    if (injection_status.emitted_edges >= injection_target_edges) {
        injection_status.active = 0U;
        injection_status.synthetic_a = 0U;
        return;
    }
    /* A synthetic button edge is known state, unlike a sampled physical
     * input. Never overwrite it before the selected profile accepts it. */
    if (injection_last_emitted != injection_status.synthetic_a) return;
    half_period = injection_status.period_ms / 2U;
    if ((s32)(now - injection_next_transition_ms) < 0) return;
    if (now - injection_next_transition_ms >= half_period) {
        injection_status.overruns++;
        injection_next_transition_ms = now;
    }
    injection_status.synthetic_a ^= 1U;
    injection_next_transition_ms += half_period;
}

static u16 boot_request_check(u8 profile, u8 pairing)
{
    return (u16)(0xc200U ^ profile ^ ((u16)pairing << 8));
}

static int boot_request_consume(u8 *slot, u8 *slot_pairing)
{
    u32 magic;
    u16 check;
    u8 profile;
    u8 pairing;

    magic = reset_mailbox.magic;
    profile = reset_mailbox.profile;
    pairing = reset_mailbox.pairing;
    check = reset_mailbox.check;

    if (magic != CYCLONE2_BOOT_REQUEST_MAGIC) return 0;

    reset_mailbox.magic = 0U;
    __asm__ volatile("csync");

    if (profile >= CYCLONE2_PROFILE_COUNT || pairing > 1U ||
        check != boot_request_check(profile, pairing)) return 0;

    /* This is a reset handoff, not a saved active profile. Consume it before
     * starting the radio so a later unrelated reset returns to slot select. */
    *slot = profile;
    *slot_pairing = pairing;
    ignore_chord_until_release = 1U;
    return 1;
}

static void soft_reset_now(void *private_data)
{
    (void)private_data;
    p33_soft_reset();
}

/* Adopt the queued profile: tear the current radio session down in place and
 * bring the new one up on the same firmware image.
 *
 * The BTstack task parks when its radio session is shut down and resumes on
 * the next start (see btstack_jl_core_run()), so a slot/profile change is a
 * stop and a start of the stack -- never a p33_soft_reset() of the chip.  That
 * reset used to be the only way to re-initialise the stack; it also dropped
 * the USB console (so a host saw the pad vanish and re-enumerate) and could
 * leave the radio wedged if it happened at the wrong moment. */
static void profile_transition_now(void *private_data)
{
    struct cyclone2_stop *stop = &state.as.stopping;

    (void)private_data;
    cyclone2_rumble_stop();
    if (state.kind != CYCLONE2_CONTROLLER_STOPPING) return;
    if (stop->has_session) {
        if (!cyclone2_profile_exit()) {
            /* Still going down: the stack's halt waits for the peer to confirm
             * the disconnect, which can take the supervision timeout.  Poll
             * from a timer rather than block here - the console shares this
             * task.  Starting the next session before this one is down is what
             * used to reset the chip. */
            if ((s32)(sys_timer_get_ms() - stop->since_ms) <
                (s32)CYCLONE2_PROFILE_EXIT_MAX_MS) {
                sys_timeout_add(NULL, profile_transition_now,
                                CYCLONE2_PROFILE_EXIT_POLL_MS);
                return;
            }
            become_idle();
            cyclone2_usb_debug(
                "profile switch aborted: the previous session did not stop");
            return;
        }
        stop->has_session = 0U;
    }
    switch (stop->after) {
    case CYCLONE2_AFTER_STOP_START: {
        struct cyclone2_stop_start next = stop->then.start;

        home_state = CYCLONE2_HOME_IDLE;
        ignore_chord_until_release = 1U;
        start_session(next.slot, next.pairing, next.type);
        break;
    }
    case CYCLONE2_AFTER_STOP_WAKE:
        /* The radio is quiet now, which is the only state the wake can take
         * the baseband in. */
        start_wake(stop->then.wake);
        break;
    default:
        become_idle();
        break;
    }
}

/* Take the running session down in place and then go where `after` says,
 * on the same firmware image; NULL if a stop is already under way.  The
 * caller fills in the stop's `then`. */
static struct cyclone2_stop *request_transition(enum cyclone2_after_stop after)
{
    struct cyclone2_stop *stop;
    int connected;

    if (state.kind == CYCLONE2_CONTROLLER_STOPPING) return NULL;
    injection_cancel();
    cyclone2_rumble_stop();

    connected = session() && cyclone2_profile_is_connected();
    stop = begin_stop(after);
    if (connected) cyclone2_profile_disconnect();
    sys_timeout_add(NULL, profile_transition_now, CYCLONE2_PROFILE_SWITCH_MS);
    return stop;
}

static void profile_select(u8 profile, u8 pairing)
{
    injection_cancel();
    if (state.kind == CYCLONE2_CONTROLLER_STOPPING) return;
    if (state.kind == CYCLONE2_CONTROLLER_WAKING) cyclone2_wake_cancel();
    if (session()) {
        struct cyclone2_stop *stop =
            request_transition(CYCLONE2_AFTER_STOP_START);

        stop->then.start.slot = profile;
        stop->then.start.pairing = pairing;
        stop->then.start.type = slot_type(profile);
        return;
    }

    home_state = CYCLONE2_HOME_IDLE;
    ignore_chord_until_release = 1U;
    start_session(profile, pairing, slot_type(profile));
}

static void poweroff_now(void *private_data)
{
    u32 sense = gpio_read(CYCLONE2_USB_SENSE) ? 1U : 0U;
    u32 otg = usb_otg_online(USB0) != DISCONN_MODE ? 1U : 0U;
    u32 terminal = cyclone2_cdc_terminal_open() ? 1U : 0U;
    struct cyclone2_stop *stop = &state.as.stopping;

    (void)private_data;
    if (!stopping_for(CYCLONE2_AFTER_STOP_SLEEP)) return;

    cyclone2_bringup_record(0x5200u | sense | (otg << 1));
    cyclone2_bringup_record(0x5400u | terminal);
    /* The CDC DTR/RTS latch may survive a physical unplug. Only fresh USB
     * frames can veto power-off; keep the raw signals in the trace. */
    if (cyclone2_usb_present()) {
        if (stop->has_session) become_disconnected(stop->session);
        else become_idle();
        cyclone2_usb_debug("sleep cancelled; USB is connected");
        return;
    }
    if (++stop->then.sleep.confirmations < CYCLONE2_SLEEP_CONFIRM) {
        /* Keep asking: a glitch that clears on the next poll must not put the
         * pad to sleep, because that leaves it dark until a power cycle. */
        sys_timeout_add(NULL, poweroff_now, CYCLONE2_SLEEP_RECHECK_MS);
        return;
    }
    /* Settings not written yet (a pairing, a calibration): give the radio a
     * moment to settle rather than lose them to the power-off.  With
     * autocommit off they are dropped, as a reboot would. */
    if (config_pending() && config.autocommit) {
        if (!stop->then.sleep.waited_for_flash) {
            stop->then.sleep.waited_for_flash = 1U;
            sys_timeout_add(NULL, poweroff_now, CYCLONE2_RADIO_SETTLE_MS);
            return;
        }
        config_commit(1);
    }
    cyclone2_bringup_record(0x5300u);
    power_set_soft_poweroff();
}

static void radio_exit_and_poweroff(void *private_data)
{
    struct cyclone2_stop *stop = &state.as.stopping;

    (void)private_data;
    cyclone2_rumble_stop();
    if (!stopping_for(CYCLONE2_AFTER_STOP_SLEEP)) return;

    if (cyclone2_usb_present()) {
        if (!stop->has_session) become_idle();
        else if (cyclone2_profile_is_connected())
            become_connected(stop->session);
        else become_disconnected(stop->session);
        cyclone2_usb_debug("sleep cancelled; USB is connected");
        return;
    }
    /* One call, not waited for: the pad powers off regardless.  The session
     * stays recorded, so a USB cancel below leaves it disconnected and the
     * next connect tears it down properly. */
    if (stop->has_session) cyclone2_profile_exit();
    sys_timeout_add(NULL, poweroff_now, 300U);
}

static void begin_sleep(void)
{
    if (cyclone2_usb_present()) {
        if (state.kind == CYCLONE2_CONTROLLER_CONNECTED) {
            cyclone2_usb_debug(
                "Home hold: disconnecting Bluetooth; USB keeps console awake");
            cyclone2_disconnect_request();
        } else {
            cyclone2_usb_debug(
                "Home hold: USB keeps console awake; Bluetooth is not connected");
        }

        home_state = CYCLONE2_HOME_INVALID_CHORD;
        return;
    }
    if (state.kind == CYCLONE2_CONTROLLER_STOPPING) return;
    if (state.kind == CYCLONE2_CONTROLLER_WAKING) cyclone2_profile_wake_stop();
    begin_stop(CYCLONE2_AFTER_STOP_SLEEP);
    sys_timeout_add(NULL, radio_exit_and_poweroff, 30U);
}

static void begin_automatic_sleep(const char *reason)
{
    u32 sense = gpio_read(CYCLONE2_USB_SENSE) ? 1U : 0U;
    u32 otg = usb_otg_online(USB0) != DISCONN_MODE ? 1U : 0U;

    cyclone2_bringup_record(0x5000u | sense | (otg << 1));
    if (cyclone2_usb_present()) {
        cyclone2_usb_debug("%s; staying awake because USB is connected", reason);
        return;
    }
    begin_sleep();
}

/* Home+Start while USB is plugged in enters UBOOT, as the stock pad does:
 * held at the plug-in, or at the boot (reset, wake) that found USB. */
static void usb_poll(u32 now)
{
    const u16 uboot_chord = CYCLONE2_REPORT_BUTTON_HOME |
                            CYCLONE2_REPORT_BUTTON_START;
    int present = cyclone2_usb_present();

    if (uboot_chord_at_boot &&
        now - started_ms >= CYCLONE2_UBOOT_BOOT_WINDOW_MS)
        uboot_chord_at_boot = 0U;
    if (present) usb_absent_since_ms = now;
    if (present && !usb_was_present &&
        ((current_report.buttons & uboot_chord) == uboot_chord ||
         uboot_chord_at_boot) &&
        !cyclone2_usb_uboot_pending()) {
        cyclone2_usb_debug("Home+Start at USB plug-in: entering UBOOT");
        cyclone2_usb_uboot_request();
    }
    if (present) uboot_chord_at_boot = 0U;
    usb_was_present = (u8)present;
}

/* The keys that pick a slot with Home, as a bit per slot: A B X Y, then the
 * d-pad's up, down, left and right.  A diagonal is two of them. */
static u8 chord_keys(const struct cyclone2_input_report *report)
{
    static const u8 hat_keys[CYCLONE2_HAT_NEUTRAL] = {
        0x10U, 0x10U | 0x80U, 0x80U, 0x20U | 0x80U,
        0x20U, 0x20U | 0x40U, 0x40U, 0x10U | 0x40U,
    };
    u16 buttons = report->buttons;
    u8 keys = 0U;

    if (buttons & CYCLONE2_REPORT_BUTTON_A) keys |= 0x01U;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) keys |= 0x02U;
    if (buttons & CYCLONE2_REPORT_BUTTON_X) keys |= 0x04U;
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) keys |= 0x08U;
    if (report->hat < CYCLONE2_HAT_NEUTRAL) keys |= hat_keys[report->hat];
    return keys;
}

/* One key alone picks its slot. */
static int selected_slot(u8 keys, u8 *profile)
{
    u8 slot;

    for (slot = 0U; slot < CYCLONE2_PROFILE_COUNT; slot++) {
        if (keys == (u8)(1U << slot)) {
            *profile = slot;
            return 1;
        }
    }
    return 0;
}

CYCLONE2_HOT static void update_home_state(void)
{
    u16 buttons = current_report.buttons;
    u8 keys = chord_keys(&current_report);
    u32 now = sys_timer_get_ms();
    u8 profile;
    int home = (buttons & CYCLONE2_REPORT_BUTTON_HOME) != 0U;

    if (ignore_chord_until_release) {
        if (!home && !keys) ignore_chord_until_release = 0U;
        return;
    }

    switch (home_state) {
    case CYCLONE2_HOME_IDLE:
        if (home) {
            home_start_ms = now;
            if (!keys && (buttons & CYCLONE2_REPORT_BUTTON_M)) {
                chord_start_ms = now;
                home_state = CYCLONE2_HOME_CALIBRATE_CHORD;
            } else if (selected_slot(keys, &profile)) {
                chord_key = keys;
                chord_start_ms = now;
                requested_profile = profile;
                home_state = CYCLONE2_HOME_CHORD;
            } else if (keys) {
                home_state = CYCLONE2_HOME_INVALID_CHORD;
            } else {
                home_state = CYCLONE2_HOME_PENDING;
            }
        }
        break;
    case CYCLONE2_HOME_PENDING:
        if (!home) {
            home_tap_start_ms = now;
            home_tap = 1U;
            home_state = CYCLONE2_HOME_IDLE;
        } else if (!keys && (buttons & CYCLONE2_REPORT_BUTTON_M)) {
            chord_start_ms = now;
            home_state = CYCLONE2_HOME_CALIBRATE_CHORD;
        } else if (selected_slot(keys, &profile)) {
            chord_key = keys;
            chord_start_ms = now;
            requested_profile = profile;
            home_state = CYCLONE2_HOME_CHORD;
        } else if (keys) {
            home_state = CYCLONE2_HOME_INVALID_CHORD;
        } else if (now - home_start_ms >= CYCLONE2_HOME_GRACE_MS) {
            home_state = CYCLONE2_HOME_REPORTED;
        }
        break;
    case CYCLONE2_HOME_REPORTED:
        if (!home) home_state = CYCLONE2_HOME_IDLE;
        else if (now - home_start_ms >= CYCLONE2_LONG_HOLD_MS)
            begin_sleep();
        break;
    case CYCLONE2_HOME_CHORD:
        if (!home || (keys & chord_key) == 0U) {
            profile_select(requested_profile, 0U);
        } else if (keys != chord_key) {
            home_state = CYCLONE2_HOME_INVALID_CHORD;
        } else if (now - chord_start_ms >= CYCLONE2_LONG_HOLD_MS) {
            profile_select(requested_profile, 1U);
        }
        break;
    case CYCLONE2_HOME_CALIBRATE_CHORD:

        if (!home || !(buttons & CYCLONE2_REPORT_BUTTON_M) || keys) {
            home_state = CYCLONE2_HOME_INVALID_CHORD;
        } else if (now - chord_start_ms >= CYCLONE2_CALIBRATE_HOLD_MS) {
            cyclone2_calibration_start();
            home_state = CYCLONE2_HOME_INVALID_CHORD;
        }
        break;
    case CYCLONE2_HOME_INVALID_CHORD:
        if (!home && !keys && !(buttons & CYCLONE2_REPORT_BUTTON_M))
            home_state = CYCLONE2_HOME_IDLE;
        break;
    }
}

CYCLONE2_HOT static void make_host_report(struct cyclone2_input_report *report)
{

    if (calibrating) {
        memset(report, 0, sizeof(*report));
        report->hat = CYCLONE2_HAT_NEUTRAL;
        return;
    }
    *report = current_report;
    if (home_state == CYCLONE2_HOME_CALIBRATE_CHORD ||
        (home_state == CYCLONE2_HOME_INVALID_CHORD &&
         (report->buttons & CYCLONE2_REPORT_BUTTON_M) &&
         !chord_keys(report))) {
        report->buttons &= ~CYCLONE2_REPORT_BUTTON_M;
    }
    /* The chord's keys (face buttons and d-pad) stay away from the host. */
    if (home_state == CYCLONE2_HOME_PENDING ||
        home_state == CYCLONE2_HOME_CHORD ||
        home_state == CYCLONE2_HOME_CALIBRATE_CHORD ||
        home_state == CYCLONE2_HOME_INVALID_CHORD ||
        ignore_chord_until_release ||
        state.kind == CYCLONE2_CONTROLLER_STOPPING) {
        report->buttons &= ~(CYCLONE2_REPORT_BUTTON_HOME |
                             CYCLONE2_FACE_BUTTONS);
        report->hat = CYCLONE2_HAT_NEUTRAL;
    }
    if (home_tap) {
        if (sys_timer_get_ms() - home_tap_start_ms < CYCLONE2_HOME_TAP_MS)
            report->buttons |= CYCLONE2_REPORT_BUTTON_HOME;
        else home_tap = 0U;
    }
}

/* Eager debounce: a contact that changes is taken at once, and then held for
 * CYCLONE2_DEBOUNCE_US while it bounces.  Waiting for it to settle first
 * cost every press and release 10-20 ms.  The worst bounce measured (debug
 * bounce, docs/peripherals.md) is ~0.4 ms, on the face buttons; 2 ms is
 * five times that and two control polls. */
CYCLONE2_HOT static void debounce_digital_input(const struct cyclone2_input_report *sample,
                                   u32 now_us)
{
    unsigned bit;

    for (bit = 0; bit < 16U; bit++) {
        u16 mask = (u16)(1U << bit);

        if ((sample->buttons & mask) != (current_report.buttons & mask) &&
            now_us - button_changed_us[bit] >= CYCLONE2_DEBOUNCE_US) {
            current_report.buttons ^= mask;
            button_changed_us[bit] = now_us;
        }
    }
    if (sample->hat != current_report.hat &&
        now_us - hat_changed_us >= CYCLONE2_DEBOUNCE_US) {
        current_report.hat = sample->hat;
        hat_changed_us = now_us;
    }
}

static int moved(s32 now_value, s32 then_value, s32 threshold)
{
    s32 delta = now_value - then_value;

    return delta >= threshold || -delta >= threshold;
}

/* Someone is holding the pad: a button or the hat changed, or a stick or
 * trigger moved further than drift would. */
static void track_activity(u32 now)
{
    const struct cyclone2_input_report *in = &current_report;
    const struct cyclone2_input_report *ref = &activity_reference;

    if (in->buttons != ref->buttons || in->hat != ref->hat ||
        moved(in->left_x, ref->left_x, CYCLONE2_ACTIVITY_STICK) ||
        moved(in->left_y, ref->left_y, CYCLONE2_ACTIVITY_STICK) ||
        moved(in->right_x, ref->right_x, CYCLONE2_ACTIVITY_STICK) ||
        moved(in->right_y, ref->right_y, CYCLONE2_ACTIVITY_STICK) ||
        moved(in->left_trigger, ref->left_trigger, CYCLONE2_ACTIVITY_TRIGGER) ||
        moved(in->right_trigger, ref->right_trigger,
              CYCLONE2_ACTIVITY_TRIGGER)) {
        activity_reference = *in;
        last_activity_ms = now;
    } else if (now - last_activity_ms >= CYCLONE2_IDLE_SLEEP_MS &&
               state.kind != CYCLONE2_CONTROLLER_STOPPING &&
               state.kind != CYCLONE2_CONTROLLER_DISCONNECTED &&
               state.kind != CYCLONE2_CONTROLLER_IDLE) {
        /* Counted again from here, so on USB this asks once per period. */
        last_activity_ms = now;
        begin_automatic_sleep("idle for 5 minutes");
    }
}

static void accept_analog_input(const struct cyclone2_input_report *sample)
{
    current_report.left_x = sample->left_x;
    current_report.left_y = sample->left_y;
    current_report.right_x = sample->right_x;
    current_report.right_y = sample->right_y;
    current_report.left_trigger = sample->left_trigger;
    current_report.right_trigger = sample->right_trigger;
}

static void send_input_report(const struct cyclone2_input_report *report)
{
    int result;

    if (state.kind != CYCLONE2_CONTROLLER_CONNECTED) return;
    report_statistics.send_attempts++;
    result = cyclone2_profile_send(report);
    if (result > 0) {
        u32 now_us = cyclone2_time_us();

        report_statistics.send_accepted++;
        cyclone2_timing_sent(now_us);
        cyclone2_probe_sent(report->buttons, now_us);

        if ((injection_status.active || injection_release_pending) &&
            injection_last_emitted != injection_status.synthetic_a) {
            injection_last_emitted = injection_status.synthetic_a;
            injection_status.emitted_edges++;
            if (injection_release_pending) injection_release_pending = 0U;
        }
    }
    else if (result == 0) report_statistics.send_deferred++;
    else if (result == -1) report_statistics.send_busy++;
    else report_statistics.send_errors++;
}

/* The slot's report rate: its own setting, else its type's default. */
static u32 active_report_hz(void)
{
    const struct cyclone2_session *s = session();
    u32 hz = 0U;

    if (s) hz = config.profile[s->slot].report_hz;
    if (!hz) hz = cyclone2_profile_default_hz(s ? s->type : slot_type(0U));
    return hz ? hz : 125U;
}

CYCLONE2_HOT static void cyclone2_poll(void *private_data)
{
    struct cyclone2_input_report sample;
    struct cyclone2_input_report host_report;
    u32 now;
    u32 now_us;
    int probe_a;
    int report_due;

    (void)private_data;
    cyclone2_prof_start();
    now = sys_timer_get_ms();
    usb_poll(now);
    report_statistics.poll_count++;
    cyclone2_usb_control_poll();
    cyclone2_prof_mark(CYCLONE2_PROF_USB);

    config_poll(now, radio_quiet(now));
    cyclone2_prof_mark(CYCLONE2_PROF_CONFIG);

    if (state.kind == CYCLONE2_CONTROLLER_WAKING &&
        cyclone2_profile_wake_step(now)) wake_finished();
    cyclone2_rumble_poll(now);
    cyclone2_imu_tick();
    cyclone2_prof_mark(CYCLONE2_PROF_RUMBLE);
    cyclone2_input_read(&sample);
    now_us = cyclone2_time_us();
    report_due = (s32)(now_us - next_report_us) >= 0;
    probe_a = cyclone2_probe_sample(now_us);
    cyclone2_timing_poll(now_us);
    /* The probe stands in for the A contact, so it goes through the
     * debounce and everything after it like a real press. */
    if (probe_a > 0) sample.buttons |= CYCLONE2_REPORT_BUTTON_A;
    else if (probe_a == 0) sample.buttons &= (u16)~CYCLONE2_REPORT_BUTTON_A;
    /* A console command may have started injection during this poll. Use a
     * timestamp taken after it, so unsigned elapsed time cannot underflow. */
    now = sys_timer_get_ms();
    if (report_due) {
        cyclone2_input_map_analog(&sample);
        accept_analog_input(&sample);
    }
    debounce_digital_input(&sample, now_us);
    cyclone2_calibration_step(now, current_report.buttons);
    if (cyclone2_calibration_phase() != CYCLONE2_CAL_IDLE) {
        calibrating = 1U;
    } else if (calibrating) {
        /* Whatever is still held when the routine ends (A, usually) must
         * not start a chord or reach the host as a fresh press. */
        calibrating = 0U;
        home_state = CYCLONE2_HOME_IDLE;
        ignore_chord_until_release = 1U;
    }
    if (!calibrating) update_home_state();
    track_activity(now);
    if (cyclone2_offline_sleep_due(&state, now, usb_absent_since_ms,
                                  cyclone2_usb_present()))
        begin_automatic_sleep("offline on battery");
    update_injection(now);
    /* The ACL connection event precedes authentication and opening the HID
     * control/interrupt channels. Only the latter is a usable connection.
     *
     * Any state except teardown can reach it: a console may connect on its own
     * long after this controller gave up, and the Bluetooth stack stays alive
     * in that case (USB keeps the controller awake), so a reconnect must be
     * picked up rather than ignored. Otherwise the link comes up and carries
     * no reports at all. */
    if (session() && state.kind != CYCLONE2_CONTROLLER_STOPPING &&
        state.kind != CYCLONE2_CONTROLLER_CONNECTED &&
        cyclone2_profile_is_connected()) {
        cyclone2_bringup_record(0x6100u);
        become_connected(*session());
    }
    make_host_report(&host_report);
    if ((injection_status.active || injection_release_pending) &&
        injection_status.synthetic_a)
        host_report.buttons |= CYCLONE2_REPORT_BUTTON_A;
    cyclone2_prof_mark(CYCLONE2_PROF_LOGIC);
    if (report_due) {
        u32 period_us = 1000000U / active_report_hz();

        send_input_report(&host_report);
        cyclone2_prof_mark(CYCLONE2_PROF_SEND);
        /* Advance from the intended deadline rather than from a late poll,
         * so the average rate holds; a poll more than a period late starts
         * the schedule again instead of sending a burst. */
        next_report_us += period_us;
        if ((s32)(now_us - next_report_us) >= 0) next_report_us = now_us + period_us;
    }
    cyclone2_timing_poll_done(now_us);
    cyclone2_lights_poll(now, &current_report);
    cyclone2_prof_mark(CYCLONE2_PROF_LED);
    if (state.kind == CYCLONE2_CONTROLLER_CONNECTING &&
        now - state.as.connecting.since_ms >= CYCLONE2_CONNECT_TIMEOUT_MS) {
        if (!cyclone2_usb_present()) {
            begin_automatic_sleep("Bluetooth timeout");
        } else if (state.as.connecting.attempts < CYCLONE2_CONNECT_RETRIES &&
                   cyclone2_profile_reconnect()) {
            /* An attempt that ended with the link up but no HID session leaves
             * the controller stuck; ask the profile for a fresh one instead of
             * idling on a dead connection until the user resets. */
            state.as.connecting.attempts++;
            cyclone2_usb_debug("Bluetooth connect attempt %u timed out; retrying",
                               state.as.connecting.attempts);
            state.as.connecting.since_ms = now;
        } else {
            cyclone2_usb_debug("Bluetooth timeout; staying awake for USB console");
            state.as.connecting.since_ms = now;
        }
    } else if (state.kind == CYCLONE2_CONTROLLER_PAIRING &&
               now - state.as.pairing.since_ms >= CYCLONE2_PAIR_TIMEOUT_MS) {
        if (!cyclone2_usb_present()) {
            begin_automatic_sleep("Bluetooth timeout");
        } else {
            cyclone2_usb_debug("Bluetooth timeout; staying awake for USB console");
            state.as.pairing.since_ms = now;
        }
    }
    cyclone2_prof_mark(CYCLONE2_PROF_TAIL);
    cyclone2_prof_end();
}

/* The slot's radio settings, before its Bluetooth session starts: the host
 * stack reads the controller's features during its own start-up. */
static void apply_radio_settings(const struct config_profile *settings)
{
    u8 high = settings->tx_power == CONFIG_TX_POWER_HIGH;

    btstack_jl_core_set_in_flight(settings->in_flight);
    /* high: the controller's production-test mode, for its pro power table
     * (a higher top step; about 3 dB at the host).  The same call sets the
     * radio supply VDC13 to 1.40 V on the way in and 1.30 V on the way out,
     * but this board already boots at 1.40 V (step 7), and 1.30 V measured
     * about 1 dB weaker: the supply is put back as it was each time. */
    if (high || tx_power_high) {
        u8 vdc13 = p33_rx_1byte(P3_ANA_CON6) & 0x07U;

        bt_production_test(high);
        P33_CON_SET(P3_ANA_CON6, 0, 3, vdc13);
    }
    tx_power_high = high;
    set_bt_data_rate_acl_3mbs_mode(settings->edr3 ? 1U : 0U);
}

static void start_session(u8 slot, u8 pairing, enum cyclone2_profile_type type)
{
    u32 now = sys_timer_get_ms();
    struct cyclone2_session started = { .slot = slot, .type = type,
                                        .started_ms = now };
    struct cyclone2_controller_state next;

    memset(&next, 0, sizeof(next));
    if (pairing) {
        next.kind = CYCLONE2_CONTROLLER_PAIRING;
        next.as.pairing.session = started;
        next.as.pairing.since_ms = now;
    } else {
        next.kind = CYCLONE2_CONTROLLER_CONNECTING;
        next.as.connecting.session = started;
        next.as.connecting.since_ms = now;
    }
    become(&next);
    cyclone2_lights_host_clear();
    cyclone2_usb_debug("start slot %s type=%s pairing=%u",
                       config_slot_name(slot), cyclone2_profile_name(type),
                       pairing);

    apply_radio_settings(&config.profile[slot]);
    cyclone2_profile_start(type, slot, pairing);
}

static void cyclone2_start(void)
{
    u32 system_clock;
    u8 early_chord;
    u8 boot_slot;
    u8 boot_pairing;

    cyclone2_led_initialize();
    cyclone2_lights_initialize();
    cyclone2_rumble_initialize();
    clk_set("sys", BT_NORMAL_HZ);
    system_clock = clk_get("sys");
    bt_pll_para(TCFG_CLOCK_OSC_HZ, system_clock, 0, 0);
    /* Transmit power levels: connection, page, inquiry, LE.  Without this
     * call they stay 0, the lowest step of the controller's TXSET table, and
     * a Linux host measured our pad 20 dB below the stock one at the same
     * spot (link quality 4 of 255 against 255).  These are the SDK's own
     * defaults (apps/hid/modules/user_cfg.c); in normal mode the controller
     * caps every level at its highest step, 4 (bt_get_pwr_max_level()). */
    bt_max_pwr_set(10, 5, 8, 6);
    cyclone2_input_initialize();
    task_create(cyclone2_imu_task, NULL, "imu");
    cyclone2_usb_control_initialize();
    cyclone2_input_read(&candidate_report);
    current_report = candidate_report;
    activity_reference = current_report;
    last_activity_ms = sys_timer_get_ms();
    usb_absent_since_ms = last_activity_ms;
    started_ms = last_activity_ms;
    uboot_chord_at_boot = cyclone2_take_early_uboot_chord();
    cyclone2_timing_reset();
    memset(button_changed_us, 0, sizeof(button_changed_us));
    hat_changed_us = 0U;
    next_report_us = cyclone2_time_us();
    early_chord = cyclone2_take_early_profile_chord();
    if (boot_request_consume(&boot_slot, &boot_pairing)) {
        start_session(boot_slot, boot_pairing, slot_type(boot_slot));
    } else if (early_chord != 0U) {
        profile_select(early_chord - 1U, 0U);
    }
    cyclone2_control_clock_start(cyclone2_poll, CYCLONE2_CONTROL_PERIOD_US);
}

static int cyclone2_state_machine(struct application *application,
                                  enum app_state state,
                                  struct intent *intent)
{
    (void)application;
    if (state == APP_STA_START && intent && intent->action == ACTION_CYCLONE2)
        cyclone2_start();
    return 0;
}

static int cyclone2_event_handler(struct application *application,
                                  struct sys_event *event)
{
    enum cyclone2_profile_event result;
    struct cyclone2_session s;

    (void)application;
    if (event->type != SYS_BT_EVENT || !session()) return 0;
    s = *session();

    result = cyclone2_profile_handle_event(event);
    if ((result & CYCLONE2_PROFILE_EVENT_CONNECTED) &&
        state.kind != CYCLONE2_CONTROLLER_STOPPING)
        become_connected(s);
    if (result & CYCLONE2_PROFILE_EVENT_DISCONNECTED) {
        /* A disconnect from the session before this one, arriving late. */
        if (state.kind != CYCLONE2_CONTROLLER_CONNECTED &&
            sys_timer_get_ms() - s.started_ms < CYCLONE2_STALE_EVENT_MS) {
            return 0;
        }
        injection_cancel();
        cyclone2_rumble_stop();
        /* What the host set belonged to that link. */
        cyclone2_lights_host_clear();
        if (state.kind != CYCLONE2_CONTROLLER_STOPPING) {
            become_disconnected(s);
            /* The control poll starts battery sleep after a reconnect grace
             * period, independent of stick drift or button activity. */
        }
    }
    return 0;
}

static u8 cyclone2_idle_query(void)
{
    return 0;
}

REGISTER_LP_TARGET(cyclone2_low_power_target) = {
    .name = "cyclone2",
    .is_idle = cyclone2_idle_query,
};

static const struct application_operation cyclone2_operations = {
    .state_machine = cyclone2_state_machine,
    .event_handler = cyclone2_event_handler,
};

REGISTER_APPLICATION(cyclone2_application) = {
    .name = "cyclone2",
    .action = ACTION_CYCLONE2,
    .ops = &cyclone2_operations,
    .state = APP_STA_DESTROY,
};

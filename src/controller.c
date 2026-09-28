#include <system/app_core.h>
#include <system/includes.h>
#include <usb/otg.h>

#include "app_config.h"
#include "btstack_jl.h"
#include "calibration.h"
#include "controller.h"
#include "main.h"
#include "imu.h"
#include "profile.h"
#include "profile_classic.h"
#include "rumble.h"
#include "settings.h"
#include "user_cfg_id.h"
#include "wake/wake.h"

/* The controller library on its own, without BTstack (btstack-jl-port's
 * jieli-br23 bridge).  The wake needs the baseband set up - its buffer window
 * base is 0 once the library has been shut down, and __write_reg_txinfo()
 * resets the chip for a payload outside that window. */
extern int jieli_btctrler_open(const uint8_t *bd_addr);
extern void jieli_btctrler_close(void);

#define CYCLONE2_CONTROL_PERIOD_MS           5U
#define CYCLONE2_DEFAULT_REPORT_PERIOD_MS   16U
#define CYCLONE2_DEBOUNCE_MS                10U
#define CYCLONE2_PROFILE_COUNT              4U
#define CYCLONE2_SETTINGS_MAGIC     0x43325052U
#define CYCLONE2_SETTINGS_VERSION           2U
#define CYCLONE2_HOME_GRACE_MS              150U
#define CYCLONE2_LONG_HOLD_MS              5000U
/* Home+M held this long starts the stick and trigger calibration. */
#define CYCLONE2_CALIBRATE_HOLD_MS         3000U
#define CYCLONE2_CONNECT_TIMEOUT_MS       30000U
/* 3 minutes: with its Wi-Fi on, a Switch 2 took 24 s and a retry to find the
 * pad (2026-09-25), and it can miss a 60 s window entirely. */
#define CYCLONE2_PAIR_TIMEOUT_MS          180000U
/* How long the radio must have been idle before syscfg may be written.  The
 * controller keeps working for a moment after a session ends, and a flash
 * write in that window resets the pad. */
#define CYCLONE2_FLASH_IDLE_MS             3000U
/* Extra outgoing connection attempts after one ends without a usable session.
 * Small and bounded: an unreachable host must not become a retry storm. */
#define CYCLONE2_CONNECT_RETRIES               2U
#define CYCLONE2_BOOT_LED_MS                500U
#define CYCLONE2_LED_DIM_RED                 8U
#define CYCLONE2_LED_SLOW_HALF_PERIOD_MS   500U
#define CYCLONE2_LED_FAST_HALF_PERIOD_MS   120U
#define CYCLONE2_INJECT_MIN_PERIOD_MS       20U
#define CYCLONE2_INJECT_MAX_PERIOD_MS       1000U
#define CYCLONE2_INJECT_MAX_DURATION_MS    60000U
/* The console re-enumerates on its own (measured), and the USB-sense pin never
 * asserts on this board, so a single "USB absent" sample is not trustworthy.
 * Power off only after the condition has held this many times, and never
 * while a host is holding the CDC console open. */
#define CYCLONE2_SLEEP_CONFIRM              4U
#define CYCLONE2_SLEEP_RECHECK_MS         250U
/* Time a live profile is given to emit its disconnect before the radio
 * session is stopped and the next profile is started. */
#define CYCLONE2_PROFILE_SWITCH_MS          100U
/* The stack's halt waits for the peer to confirm the disconnect, so the
 * teardown is polled from a timer instead of blocking the application task
 * (which also runs the USB console).  The profile's supervision timeout is
 * 8 s; past that we give up rather than wait forever. */
#define CYCLONE2_PROFILE_EXIT_POLL_MS       100U
#define CYCLONE2_PROFILE_EXIT_MAX_MS      12000U
/* After a profile switch, the events still in flight belong to the session
 * that was just stopped.  Ignore a disconnect reported in this window while
 * the new session is still coming up. */
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

enum cyclone2_radio_state {
    CYCLONE2_RADIO_OFF,
    CYCLONE2_RADIO_CONNECTING,
    CYCLONE2_RADIO_PAIRING,
    CYCLONE2_RADIO_CONNECTED,
    CYCLONE2_RADIO_STOPPING,
    /* The baseband belongs to the Switch 1 wake sequence: no profile is
     * running, and the wake owns the transmit scheduler. */
    CYCLONE2_RADIO_WAKING,
};

struct cyclone2_settings {
    u32 magic;
    u16 version;
    u16 size;
    u8 profile_type[CYCLONE2_PROFILE_COUNT];
    u8 report_period_ms;
    u8 reserved[3];
};

static struct cyclone2_settings settings;
static struct cyclone2_input_report candidate_report;
static struct cyclone2_input_report current_report;
static enum cyclone2_home_state home_state;
static enum cyclone2_radio_state radio_state;
static enum cyclone2_profile_type active_type;
static u32 started_at_ms;
static u32 home_start_ms;
static u32 chord_start_ms;
static u32 radio_start_ms;
static u32 button_candidate_since_ms[16];
static u32 hat_candidate_since_ms;
static u32 next_report_ms;
static u16 report_timer;
static u16 chord_button;
static u8 active_profile = 0xffU;
static u8 requested_profile;
static u8 active_pairing;
static u8 pending_profile;
static u8 pending_pairing;
static enum cyclone2_profile_type pending_type;
static u32 transition_started_ms;
static u32 ignore_disconnect_until_ms;
static u8 home_tap_phase;
static u8 led_is_on;
static u32 calibration_led_colour;
static u8 calibrating;
static u8 poweroff_flash_wait;
static u8 sleep_pending;
static u8 sleep_confirm_count;
static u8 ignore_chord_until_release;
static u8 connect_attempts;
static u32 idle_since_ms;
static struct cyclone2_report_statistics report_statistics;
static struct cyclone2_injection_status injection_status;
static u32 injection_next_transition_ms;
static u32 injection_target_edges;
static u8 injection_last_emitted;
static u8 injection_release_pending;
/* A wake request names a slot, because the access code is derived from the
 * console's address and each slot is its own console.  The request survives
 * the profile teardown that has to happen before the baseband is free. */
static u8 wake_pending;
static u8 wake_pending_slot;
static u8 wake_pending_channel;
static u8 wake_pending_seconds;
static u8 wake_slot = 0xffU;
/* The baseband registers and the transmit scheduler belong to the controller
 * library, which btstack_init() brings up with the first profile.  The wake
 * takes the scheduler over, so it must not run before that has ever happened:
 * a stored peer address survives a reflash, the controller does not. */
static u8 controller_ready;

extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);
static void start_selected_profile(void);
static void profile_select(u8 profile, u8 pairing);
static void injection_cancel(void);
static void soft_reset_now(void *private_data);
static void start_pending_wake(void);
static void profile_request_transition(u8 profile, u8 pairing);

static void radio_exit_only(void *private_data)
{
    (void)private_data;
    cyclone2_rumble_stop();
    if (active_profile < CYCLONE2_PROFILE_COUNT) {
        cyclone2_profile_exit();
    }
    radio_state = CYCLONE2_RADIO_OFF;
}

int cyclone2_usb_present(void)
{
    return gpio_read(CYCLONE2_USB_SENSE) ||
           usb_otg_online(USB0) != DISCONN_MODE;
}

u8 get_cur_bt_idx(void)
{
    return active_profile < CYCLONE2_PROFILE_COUNT ? active_profile : 0U;
}

/* Written from cyclone2_poll() when the radio is connected or off, like the
 * calibration; held back in the store's RAM mode (settings.h). */
static u8 settings_dirty;

static void settings_flush(void)
{
    if (!settings_dirty || cyclone2_store_ram_mode()) return;
    settings_dirty = 0U;
    syscfg_write(CFG_CYCLONE2_PROFILE_SETTINGS, &settings, sizeof(settings));
}

static void settings_load(void)
{
    settings_dirty = 0U;
    int length = syscfg_read(CFG_CYCLONE2_PROFILE_SETTINGS, &settings,
                             sizeof(settings));

    if (length == sizeof(settings) &&
        settings.magic == CYCLONE2_SETTINGS_MAGIC &&
        settings.version == CYCLONE2_SETTINGS_VERSION &&
        settings.size == sizeof(settings)) {
        if (settings.report_period_ms == 5U ||
            settings.report_period_ms == 8U ||
            settings.report_period_ms == 10U ||
            settings.report_period_ms == 16U) return;
        settings.report_period_ms = CYCLONE2_DEFAULT_REPORT_PERIOD_MS;
    } else {
        memset(&settings, 0, sizeof(settings));
        settings.magic = CYCLONE2_SETTINGS_MAGIC;
        settings.version = CYCLONE2_SETTINGS_VERSION;
        settings.size = sizeof(settings);
        settings.profile_type[0] = CYCLONE2_PROFILE_SWITCH;
        settings.profile_type[1] = CYCLONE2_PROFILE_SWITCH;
        settings.profile_type[2] = CYCLONE2_PROFILE_GENERIC;
        settings.profile_type[3] = CYCLONE2_PROFILE_SWITCH2;
        settings.report_period_ms = CYCLONE2_DEFAULT_REPORT_PERIOD_MS;
    }
}

u8 cyclone2_report_period_get(void)
{
    return settings.report_period_ms;
}

int cyclone2_report_period_set(u8 period_ms)
{
    if (period_ms != 5U && period_ms != 8U && period_ms != 10U &&
        period_ms != 16U) return -1;
    settings.report_period_ms = period_ms;
    next_report_ms = sys_timer_get_ms();
    settings_dirty = 1U;
    return 0;
}

/* Any persistent change not in flash yet. */
int cyclone2_store_pending(void)
{
    return settings_dirty || cyclone2_calibration_pending() ||
           cyclone2_settings_pending() || btstack_jl_link_key_pending();
}

/* Drop pending changes and reload everything from flash.  Only with the
 * radio down: a running session uses the link keys and addresses. */
int cyclone2_store_revert(void)
{
    if (!cyclone2_radio_stopped()) return -1;
    btstack_jl_link_key_load();
    cyclone2_settings_load();
    settings_load();
    cyclone2_calibration_load();
    return 0;
}

u8 cyclone2_profile_type_get(u8 slot)
{
    if (slot >= CYCLONE2_PROFILE_COUNT) return CYCLONE2_PROFILE_SWITCH;
    return settings.profile_type[slot] < CYCLONE2_PROFILE_TYPE_COUNT ?
           settings.profile_type[slot] : CYCLONE2_PROFILE_SWITCH;
}

/* Choose what a slot speaks.  Takes effect the next time the slot starts
 * (connect, pair, Home chord); a running session keeps its type.  Each type
 * keeps its own bond with its host, but the slot's stored peer address is
 * shared, so pair the slot again after changing its type. */
int cyclone2_profile_type_set(u8 slot, u8 type)
{
    if (slot >= CYCLONE2_PROFILE_COUNT || type >= CYCLONE2_PROFILE_TYPE_COUNT)
        return -1;
    settings.profile_type[slot] = type;
    settings_dirty = 1U;
    return 0;
}

void cyclone2_report_statistics_get(struct cyclone2_report_statistics *stats)
{
    *stats = report_statistics;
}

int cyclone2_connect_request(u8 profile)
{
    if (profile >= CYCLONE2_PROFILE_COUNT) return -1;
    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;
    if (radio_state == CYCLONE2_RADIO_CONNECTED &&
        active_profile == profile &&
        cyclone2_profile_is_connected()) return 0;
    injection_cancel();
    profile_select(profile, 0U);
    return 1;
}

/* Wake the console this slot last connected to, then reconnect it.
 *
 * A sleeping Switch does not answer paging, which is why a pad that looks
 * paired can sit there doing nothing until the console is woken by hand.  The
 * wake is a radio-level sequence the standard host interface cannot produce
 * (see docs/ns1-wakeup.md), so this takes the baseband away from the profile
 * for the duration: the running session is torn down first, the bursts run
 * for `seconds`, and then the slot reconnects normally.
 *
 * The target address is read from the slot's own peer record, the same one a
 * working connection writes, so a pad paired with two different consoles
 * wakes the one the requested slot belongs to. */
int cyclone2_wake_request(u8 slot, u8 channel, u8 seconds)
{
    u8 target[6];
    static const u8 zero[6] = {0, 0, 0, 0, 0, 0};

    if (slot >= CYCLONE2_PROFILE_COUNT) return -1;
    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;
    if (radio_state == CYCLONE2_RADIO_WAKING) return -1;
    /* No "controller must have been started" check any more: the wake opens
     * the controller library itself (start_pending_wake). */
    if (channel > WAKE_CHANNEL_MAX ||
        seconds < WAKE_SECONDS_MIN || seconds > WAKE_SECONDS_MAX) return -2;
    /* Nothing to aim at: a slot that has never connected has no console
     * address, and the wake code is derived from that address. */
    if (!cyclone2_peer_address_read(slot, target)) return -3;
    if (!memcmp(target, zero, sizeof(target))) return -3;

    injection_cancel();
    wake_pending = 1U;
    wake_pending_slot = slot;
    wake_pending_channel = channel;
    wake_pending_seconds = seconds;

    /* Always deferred, even with no session running: profile_transition_now()
     * runs from a timer, and going through it keeps the command parser from
     * carrying the profile-start path on its own stack - the same reason the
     * other commands queue their work.  That function sees wake_pending and
     * starts the wake instead of bringing a profile up. */
    profile_request_transition(slot, 0U);
    return 1;
}

/* The wake sequence has finished (or gave up); put the slot back on its
 * normal reconnect path.  Called from the wake session in the poll. */
void cyclone2_wake_finished(void)
{
    u8 slot = wake_slot;

    if (radio_state != CYCLONE2_RADIO_WAKING) return;
    radio_state = CYCLONE2_RADIO_OFF;
    wake_slot = 0xffU;
    /* Hand the controller back closed, so the profile brings it up with
     * BTstack exactly as it does from cold. */
    jieli_btctrler_close();
    if (slot < CYCLONE2_PROFILE_COUNT) {
        cyclone2_usb_debug("wake done; reconnecting slot %u", slot);
        profile_select(slot, 0U);
    }
}

/* Take the baseband for the wake and start the bursts.  Only reached with no
 * profile running, which is what makes it safe to disable the idle task and
 * put our own task in the transmit scheduler. */
static void start_pending_wake(void)
{
    int result;

    active_profile = 0xffU;
    active_pairing = 0U;
    wake_slot = wake_pending_slot;
    wake_pending = 0U;
    radio_state = CYCLONE2_RADIO_WAKING;
    /* Bring the controller library up without a host: the teardown before
     * this shut it down, which also clears the baseband setup the wake's
     * frame needs. */
    result = jieli_btctrler_open(NULL);
    if (result == 0) {
        result = wake_start(wake_pending_slot, wake_pending_channel,
                            wake_pending_seconds);
    }
    if (result != 0) {
        cyclone2_usb_debug("wake not started (%d); reconnecting slot %u",
                           result, wake_pending_slot);
        jieli_btctrler_close();
        radio_state = CYCLONE2_RADIO_OFF;
        wake_slot = 0xffU;
        profile_select(wake_pending_slot, 0U);
    }
}

/* Same as a connect request except that the slot forgets its stored link key
 * and becomes pairable.  The physical chord is Home+face for five seconds;
 * this is the equivalent for someone driving the controller over USB, e.g.
 * while the console sits on its "Change Grip/Order" screen. */
int cyclone2_pair_request(u8 profile)
{
    if (profile >= CYCLONE2_PROFILE_COUNT) return -1;
    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;
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
    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;
    cyclone2_bt_address_randomize();
    injection_cancel();
    profile_select(profile, 1U);
    return 1;
}

/* Adopt a fresh random Bluetooth address without changing the slot or the
 * pairing mode: the running session is restarted so the controller picks the
 * new address up.  A console keys its bond by this address, so the pad has to
 * be paired again afterwards.
 *
 * The address is written to syscfg at the next flush (in the store's RAM mode
 * not at all, and it is gone at the next reset). */
int cyclone2_new_address_request(void)
{
    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;
    injection_cancel();
    cyclone2_bt_address_randomize();
    if (active_profile < CYCLONE2_PROFILE_COUNT) {
        profile_select(active_profile, active_pairing);
    }
    return 1;
}

/* Same as a new address, but with the address given instead of drawn.  Used
 * to put back an identity that was saved off the pad, so a captured pairing
 * can be replayed after a reflash. */
int cyclone2_set_address_request(const u8 address[6])
{
    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;
    injection_cancel();
    cyclone2_bt_address_set(address);
    if (active_profile < CYCLONE2_PROFILE_COUNT) {
        profile_select(active_profile, active_pairing);
    }
    return 1;
}

/* Restore a captured bond (e.g. after a reset in RAM mode).  Unlike setaddr,
 * this must start the requested slot even when no profile has been selected
 * yet. */
int cyclone2_connect_with_address_request(u8 slot, const u8 address[6])
{
    if (slot >= CYCLONE2_PROFILE_COUNT ||
        radio_state == CYCLONE2_RADIO_STOPPING) return -1;
    injection_cancel();
    cyclone2_bt_address_set(address);
    profile_select(slot, 0U);
    return 1;
}

/* Forget a specific peer's link key, then re-enter pairing with a new local
 * address.  This is the recovery for a link the two sides disagree about:
 * bonding records live in the Bluetooth controller's own memory, so they
 * survive reflashing and cannot be reached with the flash tools. */
int cyclone2_forget_peer_request(const u8 address[6])
{
    u8 slot = active_profile < CYCLONE2_PROFILE_COUNT ? active_profile : 0U;

    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;
    injection_cancel();
    cyclone2_classic_forget_peer(address);
    cyclone2_bt_address_randomize();
    profile_select(slot, 1U);
    return 1;
}

/* Record the peer address a slot should page, and clear the slot's stored link
 * key so the next connection has to authenticate against the peer's record.
 * This is the console-command equivalent of having paired with that host; it
 * exists so a slot can be restored (or re-pointed) without a pairing round. */
int cyclone2_set_peer_request(u8 slot, const u8 address[6])
{
    if (slot >= CYCLONE2_PROFILE_COUNT) return -1;
    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;
    injection_cancel();
    cyclone2_classic_forget_peer(address);
    cyclone2_peer_address_write(slot, address);
    profile_select(slot, 0U);
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

    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;
    for (index = 0U; index < CYCLONE2_BT_BOND_STORE_SIZE; index++) {
        store[index] = 0U;
    }
    __asm__ volatile("csync");
    injection_cancel();
    cyclone2_rumble_stop();
    radio_state = CYCLONE2_RADIO_STOPPING;
    if (active_profile < CYCLONE2_PROFILE_COUNT) cyclone2_profile_disconnect();
    sys_timeout_add(NULL, soft_reset_now, 100U);
    return 1;
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
    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;
    injection_cancel();
    cyclone2_rumble_stop();
    radio_state = CYCLONE2_RADIO_STOPPING;
    if (active_profile < CYCLONE2_PROFILE_COUNT) cyclone2_profile_disconnect();
    sys_timeout_add(NULL, soft_reset_now, 100U);
    return 1;
}

int cyclone2_disconnect_request(void)
{
    if (radio_state == CYCLONE2_RADIO_WAKING) {
        /* There is no profile to disconnect: the baseband is the wake's. */
        wake_stop();
        wake_slot = 0xffU;
        wake_pending = 0U;
        radio_state = CYCLONE2_RADIO_OFF;
        return 1;
    }
    if (active_profile >= CYCLONE2_PROFILE_COUNT) return 0;
    if (radio_state == CYCLONE2_RADIO_STOPPING) return -1;

    injection_cancel();
    cyclone2_rumble_stop();
    radio_state = CYCLONE2_RADIO_STOPPING;
    if (cyclone2_profile_is_connected()) cyclone2_profile_disconnect();
    /* Let the profile emit its protocol-level disconnect before shutting down
     * the shared Bluetooth stack. This also stops discoverability after a
     * failed pairing attempt, where radio_state is OFF but btstack is active.
     * A later connect command uses the existing reset handoff to initialize
     * that stack from a clean state. */
    sys_timeout_add(NULL, radio_exit_only, 100U);
    return 1;
}

/* Whether the radio is fully down: no profile stopping, no wake running, and
 * the BTstack task has powered the controller off and parked.  radio_state
 * alone goes OFF as soon as the shutdown is *requested* (radio_exit_only), so
 * it said "stopped" while the controller was still running - and entering
 * UBOOT that way hung the pad off USB (2026-09-27, DualSense slot pairing). */
int cyclone2_radio_stopped(void)
{
    return radio_state == CYCLONE2_RADIO_OFF && btstack_jl_core_idle();
}

void cyclone2_connection_status_get(struct cyclone2_connection_status *status)
{
    u8 slot = active_profile < CYCLONE2_PROFILE_COUNT ? active_profile : 0U;
    status->slot = slot;
    status->profile_type = active_profile < CYCLONE2_PROFILE_COUNT ?
                            active_type :
                            settings.profile_type[slot] < CYCLONE2_PROFILE_TYPE_COUNT ?
                            settings.profile_type[slot] : CYCLONE2_PROFILE_SWITCH;
    status->connection_state = (u8)radio_state;
    status->usb_present = (u8)cyclone2_usb_present();
    status->connect_attempts = connect_attempts;
    cyclone2_bt_address_for_slot(slot, status->local_address);
}

int cyclone2_inject_start(u32 period_ms, u32 duration_ms)
{
    if (active_profile >= CYCLONE2_PROFILE_COUNT ||
        !cyclone2_profile_is_connected() ||
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
    /* Complete whole press/release cycles even if a caller supplies a
     * duration that is not an exact multiple of the period. */
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

static int boot_request_consume(void)
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

    /* A profile record is one-shot.  Leave diagnostic records intact for the
     * USB status command after a failed recovery handoff. */
    reset_mailbox.magic = 0U;
    __asm__ volatile("csync");

    if (profile >= CYCLONE2_PROFILE_COUNT || pairing > 1U ||
        check != boot_request_check(profile, pairing)) return 0;

    /* This is a reset handoff, not a saved active profile. Consume it before
     * starting the radio so a later unrelated reset returns to slot select. */
    active_profile = profile;
    active_pairing = pairing;
    ignore_chord_until_release = 1U;
    active_type = settings.profile_type[profile] < CYCLONE2_PROFILE_TYPE_COUNT ?
                  settings.profile_type[profile] : CYCLONE2_PROFILE_SWITCH;
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
    (void)private_data;
    cyclone2_rumble_stop();
    if (active_profile < CYCLONE2_PROFILE_COUNT) {
        if (!cyclone2_profile_exit()) {
            /* Still going down: the stack's halt waits for the peer to confirm
             * the disconnect, which can take the supervision timeout.  Poll
             * from a timer rather than block here - the console shares this
             * task.  Starting the next session before this one is down is what
             * used to reset the chip. */
            if ((s32)(sys_timer_get_ms() - transition_started_ms) <
                (s32)CYCLONE2_PROFILE_EXIT_MAX_MS) {
                sys_timeout_add(NULL, profile_transition_now,
                                CYCLONE2_PROFILE_EXIT_POLL_MS);
                return;
            }
            active_profile = 0xffU;
            radio_state = CYCLONE2_RADIO_OFF;
            cyclone2_usb_debug(
                "profile switch aborted: the previous session did not stop");
            return;
        }
    }
    if (wake_pending) {
        /* The radio is quiet now, which is the only state the wake can take
         * the baseband in. */
        start_pending_wake();
        return;
    }
    active_profile = pending_profile;
    active_pairing = pending_pairing;
    active_type = pending_type;
    home_state = CYCLONE2_HOME_IDLE;
    ignore_chord_until_release = 1U;
    start_selected_profile();
}

/* Queue a switch to another profile slot.  Returns immediately; the radio is
 * stopped and restarted from profile_transition_now() once the current profile
 * has had a moment to emit its protocol-level disconnect. */
static void profile_request_transition(u8 profile, u8 pairing)
{
    if (radio_state == CYCLONE2_RADIO_STOPPING) {
        return;
    }
    injection_cancel();
    cyclone2_rumble_stop();

    pending_profile = profile;
    pending_pairing = pairing;
    pending_type = settings.profile_type[profile] < CYCLONE2_PROFILE_TYPE_COUNT ?
                   settings.profile_type[profile] : CYCLONE2_PROFILE_SWITCH;

    radio_state = CYCLONE2_RADIO_STOPPING;
    if (cyclone2_profile_is_connected()) {
        cyclone2_profile_disconnect();
    }
    transition_started_ms = sys_timer_get_ms();
    sys_timeout_add(NULL, profile_transition_now, CYCLONE2_PROFILE_SWITCH_MS);
}

static void profile_select(u8 profile, u8 pairing)
{
    injection_cancel();
    if (radio_state == CYCLONE2_RADIO_WAKING) {
        /* Anything that wants the radio back - a connect, a pairing, a
         * profile switch - takes it back from the wake. */
        wake_stop();
        wake_slot = 0xffU;
        wake_pending = 0U;
        radio_state = CYCLONE2_RADIO_OFF;
    }
    if (active_profile < CYCLONE2_PROFILE_COUNT) {
        profile_request_transition(profile, pairing);
        return;
    }

    active_profile = profile;
    active_pairing = pairing;
    active_type = settings.profile_type[profile] < CYCLONE2_PROFILE_TYPE_COUNT ?
                  settings.profile_type[profile] : CYCLONE2_PROFILE_SWITCH;
    home_state = CYCLONE2_HOME_IDLE;
    ignore_chord_until_release = 1U;
    start_selected_profile();
}

static void poweroff_now(void *private_data)
{
    u32 sense = gpio_read(CYCLONE2_USB_SENSE) ? 1U : 0U;
    u32 otg = usb_otg_online(USB0) != DISCONN_MODE ? 1U : 0U;
    u32 terminal = cyclone2_cdc_terminal_open() ? 1U : 0U;

    (void)private_data;
    /* What the sleep decision saw, so a pad that goes dark can be explained
     * afterwards: 0x52xx with the USB-sense and OTG bits, 0x5300 when the
     * firmware powers itself off. */
    cyclone2_bringup_record(0x5200u | sense | (otg << 1));
    cyclone2_bringup_record(0x5400u | terminal);
    if (sense || otg || terminal) {
        radio_state = CYCLONE2_RADIO_OFF;
        cyclone2_usb_debug("sleep cancelled; USB is connected");
        sleep_confirm_count = 0U;
        poweroff_flash_wait = 0U;
        return;
    }
    if (++sleep_confirm_count < CYCLONE2_SLEEP_CONFIRM) {
        /* Keep asking: a glitch that clears on the next poll must not put the
         * pad to sleep, because that leaves it dark until a power cycle. */
        sys_timeout_add(NULL, poweroff_now, CYCLONE2_SLEEP_RECHECK_MS);
        return;
    }
    /* Changes not written yet (a pairing, a calibration): give the radio the
     * same settling time the idle flush does rather than lose them to the
     * power-off.  In RAM mode they are dropped, as a reboot would. */
    if (cyclone2_store_pending() && !cyclone2_store_ram_mode()) {
        if (!poweroff_flash_wait) {
            poweroff_flash_wait = 1U;
            sys_timeout_add(NULL, poweroff_now, CYCLONE2_FLASH_IDLE_MS);
            return;
        }
        btstack_jl_link_key_flush();
        cyclone2_settings_flush();
        cyclone2_calibration_flush();
        settings_flush();
    }
    poweroff_flash_wait = 0U;
    cyclone2_bringup_record(0x5300u);
    power_set_soft_poweroff();
}

static void radio_exit_and_poweroff(void *private_data)
{
    (void)private_data;
    cyclone2_rumble_stop();
    /* The wake can be reached from the sleep path (a long Home hold) as well
     * as from a command, and it has to give the radio back before anything
     * else takes over. */
    if (radio_state == CYCLONE2_RADIO_WAKING) {
        wake_stop();
        wake_slot = 0xffU;
        wake_pending = 0U;
        radio_state = CYCLONE2_RADIO_OFF;
    }
    if (cyclone2_usb_present()) {
        sleep_pending = 0U;
        radio_state = active_profile < CYCLONE2_PROFILE_COUNT &&
                      cyclone2_profile_is_connected() ?
                      CYCLONE2_RADIO_CONNECTED : CYCLONE2_RADIO_OFF;
        cyclone2_usb_debug("sleep cancelled; USB is connected");
        return;
    }
    if (active_profile < CYCLONE2_PROFILE_COUNT) {
        cyclone2_profile_exit();
    }
    cyclone2_led_set_home(0U, 0U, 0U);
    sys_timeout_add(NULL, poweroff_now, 300U);
}

static void begin_sleep(void)
{
    if (cyclone2_usb_present()) {
        if (radio_state == CYCLONE2_RADIO_CONNECTED) {
            cyclone2_usb_debug(
                "Home hold: disconnecting Bluetooth; USB keeps console awake");
            cyclone2_disconnect_request();
        } else {
            cyclone2_usb_debug(
                "Home hold: USB keeps console awake; Bluetooth is not connected");
        }
        /* Make the long-hold action one-shot until Home is released. */
        home_state = CYCLONE2_HOME_INVALID_CHORD;
        return;
    }
    if (radio_state != CYCLONE2_RADIO_STOPPING) sleep_pending = 1U;
}

static void begin_automatic_sleep(const char *reason)
{
    u32 sense = gpio_read(CYCLONE2_USB_SENSE) ? 1U : 0U;
    u32 otg = usb_otg_online(USB0) != DISCONN_MODE ? 1U : 0U;

    /* 0x50xx with the USB-sense and OTG bits: entering the automatic-sleep
     * path at all is worth recording, since a momentary "USB absent" here is
     * what makes the firmware power itself off while it is on USB. */
    cyclone2_bringup_record(0x5000u | sense | (otg << 1));
    if (sense || otg) {
        cyclone2_usb_debug("%s; staying awake because USB is connected", reason);
        return;
    }
    begin_sleep();
}

static int selected_face_button(u16 buttons, u8 *profile)
{
    u16 face = buttons & CYCLONE2_FACE_BUTTONS;

    if (face == CYCLONE2_REPORT_BUTTON_A) *profile = 0U;
    else if (face == CYCLONE2_REPORT_BUTTON_B) *profile = 1U;
    else if (face == CYCLONE2_REPORT_BUTTON_X) *profile = 2U;
    else if (face == CYCLONE2_REPORT_BUTTON_Y) *profile = 3U;
    else return 0;
    return 1;
}

static void update_home_state(void)
{
    u16 buttons = current_report.buttons;
    u32 now = sys_timer_get_ms();
    u8 profile;
    int home = (buttons & CYCLONE2_REPORT_BUTTON_HOME) != 0U;

    if (ignore_chord_until_release) {
        if (!(buttons & (CYCLONE2_REPORT_BUTTON_HOME |
                         CYCLONE2_FACE_BUTTONS))) {
            ignore_chord_until_release = 0U;
        }
        return;
    }

    switch (home_state) {
    case CYCLONE2_HOME_IDLE:
        if (home) {
            home_start_ms = now;
            if (!(buttons & CYCLONE2_FACE_BUTTONS) &&
                (buttons & CYCLONE2_REPORT_BUTTON_M)) {
                chord_start_ms = now;
                home_state = CYCLONE2_HOME_CALIBRATE_CHORD;
            } else if (selected_face_button(buttons, &profile)) {
                chord_button = buttons & CYCLONE2_FACE_BUTTONS;
                chord_start_ms = now;
                requested_profile = profile;
                home_state = CYCLONE2_HOME_CHORD;
            } else if (buttons & CYCLONE2_FACE_BUTTONS) {
                home_state = CYCLONE2_HOME_INVALID_CHORD;
            } else {
                home_state = CYCLONE2_HOME_PENDING;
            }
        }
        break;
    case CYCLONE2_HOME_PENDING:
        if (!home) {
            home_tap_phase = 2U;
            home_state = CYCLONE2_HOME_IDLE;
        } else if (!(buttons & CYCLONE2_FACE_BUTTONS) &&
                   (buttons & CYCLONE2_REPORT_BUTTON_M)) {
            chord_start_ms = now;
            home_state = CYCLONE2_HOME_CALIBRATE_CHORD;
        } else if (selected_face_button(buttons, &profile)) {
            chord_button = buttons & CYCLONE2_FACE_BUTTONS;
            chord_start_ms = now;
            requested_profile = profile;
            home_state = CYCLONE2_HOME_CHORD;
        } else if (buttons & CYCLONE2_FACE_BUTTONS) {
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
        if (!home || (buttons & chord_button) == 0U) {
            profile_select(requested_profile, 0U);
        } else if ((buttons & CYCLONE2_FACE_BUTTONS) != chord_button) {
            home_state = CYCLONE2_HOME_INVALID_CHORD;
        } else if (now - chord_start_ms >= CYCLONE2_LONG_HOLD_MS) {
            profile_select(requested_profile, 1U);
        }
        break;
    case CYCLONE2_HOME_CALIBRATE_CHORD:
        /* Released early, or another button joined: no action, and nothing
         * reaches the host until everything is let go. */
        if (!home || !(buttons & CYCLONE2_REPORT_BUTTON_M) ||
            (buttons & CYCLONE2_FACE_BUTTONS)) {
            home_state = CYCLONE2_HOME_INVALID_CHORD;
        } else if (now - chord_start_ms >= CYCLONE2_CALIBRATE_HOLD_MS) {
            cyclone2_calibration_start();
            home_state = CYCLONE2_HOME_INVALID_CHORD;
        }
        break;
    case CYCLONE2_HOME_INVALID_CHORD:
        if (!home && !(buttons & (CYCLONE2_FACE_BUTTONS |
                                  CYCLONE2_REPORT_BUTTON_M)))
            home_state = CYCLONE2_HOME_IDLE;
        break;
    }
}

static void make_host_report(struct cyclone2_input_report *report)
{
    /* While calibrating, the sticks are being swung to their stops and A
     * ends the routine: none of that is meant for the host. */
    if (calibrating) {
        memset(report, 0, sizeof(*report));
        report->hat = CYCLONE2_HAT_NEUTRAL;
        return;
    }
    *report = current_report;
    if (home_state == CYCLONE2_HOME_CALIBRATE_CHORD ||
        (home_state == CYCLONE2_HOME_INVALID_CHORD &&
         (report->buttons & CYCLONE2_REPORT_BUTTON_M) &&
         !(report->buttons & CYCLONE2_FACE_BUTTONS))) {
        report->buttons &= ~CYCLONE2_REPORT_BUTTON_M;
    }
    if (home_state == CYCLONE2_HOME_PENDING ||
        home_state == CYCLONE2_HOME_CHORD ||
        home_state == CYCLONE2_HOME_CALIBRATE_CHORD ||
        home_state == CYCLONE2_HOME_INVALID_CHORD ||
        ignore_chord_until_release ||
        sleep_pending ||
        radio_state == CYCLONE2_RADIO_STOPPING) {
        report->buttons &= ~(CYCLONE2_REPORT_BUTTON_HOME |
                             CYCLONE2_FACE_BUTTONS);
    }
    if (home_tap_phase == 2U) {
        report->buttons |= CYCLONE2_REPORT_BUTTON_HOME;
    } else if (home_tap_phase == 1U) {
        report->buttons &= ~CYCLONE2_REPORT_BUTTON_HOME;
    }
}

static void debounce_digital_input(const struct cyclone2_input_report *sample,
                                   u32 now)
{
    unsigned bit;

    for (bit = 0; bit < 16U; bit++) {
        u16 mask = (u16)(1U << bit);
        if ((sample->buttons & mask) != (candidate_report.buttons & mask)) {
            candidate_report.buttons ^= mask;
            button_candidate_since_ms[bit] = now;
        } else if ((current_report.buttons & mask) !=
                   (candidate_report.buttons & mask) &&
                   now - button_candidate_since_ms[bit] >=
                   CYCLONE2_DEBOUNCE_MS) {
            current_report.buttons ^= mask;
        }
    }

    if (sample->hat != candidate_report.hat) {
        candidate_report.hat = sample->hat;
        hat_candidate_since_ms = now;
    } else if (current_report.hat != candidate_report.hat &&
               now - hat_candidate_since_ms >= CYCLONE2_DEBOUNCE_MS) {
        current_report.hat = candidate_report.hat;
    }
}

static void update_status_led(void)
{
    u32 now = sys_timer_get_ms();
    u32 half_period;
    u8 on;
    u8 red;
    u8 green;
    u8 blue;

    if (cyclone2_calibration_led(now, &red, &green, &blue)) {
        u32 colour = 0x1000000U | ((u32)red << 16) | ((u32)green << 8) | blue;

        if (colour != calibration_led_colour) {
            calibration_led_colour = colour;
            cyclone2_led_set_home(red, green, blue);
        }
        /* Neither on nor off, so the radio pattern is redrawn afterwards. */
        led_is_on = 2U;
        return;
    }
    calibration_led_colour = 0U;
    if (active_profile >= CYCLONE2_PROFILE_COUNT &&
        now - started_at_ms < CYCLONE2_BOOT_LED_MS) on = 1U;
    else if (radio_state == CYCLONE2_RADIO_CONNECTED) on = 1U;
    else if (radio_state == CYCLONE2_RADIO_CONNECTING ||
             radio_state == CYCLONE2_RADIO_PAIRING) {
        half_period = radio_state == CYCLONE2_RADIO_PAIRING ?
                      CYCLONE2_LED_FAST_HALF_PERIOD_MS :
                      CYCLONE2_LED_SLOW_HALF_PERIOD_MS;
        on = ((now / half_period) & 1U) == 0U;
    } else on = 0U;

    if (on != led_is_on) {
        led_is_on = on;
        cyclone2_led_set_home(on ? CYCLONE2_LED_DIM_RED : 0U, 0U, 0U);
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

    if (active_profile >= CYCLONE2_PROFILE_COUNT ||
        radio_state != CYCLONE2_RADIO_CONNECTED) return;
    report_statistics.send_attempts++;
    result = cyclone2_profile_send(report);
    if (result > 0) {
        report_statistics.send_accepted++;
        /* A synthesized short Home tap is a two-report transaction. Keep the
         * current phase pending until the profile actually accepts it. */
        if (home_tap_phase != 0U) home_tap_phase--;
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

static void cyclone2_poll(void *private_data)
{
    struct cyclone2_input_report sample;
    struct cyclone2_input_report host_report;
    u32 now;

    (void)private_data;
    now = sys_timer_get_ms();
    report_statistics.poll_count++;
    cyclone2_usb_control_poll();
    /* Commit bond-store changes the radio could not write itself.
     *
     * Not from the controller's own callback (that runs inside the SSP
     * exchange), and not while the radio is starting, running or tearing
     * down: a flash write then resets the pad.  Measured three ways - the
     * pairing reaches LINK KEY NOTIFICATION and the chip resets if syscfg is
     * touched there; entering pairing mode resets it if the deferred erases
     * run during the transition; and the same writes on a pad that has been
     * idle for a while are fine.  So wait for a settled idle state. */
    if (radio_state == CYCLONE2_RADIO_OFF) {
        if (idle_since_ms == 0U) idle_since_ms = now;
        else if (now - idle_since_ms >= CYCLONE2_FLASH_IDLE_MS) {
            btstack_jl_link_key_flush();
            cyclone2_settings_flush();
        }
    } else {
        idle_since_ms = 0U;
    }
    /* A new calibration is written at once, connected or not.  The resets
     * that made the bond store wait for an idle radio were measured while
     * pairing, connecting and switching profiles; a settled link takes a
     * syscfg write (the profile and report-period settings do the same).  So
     * only those transitions hold the write back. */
    if (radio_state == CYCLONE2_RADIO_CONNECTED ||
        radio_state == CYCLONE2_RADIO_OFF) {
        cyclone2_calibration_flush();
        settings_flush();
    }
    /* The wake sequence runs the baseband itself: no profile is up, and this
     * is what advances the burst sweep and ends the attempt. */
    if (radio_state == CYCLONE2_RADIO_WAKING) wake_step(now);
    cyclone2_rumble_poll(now);
    cyclone2_input_read(&sample);
    /* A console command may have started injection during this poll. Use a
     * timestamp taken after it, so unsigned elapsed time cannot underflow. */
    now = sys_timer_get_ms();
    accept_analog_input(&sample);
    debounce_digital_input(&sample, now);
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
    update_injection(now);
    /* The ACL connection event precedes authentication and opening the HID
     * control/interrupt channels. Only the latter is a usable connection.
     *
     * Any state except teardown can reach it: a console may connect on its own
     * long after this controller gave up, and the Bluetooth stack stays alive
     * in that case (USB keeps the controller awake), so a reconnect must be
     * picked up rather than ignored. Otherwise the link comes up and carries
     * no reports at all. */
    if (radio_state != CYCLONE2_RADIO_STOPPING &&
        cyclone2_profile_is_connected()) {
        if (radio_state != CYCLONE2_RADIO_CONNECTED) {
            /* First poll that sees the link usable. */
            cyclone2_bringup_record(0x6100u);
        }
        radio_state = CYCLONE2_RADIO_CONNECTED;
        connect_attempts = 0U;
    }
    make_host_report(&host_report);
    if ((injection_status.active || injection_release_pending) &&
        injection_status.synthetic_a)
        host_report.buttons |= CYCLONE2_REPORT_BUTTON_A;
    if ((s32)(now - next_report_ms) >= 0) {
        send_input_report(&host_report);
        /* Advance from the intended deadline rather than from a late callback.
         * This preserves the average 8-ms rate on the 5-ms control clock. */
        do {
            next_report_ms += settings.report_period_ms;
        } while ((s32)(now - next_report_ms) >= 0);
    }
    update_status_led();
    if (sleep_pending) {
        sleep_pending = 0U;
        radio_state = CYCLONE2_RADIO_STOPPING;
        sys_timeout_add(NULL, radio_exit_and_poweroff, 30U);
    }
    if ((radio_state == CYCLONE2_RADIO_CONNECTING &&
         now - radio_start_ms >= CYCLONE2_CONNECT_TIMEOUT_MS) ||
        (radio_state == CYCLONE2_RADIO_PAIRING &&
         now - radio_start_ms >= CYCLONE2_PAIR_TIMEOUT_MS)) {
        if (!cyclone2_usb_present()) {
            begin_automatic_sleep("Bluetooth timeout");
        } else if (radio_state == CYCLONE2_RADIO_CONNECTING &&
                   connect_attempts < CYCLONE2_CONNECT_RETRIES &&
                   cyclone2_profile_reconnect()) {
            /* An attempt that ended with the link up but no HID session leaves
             * the controller stuck; ask the profile for a fresh one instead of
             * idling on a dead connection until the user resets. */
            connect_attempts++;
            cyclone2_usb_debug("Bluetooth connect attempt %u timed out; retrying",
                               connect_attempts);
            radio_start_ms = now;
        } else {
            cyclone2_usb_debug("Bluetooth timeout; staying awake for USB console");
            radio_start_ms = now;
        }
    }
}

static void start_selected_profile(void)
{
    radio_state = active_pairing ? CYCLONE2_RADIO_PAIRING :
                                   CYCLONE2_RADIO_CONNECTING;
    radio_start_ms = sys_timer_get_ms();
    connect_attempts = 0U;
    /* A disconnect reported in the first moments of a session belongs to the
     * session that was just stopped (a profile switch) or to a link the stack
     * is still unwinding - not to this one, which has not connected yet.  Only
     * a session that actually reached CONNECTED may be ended by one. */
    ignore_disconnect_until_ms = sys_timer_get_ms() + CYCLONE2_STALE_EVENT_MS;
    cyclone2_usb_debug("start profile=%u type=%s pairing=%u",
                       active_profile,
                       cyclone2_profile_name(active_type),
                       active_pairing);
    /* Show that selection reached radio startup even if stack init fails. */
    update_status_led();

    cyclone2_profile_start(active_type, active_profile, active_pairing);
    /* The controller library is up from here on, even after this session is
     * stopped, which is what lets the wake sequence take the baseband. */
    controller_ready = 1U;
}

static void cyclone2_start(void)
{
    u32 system_clock;
    u8 early_chord;

    cyclone2_led_initialize();
    cyclone2_rumble_initialize();
    cyclone2_led_set_home(CYCLONE2_LED_DIM_RED, 0U, 0U);
    clk_set("sys", BT_NORMAL_HZ);
    system_clock = clk_get("sys");
    bt_pll_para(TCFG_CLOCK_OSC_HZ, system_clock, 0, 0);
    cyclone2_calibration_load();
    cyclone2_input_initialize();
    task_create(cyclone2_imu_task, NULL, "imu");
    cyclone2_usb_control_initialize();
    cyclone2_input_read(&candidate_report);
    current_report = candidate_report;
    started_at_ms = sys_timer_get_ms();
    memset(button_candidate_since_ms, 0, sizeof(button_candidate_since_ms));
    hat_candidate_since_ms = started_at_ms;
    next_report_ms = started_at_ms;
    settings_load();
    early_chord = cyclone2_take_early_profile_chord();
    if (boot_request_consume()) {
        start_selected_profile();
    } else if (early_chord != 0U) {
        profile_select(early_chord - 1U, 0U);
    }
    report_timer = sys_timer_add(NULL, cyclone2_poll,
                                 CYCLONE2_CONTROL_PERIOD_MS);
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

    (void)application;
    if (event->type != SYS_BT_EVENT ||
        active_profile >= CYCLONE2_PROFILE_COUNT) return 0;

    result = cyclone2_profile_handle_event(event);
    if (result & CYCLONE2_PROFILE_EVENT_CONNECTED)
        radio_state = CYCLONE2_RADIO_CONNECTED;
    if (result & CYCLONE2_PROFILE_EVENT_DISCONNECTED) {
        /* A disconnect that arrives while the session just started is a
         * leftover from the session the profile switch stopped; only a
         * session that has actually connected can be ended this way. */
        if (radio_state != CYCLONE2_RADIO_CONNECTED &&
            (s32)(sys_timer_get_ms() - ignore_disconnect_until_ms) < 0) {
            return 0;
        }
        injection_cancel();
        cyclone2_rumble_stop();
        /* A host player-light command may have changed LEDs without changing
         * led_is_on.  Force every LED off when the radio disconnects. */
        cyclone2_led_set_all(0U, 0U, 0U);
        led_is_on = 0U;
        if (radio_state != CYCLONE2_RADIO_STOPPING) {
            radio_state = CYCLONE2_RADIO_OFF;
            begin_automatic_sleep("Bluetooth disconnected");
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

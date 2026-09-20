#include <system/app_core.h>
#include <system/includes.h>
#include <usb/otg.h>

#include "app_config.h"
#include "controller.h"
#include "main.h"
#include "imu.h"
#include "profile.h"
#include "rumble.h"
#include "settings.h"
#include "user_cfg_id.h"

#define CYCLONE2_CONTROL_PERIOD_MS           5U
#define CYCLONE2_DEFAULT_REPORT_PERIOD_MS   10U
#define CYCLONE2_DEBOUNCE_MS                10U
#define CYCLONE2_PROFILE_COUNT              4U
#define CYCLONE2_SETTINGS_MAGIC     0x43325052U
#define CYCLONE2_SETTINGS_VERSION           1U
#define CYCLONE2_HOME_GRACE_MS              150U
#define CYCLONE2_LONG_HOLD_MS              5000U
#define CYCLONE2_CONNECT_TIMEOUT_MS       30000U
#define CYCLONE2_PAIR_TIMEOUT_MS          60000U
#define CYCLONE2_BOOT_LED_MS                500U
#define CYCLONE2_LED_DIM_RED                 8U
#define CYCLONE2_LED_SLOW_HALF_PERIOD_MS   500U
#define CYCLONE2_LED_FAST_HALF_PERIOD_MS   120U
#define CYCLONE2_INJECT_MIN_PERIOD_MS       20U
#define CYCLONE2_INJECT_MAX_PERIOD_MS       1000U
#define CYCLONE2_INJECT_MAX_DURATION_MS    60000U

#define CYCLONE2_FACE_BUTTONS \
    (CYCLONE2_REPORT_BUTTON_A | CYCLONE2_REPORT_BUTTON_B | \
     CYCLONE2_REPORT_BUTTON_X | CYCLONE2_REPORT_BUTTON_Y)

enum cyclone2_home_state {
    CYCLONE2_HOME_IDLE,
    CYCLONE2_HOME_PENDING,
    CYCLONE2_HOME_REPORTED,
    CYCLONE2_HOME_CHORD,
    CYCLONE2_HOME_INVALID_CHORD,
};

enum cyclone2_radio_state {
    CYCLONE2_RADIO_OFF,
    CYCLONE2_RADIO_CONNECTING,
    CYCLONE2_RADIO_PAIRING,
    CYCLONE2_RADIO_CONNECTED,
    CYCLONE2_RADIO_STOPPING,
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
static u8 home_tap_phase;
static u8 led_is_on;
static u8 sleep_pending;
static u8 ignore_chord_until_release;
static struct cyclone2_report_statistics report_statistics;
static struct cyclone2_injection_status injection_status;
static u32 injection_next_transition_ms;
static u32 injection_target_edges;
static u8 injection_last_emitted;
static u8 injection_release_pending;

extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);
static void start_selected_profile(void);
static void profile_select(u8 profile, u8 pairing);
static void injection_cancel(void);

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

static void settings_load(void)
{
    int length = syscfg_read(CFG_CYCLONE2_PROFILE_SETTINGS, &settings,
                             sizeof(settings));

    if (length == sizeof(settings) &&
        settings.magic == CYCLONE2_SETTINGS_MAGIC &&
        settings.version == CYCLONE2_SETTINGS_VERSION &&
        settings.size == sizeof(settings)) {
        if (settings.report_period_ms == 5U ||
            settings.report_period_ms == 8U ||
            settings.report_period_ms == 10U) return;
        settings.report_period_ms = CYCLONE2_DEFAULT_REPORT_PERIOD_MS;
    } else {
        memset(&settings, 0, sizeof(settings));
        settings.magic = CYCLONE2_SETTINGS_MAGIC;
        settings.version = CYCLONE2_SETTINGS_VERSION;
        settings.size = sizeof(settings);
        settings.profile_type[0] = CYCLONE2_PROFILE_SWITCH;
        settings.profile_type[1] = CYCLONE2_PROFILE_SWITCH;
        settings.profile_type[2] = CYCLONE2_PROFILE_GENERIC;
        settings.profile_type[3] = CYCLONE2_PROFILE_GENERIC;
        settings.report_period_ms = CYCLONE2_DEFAULT_REPORT_PERIOD_MS;
    }
}

u8 cyclone2_report_period_get(void)
{
    return settings.report_period_ms;
}

int cyclone2_report_period_set(u8 period_ms)
{
    if (period_ms != 5U && period_ms != 8U && period_ms != 10U) return -1;
    settings.report_period_ms = period_ms;
    next_report_ms = sys_timer_get_ms();
    syscfg_write(CFG_CYCLONE2_PROFILE_SETTINGS, &settings, sizeof(settings));
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

int cyclone2_disconnect_request(void)
{
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

void cyclone2_connection_status_get(struct cyclone2_connection_status *status)
{
    u8 slot = active_profile < CYCLONE2_PROFILE_COUNT ? active_profile : 0U;
    status->slot = slot;
    status->profile_type = active_profile < CYCLONE2_PROFILE_COUNT ?
                           active_type :
                           (settings.profile_type[slot] == CYCLONE2_PROFILE_GENERIC ?
                            CYCLONE2_PROFILE_GENERIC : CYCLONE2_PROFILE_SWITCH);
    status->connection_state = (u8)radio_state;
    status->usb_present = (u8)cyclone2_usb_present();
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
    active_type = settings.profile_type[profile] == CYCLONE2_PROFILE_GENERIC ?
                  CYCLONE2_PROFILE_GENERIC : CYCLONE2_PROFILE_SWITCH;
    return 1;
}

static void soft_reset_now(void *private_data)
{
    (void)private_data;
    p33_soft_reset();
}

static void profile_request_reset(u8 profile, u8 pairing)
{
    if (radio_state == CYCLONE2_RADIO_STOPPING) {
        return;
    }
    injection_cancel();
    cyclone2_rumble_stop();

    /* Magic is the commit word and is written last. */
    reset_mailbox.magic = 0U;
    reset_mailbox.profile = profile;
    reset_mailbox.pairing = pairing;
    reset_mailbox.check = boot_request_check(profile, pairing);
    reset_mailbox.magic = CYCLONE2_BOOT_REQUEST_MAGIC;
    __asm__ volatile("csync");

    radio_state = CYCLONE2_RADIO_STOPPING;
    cyclone2_profile_disconnect();
    sys_timeout_add(NULL, soft_reset_now, 100U);
}

static void profile_select(u8 profile, u8 pairing)
{
    injection_cancel();
    if (active_profile < CYCLONE2_PROFILE_COUNT) {
        profile_request_reset(profile, pairing);
        return;
    }

    active_profile = profile;
    active_pairing = pairing;
    active_type = settings.profile_type[profile] == CYCLONE2_PROFILE_GENERIC ?
                  CYCLONE2_PROFILE_GENERIC : CYCLONE2_PROFILE_SWITCH;
    home_state = CYCLONE2_HOME_IDLE;
    ignore_chord_until_release = 1U;
    start_selected_profile();
}

static void poweroff_now(void *private_data)
{
    (void)private_data;
    if (cyclone2_usb_present()) {
        radio_state = CYCLONE2_RADIO_OFF;
        cyclone2_usb_debug("sleep cancelled; USB is connected");
        return;
    }
    power_set_soft_poweroff();
}

static void radio_exit_and_poweroff(void *private_data)
{
    (void)private_data;
    cyclone2_rumble_stop();
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
    if (cyclone2_usb_present()) {
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
            if (selected_face_button(buttons, &profile)) {
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
    case CYCLONE2_HOME_INVALID_CHORD:
        if (!home && !(buttons & CYCLONE2_FACE_BUTTONS))
            home_state = CYCLONE2_HOME_IDLE;
        break;
    }
}

static void make_host_report(struct cyclone2_input_report *report)
{
    *report = current_report;
    if (home_state == CYCLONE2_HOME_PENDING ||
        home_state == CYCLONE2_HOME_CHORD ||
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
    cyclone2_rumble_poll(now);
    cyclone2_input_read(&sample);
    /* A console command may have started injection during this poll. Use a
     * timestamp taken after it, so unsigned elapsed time cannot underflow. */
    now = sys_timer_get_ms();
    accept_analog_input(&sample);
    debounce_digital_input(&sample, now);
    update_home_state();
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
        radio_state = CYCLONE2_RADIO_CONNECTED;
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
        if (cyclone2_usb_present()) {
            cyclone2_usb_debug("Bluetooth timeout; staying awake for USB console");
            radio_start_ms = now;
        } else begin_automatic_sleep("Bluetooth timeout");
    }
}

static void start_selected_profile(void)
{
    radio_state = active_pairing ? CYCLONE2_RADIO_PAIRING :
                                   CYCLONE2_RADIO_CONNECTING;
    radio_start_ms = sys_timer_get_ms();
    cyclone2_usb_debug("start profile=%u type=%s pairing=%u",
                       active_profile,
                       cyclone2_profile_name(active_type),
                       active_pairing);
    /* Show that selection reached radio startup even if stack init fails. */
    update_status_led();

    cyclone2_profile_start(active_type, active_profile, active_pairing);
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

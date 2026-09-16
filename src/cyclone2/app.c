#include "system/app_core.h"
#include "system/includes.h"
#include "asm/power_interface.h"
#include "btcontroller_config.h"
#include "btstack/avctp_user.h"
#include "btstack/btstack_task.h"
#include "le_common.h"
#include "le_gatt_common.h"
#include "ble_hogp_profile.h"
#include "edr_hid_user.h"

#include "app_action.h"
#include "app_comm_bt.h"
#include "app_config.h"
#include "board.h"
#include "control.h"
#include "cyclone2/input.h"
#include "cyclone2/reset_mailbox.h"
#include "cyclone2/switch_hid.h"
#include "cyclone2/usb_control.h"
#include "user_cfg_id.h"

#if CONFIG_APP_CYCLONE2

#define CYCLONE2_REPORT_ID                 1U
#define CYCLONE2_REPORT_PERIOD_MS          10U
#define CYCLONE2_CCC_WARMUP_TICKS          10U
#define CYCLONE2_DEBOUNCE_SAMPLES           2U
#define CYCLONE2_PROFILE_COUNT              4U
#define CYCLONE2_SETTINGS_MAGIC     0x43325052U
#define CYCLONE2_SETTINGS_VERSION           1U
#define CYCLONE2_HOME_GRACE_TICKS           15U
#define CYCLONE2_LONG_HOLD_TICKS           500U
#define CYCLONE2_CONNECT_TIMEOUT_TICKS    3000U
#define CYCLONE2_PAIR_TIMEOUT_TICKS       6000U
#define CYCLONE2_BOOT_LED_TICKS              50U
#define CYCLONE2_HID_CONNECT_DELAY_MS        100U
#define CYCLONE2_LED_DIM_RED                 8U
#define CYCLONE2_LED_SLOW_HALF_PERIOD       50U
#define CYCLONE2_LED_FAST_HALF_PERIOD       12U

#define CYCLONE2_FACE_BUTTONS \
    (CYCLONE2_REPORT_BUTTON_A | CYCLONE2_REPORT_BUTTON_B | \
     CYCLONE2_REPORT_BUTTON_X | CYCLONE2_REPORT_BUTTON_Y)

enum cyclone2_profile_type {
    CYCLONE2_PROFILE_SWITCH,
    CYCLONE2_PROFILE_GENERIC,
};

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
    u8 reserved[4];
};

/* Deliberately outside .bss: p33_soft_reset() preserves internal SRAM, so this
 * one-shot record can carry the next profile through reset without wearing
 * flash.  reset_mailbox.ld reserves its fixed address from the heap. */
volatile struct cyclone2_boot_request cyclone2_boot_request
    __attribute__((section(".reset_mailbox"), used, aligned(4)));

/* Report 1: sixteen buttons, a hat, four unsigned stick axes, and two
 * unsigned analog triggers.  The report ID is passed separately to the SDK
 * send API. */
static const u8 cyclone2_hid_report_map[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x05,             /* Usage (Game Pad) */
    0xa1, 0x01,             /* Collection (Application) */
    0x85, CYCLONE2_REPORT_ID,

    0x05, 0x09,             /* Usage Page (Button) */
    0x19, 0x01,             /* Usage Minimum (Button 1) */
    0x29, 0x10,             /* Usage Maximum (Button 16) */
    0x15, 0x00,             /* Logical Minimum (0) */
    0x25, 0x01,             /* Logical Maximum (1) */
    0x75, 0x01,             /* Report Size (1) */
    0x95, 0x10,             /* Report Count (16) */
    0x81, 0x02,             /* Input (Data, Variable, Absolute) */

    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x39,             /* Usage (Hat Switch) */
    0x15, 0x00,             /* Logical Minimum (0) */
    0x25, 0x07,             /* Logical Maximum (7) */
    0x75, 0x04,             /* Report Size (4) */
    0x95, 0x01,             /* Report Count (1) */
    0x81, 0x42,             /* Input (Data, Variable, Absolute, Null) */
    0x75, 0x04,             /* Report Size (4) */
    0x95, 0x01,             /* Report Count (1) */
    0x81, 0x01,             /* Input (Constant) */

    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x15, 0x00,             /* Logical Minimum (0) */
    0x26, 0xff, 0x00,       /* Logical Maximum (255) */
    0x75, 0x08,             /* Report Size (8) */
    0x95, 0x06,             /* Report Count (6) */
    0x09, 0x30,             /* X  (left stick X) */
    0x09, 0x31,             /* Y  (left stick Y) */
    0x09, 0x33,             /* Rx (right stick X) */
    0x09, 0x34,             /* Ry (right stick Y) */
    0x09, 0x32,             /* Z  (left trigger) */
    0x09, 0x35,             /* Rz (right trigger) */
    0x81, 0x02,             /* Input (Data, Variable, Absolute) */
    0xc0,                   /* End Collection */
};

static const ble_init_cfg_t cyclone2_ble_config = {
    .same_address = 0,
    .appearance = BLE_APPEARANCE_HID_GAMEPAD,
    .report_map = cyclone2_hid_report_map,
    .report_map_size = sizeof(cyclone2_hid_report_map),
};

static const u8 cyclone2_pnp_id[] = {
    0x02, 0x37, 0x35, 0x75, 0x05, 0x00, 0x01,
};

static const edr_init_cfg_t cyclone2_edr_config = {
    .class_type = 0x002508U,
    .page_timeout = 8000U,
    .super_timeout = 8000U,
    .io_capabilities = 3,
    .authentication_req = 4,
    .oob_data = 0,
    .passkey_enable = 0,
    .report_map_size = CYCLONE2_SWITCH_REPORT_MAP_SIZE,
    .report_map = cyclone2_switch_report_map,
    .sniff_param = NULL,
};

static struct cyclone2_settings settings;
static struct cyclone2_input_report candidate_report;
static struct cyclone2_input_report current_report;
static struct cyclone2_input_report sent_report;
static enum cyclone2_home_state home_state;
static enum cyclone2_radio_state radio_state;
static enum cyclone2_profile_type active_type;
static u32 control_ticks;
static u32 home_start_tick;
static u32 chord_start_tick;
static u32 radio_start_tick;
static u16 report_timer;
static u16 chord_button;
static u8 active_profile = 0xffU;
static u8 requested_profile;
static u8 active_pairing;
static u8 ccc_ticks;
static u8 debounce_samples;
static u8 report_sent;
static u8 home_tap_phase;
static u8 led_is_on;
static u8 sleep_pending;
static u8 edr_bond_delete_pending;
static u8 ignore_chord_until_release;
static u8 edr_bond_to_delete[6];
static u8 edr_reconnect_address[6];
static u8 edr_hid_was_connected;

extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);
extern const u8 *bt_get_mac_addr(void);
extern void bt_set_mac_addr(u8 *address);
extern void le_hogp_set_PNP_info(const u8 *information);
extern void le_hogp_set_pair_allow(void);
extern void le_hogp_disconnect(void);
extern void set_multi_devices_adv_flag(u8 enable);
extern void ble_module_enable(u8 enable);
extern int ble_hid_is_connected(void);
extern int ble_hid_data_send(u8 report_id, u8 *data, u16 length);
extern void bt_set_local_name(char *name, u8 len);
extern void delete_link_key(bd_addr_t *address, u8 index);

static void start_selected_profile(void);

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
        return;
    }
    memset(&settings, 0, sizeof(settings));
    settings.magic = CYCLONE2_SETTINGS_MAGIC;
    settings.version = CYCLONE2_SETTINGS_VERSION;
    settings.size = sizeof(settings);
    settings.profile_type[0] = CYCLONE2_PROFILE_SWITCH;
    settings.profile_type[1] = CYCLONE2_PROFILE_SWITCH;
    settings.profile_type[2] = CYCLONE2_PROFILE_GENERIC;
    settings.profile_type[3] = CYCLONE2_PROFILE_GENERIC;
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

    magic = cyclone2_boot_request.magic;
    profile = cyclone2_boot_request.profile;
    pairing = cyclone2_boot_request.pairing;
    check = cyclone2_boot_request.check;

    if (magic != CYCLONE2_BOOT_REQUEST_MAGIC) return 0;

    /* A profile record is one-shot.  Leave diagnostic records intact for the
     * USB status command after a failed recovery handoff. */
    cyclone2_boot_request.magic = 0U;
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

    /* Magic is the commit word and is written last. */
    cyclone2_boot_request.magic = 0U;
    cyclone2_boot_request.profile = profile;
    cyclone2_boot_request.pairing = pairing;
    cyclone2_boot_request.check = boot_request_check(profile, pairing);
    cyclone2_boot_request.magic = CYCLONE2_BOOT_REQUEST_MAGIC;
    __asm__ volatile("csync");

    radio_state = CYCLONE2_RADIO_STOPPING;
    if (active_type == CYCLONE2_PROFILE_SWITCH) user_hid_disconnect();
    else le_hogp_disconnect();
    sys_timeout_add(NULL, soft_reset_now, 100U);
}

static void profile_select(u8 profile, u8 pairing)
{
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
    power_set_soft_poweroff();
}

static void radio_exit_and_poweroff(void *private_data)
{
    (void)private_data;
    if (active_profile < CYCLONE2_PROFILE_COUNT) {
        if (active_type == CYCLONE2_PROFILE_SWITCH) btstack_edr_exit(0);
        else btstack_ble_exit(0);
    }
    cyclone2_led_set_home(0U, 0U, 0U);
    sys_timeout_add(NULL, poweroff_now, 300U);
}

static void begin_sleep(void)
{
    if (radio_state != CYCLONE2_RADIO_STOPPING) sleep_pending = 1U;
}

static void begin_automatic_sleep(const char *reason)
{
    if (gpio_read(CYCLONE2_USB_SENSE)) {
        cyclone2_usb_debug("%s; staying awake because USB is connected", reason);
        return;
    }
    begin_sleep();
}

static void edr_reconnect_saved(void *private_data)
{
    (void)private_data;
    if (active_type != CYCLONE2_PROFILE_SWITCH ||
        radio_state != CYCLONE2_RADIO_CONNECTING ||
        edr_hid_is_connected()) return;

    bt_wait_phone_connect_control(0);
    user_send_cmd_prepare(USER_CTRL_START_CONNEC_VIA_ADDR,
                          sizeof(edr_reconnect_address),
                          edr_reconnect_address);
}

static void edr_hid_connect_after_encryption(void *private_data)
{
    int result;

    (void)private_data;
    if (active_type != CYCLONE2_PROFILE_SWITCH ||
        radio_state == CYCLONE2_RADIO_STOPPING || edr_hid_is_connected())
        return;

    result = user_send_cmd_prepare(USER_CTRL_HID_CONN, 0, NULL);
    cyclone2_usb_debug("request HID channels after encryption result=%d", result);
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
            home_start_tick = control_ticks;
            if (selected_face_button(buttons, &profile)) {
                chord_button = buttons & CYCLONE2_FACE_BUTTONS;
                chord_start_tick = control_ticks;
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
            chord_start_tick = control_ticks;
            requested_profile = profile;
            home_state = CYCLONE2_HOME_CHORD;
        } else if (buttons & CYCLONE2_FACE_BUTTONS) {
            home_state = CYCLONE2_HOME_INVALID_CHORD;
        } else if (control_ticks - home_start_tick >= CYCLONE2_HOME_GRACE_TICKS) {
            home_state = CYCLONE2_HOME_REPORTED;
        }
        break;
    case CYCLONE2_HOME_REPORTED:
        if (!home) home_state = CYCLONE2_HOME_IDLE;
        else if (control_ticks - home_start_tick >= CYCLONE2_LONG_HOLD_TICKS)
            begin_sleep();
        break;
    case CYCLONE2_HOME_CHORD:
        if (!home || (buttons & chord_button) == 0U) {
            profile_select(requested_profile, 0U);
        } else if ((buttons & CYCLONE2_FACE_BUTTONS) != chord_button) {
            home_state = CYCLONE2_HOME_INVALID_CHORD;
        } else if (control_ticks - chord_start_tick >= CYCLONE2_LONG_HOLD_TICKS) {
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
        home_tap_phase = 1U;
    } else if (home_tap_phase == 1U) {
        report->buttons &= ~CYCLONE2_REPORT_BUTTON_HOME;
        home_tap_phase = 0U;
    }
}

static void update_status_led(void)
{
    u32 half_period;
    u8 on;

    if (active_profile >= CYCLONE2_PROFILE_COUNT &&
        control_ticks < CYCLONE2_BOOT_LED_TICKS) on = 1U;
    else if (radio_state == CYCLONE2_RADIO_CONNECTED) on = 1U;
    else if (radio_state == CYCLONE2_RADIO_CONNECTING ||
             radio_state == CYCLONE2_RADIO_PAIRING) {
        half_period = radio_state == CYCLONE2_RADIO_PAIRING ?
                      CYCLONE2_LED_FAST_HALF_PERIOD :
                      CYCLONE2_LED_SLOW_HALF_PERIOD;
        on = ((control_ticks / half_period) & 1U) == 0U;
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
    u16 connection_handle;

    if (active_profile >= CYCLONE2_PROFILE_COUNT ||
        radio_state == CYCLONE2_RADIO_STOPPING) return;
    if (active_type == CYCLONE2_PROFILE_SWITCH) {
        cyclone2_switch_hid_send(report);
        return;
    }
    connection_handle = ble_hid_is_connected();
    if (!connection_handle ||
        !ble_gatt_server_characteristic_ccc_get(
            connection_handle,
            ATT_CHARACTERISTIC_2a4d_01_CLIENT_CONFIGURATION_HANDLE)) {
        ccc_ticks = 0U;
        report_sent = 0U;
        return;
    }
    if (ccc_ticks < CYCLONE2_CCC_WARMUP_TICKS) {
        ccc_ticks++;
        return;
    }
    if (report_sent && !memcmp(report, &sent_report, sizeof(*report))) return;
    if (ble_hid_data_send(CYCLONE2_REPORT_ID, (u8 *)report,
                          sizeof(*report)) >= 0) {
        sent_report = *report;
        report_sent = 1U;
    }
}

static void cyclone2_poll(void *private_data)
{
    struct cyclone2_input_report sample;
    struct cyclone2_input_report host_report;

    (void)private_data;
    control_ticks++;
    cyclone2_usb_control_poll();
    cyclone2_input_read(&sample);
    accept_analog_input(&sample);
    if (sample.buttons != candidate_report.buttons ||
        sample.hat != candidate_report.hat) {
        candidate_report.buttons = sample.buttons;
        candidate_report.hat = sample.hat;
        debounce_samples = 1U;
    } else if (debounce_samples < CYCLONE2_DEBOUNCE_SAMPLES) {
        debounce_samples++;
        if (debounce_samples == CYCLONE2_DEBOUNCE_SAMPLES) {
            current_report.buttons = candidate_report.buttons;
            current_report.hat = candidate_report.hat;
        }
    }
    update_home_state();
    /* The ACL connection event precedes authentication and opening the HID
     * control/interrupt channels. Only the latter is a usable connection. */
    if (active_type == CYCLONE2_PROFILE_SWITCH &&
        (radio_state == CYCLONE2_RADIO_CONNECTING ||
         radio_state == CYCLONE2_RADIO_PAIRING) &&
        edr_hid_is_connected()) {
        radio_state = CYCLONE2_RADIO_CONNECTED;
        cyclone2_usb_debug("EDR HID channels connected");
    }
    if (active_type == CYCLONE2_PROFILE_SWITCH) {
        u8 connected = edr_hid_is_connected() != 0;
        if (connected != edr_hid_was_connected) {
            cyclone2_usb_debug("EDR HID state=%u", connected);
            edr_hid_was_connected = connected;
        }
    }
    make_host_report(&host_report);
    send_input_report(&host_report);
    update_status_led();
    if (sleep_pending) {
        sleep_pending = 0U;
        radio_state = CYCLONE2_RADIO_STOPPING;
        sys_timeout_add(NULL, radio_exit_and_poweroff, 30U);
    }
    if ((radio_state == CYCLONE2_RADIO_CONNECTING &&
         control_ticks - radio_start_tick >= CYCLONE2_CONNECT_TIMEOUT_TICKS) ||
        (radio_state == CYCLONE2_RADIO_PAIRING &&
         control_ticks - radio_start_tick >= CYCLONE2_PAIR_TIMEOUT_TICKS)) {
        if (gpio_read(CYCLONE2_USB_SENSE)) {
            cyclone2_usb_debug("Bluetooth timeout; staying awake for USB console");
            radio_start_tick = control_ticks;
        } else begin_automatic_sleep("Bluetooth timeout");
    }
}

static void prepare_profile_address(void)
{
    u8 address[6];

    memcpy(address, bt_get_mac_addr(), sizeof(address));
    address[0] ^= (u8)(active_profile * 4U);
    bt_set_mac_addr(address);
}

static void clear_profile_bond(void)
{
    u8 remote[6];

    if (active_type == CYCLONE2_PROFILE_SWITCH) {
        if (syscfg_read(CFG_EDR_ADDRESS_BEGIN + active_profile,
                        remote, sizeof(remote)) == sizeof(remote)) {
            memcpy(edr_bond_to_delete, remote, sizeof(remote));
            edr_bond_delete_pending = 1U;
        }
        memset(remote, 0, sizeof(remote));
        syscfg_write(CFG_EDR_ADDRESS_BEGIN + active_profile,
                     remote, sizeof(remote));
    }
}

static void start_selected_profile(void)
{
    prepare_profile_address();
    set_multi_devices_adv_flag(1U);
    if (active_pairing) clear_profile_bond();
    radio_state = active_pairing ? CYCLONE2_RADIO_PAIRING :
                                   CYCLONE2_RADIO_CONNECTING;
    radio_start_tick = control_ticks;
    edr_hid_was_connected = 0U;
    cyclone2_usb_debug("start profile=%u type=%s pairing=%u",
                       active_profile,
                       active_type == CYCLONE2_PROFILE_SWITCH ? "switch" : "generic",
                       active_pairing);
    /* Show that selection reached radio startup even if stack init fails. */
    update_status_led();

    /* The linked controller/stack configuration contains both Classic and
     * LE. Its common initialization expects the LE GATT tables to be prepared
     * even for a Classic-only session, as in the SDK's dual-mode examples.
     * We intentionally do not run the BLE after-init handler for a Switch
     * slot, so this does not start BLE advertising. */
    le_hogp_set_PNP_info(cyclone2_pnp_id);
    btstack_ble_start_before_init(&cyclone2_ble_config, 0);

    if (active_type == CYCLONE2_PROFILE_SWITCH) {
        bt_set_local_name("Pro Controller", sizeof("Pro Controller") - 1U);
        btstack_edr_start_before_init(&cyclone2_edr_config, 0);
        cyclone2_switch_hid_initialize();
    }
    btstack_init();
}

static void cyclone2_start(void)
{
    u32 system_clock;

    cyclone2_led_initialize();
    cyclone2_led_set_home(CYCLONE2_LED_DIM_RED, 0U, 0U);
    clk_set("sys", BT_NORMAL_HZ);
    system_clock = clk_get("sys");
    bt_pll_para(TCFG_CLOCK_OSC_HZ, system_clock, 0, 0);
    cyclone2_input_initialize();
    cyclone2_usb_control_initialize();
    cyclone2_input_read(&candidate_report);
    current_report = candidate_report;
    debounce_samples = CYCLONE2_DEBOUNCE_SAMPLES;
    settings_load();
    if (boot_request_consume()) start_selected_profile();
    report_timer = sys_timer_add(NULL, cyclone2_poll,
                                 CYCLONE2_REPORT_PERIOD_MS);
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

static int cyclone2_connection_event(struct bt_event *event)
{
    static const u8 zero_address[6];
    u8 remote[6];

    if (active_type == CYCLONE2_PROFILE_SWITCH) {
        cyclone2_usb_debug("EDR status event=%u value=%08x addr=%02x:%02x:%02x:%02x:%02x:%02x",
                           event->event, event->value,
                           event->args[0], event->args[1], event->args[2],
                           event->args[3], event->args[4], event->args[5]);
        bt_comm_edr_status_event_handler(event);
        if (event->event == BT_STATUS_INIT_OK) {
            if (active_pairing) {
                if (edr_bond_delete_pending) {
                    delete_link_key(edr_bond_to_delete,
                                    get_remote_dev_info_index());
                    edr_bond_delete_pending = 0U;
                }
                bt_wait_phone_connect_control(1);
            }
            else if (syscfg_read(CFG_EDR_ADDRESS_BEGIN + active_profile,
                                 remote, sizeof(remote)) == sizeof(remote) &&
                     memcmp(remote, zero_address, sizeof(remote))) {
                memcpy(edr_reconnect_address, remote, sizeof(remote));
                /* Let the init event unwind before asking the controller to
                 * page the saved host. */
                sys_timeout_add(NULL, edr_reconnect_saved, 100U);
            } else if (!bt_connect_phone_back_start()) {
                bt_wait_phone_connect_control(1);
            }
        } else if (event->event == BT_STATUS_FIRST_CONNECTED ||
                   event->event == BT_STATUS_SECOND_CONNECTED) {
            syscfg_write(CFG_EDR_ADDRESS_BEGIN + active_profile,
                         event->args, 6U);
        } else if (event->event == BT_STATUS_ENCRY_COMPLETE) {
            /* START_CONNEC_VIA_ADDR establishes and authenticates the ACL.
             * On BlueZ the host does not reliably initiate the HID L2CAP
             * channels afterward, so request them once encryption settles. */
            sys_timeout_add(NULL, edr_hid_connect_after_encryption,
                            CYCLONE2_HID_CONNECT_DELAY_MS);
        } else if (event->event == BT_STATUS_FIRST_DISCONNECT ||
                   event->event == BT_STATUS_SECOND_DISCONNECT) {
            if (radio_state == CYCLONE2_RADIO_CONNECTED)
                begin_automatic_sleep("EDR disconnected");
        }
    } else {
        bt_comm_ble_status_event_handler(event);
        if (event->event == BT_STATUS_INIT_OK && active_pairing) {
            ble_module_enable(0);
            le_hogp_set_pair_allow();
            ble_module_enable(1);
        }
    }
    return 0;
}

static void cyclone2_ble_status_event(struct bt_event *event)
{
    if (active_type != CYCLONE2_PROFILE_GENERIC) return;
    if (event->event == BLE_ST_CONNECT) radio_state = CYCLONE2_RADIO_CONNECTED;
    else if (event->event == BLE_ST_DISCONN &&
             radio_state == CYCLONE2_RADIO_CONNECTED)
        begin_automatic_sleep("BLE disconnected");
}

static int cyclone2_event_handler(struct application *application,
                                  struct sys_event *event)
{
    (void)application;
    if (event->type != SYS_BT_EVENT ||
        active_profile >= CYCLONE2_PROFILE_COUNT) return 0;
    if ((u32)event->arg == SYS_BT_EVENT_TYPE_CON_STATUS)
        return cyclone2_connection_event(&event->u.bt);
    if ((u32)event->arg == SYS_BT_EVENT_TYPE_HCI_STATUS) {
        if (active_type == CYCLONE2_PROFILE_SWITCH) {
            int result = bt_comm_edr_hci_event_handler(&event->u.bt);
            cyclone2_usb_debug("EDR HCI event=%02x value=%08x",
                               event->u.bt.event, event->u.bt.value);
            if (event->u.bt.event == HCI_EVENT_DISCONNECTION_COMPLETE) {
                if (radio_state == CYCLONE2_RADIO_CONNECTED)
                    begin_automatic_sleep("EDR HCI disconnect");
            }
            return result;
        }
        return bt_comm_ble_hci_event_handler(&event->u.bt);
    }
    if ((u32)event->arg == SYS_BT_EVENT_BLE_STATUS)
        cyclone2_ble_status_event(&event->u.bt);
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

#endif

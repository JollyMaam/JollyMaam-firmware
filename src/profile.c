#include <system/includes.h>
#include <btstack/btstack_task.h>
#include <btstack/avctp_user.h>

#include "profile.h"
#include "profile_classic.h"
#include "settings.h"
#include "btstack_jl_le.h"

struct profile_operations {
    const char *name;
    void (*prepare)(void);
    void (*start)(u8 slot, u8 pairing);
    void (*disconnect)(void);
    int (*exit)(void);
    int (*is_connected)(void);
    int (*reconnect)(void);
    int (*send)(const struct cyclone2_input_report *report);
    enum cyclone2_profile_event (*handle_event)(const struct sys_event *event);
};

static const struct profile_operations profiles[] = {
    [CYCLONE2_PROFILE_SWITCH] = {
        .name = "switch",
        .prepare = cyclone2_generic_prepare_stack,
        .start = cyclone2_switch_start,
        .disconnect = cyclone2_classic_disconnect,
        .exit = cyclone2_classic_exit,
        .is_connected = cyclone2_classic_is_connected,
        .reconnect = cyclone2_classic_reconnect,
        .send = cyclone2_switch_send,
        .handle_event = cyclone2_classic_handle_event,
    },
    [CYCLONE2_PROFILE_GENERIC] = {
        .name = "generic",
        .prepare = cyclone2_generic_prepare_stack,
        .start = cyclone2_generic_start,
        .disconnect = cyclone2_generic_disconnect,
        .exit = cyclone2_generic_exit,
        .is_connected = cyclone2_generic_is_connected,
        .reconnect = cyclone2_generic_reconnect,
        .send = cyclone2_generic_send,
        .handle_event = cyclone2_generic_handle_event,
    },
    [CYCLONE2_PROFILE_SWITCH2] = {
        .name = "switch2",
        .prepare = cyclone2_switch2_prepare_stack,
        .start = cyclone2_switch2_start,
        .disconnect = cyclone2_switch2_disconnect,
        .exit = cyclone2_switch2_exit,
        .is_connected = cyclone2_switch2_is_connected,
        .reconnect = cyclone2_switch2_reconnect,
        .send = cyclone2_switch2_send,
        .handle_event = cyclone2_switch2_handle_event,
    },
    [CYCLONE2_PROFILE_XBOX] = {
        .name = "xbox",
        .prepare = cyclone2_generic_prepare_stack,
        .start = cyclone2_xbox_start,
        .disconnect = cyclone2_classic_disconnect,
        .exit = cyclone2_classic_exit,
        .is_connected = cyclone2_classic_is_connected,
        .reconnect = cyclone2_classic_reconnect,
        .send = cyclone2_xbox_send,
        .handle_event = cyclone2_classic_handle_event,
    },
    [CYCLONE2_PROFILE_DUALSENSE] = {
        .name = "dualsense",
        .prepare = cyclone2_generic_prepare_stack,
        .start = cyclone2_dualsense_start,
        .disconnect = cyclone2_classic_disconnect,
        .exit = cyclone2_classic_exit,
        .is_connected = cyclone2_classic_is_connected,
        .reconnect = cyclone2_classic_reconnect,
        .send = cyclone2_dualsense_send,
        .handle_event = cyclone2_classic_handle_event,
    },
    [CYCLONE2_PROFILE_XBOX_BLE] = {
        .name = "xboxble",
        .prepare = cyclone2_xbox_ble_prepare_stack,
        .start = cyclone2_xbox_ble_start,
        .disconnect = cyclone2_xbox_ble_disconnect,
        .exit = cyclone2_xbox_ble_exit,
        .is_connected = cyclone2_xbox_ble_is_connected,
        .reconnect = cyclone2_xbox_ble_reconnect,
        .send = cyclone2_xbox_ble_send,
        .handle_event = cyclone2_xbox_ble_handle_event,
    },
};

static const struct profile_operations *active_profile =
    &profiles[CYCLONE2_PROFILE_SWITCH];

void cyclone2_profile_start(enum cyclone2_profile_type type, u8 slot,
                            u8 pairing)
{
    u8 address[6];

    if ((unsigned)type >= CYCLONE2_PROFILE_TYPE_COUNT)
        type = CYCLONE2_PROFILE_SWITCH;
    active_profile = &profiles[type];
    cyclone2_bt_address_for_slot(slot, address);
    bt_set_mac_addr(address);
    set_multi_devices_adv_flag(1U);

    /* Common btstack_init() expects LE's GATT tables even for a Classic-only
     * application. The selected profile chooses either HOGP or the Switch 2
     * raw GATT database here. */
    /* Only a profile that wants the LE GATT server installs one. */
    btstack_jl_le_configure(NULL);
    active_profile->prepare();
    active_profile->start(slot, pairing);
    btstack_init();
}

void cyclone2_profile_disconnect(void)
{
    active_profile->disconnect();
}

int cyclone2_profile_exit(void)
{
    return active_profile->exit();
}

int cyclone2_profile_is_connected(void)
{
    return active_profile->is_connected();
}

/* Ask the profile to start a fresh outgoing connection attempt after one that
 * ended without a usable session.  Returns non-zero when it took over. */
int cyclone2_profile_reconnect(void)
{
    if (!active_profile->reconnect) return 0;
    return active_profile->reconnect();
}

int cyclone2_profile_send(const struct cyclone2_input_report *report)
{
    return active_profile->send(report);
}

enum cyclone2_profile_event cyclone2_profile_handle_event(
    const struct sys_event *event)
{
    return active_profile->handle_event(event);
}

const char *cyclone2_profile_name(enum cyclone2_profile_type type)
{
    if ((unsigned)type >= CYCLONE2_PROFILE_TYPE_COUNT) return "invalid";
    return profiles[type].name;
}

#include <system/includes.h>
#include <btstack/btstack_task.h>
#include <btstack/avctp_user.h>

#include "profile.h"
#include "profile_switch_wake.h"
#include "profile_classic.h"
#include "config.h"
#include "btstack_jl_le.h"

struct profile_operations {
    const char *name;
    /* profile.#.report_hz=auto: the rate the real controller uses with its
     * console, so an unconfigured slot behaves like the real thing. */
    u16 default_hz;
    int (*wake_check)(u8 slot, u8 channel, u8 seconds);
    int (*wake_start)(u8 slot, u8 channel, u8 seconds);
    void (*wake_stop)(void);
    int (*wake_step)(u32 now_ms);
    void (*prepare)(void);
    void (*start)(u8 slot, u8 pairing);
    void (*disconnect)(void);
    int (*exit)(void);
    int (*is_connected)(void);
    int (*reconnect)(void);
    int (*send)(const struct cyclone2_input_report *report);
    enum cyclone2_profile_event (*handle_event)(const struct sys_event *event);
};

static int switch_wake_check(u8 slot, u8 channel, u8 seconds)
{
    static const u8 zero[6];
    if (channel > WAKE_CHANNEL_MAX || seconds < WAKE_SECONDS_MIN ||
        seconds > WAKE_SECONDS_MAX) return -2;
    return memcmp(config_peer(slot), zero, sizeof(zero)) ? 0 : -3;
}

static const struct profile_operations profiles[] = {
    [CYCLONE2_PROFILE_SWITCH] = {
        .name = "switch",
        .default_hz = 66U,  /* Pro Controller full input mode: 15 ms */
        .wake_check = switch_wake_check,
        .wake_start = wake_start,
        .wake_stop = wake_stop,
        .wake_step = wake_step,
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
        .default_hz = 125U,
        .prepare = cyclone2_generic_prepare_stack,
        .start = cyclone2_generic_start,
        .disconnect = cyclone2_generic_disconnect,
        .exit = cyclone2_generic_exit,
        .is_connected = cyclone2_generic_is_connected,
        .send = cyclone2_generic_send,
        .handle_event = cyclone2_generic_handle_event,
    },
    [CYCLONE2_PROFILE_SWITCH2] = {
        .name = "switch2",
        .default_hz = 125U,
        .wake_check = cyclone2_switch2_wake_check,
        .prepare = cyclone2_switch2_prepare_stack,
        .start = cyclone2_switch2_start,
        .disconnect = btstack_jl_le_disconnect,
        .exit = cyclone2_profile_le_exit,
        .is_connected = btstack_jl_le_connected,
        .send = cyclone2_switch2_send,
        .handle_event = cyclone2_profile_le_handle_event,
    },
    [CYCLONE2_PROFILE_XBOX] = {
        .name = "xbox",
        .default_hz = 125U, /* assumed; not yet measured on a real pad */
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
        /* Measured (2026-09-30): at 250 the reports and BTstack left the
         * lowest-priority imu task 20 samples a second, so most reports
         * repeated a stale gyro sample; at 125 it keeps up (240 a second). */
        .default_hz = 125U,
        .start = cyclone2_dualsense_start,
        .disconnect = cyclone2_classic_disconnect,
        .exit = cyclone2_classic_exit,
        .is_connected = cyclone2_classic_is_connected,
        .reconnect = cyclone2_classic_reconnect,
        .send = cyclone2_dualsense_send,
        .handle_event = cyclone2_classic_handle_event,
    },
    [CYCLONE2_PROFILE_DUALSHOCK4] = {
        .name = "dualshock4",
        .default_hz = 125U, /* as dualsense: the gyro needs the CPU */
        .start = cyclone2_dualshock4_start,
        .disconnect = cyclone2_classic_disconnect,
        .exit = cyclone2_classic_exit,
        .is_connected = cyclone2_classic_is_connected,
        .reconnect = cyclone2_classic_reconnect,
        .send = cyclone2_dualshock4_send,
        .handle_event = cyclone2_classic_handle_event,
    },
    [CYCLONE2_PROFILE_XBOX_BLE] = {
        .name = "xboxble",
        .default_hz = 125U, /* assumed; not yet measured on a real pad */
        .prepare = cyclone2_xbox_ble_prepare_stack,
        .start = cyclone2_xbox_ble_start,
        .disconnect = btstack_jl_le_disconnect,
        .exit = cyclone2_profile_le_exit,
        .is_connected = btstack_jl_le_connected,
        .send = cyclone2_xbox_ble_send,
        .handle_event = cyclone2_profile_le_handle_event,
    },
    [CYCLONE2_PROFILE_STICKMOUSE] = {
        .name = "stickmouse",
        /* Motion goes out as fast as the link takes it, and only while
         * something moves; 500 is the most asked of it. */
        .default_hz = 500U,
        .start = cyclone2_stickmouse_start,
        .disconnect = cyclone2_classic_disconnect,
        .exit = cyclone2_classic_exit,
        .is_connected = cyclone2_classic_is_connected,
        .reconnect = cyclone2_classic_reconnect,
        .send = cyclone2_mouse_send,
        .handle_event = cyclone2_classic_handle_event,
    },
    [CYCLONE2_PROFILE_GYROMOUSE] = {
        .name = "gyromouse",
        .default_hz = 500U,
        .start = cyclone2_gyromouse_start,
        .disconnect = cyclone2_classic_disconnect,
        .exit = cyclone2_classic_exit,
        .is_connected = cyclone2_classic_is_connected,
        .reconnect = cyclone2_classic_reconnect,
        .send = cyclone2_mouse_send,
        .handle_event = cyclone2_classic_handle_event,
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

    if (pairing && slot < CONFIG_PROFILES) config_bond_forget_keys(slot);
    config_slot_address(slot, address);
    bt_set_mac_addr(address);
    set_multi_devices_adv_flag(1U);

    btstack_jl_le_configure(NULL);
    if (active_profile->prepare) active_profile->prepare();
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

u16 cyclone2_profile_default_hz(enum cyclone2_profile_type type)
{
    if ((unsigned)type >= CYCLONE2_PROFILE_TYPE_COUNT) return 125U;
    return profiles[type].default_hz;
}

const char *cyclone2_profile_name(enum cyclone2_profile_type type)
{
    if ((unsigned)type >= CYCLONE2_PROFILE_TYPE_COUNT) return "invalid";
    return profiles[type].name;
}

static const struct profile_operations *waking_profile;

int cyclone2_profile_wake_check(enum cyclone2_profile_type type, u8 slot,
                               u8 channel, u8 seconds)
{
    if ((unsigned)type >= CYCLONE2_PROFILE_TYPE_COUNT || slot >= CONFIG_PROFILES)
        return -2;
    if (!profiles[type].wake_check) return -4;
    return profiles[type].wake_check(slot, channel, seconds);
}

int cyclone2_profile_wake_start(enum cyclone2_profile_type type, u8 slot,
                               u8 channel, u8 seconds)
{
    int result = cyclone2_profile_wake_check(type, slot, channel, seconds);
    if (result) return result;
    /* Advertising profiles wake as part of their normal reconnect start. */
    if (!profiles[type].wake_start) return 1;
    waking_profile = &profiles[type];
    return waking_profile->wake_start(slot, channel, seconds);
}

void cyclone2_profile_wake_stop(void)
{
    if (waking_profile) waking_profile->wake_stop();
    waking_profile = NULL;
}

int cyclone2_profile_wake_step(u32 now_ms)
{
    return !waking_profile || waking_profile->wake_step(now_ms);
}

#include "system/includes.h"
#include "btstack/btstack_task.h"
#include "btstack/avctp_user.h"

#include "profile.h"

struct profile_operations {
    const char *name;
    void (*start)(u8 slot, u8 pairing);
    void (*disconnect)(void);
    void (*exit)(void);
    int (*is_connected)(void);
    int (*send)(const struct cyclone2_input_report *report);
    enum cyclone2_profile_event (*handle_event)(const struct sys_event *event);
};

static const struct profile_operations profiles[] = {
    [CYCLONE2_PROFILE_SWITCH] = {
        "switch", cyclone2_switch_start, cyclone2_switch_disconnect,
        cyclone2_switch_exit, cyclone2_switch_is_connected,
        cyclone2_switch_send, cyclone2_switch_handle_event,
    },
    [CYCLONE2_PROFILE_GENERIC] = {
        "generic", cyclone2_generic_start, cyclone2_generic_disconnect,
        cyclone2_generic_exit, cyclone2_generic_is_connected,
        cyclone2_generic_send, cyclone2_generic_handle_event,
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
    memcpy(address, bt_get_mac_addr(), sizeof(address));
    address[0] ^= (u8)(slot * 4U);
    bt_set_mac_addr(address);
    set_multi_devices_adv_flag(1U);

    /* Common btstack_init() expects LE's GATT tables even for a Classic-only
     * application. Keep this SDK implementation detail out of main.c. */
    cyclone2_generic_prepare_stack();
    active_profile->start(slot, pairing);
    btstack_init();
}

void cyclone2_profile_disconnect(void)
{
    active_profile->disconnect();
}

void cyclone2_profile_exit(void)
{
    active_profile->exit();
}

int cyclone2_profile_is_connected(void)
{
    return active_profile->is_connected();
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

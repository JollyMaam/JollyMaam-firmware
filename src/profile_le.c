#include <system/includes.h>
#include <btstack/btstack_task.h>
#include <btstack/third_party/common/ble_user.h>

#include "profile.h"
#include "btstack_jl_le.h"
#include "rumble.h"

int cyclone2_profile_le_exit(void)
{
    cyclone2_rumble_stop();
    btstack_jl_le_set_scan_response(NULL, 0);
    btstack_jl_le_set_advertising(NULL, 0);
    /* Shutdown must finish before the loader or another profile takes the radio. */
    return btstack_edr_exit(0);
}

enum cyclone2_profile_event cyclone2_profile_le_handle_event(
    const struct sys_event *event)
{
    if ((u32)event->arg != SYS_BT_EVENT_BLE_STATUS)
        return CYCLONE2_PROFILE_EVENT_NONE;
    if (event->u.bt.event == BLE_ST_CONNECT)
        return CYCLONE2_PROFILE_EVENT_CONNECTED;
    if (event->u.bt.event == BLE_ST_DISCONN)
        return CYCLONE2_PROFILE_EVENT_DISCONNECTED;
    return CYCLONE2_PROFILE_EVENT_NONE;
}

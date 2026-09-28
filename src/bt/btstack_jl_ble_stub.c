/* Generic HOGP still uses the removed vendor host API. Switch 2 and Xbox BLE
 * use btstack_jl_le.c; these compatibility stubs do not implement HOGP. */

#include <system/includes.h>

#include "app_comm_bt.h"
#include "le_common.h"
#include "le_gatt_common.h"

#include "config.h"
#include "usb.h"

#define BTSTACK_JL_BLE_LOG(...) \
    do { if (config.log.btstack) \
        cyclone2_usb_debug("BTSTACK BLE unavailable: " __VA_ARGS__); } while (0)

void btstack_ble_start_before_init(const ble_init_cfg_t *cfg, int param)
{
    (void) cfg;
    (void) param;
    BTSTACK_JL_BLE_LOG("profile not ported");
}

void btstack_ble_exit(int param)
{
    (void) param;
}

int bt_comm_ble_status_event_handler(struct bt_event *bt)
{
    (void) bt;
    return 0;
}

int bt_comm_ble_hci_event_handler(struct bt_event *bt)
{
    (void) bt;
    return 0;
}

void le_hogp_set_PNP_info(const u8 *information)
{
    (void) information;
}

void le_hogp_set_pair_allow(void)
{
}

void le_hogp_disconnect(void)
{
}

void ble_module_enable(u8 enable)
{
    (void) enable;
    BTSTACK_JL_BLE_LOG("profile not ported");
}

int ble_hid_is_connected(void)
{
    return 0;
}

int ble_hid_data_send(u8 report_id, u8 *data, u16 length)
{
    (void) report_id;
    (void) data;
    (void) length;
    return APP_BLE_IS_DISCONN;
}

u16 ble_gatt_server_characteristic_ccc_get(u16 conn_handle, u16 att_ccc_handle)
{
    (void) conn_handle;
    (void) att_ccc_handle;
    return 0;
}

/*
 * BLE profile placeholders for the BTstack port.
 *
 * The port currently covers the Bluetooth Classic side of the firmware: the
 * application's Nintendo Switch profile runs on upstream BTstack with the
 * Jieli BR23 controller (see btstack_jl_core.c).  The two BLE profiles
 * (generic HOGP gamepad and Nintendo Switch 2 GATT controller) were built on
 * the vendor host library's LE stack, which this port removes.
 *
 * These stubs keep the profile modules compiling and make the missing
 * functionality explicit on the console instead of failing at link time.
 * Porting the LE profiles means moving them onto upstream BTstack's
 * att_server/gatt/sm, which is a separate piece of work.
 */

#include <system/includes.h>

#include "app_comm_bt.h"
#include "le_common.h"
#include "le_gatt_common.h"

#include "usb.h"

#define BTSTACK_JL_BLE_LOG(...) \
    cyclone2_usb_debug("BTSTACK BLE unavailable: " __VA_ARGS__)

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

void le_hogp_set_icon(u16 class_type)
{
    (void) class_type;
}

void le_hogp_set_ReportMap(u8 *map, u16 size)
{
    (void) map;
    (void) size;
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

void ble_comm_init(const gatt_ctrl_t *control_blk)
{
    (void) control_blk;
}

void ble_comm_exit(void)
{
}

void ble_comm_set_config_name(const char *name_p, u8 add_ext_name)
{
    (void) name_p;
    (void) add_ext_name;
}

int ble_comm_att_send_data(u16 conn_handle, u16 att_handle, u8 *data, u16 len,
                           att_op_type_e op_type)
{
    (void) conn_handle;
    (void) att_handle;
    (void) data;
    (void) len;
    (void) op_type;
    return APP_BLE_IS_DISCONN;
}

int ble_comm_disconnect(u16 conn_handle)
{
    (void) conn_handle;
    return 0;
}

void ble_gatt_server_set_profile(const u8 *profile_table, u16 size)
{
    (void) profile_table;
    (void) size;
}

void ble_gatt_server_set_adv_config(adv_cfg_t *adv_cfg)
{
    (void) adv_cfg;
}

ble_state_e ble_gatt_server_get_connect_state(u16 conn_handle)
{
    (void) conn_handle;
    return BLE_ST_DISCONN;
}

u16 ble_gatt_server_characteristic_ccc_get(u16 conn_handle, u16 att_ccc_handle)
{
    (void) conn_handle;
    (void) att_ccc_handle;
    return 0;
}

int ble_gatt_server_characteristic_ccc_set(u16 conn_handle, u16 att_ccc_handle,
                                           u16 ccc_config)
{
    (void) conn_handle;
    (void) att_ccc_handle;
    (void) ccc_config;
    return 0;
}

int ble_gatt_server_connetion_update_request(
    u16 conn_handle, const struct conn_update_param_t *update_table,
    u16 table_count)
{
    (void) conn_handle;
    (void) update_table;
    (void) table_count;
    return 0;
}

void cyclone2_ble_set_custom_mode(u8 enable)
{
    (void) enable;
}

#include <system/includes.h>
#include <btstack/avctp_user.h>
#include <btstack/btstack_task.h>
#include <btstack/third_party/common/ble_user.h>

#include "app_comm_bt.h"
#include "ble_hogp_profile.h"
#include "le_common.h"
#include "le_gatt_common.h"
#include "profile.h"

#define GENERIC_REPORT_ID 1U
#define GENERIC_CCC_WARMUP_MS 100U

extern void le_hogp_set_PNP_info(const u8 *information);
extern void le_hogp_set_pair_allow(void);
extern void le_hogp_disconnect(void);
extern void ble_module_enable(u8 enable);
extern int ble_hid_is_connected(void);
extern int ble_hid_data_send(u8 report_id, u8 *data, u16 length);

static const u8 generic_report_map[] = {
    0x05,0x01, 0x09,0x05, 0xa1,0x01, 0x85,GENERIC_REPORT_ID,
    0x05,0x09, 0x19,0x01, 0x29,0x10, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x10, 0x81,0x02,
    0x05,0x01, 0x09,0x39, 0x15,0x00, 0x25,0x07, 0x75,0x04,
    0x95,0x01, 0x81,0x42, 0x75,0x04, 0x95,0x01, 0x81,0x01,
    0x05,0x01, 0x15,0x00, 0x26,0xff,0x00, 0x75,0x08, 0x95,0x06,
    0x09,0x30, 0x09,0x31, 0x09,0x33, 0x09,0x34, 0x09,0x32, 0x09,0x35,
    0x81,0x02, 0xc0,
};

static const ble_init_cfg_t generic_ble_config = {
    .same_address = 0,
    .appearance = BLE_APPEARANCE_HID_GAMEPAD,
    .report_map = generic_report_map,
    .report_map_size = sizeof(generic_report_map),
};

static const u8 generic_pnp_id[] = {
    0x02, 0x37, 0x35, 0x75, 0x05, 0x00, 0x01,
};

static struct cyclone2_input_report last_report;
static u32 connected_at_ms;
static u8 report_sent;
static u8 pairing_requested;

void cyclone2_generic_prepare_stack(void)
{
    le_hogp_set_PNP_info(generic_pnp_id);
    btstack_ble_start_before_init(&generic_ble_config, 0);
}

void cyclone2_generic_start(u8 slot, u8 pairing)
{
    (void)slot;
    pairing_requested = pairing;
    report_sent = 0U;
}

void cyclone2_generic_disconnect(void)
{
    le_hogp_disconnect();
}

void cyclone2_generic_exit(void)
{
    btstack_ble_exit(0);
}

int cyclone2_generic_is_connected(void)
{
    return ble_hid_is_connected() != 0;
}

/* BLE reconnection is advertising-driven: the controller keeps advertising for
 * the bonded host, so there is no per-attempt page to re-arm here. */
int cyclone2_generic_reconnect(void)
{
    return 0;
}

int cyclone2_generic_send(const struct cyclone2_input_report *report)
{
    u16 handle = ble_hid_is_connected();
    int result;

    if (!handle ||
        !ble_gatt_server_characteristic_ccc_get(
            handle, ATT_CHARACTERISTIC_2a4d_01_CLIENT_CONFIGURATION_HANDLE)) {
        report_sent = 0U;
        return 0;
    }
    if (sys_timer_get_ms() - connected_at_ms < GENERIC_CCC_WARMUP_MS)
        return 0;
    if (report_sent && !memcmp(report, &last_report, sizeof(*report)))
        return 0;

    result = ble_hid_data_send(GENERIC_REPORT_ID, (u8 *)report,
                               sizeof(*report));
    if (result == APP_BLE_NO_ERROR) {
        last_report = *report;
        report_sent = 1U;
        return 1;
    }
    if (result == APP_BLE_BUFF_FULL) return -1;
    if (result == APP_BLE_IS_DISCONN || result == APP_BLE_NO_WRITE_CCC)
        return 0;
    return -2;
}

enum cyclone2_profile_event cyclone2_generic_handle_event(
    const struct sys_event *event)
{
    if ((u32)event->arg == SYS_BT_EVENT_TYPE_CON_STATUS) {
        bt_comm_ble_status_event_handler((struct bt_event *)&event->u.bt);
        if (event->u.bt.event == BT_STATUS_INIT_OK && pairing_requested) {
            ble_module_enable(0);
            le_hogp_set_pair_allow();
            ble_module_enable(1);
        }
    } else if ((u32)event->arg == SYS_BT_EVENT_TYPE_HCI_STATUS) {
        bt_comm_ble_hci_event_handler((struct bt_event *)&event->u.bt);
    } else if ((u32)event->arg == SYS_BT_EVENT_BLE_STATUS) {
        if (event->u.bt.event == BLE_ST_CONNECT) {
            connected_at_ms = sys_timer_get_ms();
            return CYCLONE2_PROFILE_EVENT_CONNECTED;
        }
        if (event->u.bt.event == BLE_ST_DISCONN)
            return CYCLONE2_PROFILE_EVENT_DISCONNECTED;
    }
    return CYCLONE2_PROFILE_EVENT_NONE;
}

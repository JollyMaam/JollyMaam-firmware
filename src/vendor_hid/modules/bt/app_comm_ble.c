/*********************************************************************************************
    *   Filename        : app_comm_ble.c

    *   Description     :

    *   Author          : MRL

    *   Email           : MRL@zh-jieli.com

    *   Last modifiled  : 2021-06-172 14:01

    *   Copyright:(c)JIELI  2011-2021  @ , All Rights Reserved.
*********************************************************************************************/
#include "system/app_core.h"
#include "system/includes.h"
#include "server/server_core.h"
#include "app_config.h"
#include "app_action.h"
#include "os/os_api.h"
#include "btcontroller_config.h"
#include "btctrler/btctrler_task.h"
#include "config/config_transport.h"
#include "btstack/avctp_user.h"
#include "btstack/btstack_task.h"
#include "bt_common.h"
#include "edr_hid_user.h"
#include "le_common.h"
#include <stdlib.h>
#include "standard_hid.h"
#include "rcsp_bluetooth.h"
#include "app_charge.h"
#include "app_power_manage.h"
#include "app_chargestore.h"
#include "app_comm_bt.h"

#define LOG_TAG_CONST       COMM_BLE
#define LOG_TAG             "[COMM_BLE]"
#define LOG_ERROR_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_INFO_ENABLE
/* #define LOG_DUMP_ENABLE */
#define LOG_CLI_ENABLE
#include "debug.h"

#include "usb_control.h"
#undef log_info
#undef log_debug
#undef log_error
#define log_info(format, ...)  cyclone2_usb_debug("COMM_BLE: " format, ##__VA_ARGS__)
#define log_debug(format, ...) cyclone2_usb_debug("COMM_BLE: " format, ##__VA_ARGS__)
#define log_error(format, ...) cyclone2_usb_debug("COMM_BLE error: " format, ##__VA_ARGS__)

extern void ble_standard_dut_test_init(void);
/*************************************************************************************************/
/*!
 *  TRANSLATED: Configure the stack before initialization.
 *
 *  TRANSLATED: [in] Configuration parameters.
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void btstack_ble_start_before_init(const ble_init_cfg_t *cfg, int param)
{
    u8 tmp_ble_addr[6];

    if (!cfg) {
        log_error("cfg is null!!!");
        ASSERT(0);
    }

    if (cfg->same_address) {
        // TRANSLATED: Use the same address for BLE and EDR.
        memcpy(tmp_ble_addr, bt_get_mac_addr(), 6);
    } else {
        // TRANSLATED: Generate a unique address corresponding to the EDR address.
        lib_make_ble_address(tmp_ble_addr, (void *)bt_get_mac_addr());
    }

    le_controller_set_mac((void *)tmp_ble_addr);

    le_hogp_set_icon(cfg->appearance);
    le_hogp_set_ReportMap(cfg->report_map, cfg->report_map_size);
    le_hogp_regiest_get_battery(get_vbat_percent);

    log_info("---ble's address");
    printf_buf((void *)tmp_ble_addr, 6);

    extern void bt_ble_before_start_init(void);
    bt_ble_before_start_init();

}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Called after stack initialization.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void btstack_ble_start_after_init(int param)
{
    if (BT_MODE_IS(BT_BQB)) {
        void ble_bqb_test_thread_init(void);
        ble_bqb_test_thread_init();
    } else {
        extern void bt_ble_init(void);
        bt_ble_init();
    }
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Called after stack shutdown.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void btstack_ble_exit(int param)
{
    ble_module_enable(0);
    bt_ble_exit();

}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Handle common event messages.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
int bt_comm_ble_status_event_handler(struct bt_event *bt)
{
    /* log_info("--------%s: %d",__FUNCTION__, bt->event); */

    switch (bt->event) {
    case BT_STATUS_INIT_OK:
        /*
         * TRANSLATED: Bluetooth initialization complete.
         */
        log_info("STATUS_INIT_OK\n");

        btstack_ble_start_after_init(0);
        break;

    default:
        break;
    }
    return 0;
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Handle HCI event messages.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
int bt_comm_ble_hci_event_handler(struct bt_event *bt)
{
    if (bt->event == HCI_EVENT_VENDOR_REMOTE_TEST) {
        log_info("TEST_BOX:%d", bt->value);
        switch (bt->value) {
        case VENDOR_TEST_DISCONNECTED:
            set_remote_test_flag(0);
            log_info("clear_test_box_flag");
            cpu_reset();
            return 0;
            break;

        case VENDOR_TEST_LEGACY_CONNECTED_BY_BLE:
            break;

        default:
            break;
        }
    }
    return 0;
}

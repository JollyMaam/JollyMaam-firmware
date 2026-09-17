/*********************************************************************************************
    *   Filename        : btstack_config.c

    *   TRANSLATED: Description: code and RAM optimization settings.

    *   Author          : Bingquan

    *   Email           : caibingquan@zh-jieli.com

    *   Last modifiled  : 2019-03-16 11:49

    *   Copyright:(c)JIELI  2011-2019  @ , All Rights Reserved.
*********************************************************************************************/
#include "app_config.h"
#include "system/includes.h"
#include "btcontroller_config.h"
#include "bt_common.h"

/**
 * @brief Bluetooth Stack Module
 */

const int CONFIG_BTSTACK_BIG_FLASH_ENABLE     = 1;

const int CONFIG_BTSTACK_SUPPORT_AAC    = 0;

const int config_btstask_auto_exit_sniff = 1;

#if SMART_BOX_EN
const int config_rcsp_stack_enable = 1;
#else
const int config_rcsp_stack_enable = 0;
#endif

// TRANSLATED: LE settings for optimizing code and RAM.
const int config_le_hci_connection_num = CONFIG_BT_GATT_CONNECTION_NUM;// TRANSLATED: Number of simultaneous connections.
const int config_le_sm_support_enable = CONFIG_BT_SM_SUPPORT_ENABLE; // TRANSLATED: Support encrypted pairing.
const int config_le_gatt_server_num = CONFIG_BT_GATT_SERVER_NUM;   // TRANSLATED: Number of server roles.
const int config_le_gatt_client_num = CONFIG_BT_GATT_CLIENT_NUM;   // TRANSLATED: Number of client roles.

/* TRANSLATED: Enable Secure Connections encryption; requires config_le_sm_support_enable=1. */
const int config_le_sm_sub_sc_enable = CONFIG_BT_SM_SUPPORT_ENABLE & 0;//

const int config_le_sm_sub_sc_bridge_edr_enable = 0; /*default 0*/

/*
   u8 l2cap_debug_enable = 0xf0;
   u8 rfcomm_debug_enable = 0xf;
   u8 profile_debug_enable = 0xff;
   u8 ble_debug_enable    = 0xff;
 */

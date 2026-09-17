#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define CONFIG_RELEASE_ENABLE
#undef CONFIG_DEBUG_ENABLE
#define LIB_DEBUG 0
#define CONFIG_DEBUG_LIB(x)         (x & LIB_DEBUG)

#define CONFIG_HOGP_COMMON_ENABLE          1 // TRANSLATED: Shared HOGP module.

// TRANSLATED: BLE configuration.
#define CONFIG_BT_GATT_COMMON_ENABLE       1 // TRANSLATED: Use the shared GATT module.
#define CONFIG_BT_SM_SUPPORT_ENABLE        1 // TRANSLATED: Support encryption.
#define CONFIG_BT_GATT_CLIENT_NUM          0 // TRANSLATED: Number of central/client roles (not supported by this app).
#define CONFIG_BT_GATT_SERVER_NUM          1 // TRANSLATED: Number of peripheral/server roles.
#define CONFIG_BT_GATT_CONNECTION_NUM      (CONFIG_BT_GATT_SERVER_NUM + CONFIG_BT_GATT_CLIENT_NUM) // TRANSLATED: Total connection count.
#define CONFIG_BLE_HIGH_SPEED              0 // TRANSLATED: BLE high-speed mode: DLE + 2M PHY; payload must fit the PDU size.
#if CONFIG_BT_GATT_CONNECTION_NUM > 8
#error "SUPPORT MAX IS 8 !!!"
#endif

// TRANSLATED: Allow BLE central mode to discover/connect to the JL test box.
#define SUPPORT_TEST_BOX_BLE_MASTER_TEST_EN    0

// TRANSLATED: Default app configuration.
#define TCFG_AEC_ENABLE                     1
#define TCFG_MEDIA_LIB_USE_MALLOC           1

#include "board_config.h"
#include "usb_common_def.h"
#include "btcontroller_mode.h"
#include "user_cfg_id.h"

#define APP_PRIVATE_PROFILE_CFG // used by bt_common.h

#define CONFIG_BT_RX_BUFF_SIZE  (5 * 512)
#define CONFIG_BT_TX_BUFF_SIZE  (5 * 512)
#define BT_NORMAL_HZ (24 * 1000000L)

#endif

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define CONFIG_RELEASE_ENABLE
#undef CONFIG_DEBUG_ENABLE
/* Enabling every SDK log tag makes LTO retain enough code to overflow flash. */
#ifdef CYCLONE2_LOG_TAGS_VERBOSE
#define CYCLONE2_TAG(x)             (x)
#else
#define CYCLONE2_TAG(x)             CONFIG_DEBUG_LIB(x)
#endif
#ifdef CYCLONE2_LOG_TAGS_VERBOSE_VERBOSE
#define CYCLONE2_TAG_VERBOSE(x)     (x)
#else
#define CYCLONE2_TAG_VERBOSE(x)     0
#endif
#define LIB_DEBUG 0
#define CONFIG_DEBUG_LIB(x)         (x & LIB_DEBUG)

#define CONFIG_HOGP_COMMON_ENABLE          1

#define CONFIG_BT_GATT_COMMON_ENABLE       1
#define CONFIG_BT_SM_SUPPORT_ENABLE        1
#define CONFIG_BT_GATT_CLIENT_NUM          0 // TRANSLATED: Number of central/client roles (not supported by this app).
#define CONFIG_BT_GATT_SERVER_NUM          1
#define CONFIG_BT_GATT_CONNECTION_NUM      (CONFIG_BT_GATT_SERVER_NUM + CONFIG_BT_GATT_CLIENT_NUM)
#define CONFIG_BLE_HIGH_SPEED              0 // TRANSLATED: BLE high-speed mode: DLE + 2M PHY; payload must fit the PDU size.
#if CONFIG_BT_GATT_CONNECTION_NUM > 8
#error "SUPPORT MAX IS 8 !!!"
#endif

#define SUPPORT_TEST_BOX_BLE_MASTER_TEST_EN    0

#define TCFG_AEC_ENABLE                     1
#define TCFG_MEDIA_LIB_USE_MALLOC           1

#include "board_config.h"
#include "usb_common_def.h"
#include "btcontroller_mode.h"
#include "user_cfg_id.h"

#define APP_PRIVATE_PROFILE_CFG

#define CONFIG_BT_RX_BUFF_SIZE  (5 * 512)
#define CONFIG_BT_TX_BUFF_SIZE  (5 * 512)
#define BT_NORMAL_HZ (24 * 1000000L)

#endif

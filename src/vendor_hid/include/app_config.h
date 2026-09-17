#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*
 * TRANSLATED: Master switch for system debug prints.
 */

#ifdef CONFIG_RELEASE_ENABLE
#define LIB_DEBUG    0
#else
#define LIB_DEBUG    1
#endif
#define CONFIG_DEBUG_LIB(x)         (x & LIB_DEBUG)

#define CONFIG_DEBUG_ENABLE

// TRANSLATED: Select exactly one app and configure its board_config.h.
#define CONFIG_APP_CYCLONE2                 1//Gamesir Cyclone 2
#define CONFIG_APP_KEYBOARD                 0// TRANSLATED: HID keys; default example.
#define CONFIG_APP_KEYFOB                   0// TRANSLATED: Selfie remote; board_ac6368a, board_6318, board_6379b.
#define CONFIG_APP_MOUSE_SINGLE             0// TRANSLATED: Single-mode switching.
#define CONFIG_APP_MOUSE_DUAL               0// TRANSLATED: Enable both modes simultaneously.
#define CONFIG_APP_STANDARD_KEYBOARD        0// TRANSLATED: Standard HID keyboard; board_ac6351d.
#define CONFIG_APP_KEYPAGE                  0// TRANSLATED: Page-turning remote.
#define CONFIG_APP_GAMEBOX                  0// TRANSLATED: Gamebox adapter.
#define CONFIG_APP_REMOTE_CONTROL           0// TRANSLATED: Voice remote control.
#define CONFIG_APP_IDLE                     0//IDLE

// TRANSLATED: EDR sniff mode selection; tune parameters when porting (see the SDK app_keyboard.c example).
#if CONFIG_APP_MOUSE_SINGLE || CONFIG_APP_MOUSE_DUAL || CONFIG_APP_STANDARD_KEYBOARD || CONFIG_APP_REMOTE_CONTROL //|| CONFIG_APP_KEYBOARD
#define SNIFF_MODE_RESET_ANCHOR             1// TRANSLATED: Keyboard/mouse sniff: short fixed packet intervals for fast multi-key response.
#else
#define SNIFF_MODE_RESET_ANCHOR             0// TRANSLATED: Fixed 500 ms idle sniff interval: lower standby power, but key wakeup has latency.
#endif

// TRANSLATED: Enable Bluetooth 5.0 extended advertising/scanning.
#define CONFIG_BT_EXT_ADV_MODE            0

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
#define SUPPORT_TEST_BOX_BLE_MASTER_TEST_EN	   0

// TRANSLATED: Default app configuration.
#define TCFG_AEC_ENABLE                     1

#define TCFG_MEDIA_LIB_USE_MALLOC		    1

#include "board_config.h"

/* Cyclone 2 profiles select either EDR/Switch or BLE/generic HID at runtime.
 * Both controller stacks must therefore be present in every image. */
#if CONFIG_APP_CYCLONE2
#undef TCFG_USER_BLE_ENABLE
#define TCFG_USER_BLE_ENABLE                    1
#undef TCFG_USER_EDR_ENABLE
#define TCFG_USER_EDR_ENABLE                    1
#undef USER_SUPPORT_PROFILE_HID
#define USER_SUPPORT_PROFILE_HID                1
#undef USER_SUPPORT_PROFILE_PNP
#define USER_SUPPORT_PROFILE_PNP                1
#endif

#include "usb_common_def.h"

#include "btcontroller_mode.h"

#include "user_cfg_id.h"

#define APP_PRIVATE_PROFILE_CFG

#if CONFIG_BT_EXT_ADV_MODE
#define APP_TO_ALLOW_EXT_ADV
#endif

#if (CONFIG_BT_MODE == BT_NORMAL)
//enable dut mode,need disable sleep(TCFG_LOWPOWER_LOWPOWER_SEL = 0)
#define TCFG_NORMAL_SET_DUT_MODE                  0
#if TCFG_NORMAL_SET_DUT_MODE
#undef  TCFG_LOWPOWER_LOWPOWER_SEL
#define TCFG_LOWPOWER_LOWPOWER_SEL                0
#if TCFG_USER_EDR_ENABLE && TCFG_USER_BLE_ENABLE
// TRANSLATED: Cannot enable both simultaneously.
#error "dut need disable one bt!!!"
#endif

#endif//if TCFG_NORMAL_SET_DUT_MODE

#else

#undef  TCFG_BD_NUM
#define TCFG_BD_NUM						          1

#undef  TCFG_USER_TWS_ENABLE
#define TCFG_USER_TWS_ENABLE                      0     // TRANSLATED: Enable TWS.

#undef  TCFG_USER_BLE_ENABLE
#define TCFG_USER_BLE_ENABLE                      1     // TRANSLATED: Enable BLE.

#undef  TCFG_AUTO_SHUT_DOWN_TIME
#define TCFG_AUTO_SHUT_DOWN_TIME		          0

#undef  TCFG_SYS_LVD_EN
#define TCFG_SYS_LVD_EN						      0

#undef  TCFG_LOWPOWER_LOWPOWER_SEL
#define TCFG_LOWPOWER_LOWPOWER_SEL                0

#undef TCFG_AUDIO_DAC_LDO_VOLT
#define TCFG_AUDIO_DAC_LDO_VOLT				DACVDD_LDO_2_65V

#undef TCFG_LOWPOWER_POWER_SEL
#define TCFG_LOWPOWER_POWER_SEL				PWR_LDO15

#undef  TCFG_PWMLED_ENABLE
#define TCFG_PWMLED_ENABLE					DISABLE_THIS_MOUDLE

#undef  TCFG_ADKEY_ENABLE
#define TCFG_ADKEY_ENABLE                   DISABLE_THIS_MOUDLE

#undef  TCFG_IOKEY_ENABLE
#define TCFG_IOKEY_ENABLE					DISABLE_THIS_MOUDLE

#undef TCFG_TEST_BOX_ENABLE
#define TCFG_TEST_BOX_ENABLE			    0

#undef TCFG_AUTO_SHUT_DOWN_TIME
#define TCFG_AUTO_SHUT_DOWN_TIME	        0

#undef TCFG_POWER_ON_NEED_KEY
#define TCFG_POWER_ON_NEED_KEY		        0

#undef TCFG_UART0_ENABLE
#define TCFG_UART0_ENABLE					DISABLE_THIS_MOUDLE

#endif

#define BT_FOR_APP_EN                     0

// TRANSLATED: Enable these macros for app-based BLE updates.
#if CONFIG_APP_OTA_ENABLE
#define RCSP_BTMATE_EN                    1
#define RCSP_UPDATE_EN                    1
#define UPDATE_MD5_ENABLE                 0
#else
#define RCSP_BTMATE_EN                    0
#define RCSP_UPDATE_EN                    0
#define UPDATE_MD5_ENABLE                 0
#endif

#ifdef CONFIG_SDFILE_ENABLE
#define SDFILE_DEV				"sdfile"
#define SDFILE_MOUNT_PATH     	"mnt/sdfile"

#if (USE_SDFILE_NEW)
#define SDFILE_APP_ROOT_PATH       	SDFILE_MOUNT_PATH"/app/"  // TRANSLATED: Application partition.
#define SDFILE_RES_ROOT_PATH       	SDFILE_MOUNT_PATH"/res/"  // TRANSLATED: Resource-file partition.
#else
#define SDFILE_RES_ROOT_PATH       	SDFILE_MOUNT_PATH"/C/"
#endif

#endif
#if TCFG_USER_EDR_ENABLE
#if RCSP_BTMATE_EN
#define CONFIG_BT_RX_BUFF_SIZE  (3 * 512)
#define CONFIG_BT_TX_BUFF_SIZE  (3 * 512)
#else
#define CONFIG_BT_RX_BUFF_SIZE  (5 * 512)
#define CONFIG_BT_TX_BUFF_SIZE  (5 * 512)
#endif
#else
#define CONFIG_BT_RX_BUFF_SIZE  (0)
#define CONFIG_BT_TX_BUFF_SIZE  (0)
#endif

#if (CONFIG_BT_MODE != BT_NORMAL)
// TRANSLATED: For BQB 3M tests, increase the TX buffer if needed.
#undef  CONFIG_BT_TX_BUFF_SIZE
#define CONFIG_BT_TX_BUFF_SIZE  (6 * 1024)

#endif
#define BT_NORMAL_HZ	            CONFIG_BT_NORMAL_HZ
//*********************************************************************************//
//                                 TRANSLATED: Clock switching configuration.
//*********************************************************************************//

#define BT_NORMAL_HZ	            CONFIG_BT_NORMAL_HZ
#define BT_CONNECT_HZ               CONFIG_BT_CONNECT_HZ

#define BT_A2DP_HZ	        	    CONFIG_BT_A2DP_HZ
#define BT_TWS_DEC_HZ	        	CONFIG_TWS_DEC_HZ

//#define MUSIC_DEC_CLOCK			    CONFIG_MUSIC_DEC_CLOCK
//#define MUSIC_IDLE_CLOCK		    CONFIG_MUSIC_IDLE_CLOCK

#define BT_CALL_HZ		            CONFIG_BT_CALL_HZ
#define BT_CALL_ADVANCE_HZ          CONFIG_BT_CALL_ADVANCE_HZ
#define BT_CALL_16k_HZ	            CONFIG_BT_CALL_16k_HZ
#define BT_CALL_16k_ADVANCE_HZ      CONFIG_BT_CALL_16k_ADVANCE_HZ

//*********************************************************************************//
//                                 TRANSLATED: Update configuration.
//*********************************************************************************//
#if (defined(CONFIG_CPU_BR30))
// TRANSLATED: Enable update LED indication.
//#define UPDATE_LED_REMIND
// TRANSLATED: Enable update prompt tones.
//#define UPDATE_VOICE_REMIND
#endif

#if (defined(CONFIG_CPU_BR23) || defined(CONFIG_CPU_BR25))
// TRANSLATED: Retain GPIO states during updates.
// TRANSLATED: Optional DEV_UPDATE_SUPPORT_JUMP; currently supported only on BR23/BR25.
#endif
#endif

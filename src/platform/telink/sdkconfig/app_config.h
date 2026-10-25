#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Read first by every SDK and app file (-include, make/telink.mk): what the
 * SDK's samples keep in vendor/<sample>/app_config.h.  The SDK's
 * vendor/common/default_config.h fills in the rest. */

#define FLASH_SIZE_OPTION FLASH_SIZE_OPTION_512K

/* The SDK's sample features: all off; the app does its own. */
#define BLE_APP_SECURITY_ENABLE       0
#define BLE_OTA_SERVER_ENABLE         0
#define BLE_APP_PM_ENABLE             0
#define PM_DEEPSLEEP_ENABLE           0
#define PM_DEEPSLEEP_RETENTION_ENABLE 0
#define APP_FLASH_PROTECTION_ENABLE   0
#define UI_LED_ENABLE                 0
#define UI_KEYBOARD_ENABLE            0
#define APP_BATT_CHECK_ENABLE         0
#define DEBUG_GPIO_ENABLE             0
#define MODULE_WATCHDOG_ENABLE        0
#define FIRMWARES_SIGNATURE_ENABLE    0

/* The CPU on the 24 MHz crystal.  The system timer (clock_time()) runs at
 * 16 MHz whatever the CPU clock (drivers/timer.h). */
#define CLOCK_SYS_CLOCK_HZ 24000000
#define SYS_CLK_TYPE       SYS_CLK_24M_Crystal
#ifndef __ASSEMBLER__
enum { CLOCK_SYS_CLOCK_1S = 24000000, CLOCK_SYS_CLOCK_1MS = 24000, CLOCK_SYS_CLOCK_1US = 24 };
#endif

#endif

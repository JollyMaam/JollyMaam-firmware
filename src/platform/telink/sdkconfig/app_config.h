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

/* The CPU from the 24 MHz crystal: 24 MHz, or 48 (CLOCK_SYS_CLOCK_HZ).  The
 * system timer (clock_time()) runs at 16 MHz whatever the CPU clock
 * (drivers/timer.h). */
#ifndef CLOCK_SYS_CLOCK_HZ
#define CLOCK_SYS_CLOCK_HZ 24000000
#endif
#if CLOCK_SYS_CLOCK_HZ == 48000000
#define SYS_CLK_TYPE       SYS_CLK_48M_Crystal
#elif CLOCK_SYS_CLOCK_HZ == 24000000
#define SYS_CLK_TYPE       SYS_CLK_24M_Crystal
#else
#error "CLOCK_SYS_CLOCK_HZ: 24000000 or 48000000"
#endif
#ifndef __ASSEMBLER__
enum { CLOCK_SYS_CLOCK_1S = CLOCK_SYS_CLOCK_HZ, CLOCK_SYS_CLOCK_1MS = CLOCK_SYS_CLOCK_HZ / 1000,
       CLOCK_SYS_CLOCK_1US = CLOCK_SYS_CLOCK_HZ / 1000000 };
#endif

#endif

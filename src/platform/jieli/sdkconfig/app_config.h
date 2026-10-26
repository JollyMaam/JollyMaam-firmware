#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Included by every SDK source and header that wants the app's configuration.
 * Only what the compiled SDK sources, the linker script or our platform code
 * read is here; removing any line changes the image. */

#include "board_config.h"
#include "usb_common_def.h"

/* The controller's ACL buffer pools (include_lib/btctrler/btctler_lib_bss.ld). */
#ifdef CONFIG_CPU_BD19
#define CONFIG_BT_RX_BUFF_SIZE  (2 * 512)
#define CONFIG_BT_TX_BUFF_SIZE  (2 * 512)
#else
#define CONFIG_BT_RX_BUFF_SIZE  (5 * 512)
#define CONFIG_BT_TX_BUFF_SIZE  (5 * 512)
#endif

/* The CPU clock (clk_set("sys") at boot).  48 MHz, not the SDK's 24: at 24 the
 * old firmware's control loop starved the idle task that feeds the watchdog
 * (commit 5575ae7).  The peripheral clock stays 24 MHz either way. */
#define BT_NORMAL_HZ (48 * 1000000L)

#endif

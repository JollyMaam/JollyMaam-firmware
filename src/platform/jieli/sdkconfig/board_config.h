#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* The flash image: isd_config.ini (cpu/br23/tools/isd_config_rule.c).
 * CONFIG_FLASH_SIZE comes from the Makefile (the board's flash). */
#define CONFIG_UPDATE_JUMP_TO_MASK 0
#define CONFIG_ONLY_GRENERATE_ALIGN_4K_CODE 1
#ifdef CONFIG_CPU_BD19
#define CONFIG_CHIP_NAME           AC632N
#else
#define CONFIG_CHIP_NAME           AC635N
#endif
#define CONFIG_PID                 AC635N
#define CONFIG_VID                 0.01
#ifndef CONFIG_RESET_PIN           /* a board's own: make/<product>.mk */
#define CONFIG_RESET_PIN           PB01
#define CONFIG_RESET_TIME          8
#define CONFIG_RESET_LEVEL         0
#endif

#define ENABLE           1   /* usb_device.c, platform_jieli.c */
#define DISABLE          0
#define NO_CONFIG_PORT   (-1)

/* USB: none of the SDK's classes; ours set up the core (platform_jieli_usb.c;
 * usb_common_def.h, usb_std_class_def.h). */
#define MODULE_ENABLED   1
#define TCFG_PC_ENABLE   MODULE_ENABLED
#include "usb_std_class_def.h"
#define USB_DEVICE_CLASS_CONFIG     0
#ifndef USB_DMA_BUF_MAX_SIZE
#define USB_DMA_BUF_MAX_SIZE        1024    /* our endpoints, audio included */
#endif

/* Clock and power set-up at boot: setup.c and platform_jieli.c. */
#define TCFG_CLOCK_SYS_SRC  SYS_CLOCK_INPUT_PLL_BT_OSC
#define TCFG_CLOCK_SYS_HZ   24000000
#define TCFG_CLOCK_OSC_HZ   24000000
#define TCFG_CLOCK_MODE     CLOCK_MODE_ADAPTIVE

#define TCFG_LOWPOWER_POWER_SEL      PWR_LDO15
#define TCFG_LOWPOWER_LOWPOWER_SEL   SLEEP_EN
#define TCFG_LOWPOWER_BTOSC_DISABLE  0
#define TCFG_LOWPOWER_VDDIOM_LEVEL   VDDIOM_VOL_30V
#define TCFG_LOWPOWER_VDDIOW_LEVEL   VDDIOW_VOL_28V
#define TCFG_DCDC_PORT_SEL           NO_CONFIG_PORT

#endif

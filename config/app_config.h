#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Project-owned compile-time policy. sdk_ld.c also preprocesses this header
 * as linker-script input, so it intentionally emits no C declarations. */
#define ENABLE_THIS_MOUDLE       1
#define DISABLE_THIS_MOUDLE      0
#define ENABLE                   1
#define DISABLE                  0
#define NO_CONFIG_PORT          (-1)

#define LIB_DEBUG                0
#define CONFIG_DEBUG_LIB(value) ((value) & LIB_DEBUG)

#define FLASH_SIZE_1M            0x100000
#define CONFIG_FLASH_SIZE        FLASH_SIZE_1M
#define CONFIG_DOUBLE_BANK_ENABLE 0
#define CONFIG_APP_OTA_ENABLE    0

/* Native isd_download packaging policy. Keep the product identity and loader
 * pins compatible with the stock Cyclone 2 image. */
#define CONFIG_ONLY_GRENERATE_ALIGN_4K_CODE 1
#define CONFIG_CHIP_NAME        AC635N
#define CONFIG_PID              GS_C2_ADC_DEVICE
#define CONFIG_VID              3.52
#define CONFIG_PLL_SOURCE_USING_LRC 0
#define ALIGN_UNIT_256B         0
#define CONFIG_UBOOT_DEBUG_PIN  PB04
#define CONFIG_RESET_PIN        PB01
#define CONFIG_RESET_TIME       00
#define CONFIG_RESET_LEVEL      0
#define CONFIG_UPDATE_JUMP_TO_MASK 0
#define CONFIG_VDDIO_LVD_LEVEL  4
#define CONFIG_VM_LEAST_SIZE    8K
#define CONFIG_VM_OPT           1
#define CONFIG_BTIF_OPT         1

/* Minimal platform clock/power policy consumed by cpu/br23/setup.c. */
#define AUDIO_OUTPUT_WAY_DAC     0
#define AUDIO_OUTPUT_WAY_FM      2
#define AUDIO_OUTPUT_WAY         AUDIO_OUTPUT_WAY_DAC
#define TCFG_CLOCK_SYS_SRC       SYS_CLOCK_INPUT_PLL_BT_OSC
#define TCFG_CLOCK_SYS_HZ        24000000
#define TCFG_CLOCK_OSC_HZ        24000000
#define TCFG_CLOCK_MODE          CLOCK_MODE_ADAPTIVE
#define TCFG_LOWPOWER_POWER_SEL  PWR_LDO15
#define TCFG_LOWPOWER_LOWPOWER_SEL SLEEP_EN
#define TCFG_USE_VIRTUAL_RTC     0

/* PA0 is the A button, so the SDK debug UART must remain disabled. */
#define TCFG_UART0_ENABLE        DISABLE_THIS_MOUDLE
#define TCFG_UART0_RX_PORT       NO_CONFIG_PORT
#define TCFG_UART0_TX_PORT       NO_CONFIG_PORT
#define TCFG_UART0_BAUDRATE      1000000

/* Enable drivers deliberately when their project modules are added. */
#define TCFG_ADKEY_ENABLE        DISABLE_THIS_MOUDLE
#define TCFG_IOKEY_ENABLE        DISABLE_THIS_MOUDLE
#define TCFG_PWMLED_ENABLE       DISABLE_THIS_MOUDLE
#define TCFG_CHARGE_ENABLE       DISABLE_THIS_MOUDLE
#define TCFG_SYS_LVD_EN          0
#define TCFG_CODE_SWITCH_ENABLE  DISABLE_THIS_MOUDLE
#define TCFG_POWER_MODE_QUIET_ENABLE 0
#define TCFG_SD0_SD1_USE_THE_SAME_HW 0
#define TCFG_KEEP_CARD_AT_ACTIVE_STATUS 0
#define TCFG_SDX_CAN_OPERATE_MMC_CARD 0

/* Bluetooth will be enabled when its controller/stack modules are added. */
#define TCFG_USER_EDR_ENABLE     0
#define TCFG_USER_BLE_ENABLE     0
#define TCFG_USER_TWS_ENABLE     0
#define TCFG_BD_NUM              1

/* From board_config.h / apps/hid/board/br23/board_ac635n_demo_cfg.h */
#define AUDIO_OUTPUT_WAY_DAC        0
#define AUDIO_OUTPUT_WAY_IIS        1
#define AUDIO_OUTPUT_WAY_FM         2
#define AUDIO_OUTPUT_WAY_HDMI       3
#define AUDIO_OUTPUT_WAY_SPDIF      4
#define AUDIO_OUTPUT_WAY_BT         5
#define AUDIO_OUTPUT_WAY_DAC_IIS    6
#define AUDIO_OUTPUT_WAY_DONGLE     7
#define AUDIO_OUTPUT_WAY            AUDIO_OUTPUT_WAY_DAC
#define LINEIN_INPUT_WAY            LINEIN_INPUT_WAY_ANALOG

#if 0
/* needed when building hid app */
#define CONFIG_BT_GATT_COMMON_ENABLE       1 //配置使用gatt公共模块
#define CONFIG_BT_SM_SUPPORT_ENABLE        1 //配置是否支持加密
#define CONFIG_BT_GATT_CLIENT_NUM          0 //配置主机client个数(app not support)
#define CONFIG_BT_GATT_SERVER_NUM          1 //配置从机server个数
#define CONFIG_BT_GATT_CONNECTION_NUM      (CONFIG_BT_GATT_SERVER_NUM + CONFIG_BT_GATT_CLIENT_NUM)
#include "board_config.h"
#define CONFIG_BT_SM_SUPPORT_ENABLE 1
#define TCFG_AUTO_SHUT_DOWN_TIME 0
#endif

#endif

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#define CONFIG_DOUBLE_BANK_ENABLE  0
#define CONFIG_APP_OTA_ENABLE      0
#define CONFIG_UPDATE_JUMP_TO_MASK 0 // TRANSLATED: Enter the update loader by 0=reset or 1=jump. Jump supports GPIO-held power; first shut down all DMA peripherals.
#define CONFIG_ANC_ENABLE          0
#define CONFIG_FLASH_SIZE (0x100000)
#define CONFIG_ONLY_GRENERATE_ALIGN_4K_CODE 1

#define CONFIG_CHIP_NAME           AC635N
#define CONFIG_PID                 AC635N
#define CONFIG_VID                 0.01
#define CONFIG_RESET_PIN           PB01
#define CONFIG_RESET_TIME          8
#define CONFIG_RESET_LEVEL         0
#define CONFIG_DOUBLE_BANK_ENABLE  0
#define CONFIG_VM_LEAST_SIZE       8K
#define CONFIG_VM_OPT              1
#define CONFIG_BTIF_OPT            1

#define ENABLE           1
#define DISABLE          0
#define NO_CONFIG_PORT   (-1)

#define MODULE_ENABLED   1
#define MODULE_DISABLED  0
#define AUDIO_MIDI_CTRL_CONFIG                  MODULE_DISABLED
#define TCFG_ADB_ENABLE                         MODULE_DISABLED
#define TCFG_ADKEY_ENABLE                       MODULE_DISABLED
#define TCFG_AOA_ENABLE                         MODULE_DISABLED
#define TCFG_APP_PC_EN                          MODULE_DISABLED
#define TCFG_AUDIO_ADC_ENABLE                   MODULE_DISABLED
#define TCFG_AUDIO_DAC_ENABLE                   MODULE_DISABLED
#define TCFG_CHARGE_ENABLE                      MODULE_DISABLED
#define TCFG_HID_HOST_ENABLE                    MODULE_DISABLED
#define TCFG_IOKEY_ENABLE                       MODULE_DISABLED
#define TCFG_IRKEY_ENABLE                       MODULE_DISABLED
#define TCFG_MIC_CAPLESS_ENABLE                 MODULE_DISABLED
#define TCFG_PC_ENABLE                          MODULE_ENABLED
#define TCFG_RTC_ALARM_ENABLE                   MODULE_DISABLED
#define TCFG_SD0_ENABLE                         MODULE_DISABLED
#define TCFG_SD1_ENABLE                         MODULE_DISABLED
#define TCFG_SOFTOFF_WAKEUP_KEY_DRIVER_ENABLE   MODULE_DISABLED
#define TCFG_SUPPORT_MIC_CAPLESS                MODULE_DISABLED
#define TCFG_TOUCH_KEY_ENABLE                   MODULE_DISABLED
#define TCFG_UART0_ENABLE                       MODULE_DISABLED
#define TCFG_UDISK_ENABLE                       MODULE_DISABLED
#define TCFG_USB_CUSTOM_HID_ENABLE              MODULE_DISABLED
#undef MODULE_ENABLE
#undef MODULE_DISABLED

#include "usb_std_class_def.h"
#define USB_DEVICE_CLASS_CONFIG     MASSSTORAGE_CLASS
#define TCFG_OTG_USB_DEV_EN         BIT(0)

#define AUDIO_OUTPUT_MODE           AUDIO_OUTPUT_STEREO
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
#define AUDIO_OUTPUT_AUTOMUTE   0
#define VOL_TYPE_DIGITAL        0
#define VOL_TYPE_ANALOG         1
#define VOL_TYPE_AD             2
#define VOL_TYPE_DIGITAL_HW     3
#define SYS_VOL_TYPE            VOL_TYPE_ANALOG
#define TCFG_CALL_USE_DIGITAL_VOLUME        0
#define APP_AUDIO_STATE_WTONE_BY_MUSIC      (1)
#define TONE_MODE_DEFAULE_VOLUME    (0)

#define TCFG_CLOCK_SYS_SRC  SYS_CLOCK_INPUT_PLL_BT_OSC
#define TCFG_CLOCK_SYS_HZ   24000000
#define TCFG_CLOCK_OSC_HZ   24000000
#define TCFG_CLOCK_MODE     CLOCK_MODE_ADAPTIVE

#define TCFG_LOWPOWER_POWER_SEL      PWR_LDO15
#define TCFG_LOWPOWER_LOWPOWER_SEL   SLEEP_EN
#define TCFG_LOWPOWER_BTOSC_DISABLE  0
#define TCFG_LOWPOWER_VDDIOM_LEVEL   VDDIOM_VOL_30V
#define TCFG_LOWPOWER_VDDIOW_LEVEL   VDDIOW_VOL_28V
#define TCFG_LOWPOWER_OSC_TYPE       OSC_TYPE_LRC
#define TCFG_DCDC_PORT_SEL           NO_CONFIG_PORT

#define TCFG_USER_TWS_ENABLE 0
#define TCFG_USER_BLE_ENABLE 1
#define TCFG_USER_EDR_ENABLE 1

#endif

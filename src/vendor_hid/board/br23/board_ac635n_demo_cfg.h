#ifndef CONFIG_BOARD_AC635N_DEMO_CFG_H
#define CONFIG_BOARD_AC635N_DEMO_CFG_H

#include "board_ac635n_demo_global_build_cfg.h"

#ifdef CONFIG_BOARD_AC635N_DEMO

#define CONFIG_SDFILE_ENABLE

//*********************************************************************************//
//                                 TRANSLATED: Configuration begins.
//*********************************************************************************//
#define ENABLE_THIS_MOUDLE					1
#define DISABLE_THIS_MOUDLE					0

#define ENABLE								1
#define DISABLE								0

#define NO_CONFIG_PORT						(-1)

//*********************************************************************************//
//                                 TRANSLATED: UART configuration.
//*********************************************************************************//
#define TCFG_UART0_ENABLE					ENABLE_THIS_MOUDLE                     // TRANSLATED: Enable the debug UART.
#define TCFG_UART0_RX_PORT					NO_CONFIG_PORT                         // TRANSLATED: UART RX pin; NO_CONFIG_PORT is allowed for debug output only.
#define TCFG_UART0_TX_PORT  				IO_PORTA_00                            // TRANSLATED: UART TX pin.
#define TCFG_UART0_BAUDRATE  				1000000                                // TRANSLATED: UART baud rate.

//*********************************************************************************//
//                                 TRANSLATED: USB configuration.
//*********************************************************************************//
#define TCFG_PC_ENABLE						ENABLE_THIS_MOUDLE //Cyclone 2 CDC console
#define TCFG_UDISK_ENABLE					DISABLE_THIS_MOUDLE // TRANSLATED: Enable USB mass-storage support.
#define TCFG_HID_HOST_ENABLE                DISABLE_THIS_MOUDLE// TRANSLATED: Gamebox mode (ENABLE_THIS_MOUDLE).
#define TCFG_ADB_ENABLE                     DISABLE_THIS_MOUDLE//ENABLE_THIS_MOUDLE
#define TCFG_AOA_ENABLE                     DISABLE_THIS_MOUDLE//ENABLE_THIS_MOUDLE

#define TCFG_OTG_USB_DEV_EN                 BIT(0)
#define TCFG_USB_CUSTOM_HID_ENABLE          DISABLE_THIS_MOUDLE
#define CDC_INTR_EP_ENABLE                  ENABLE_THIS_MOUDLE

#include "usb_std_class_def.h"
// TRANSLATED: Override USB configuration.
#undef USB_DEVICE_CLASS_CONFIG
#define USB_DEVICE_CLASS_CONFIG             CDC_CLASS
#define TCFG_APP_PC_EN                     TCFG_PC_ENABLE
//*********************************************************************************//
//                                 TRANSLATED: I2C configuration.
//*********************************************************************************//
/* TRANSLATED: Software I2C settings. */
#define TCFG_SW_I2C0_CLK_PORT               IO_PORTA_09                             // TRANSLATED: Software I2C clock pin.
#define TCFG_SW_I2C0_DAT_PORT               IO_PORTA_10                             // TRANSLATED: Software I2C data pin.
#define TCFG_SW_I2C0_DELAY_CNT              50                                      // TRANSLATED: I2C delay parameter; controls the bus clock frequency.

/* TRANSLATED: Hardware I2C pin selection.
  SCL         SDA
  'A': IO_PORT_DP   IO_PORT_DM
  'B': IO_PORTA_09  IO_PORTA_10
  'C': IO_PORTA_07  IO_PORTA_08
  'D': IO_PORTA_05  IO_PORTA_06
 */
#define TCFG_HW_I2C0_PORTS                  'B'
#define TCFG_HW_I2C0_CLK                    100000                                  // TRANSLATED: Hardware I2C bus speed.

//*********************************************************************************//
//                                 TRANSLATED: Hardware SPI configuration.
//*********************************************************************************//
#define	TCFG_HW_SPI1_ENABLE		ENABLE_THIS_MOUDLE
// TRANSLATED: Pin group A: DI=PB2, DO=PB1, CLK=PB0.
// TRANSLATED: Pin group B: DI=PC3, DO=PC5, CLK=PC4.
#define TCFG_HW_SPI1_PORT		'A'
#define TCFG_HW_SPI1_BAUD		2000000L
#define TCFG_HW_SPI1_MODE		SPI_MODE_BIDIR_1BIT
#define TCFG_HW_SPI1_ROLE		SPI_ROLE_MASTER

#define	TCFG_HW_SPI2_ENABLE		ENABLE_THIS_MOUDLE
// TRANSLATED: Pin group A: DI=PB8, DO=PB10, CLK=PB9.
// TRANSLATED: Pin group B: DI=PA13, DO=DM, CLK=DP.
#define TCFG_HW_SPI2_PORT		'A'
#define TCFG_HW_SPI2_BAUD		2000000L
#define TCFG_HW_SPI2_MODE		SPI_MODE_BIDIR_1BIT
#define TCFG_HW_SPI2_ROLE		SPI_ROLE_MASTER

//*********************************************************************************//
//                                 TRANSLATED: Flash configuration.
//*********************************************************************************//
#define TCFG_CODE_FLASH_ENABLE				DISABLE_THIS_MOUDLE
#define TCFG_FLASH_DEV_SPI_HW_NUM			1// 1: SPI1    2: SPI2
#define TCFG_FLASH_DEV_SPI_CS_PORT	    	IO_PORTA_03

//*********************************************************************************//
//                                  TRANSLATED: SD configuration.
//*********************************************************************************//
#define TCFG_SD0_ENABLE						DISABLE_THIS_MOUDLE
// TRANSLATED: Pin group A: CMD=PA9, CLK=PA10, DAT0=PA5, DAT1=PA6, DAT2=PA7, DAT3=PA8.
// TRANSLATED: Pin group B: CMD=PB10, CLK=PB9, DAT0=PB8, DAT1=PB6, DAT2=PB5, DAT3=PB4.
#define TCFG_SD0_PORTS						'B'
#define TCFG_SD0_DAT_MODE					1
#define TCFG_SD0_DET_MODE					SD_CMD_DECT
#define TCFG_SD0_DET_IO 					IO_PORT_DM// TRANSLATED: Used when SD_DET_MODE is 2.
#define TCFG_SD0_DET_IO_LEVEL				0// TRANSLATED: Card detect: 0=low; 1=high with external power; 2=high with SD-card power.
#define TCFG_SD0_CLK						(3000000*4L)

#define TCFG_SD1_ENABLE						DISABLE_THIS_MOUDLE
// TRANSLATED: Pin group A: CMD=PC4, CLK=PC5, DAT0=PC3, DAT1=PC2, DAT2=PC1, DAT3=PC0.
// TRANSLATED: Pin group B: CMD=PB5, CLK=PB6, DAT0=PB4, DAT1=PB8, DAT2=PB9, DAT3=PB10.
#define TCFG_SD1_PORTS						'A'
#define TCFG_SD1_DAT_MODE					1
#define TCFG_SD1_DET_MODE					SD_CLK_DECT
#define TCFG_SD1_DET_IO 					IO_PORT_DM// TRANSLATED: Used when SD_DET_MODE is 2.
#define TCFG_SD1_DET_IO_LEVEL				0// TRANSLATED: Card detect: 0=low; 1=high with external power; 2=high with SD-card power.
#define TCFG_SD1_CLK						(3000000*4L)

//*********************************************************************************//
//                                  TRANSLATED: RTC alarm configuration.
//*********************************************************************************//
#define TCFG_RTC_ALARM_ENABLE               DISABLE_THIS_MOUDLE

//*********************************************************************************//
//                                 TRANSLATED: Key configuration.
//*********************************************************************************//
#define KEY_NUM_MAX                        	10
#define KEY_NUM                            	3

#define MULT_KEY_ENABLE						DISABLE 		// TRANSLATED: Enable key chords; requires a chord mapping table.

//*********************************************************************************//
//                         TRANSLATED: Soft-poweroff wakeup key-driver configuration.
//*********************************************************************************//
#define TCFG_SOFTOFF_WAKEUP_KEY_DRIVER_ENABLE       DISABLE_THIS_MOUDLE  // TRANSLATED: Preserve the key event that wakes from soft poweroff; currently supports IOKEY only.

// TRANSLATED: Match the wakeup_param table in board.c.
#define TCFG_WAKEUP_PORT_POWER_SRC          BIT(1)  // TRANSLATED: Wakeup port[1].
#define TCFG_WAKEUP_PORT_PREV_SRC           BIT(2)  // TRANSLATED: Wakeup port[2].
#define TCFG_WAKEUP_PORT_NEXT_SRC           BIT(3)  // TRANSLATED: Wakeup port[3].

#define TCFG_LONGKEY_SUPPLEMENT_TIME        15  // TRANSLATED: Long-press compensation: use measured power_on-to-key_init time. 15 scans * 10 ms = 150 ms.

//*********************************************************************************//
//                                 TRANSLATED: GPIO key configuration.
//*********************************************************************************//
#define TCFG_IOKEY_ENABLE					DISABLE_THIS_MOUDLE // TRANSLATED: Enable GPIO keys.

#define TCFG_IOKEY_POWER_CONNECT_WAY		ONE_PORT_TO_LOW    // TRANSLATED: Key connects the GPIO to ground.

#define TCFG_IOKEY_POWER_ONE_PORT			IO_PORTB_01        // TRANSLATED: GPIO key pin.
#define TCFG_IOKEY_POWER_ONE_PORT_VALUE		0x1                // TRANSLATED: Power key code must not be zero: key_value starts at zero, which would generate a spurious keep event.

#define TCFG_IOKEY_PREV_CONNECT_WAY			ONE_PORT_TO_LOW  // TRANSLATED: Key connects the GPIO to ground.
#define TCFG_IOKEY_PREV_ONE_PORT			IO_PORTB_00
#define TCFG_IOKEY_PREV_ONE_PORT_VALUE		0x2              // TRANSLATED: Previous-key code.

#define TCFG_IOKEY_NEXT_CONNECT_WAY 		ONE_PORT_TO_LOW  // TRANSLATED: Key connects the GPIO to ground.
#define TCFG_IOKEY_NEXT_ONE_PORT			IO_PORTB_02
#define TCFG_IOKEY_NEXT_ONE_PORT_VALUE		0x3              // TRANSLATED: Next-key code.

//*********************************************************************************//
//                                 TRANSLATED: ADC key configuration.
//*********************************************************************************//
#define TCFG_ADKEY_ENABLE                   ENABLE_THIS_MOUDLE // TRANSLATED: Enable ADC keys (DISABLE_THIS_MOUDLE disables them).
#define TCFG_ADKEY_PORT                     IO_PORTB_01         // TRANSLATED: ADC key pin; ensure the selected GPIO supports ADC input.
/* TRANSLATED: ADC channel must match the ADC key pin:
    AD_CH_PA1    AD_CH_PA3    AD_CH_PA4    AD_CH_PA5
    AD_CH_PA9    AD_CH_PA1    AD_CH_PB1    AD_CH_PB4
    AD_CH_PB6    AD_CH_PB7    AD_CH_DP     AD_CH_DM
    AD_CH_PB2
*/
#define TCFG_ADKEY_AD_CHANNEL               AD_CH_PB1
#define TCFG_ADKEY_EXTERN_UP_ENABLE         ENABLE_THIS_MOUDLE // TRANSLATED: Use an external pull-up.

#if TCFG_ADKEY_EXTERN_UP_ENABLE
#define R_UP    220                 // TRANSLATED: 22 kOhm; set this to the external pull-up resistance.
#else
#define R_UP    100                 // TRANSLATED: 10 kOhm; default internal pull-up resistance.
#endif

// TRANSLATED: List resistances in ascending order; use 0x3ffL for absent keys (VDDIO).
#define TCFG_ADKEY_AD0      (0)                                 //0R
#define TCFG_ADKEY_AD1      (0x3ffL * 30   / (30   + R_UP))     //3k
#define TCFG_ADKEY_AD2      (0x3ffL * 62   / (62   + R_UP))     //6.2k
#define TCFG_ADKEY_AD3      (0x3ffL * 91   / (91   + R_UP))     //9.1k
#define TCFG_ADKEY_AD4      (0x3ffL * 150  / (150  + R_UP))     //15k
#define TCFG_ADKEY_AD5      (0x3ffL * 240  / (240  + R_UP))     //24k
#define TCFG_ADKEY_AD6      (0x3ffL * 330  / (330  + R_UP))     //33k
#define TCFG_ADKEY_AD7      (0x3ffL * 510  / (510  + R_UP))     //51k
#define TCFG_ADKEY_AD8      (0x3ffL * 1000 / (1000 + R_UP))     //100k
#define TCFG_ADKEY_AD9      (0x3ffL * 2200 / (2200 + R_UP))     //220k
#define TCFG_ADKEY_VDDIO    (0x3ffL)

#define TCFG_ADKEY_VOLTAGE0 ((TCFG_ADKEY_AD0 + TCFG_ADKEY_AD1) / 2)
#define TCFG_ADKEY_VOLTAGE1 ((TCFG_ADKEY_AD1 + TCFG_ADKEY_AD2) / 2)
#define TCFG_ADKEY_VOLTAGE2 ((TCFG_ADKEY_AD2 + TCFG_ADKEY_AD3) / 2)
#define TCFG_ADKEY_VOLTAGE3 ((TCFG_ADKEY_AD3 + TCFG_ADKEY_AD4) / 2)
#define TCFG_ADKEY_VOLTAGE4 ((TCFG_ADKEY_AD4 + TCFG_ADKEY_AD5) / 2)
#define TCFG_ADKEY_VOLTAGE5 ((TCFG_ADKEY_AD5 + TCFG_ADKEY_AD6) / 2)
#define TCFG_ADKEY_VOLTAGE6 ((TCFG_ADKEY_AD6 + TCFG_ADKEY_AD7) / 2)
#define TCFG_ADKEY_VOLTAGE7 ((TCFG_ADKEY_AD7 + TCFG_ADKEY_AD8) / 2)
#define TCFG_ADKEY_VOLTAGE8 ((TCFG_ADKEY_AD8 + TCFG_ADKEY_AD9) / 2)
#define TCFG_ADKEY_VOLTAGE9 ((TCFG_ADKEY_AD9 + TCFG_ADKEY_VDDIO) / 2)

#define TCFG_ADKEY_VALUE0                   0
#define TCFG_ADKEY_VALUE1                   1
#define TCFG_ADKEY_VALUE2                   2
#define TCFG_ADKEY_VALUE3                   3
#define TCFG_ADKEY_VALUE4                   4
#define TCFG_ADKEY_VALUE5                   5
#define TCFG_ADKEY_VALUE6                   6
#define TCFG_ADKEY_VALUE7                   7
#define TCFG_ADKEY_VALUE8                   8
#define TCFG_ADKEY_VALUE9                   9

//*********************************************************************************//
//                                 TRANSLATED: Infrared key configuration.
//*********************************************************************************//
#define TCFG_IRKEY_ENABLE                   DISABLE_THIS_MOUDLE// TRANSLATED: Enable ADC keys (original SDK comment on the IR enable setting).
#define TCFG_IRKEY_PORT                     IO_PORTA_08        // TRANSLATED: Infrared key pin.

//*********************************************************************************//
//                             TRANSLATED: Touch-key configuration.
//*********************************************************************************//
#define TCFG_TOUCH_KEY_ENABLE 				DISABLE_THIS_MOUDLE 		// TRANSLATED: Enable touch keys.

/* TRANSLATED: Touch counter reference clock: higher frequency gives better precision.
** TRANSLATED: Available options:
	1.TOUCH_KEY_OSC_CLK,
    TRANSLATED: 2.TOUCH_KEY_MUX_IN_CLK: external input; reserved and normally unused.
    3.TOUCH_KEY_PLL_192M_CLK,
    4.TOUCH_KEY_PLL_240M_CLK,
*/
#define TCFG_TOUCH_KEY_CLK 					TOUCH_KEY_PLL_192M_CLK 	// TRANSLATED: Touch-key clock.
#define TCFG_TOUCH_KEY_CHANGE_GAIN 			4 	// TRANSLATED: Change amplification factor; normally fixed.
#define TCFG_TOUCH_KEY_PRESS_CFG 			-2 	// TRANSLATED: Touch press sensitivity (s16); larger values increase sensitivity.
#define TCFG_TOUCH_KEY_RELEASE_CFG0 		-50 // TRANSLATED: Touch release sensitivity 0 (s16); larger values increase sensitivity.
#define TCFG_TOUCH_KEY_RELEASE_CFG1 		-80 // TRANSLATED: Touch release sensitivity 1 (s16); larger values increase sensitivity.

// TRANSLATED: Key 0 configuration.
#define TCFG_TOUCH_KEY0_PORT 				IO_PORTB_06  // TRANSLATED: Touch-key GPIO.
#define TCFG_TOUCH_KEY0_VALUE 				1 		 	 // TRANSLATED: Touch key 0 code.

// TRANSLATED: Key 1 configuration.
#define TCFG_TOUCH_KEY1_PORT 				IO_PORTB_07  // TRANSLATED: Touch key 1 GPIO.
#define TCFG_TOUCH_KEY1_VALUE 				2 		 	 // TRANSLATED: Touch key 1 code.

//*********************************************************************************//
//                                 TRANSLATED: Audio configuration.
//*********************************************************************************//
#define TCFG_AUDIO_ADC_ENABLE				DISABLE_THIS_MOUDLE
// TRANSLATED: MIC has only one channel; always select the right channel.
#define TCFG_AUDIO_ADC_MIC_CHA				LADC_CH_MIC_R
#define TCFG_AUDIO_ADC_LINE_CHA				LADC_LINE0_MASK
/* TRANSLATED: MIC LDO current settings:
    0:0.625ua    1:1.25ua    2:1.875ua    3:2.5ua*/
#define TCFG_AUDIO_ADC_LDO_SEL				2

// TRANSLATED: LADC channel.
#define TCFG_AUDIO_ADC_LINE_CHA0			LADC_LINE1_MASK
#define TCFG_AUDIO_ADC_LINE_CHA1			LADC_CH_LINE0_L

#define TCFG_AUDIO_DAC_ENABLE				DISABLE_THIS_MOUDLE

// TRANSLATED: Audio support must be enabled before enabling DAC/ADC modules.
#ifdef CONFIG_LITE_AUDIO
#define TCFG_AUDIO_ENABLE					DISABLE
#if TCFG_AUDIO_ENABLE
#undef TCFG_AUDIO_ADC_ENABLE
#undef TCFG_AUDIO_DAC_ENABLE
#define TCFG_AUDIO_ADC_ENABLE				ENABLE_THIS_MOUDLE
#define TCFG_AUDIO_DAC_ENABLE				ENABLE_THIS_MOUDLE
#define TCFG_DEC_G729_ENABLE                ENABLE
#define TCFG_DEC_PCM_ENABLE					ENABLE
#define TCFG_DEC_WTGV2_ENABLE               DISABLE
#define TCFG_DEC_MP3_ENABLE					DISABLE
#define TCFG_ENC_OPUS_ENABLE               	DISABLE
#define TCFG_ENC_SPEEX_ENABLE              	DISABLE
#define TCFG_ENC_LC3_ENABLE                 DISABLE
#define TCFG_ENC_ADPCM_ENABLE               DISABLE
#define TCFG_ENC_SBC_ENABLE                 DISABLE
#define TCFG_ENC_MSBC_ENABLE                DISABLE

// TRANSLATED: LC3 encoder parameters.
#if TCFG_ENC_LC3_ENABLE
#define LC3_CODING_SAMPLERATE  16000 // TRANSLATED: LC3 encoder sample rate.
#define LC3_CODING_FRAME_LEN   100  // TRANSLATED: Frame length; only 25, 50, and 100 are supported.
#define LC3_CODING_CHANNEL     2  // TRANSLATED: LC3 channel count.
#endif

// TRANSLATED: Encoder demo file.
#define ENC_DEMO_EN                        DISABLE
#define TCFG_SPEED_PITCH_ENABLE            DISABLE    // TRANSLATED: Prompt-tone pitch shifting.
#define TCFG_SPEED_MODE           		   (1.0f)    // TRANSLATED: Playback speed multiplier; e.g. 2.0f for double speed.

#define TCFG_LINEIN_LR_CH                   AUDIO_LIN0_LR
#define TCFG_DEC_WAV_ENABLE					DISABLE
#else
#define TCFG_DEC_PCM_ENABLE					DISABLE
#endif/*TCFG_AUDIO_ENABLE*/
#endif/*CONFIG_LITE_AUDIO*/

#define TCFG_AUDIO_DAC_LDO_SEL				1
/*
TRANSLATED: DACVDD voltage settings; choose according to the board wiring:
    DACVDD_LDO_1_20V        DACVDD_LDO_1_30V        DACVDD_LDO_2_35V        DACVDD_LDO_2_50V
    DACVDD_LDO_2_65V        DACVDD_LDO_2_80V        DACVDD_LDO_2_95V        DACVDD_LDO_3_10V*/
#define TCFG_AUDIO_DAC_LDO_VOLT				DACVDD_LDO_2_70V
/* TRANSLATED: Reserved interface; unused. */
#define TCFG_AUDIO_DAC_PA_PORT				NO_CONFIG_PORT
/*
TRANSLATED: DAC hardware wiring options:
    TRANSLATED: DAC_OUTPUT_MONO_L: left channel.
    TRANSLATED: DAC_OUTPUT_MONO_R: right channel.
    TRANSLATED: DAC_OUTPUT_LR: stereo.
    TRANSLATED: DAC_OUTPUT_MONO_LR_DIFF: differential mono output.
*/
#define TCFG_AUDIO_DAC_CONNECT_MODE         DAC_OUTPUT_MONO_LR_DIFF

/*
TRANSLATED: Decoded audio output mode:
    TRANSLATED: AUDIO_OUTPUT_ORIG_CH: original channels.
    TRANSLATED: AUDIO_OUTPUT_STEREO: stereo.
    TRANSLATED: AUDIO_OUTPUT_L_CH: original left channel only.
    TRANSLATED: AUDIO_OUTPUT_R_CH: original right channel only.
    TRANSLATED: AUDIO_OUTPUT_MONO_LR_CH: mix left and right to mono.
 */
#define AUDIO_OUTPUT_MODE                   AUDIO_OUTPUT_STEREO

#define AUDIO_OUTPUT_WAY_DAC        0
#define AUDIO_OUTPUT_WAY_IIS        1
#define AUDIO_OUTPUT_WAY_FM         2
#define AUDIO_OUTPUT_WAY_HDMI       3
#define AUDIO_OUTPUT_WAY_SPDIF      4
#define AUDIO_OUTPUT_WAY_BT      	5	// bt emitter
#define AUDIO_OUTPUT_WAY_DAC_IIS    6
#define AUDIO_OUTPUT_WAY_DONGLE		7
#define AUDIO_OUTPUT_WAY            AUDIO_OUTPUT_WAY_DAC
#define LINEIN_INPUT_WAY            LINEIN_INPUT_WAY_ANALOG

#define AUDIO_OUTPUT_AUTOMUTE       0//ENABLE
/*
 *TRANSLATED: System volume-control type.
 *TRANSLATED: Software digital volume processes samples entirely in software.
 *TRANSLATED: Hardware digital volume processes samples in the DAC's digital block.
 */
#define VOL_TYPE_DIGITAL		0	// TRANSLATED: Software digital volume.
#define VOL_TYPE_ANALOG			1	// TRANSLATED: Hardware analog volume.
#define VOL_TYPE_AD				2	// TRANSLATED: Combined analog/digital volume control.
#define VOL_TYPE_DIGITAL_HW		3  	// TRANSLATED: Hardware digital volume.
#define SYS_VOL_TYPE            VOL_TYPE_ANALOG
/*
 *TRANSLATED: Use digital volume during calls.
 *TRANSLATED: 0: use the volume-control type selected by SYS_VOL_TYPE during calls.
 *TRANSLATED: 1: use digital volume during calls for smoother adjustment.
 */
#define TCFG_CALL_USE_DIGITAL_VOLUME		0

// TRANSLATED: Use music volume for prompt tones when enabled.
#define APP_AUDIO_STATE_WTONE_BY_MUSIC      (1)
// TRANSLATED: 0: no default prompt volume; 1: use the default prompt volume.
#define TONE_MODE_DEFAULE_VOLUME            (0)

/*
 *TRANSLATED: Support the capacitorless MIC module.
 *TRANSLATED: (1) TCFG_SUPPORT_MIC_CAPLESS must be enabled to support the module.
 *TRANSLATED: (2) TCFG_MIC_CAPLESS_ENABLE can enable it only when support is present.
 */
#define TCFG_SUPPORT_MIC_CAPLESS			DISABLE_THIS_MOUDLE
// TRANSLATED: Enable capacitorless MIC.
#define TCFG_MIC_CAPLESS_ENABLE				DISABLE_THIS_MOUDLE

#define AUDIO_MIDI_CTRL_CONFIG    0 // TRANSLATED: Enable the MIDI keyboard interface; disable low power when using this option.

//*********************************************************************************//
//                                  TRANSLATED: Charging-case configuration.
//*********************************************************************************//
#define TCFG_CHARGESTORE_ENABLE				DISABLE_THIS_MOUDLE       // TRANSLATED: Support a smart charging case.
#define TCFG_TEST_BOX_ENABLE			    0
#define TCFG_CHARGESTORE_PORT				IO_PORTB_05               // TRANSLATED: GPIO for communication between earbud and charging case.
#define TCFG_CHARGESTORE_UART_ID			IRQ_UART1_IDX             // TRANSLATED: UART used for communication.

//*********************************************************************************//
//                                  TRANSLATED: Charging parameters.
//*********************************************************************************//
// TRANSLATED: Support the on-chip charger.
#define TCFG_CHARGE_ENABLE					DISABLE_THIS_MOUDLE
// TRANSLATED: Support charging while powered on.
#define TCFG_CHARGE_POWERON_ENABLE			DISABLE
// TRANSLATED: Automatically power on when removed from the charger.
#define TCFG_CHARGE_OFF_POWERON_NE			DISABLE
// TRANSLATED: Support the Lightning handshake protocol.
#define TCFG_HANDSHAKE_ENABLE               DISABLE
#define TCFG_HANDSHAKE_IO_DATA1             IO_PORTB_02// TRANSLATED: Handshake GPIO near the middle of the Lightning connector.
#define TCFG_HANDSHAKE_IO_DATA2             IO_PORTB_07// TRANSLATED: Handshake GPIO near the edge of the Lightning connector.
/*
TRANSLATED: Charge termination voltage options:
    CHARGE_FULL_V_3869  CHARGE_FULL_V_3907  CHARGE_FULL_V_3946   CHARGE_FULL_V_3985
    CHARGE_FULL_V_4026  CHARGE_FULL_V_4068  CHARGE_FULL_V_4122   CHARGE_FULL_V_4157
    CHARGE_FULL_V_4202  CHARGE_FULL_V_4249  CHARGE_FULL_V_4295   CHARGE_FULL_V_4350
    CHARGE_FULL_V_4398  CHARGE_FULL_V_4452  CHARGE_FULL_V_4509   CHARGE_FULL_V_4567
*/
#define TCFG_CHARGE_FULL_V					CHARGE_FULL_V_4202
/*
TRANSLATED: Charge termination current options:
    CHARGE_FULL_mA_2	CHARGE_FULL_mA_5	CHARGE_FULL_mA_7	 CHARGE_FULL_mA_10
    CHARGE_FULL_mA_15	CHARGE_FULL_mA_20	CHARGE_FULL_mA_25	 CHARGE_FULL_mA_30
*/
#define TCFG_CHARGE_FULL_MA					CHARGE_FULL_mA_10
/*
TRANSLATED: Charging current options:
    CHARGE_mA_20		CHARGE_mA_40		CHARGE_mA_60		CHARGE_mA_80
    CHARGE_mA_100		CHARGE_mA_120		CHARGE_mA_140		CHARGE_mA_160
    CHARGE_mA_180		CHARGE_mA_200		CHARGE_mA_220		CHARGE_mA_240
    CHARGE_mA_260		CHARGE_mA_280		CHARGE_mA_300		CHARGE_mA_320
 */
#define TCFG_CHARGE_MA						CHARGE_mA_60

//*********************************************************************************//
//                                  TRANSLATED: LED configuration.
//*********************************************************************************//
#define TCFG_PWMLED_ENABLE					DISABLE_THIS_MOUDLE			// TRANSLATED: Support the PWM LED driver.
#define TCFG_PWMLED_IOMODE					LED_ONE_IO_MODE				// TRANSLATED: LED mode: drive with one GPIO or two.
#define TCFG_PWMLED_PIN						IO_PORTB_06					// TRANSLATED: LED GPIO.
//*********************************************************************************//
//                                  TRANSLATED: Clock configuration.
//*********************************************************************************//
#if CONFIG_PLL_SOURCE_USING_LRC
#define TCFG_CLOCK_SYS_SRC     SYS_CLOCK_INPUT_PLL_RCL   // TRANSLATED: System clock source.
#else
#define TCFG_CLOCK_SYS_SRC     SYS_CLOCK_INPUT_PLL_BT_OSC   // TRANSLATED: System clock source.
#endif
#define TCFG_CLOCK_SYS_HZ					24000000                     // TRANSLATED: System clock frequency.
#define TCFG_CLOCK_OSC_HZ					24000000                     // TRANSLATED: External crystal frequency.
#define TCFG_CLOCK_MODE                     CLOCK_MODE_ADAPTIVE

//*********************************************************************************//
//                                  TRANSLATED: Low-power configuration.
//*********************************************************************************//
#define TCFG_LOWPOWER_POWER_SEL				PWR_LDO15                    // TRANSLATED: Power mode: DCDC or LDO.
#define TCFG_DCDC_PORT_SEL				    NO_CONFIG_PORT               // TRANSLATED: DCDC control pin; used only in DCDC mode.
#define TCFG_LOWPOWER_BTOSC_DISABLE			0                            // TRANSLATED: Whether to retain BTOSC during low power.
#define TCFG_LOWPOWER_LOWPOWER_SEL			SLEEP_EN                     // TRANSLATED: Whether to enter powerdown during sniff.
/* TRANSLATED: Strong VDDIO level options:
    VDDIOM_VOL_20V    VDDIOM_VOL_22V    VDDIOM_VOL_24V    VDDIOM_VOL_26V
    VDDIOM_VOL_30V    VDDIOM_VOL_30V    VDDIOM_VOL_32V    VDDIOM_VOL_36V*/
#define TCFG_LOWPOWER_VDDIOM_LEVEL			VDDIOM_VOL_30V
/* TRANSLATED: Weak VDDIO level options:
    VDDIOW_VOL_21V    VDDIOW_VOL_24V    VDDIOW_VOL_28V    VDDIOW_VOL_32V*/
#define TCFG_LOWPOWER_VDDIOW_LEVEL			VDDIOW_VOL_28V               // TRANSLATED: Weak VDDIO level.
#define TCFG_LOWPOWER_OSC_TYPE              OSC_TYPE_LRC

//*********************************************************************************//
//                                  TRANSLATED: EQ configuration.
//*********************************************************************************//
// TRANSLATED: Online EQ overrides EQ files and modes. With an EQ file, mode switching is disabled by default.
#define TCFG_EQ_ENABLE                            0     // TRANSLATED: Enable EQ.
#define TCFG_EQ_ONLINE_ENABLE                     0     // TRANSLATED: Enable online EQ tuning.
#define TCFG_BT_MUSIC_EQ_ENABLE                   0     // TRANSLATED: Enable Bluetooth music EQ.
#define TCFG_PHONE_EQ_ENABLE                      0     // TRANSLATED: Enable near-end call EQ.

// ONLINE CCONFIG
#define TCFG_ONLINE_ENABLE                        (TCFG_EQ_ONLINE_ENABLE)    // TRANSLATED: Enable online EQ tuning.
#define TCFG_ONLINE_TX_PORT						  IO_PORT_DP                 // TRANSLATED: EQ debug TX pin.
#define TCFG_ONLINE_RX_PORT						  IO_PORT_DM                 // TRANSLATED: EQ debug RX pin.

//*********************************************************************************//
//                                  TRANSLATED: Accelerometer configuration.
//*********************************************************************************//
#define TCFG_GSENSOR_ENABLE                       0     // TRANSLATED: Enable the accelerometer.
#define TCFG_DA230_EN                             0
#define TCFG_SC7A20_EN                            0
#define TCFG_STK8321_EN                           0
#define TCFG_GSENOR_USER_IIC_TYPE                 0     // TRANSLATED: 0: software I2C; 1: hardware I2C.

//*********************************************************************************//
//                                  TRANSLATED: Code-switch configuration.
//*********************************************************************************//
#define TCFG_CODE_SWITCH_ENABLE                   ENABLE_THIS_MOUDLE // TRANSLATED: Enable code-switch support.
#define TCFG_CODE_SWITCH_A_PHASE_PORT             IO_PORTB_06
#define TCFG_CODE_SWITCH_B_PHASE_PORT             IO_PORTB_07

//*********************************************************************************//
//                                  TRANSLATED: System configuration.
//*********************************************************************************//
#define TCFG_AUTO_SHUT_DOWN_TIME		          0   // TRANSLATED: Automatic shutdown timeout when Bluetooth is disconnected.
#define TCFG_SYS_LVD_EN						      1   // TRANSLATED: Enable battery monitoring.
#define TCFG_POWER_ON_NEED_KEY				      0	  // TRANSLATED: Require a key press to power on.
#define TCFG_HID_AUTO_SHUTDOWN_TIME             (0 * 60)      // TRANSLATED: HID inactivity shutdown timeout, in seconds.

//*********************************************************************************//
//                                  TRANSLATED: Bluetooth configuration.
//*********************************************************************************//
#define TCFG_USER_TWS_ENABLE                      0   // TRANSLATED: Enable TWS.
#define TCFG_USER_BLE_ENABLE                      1   // TRANSLATED: Enable BLE.
#define TCFG_USER_EDR_ENABLE                      1   // TRANSLATED: Enable EDR.

#if TCFG_USER_EDR_ENABLE
#define USER_SUPPORT_PROFILE_SPP    0
#define USER_SUPPORT_PROFILE_HFP    0
#define USER_SUPPORT_PROFILE_A2DP   0
#define USER_SUPPORT_PROFILE_AVCTP  0
#define USER_SUPPORT_PROFILE_HID    1
#define USER_SUPPORT_PROFILE_PNP    1
#define USER_SUPPORT_PROFILE_PBAP   0
#endif

#if TCFG_USER_TWS_ENABLE
#define TCFG_BD_NUM						          1   // TRANSLATED: Number of connected devices.
#define TCFG_AUTO_STOP_PAGE_SCAN_TIME             0   // TRANSLATED: In two-device mode, stop page scanning this many minutes after the first device connects.
#else
#define TCFG_BD_NUM						          1   // TRANSLATED: Number of connected devices.
#define TCFG_AUTO_STOP_PAGE_SCAN_TIME             0 // TRANSLATED: In two-device mode, stop page scanning this many minutes after the first device connects.
#endif

#define BT_INBAND_RINGTONE                        0   // TRANSLATED: Play the phone's incoming-call ringtone.
#define BT_PHONE_NUMBER                           0   // TRANSLATED: Announce the incoming caller's number.
#define BT_SUPPORT_DISPLAY_BAT                    0   // TRANSLATED: Enable battery monitoring.
#define BT_SUPPORT_MUSIC_VOL_SYNC                 0   // TRANSLATED: Enable volume synchronization.

//*********************************************************************************//
//                                 TRANSLATED: Power switching configuration.
//*********************************************************************************//

#define CONFIG_PHONE_CALL_USE_LDO15	    1

//*********************************************************************************//
//                                 TRANSLATED: Clock switching configuration.
//*********************************************************************************//
#define CONFIG_BT_NORMAL_HZ	            (24 * 1000000L)

#if TCFG_HID_HOST_ENABLE

#undef TCFG_LOWPOWER_LOWPOWER_SEL
#define TCFG_LOWPOWER_LOWPOWER_SEL			0

#undef TCFG_LOWPOWER_VDDIOM_LEVEL
#define TCFG_LOWPOWER_VDDIOM_LEVEL			VDDIOM_VOL_34V

#undef TCFG_LOWPOWER_VDDIOW_LEVEL
#define TCFG_LOWPOWER_VDDIOW_LEVEL			VDDIOW_VOL_32V

#undef TCFG_USER_EDR_ENABLE
#define     TCFG_USER_EDR_ENABLE    0
#endif

//*********************************************************************************//
//                           TRANSLATED: Enable Yes/No speech recognition.
//*********************************************************************************//
#if TCFG_AUDIO_ENABLE
#define TCFG_KWS_VOICE_RECOGNITION_ENABLE 			 	0 //DISABLE_THIS_MOUDLE
#endif /* #if TCFG_AUDIO_ENABLE */

//*********************************************************************************//
//                                 TRANSLATED: Configuration ends.
//*********************************************************************************//

#endif //CONFIG_BOARD_AC693X_DEMO
#endif //CONFIG_BOARD_AC693X_DEMO_CFG_H

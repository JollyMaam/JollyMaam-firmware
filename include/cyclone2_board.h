#ifndef CYCLONE2_BOARD_H
#define CYCLONE2_BOARD_H

#include "asm/gpio.h"

/*
 * Gamesir Cyclone 2 board map established by live RAM diagnostics.
 *
 * These aliases intentionally use the BR23 SDK's IO_PORTx_xx identifiers.
 * The early recovery path can turn an encoded SDK pin into its per-port bit
 * with CYCLONE2_PIN_BIT() without duplicating the pin number.
 */
#define CYCLONE2_PIN_BIT(pin)       (1U << ((pin) & 15U))

/* Digital buttons; all are active low. */
#define CYCLONE2_BUTTON_A           IO_PORTA_00
#define CYCLONE2_DPAD_UP            IO_PORTA_02
#define CYCLONE2_DPAD_DOWN          IO_PORTA_04
#define CYCLONE2_BUTTON_L4          IO_PORTA_05
#define CYCLONE2_BUTTON_B           IO_PORTA_06
#define CYCLONE2_BUTTON_X           IO_PORTA_07
#define CYCLONE2_DPAD_RIGHT         IO_PORTA_10
#define CYCLONE2_BUTTON_Y           IO_PORTA_13
#define CYCLONE2_BUTTON_START       IO_PORTA_14
#define CYCLONE2_BUTTON_SELECT      IO_PORTA_15

#define CYCLONE2_TRIGGER_L2         IO_PORTB_03
#define CYCLONE2_BUTTON_M           IO_PORTB_07
#define CYCLONE2_DPAD_LEFT          IO_PORTB_08
#define CYCLONE2_TRIGGER_R2         IO_PORTB_10
#define CYCLONE2_BUTTON_HOME        IO_PORTB_11

#define CYCLONE2_BUTTON_R1          IO_PORTC_00
#define CYCLONE2_BUTTON_L3          IO_PORTC_01
#define CYCLONE2_BUTTON_R3          IO_PORTC_02
#define CYCLONE2_BUTTON_R4          IO_PORTC_04
#define CYCLONE2_BUTTON_L1          IO_PORTC_07

#define CYCLONE2_BUTTON_SCREENSHOT  IO_PORTD_05

/* L2/R2 are analog despite sharing GPIO pin names. Values fall on press. */
#define CYCLONE2_TRIGGER_L2_ADC_CHANNEL  6U /* SDK AD_CH_PB3 */
#define CYCLONE2_TRIGGER_R2_ADC_CHANNEL  9U /* SDK AD_CH_PB10 */

/* QMI8658A (write address 0xd4) and 24C64 (write address 0xa0). */
#define CYCLONE2_I2C_SCL            IO_PORTB_04
#define CYCLONE2_I2C_SDA            IO_PORTB_06
#define CYCLONE2_IMU_I2C_ADDRESS    0xd4U
#define CYCLONE2_EEPROM_I2C_ADDRESS 0xa0U

/* PC3 is cable-present high and is also SPI1 port-B's unused DI input. */
#define CYCLONE2_USB_SENSE          IO_PORTC_03

/* Eight one-wire RGB LEDs: PB9 powers the chain, SPI1-B transmits on PC5. */
#define CYCLONE2_LED_POWER          IO_PORTB_09 /* active low */
#define CYCLONE2_LED_SPI_DI         IO_PORTC_03
#define CYCLONE2_LED_SPI_CLOCK      IO_PORTC_04
#define CYCLONE2_LED_SPI_DATA       IO_PORTC_05
#define CYCLONE2_LED_COUNT          8U

/* Home was the sole responder to short/partially decoded transfers, strongly
 * identifying it as the first physical device in the daisy chain. */
#define CYCLONE2_LED_HOME_INDEX     0U

#endif

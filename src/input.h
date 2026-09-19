#ifndef CYCLONE2_INPUT_H
#define CYCLONE2_INPUT_H

#include "generic/typedef.h"
#include "asm/gpio.h"
#include "asm/adc_api.h"

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
#define CYCLONE2_TRIGGER_L2_ADC_CHANNEL  AD_CH_PB3
#define CYCLONE2_TRIGGER_R2_ADC_CHANNEL  AD_CH_PB10

/* The trigger-stop microswitches are not ordinary PA-PD GPIOs. They use the
 * two RTC-domain PR inputs and are active low. SDK APIs number them PR0/PR1;
 * board/physical descriptions sometimes call the same pins PR1/PR2. */
#define CYCLONE2_TRIGGER_L2_SWITCH_PR    1U
#define CYCLONE2_TRIGGER_R2_SWITCH_PR    0U

/* The stick-sensor board transmits four-byte frames on UART2 RX. Channel zero
 * also routes TX to PA2, so firmware restores that shared pin as d-pad up. */
#define CYCLONE2_STICK_UART_RX      IO_PORTA_03
#define CYCLONE2_SENSOR_POWER       IO_PORTB_05 /* active high */

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

enum cyclone2_button {
    /* Keep these bit positions aligned with Linux's mapping of HID Button
     * usages 1..16 to BTN_SOUTH..BTN_DEAD.  SDL can then construct the
     * standard GameController mapping without a device-specific database
     * entry.  BTN_C, BTN_Z, BTN_TL2 and BTN_TR2 have no matching standard
     * digital role here, so use those slots for the four extra controls. */
    CYCLONE2_REPORT_BUTTON_A          = 1U << 0,
    CYCLONE2_REPORT_BUTTON_B          = 1U << 1,
    CYCLONE2_REPORT_BUTTON_SCREENSHOT = 1U << 2,  /* BTN_C */
    CYCLONE2_REPORT_BUTTON_X          = 1U << 3,
    CYCLONE2_REPORT_BUTTON_Y          = 1U << 4,
    CYCLONE2_REPORT_BUTTON_M          = 1U << 5,  /* BTN_Z */
    CYCLONE2_REPORT_BUTTON_L1         = 1U << 6,
    CYCLONE2_REPORT_BUTTON_R1         = 1U << 7,
    CYCLONE2_REPORT_BUTTON_L4         = 1U << 8,  /* BTN_TL2 */
    CYCLONE2_REPORT_BUTTON_R4         = 1U << 9,  /* BTN_TR2 */
    CYCLONE2_REPORT_BUTTON_SELECT     = 1U << 10,
    CYCLONE2_REPORT_BUTTON_START      = 1U << 11,
    CYCLONE2_REPORT_BUTTON_HOME       = 1U << 12,
    CYCLONE2_REPORT_BUTTON_L3         = 1U << 13,
    CYCLONE2_REPORT_BUTTON_R3         = 1U << 14,
};

enum cyclone2_hat {
    CYCLONE2_HAT_UP = 0,
    CYCLONE2_HAT_UP_RIGHT,
    CYCLONE2_HAT_RIGHT,
    CYCLONE2_HAT_DOWN_RIGHT,
    CYCLONE2_HAT_DOWN,
    CYCLONE2_HAT_DOWN_LEFT,
    CYCLONE2_HAT_LEFT,
    CYCLONE2_HAT_UP_LEFT,
    CYCLONE2_HAT_NEUTRAL,
};

struct cyclone2_input_report {
    u16 buttons;
    u8 hat;
    u8 left_x;
    u8 left_y;
    u8 right_x;
    u8 right_y;
    u8 left_trigger;
    u8 right_trigger;
} __attribute__((packed));

struct cyclone2_input_stats {
    u32 dropped_bytes;
    u32 crc_failures;
    u32 overflow_events;
    u32 valid_frames;
    u32 last_axes_timestamp_ms;
};

/* Returns 1..4 for a Home+A/B/X/Y chord captured during early board startup,
 * or zero if no complete chord was present. The result is consumed once. */
u8 cyclone2_take_early_profile_chord(void);

void cyclone2_input_initialize(void);
void cyclone2_input_read(struct cyclone2_input_report *report);
void cyclone2_input_get_stats(struct cyclone2_input_stats *stats);

#endif

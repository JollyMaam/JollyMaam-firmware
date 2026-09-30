#ifndef CYCLONE2_INPUT_H
#define CYCLONE2_INPUT_H

#include "generic/typedef.h"
#include "asm/gpio.h"
#include "asm/adc_api.h"

/* Face buttons are named by the labels printed on the pad, which is
 * Xbox-ordered: A south, B east, X west, Y north (measured with
 * `debug bounce`, 2026-09-29).  Profiles that want Nintendo positions
 * translate there; do not swap these names. */
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

#define CYCLONE2_TRIGGER_L2_ADC_CHANNEL  AD_CH_PB3
#define CYCLONE2_TRIGGER_R2_ADC_CHANNEL  AD_CH_PB10

#define CYCLONE2_TRIGGER_L2_SWITCH_PR    1U
#define CYCLONE2_TRIGGER_R2_SWITCH_PR    0U

#define CYCLONE2_STICK_UART_RX      IO_PORTA_03
#define CYCLONE2_SENSOR_POWER       IO_PORTB_05

#define CYCLONE2_I2C_SCL            IO_PORTB_04
#define CYCLONE2_I2C_SDA            IO_PORTB_06
#define CYCLONE2_IMU_I2C_ADDRESS    0xd4U
#define CYCLONE2_EEPROM_I2C_ADDRESS 0xa0U

#define CYCLONE2_USB_SENSE          IO_PORTC_03

#define CYCLONE2_LED_POWER          IO_PORTB_09
#define CYCLONE2_LED_SPI_DI         IO_PORTC_03
#define CYCLONE2_LED_SPI_CLOCK      IO_PORTC_04
#define CYCLONE2_LED_SPI_DATA       IO_PORTC_05
#define CYCLONE2_LED_COUNT          8U

#define CYCLONE2_LED_HOME_INDEX     0U

enum cyclone2_button {
    /* Keep these bit positions aligned with Linux's mapping of HID Button
     * usages 1..16 to BTN_SOUTH..BTN_DEAD.  SDL can then construct the
     * standard GameController mapping without a device-specific database
     * entry.  BTN_C, BTN_Z, BTN_TL2 and BTN_TR2 have no matching standard
     * digital role here, so use those slots for the four extra controls. */
    CYCLONE2_REPORT_BUTTON_A          = 1U << 0,
    CYCLONE2_REPORT_BUTTON_B          = 1U << 1,
    CYCLONE2_REPORT_BUTTON_SCREENSHOT = 1U << 2,
    CYCLONE2_REPORT_BUTTON_X          = 1U << 3,
    CYCLONE2_REPORT_BUTTON_Y          = 1U << 4,
    CYCLONE2_REPORT_BUTTON_M          = 1U << 5,
    CYCLONE2_REPORT_BUTTON_L1         = 1U << 6,
    CYCLONE2_REPORT_BUTTON_R1         = 1U << 7,
    CYCLONE2_REPORT_BUTTON_L4         = 1U << 8,
    CYCLONE2_REPORT_BUTTON_R4         = 1U << 9,
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

#define CYCLONE2_STICK_MAX    32767
#define CYCLONE2_TRIGGER_MAX  65535U

struct cyclone2_input_report {
    u16 buttons;
    s16 left_x;
    s16 left_y;
    s16 right_x;
    s16 right_y;
    u16 left_trigger;
    u16 right_trigger;
    u8 hat;
    u8 reserved;
};

static inline u8 cyclone2_stick_u8(s16 value)
{
    return (u8)(((s32)value + 32768) >> 8);
}

static inline u16 cyclone2_stick_u16(s16 value)
{
    return (u16)((s32)value + 32768);
}

static inline u16 cyclone2_stick_scaled(s16 value, u16 center, u16 half_range)
{
    s32 scaled = (s32)value * half_range;

    scaled += scaled < 0 ? -(CYCLONE2_STICK_MAX / 2) : CYCLONE2_STICK_MAX / 2;
    return (u16)((s32)center + scaled / CYCLONE2_STICK_MAX);
}

static inline u8 cyclone2_trigger_u8(u16 value)
{
    return (u8)(value >> 8);
}

struct cyclone2_raw_input {
    u16 stick[4];
    u16 trigger[2];
    u8 trigger_stop[2];
    u8 sticks_valid;
};

struct cyclone2_input_stats {
    u32 dropped_bytes;
    u32 crc_failures;
    u32 overflow_events;
    u32 valid_frames;
    u32 last_axes_timestamp_ms;
    u32 dma_interrupts;
    u32 timeout_interrupts;
    u32 max_backlog;
};

u8 cyclone2_take_early_profile_chord(void);

void cyclone2_input_initialize(void);
/* Buttons, d-pad and raw analog values; the report's analog fields are left
 * alone (cyclone2_input_map_analog fills them). */
void cyclone2_input_read(struct cyclone2_input_report *report);
void cyclone2_input_map_analog(struct cyclone2_input_report *report);

void cyclone2_input_get_raw(struct cyclone2_raw_input *raw);
void cyclone2_input_get_stats(struct cyclone2_input_stats *stats);
void cyclone2_input_reset_stats(void);

#endif

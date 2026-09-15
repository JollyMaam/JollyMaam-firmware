#include "asm/gpio.h"
#include "asm/br23.h"
#include "asm/clock.h"

#include "board.h"
#include "cyclone2/input.h"

#define STICK_UART_BAUD             1000000U
#define STICK_UART_FRAME_SIZE       4U
#define STICK_UART_CAPTURE_BYTES    32U
#define STICK_UART_POLL_ITERATIONS  200000U

#define UART_ENABLE                 (1U << 0)
#define UART_TIMEOUT_PENDING_CLEAR  (1U << 10)
#define UART_RX_PENDING_CLEAR       (1U << 12)
#define UART_TX_PENDING_CLEAR       (1U << 13)
#define UART_RX_PENDING             (1U << 14)

#define STICK_TAG_LEFT_X            0xaaU
#define STICK_TAG_LEFT_Y            0x55U
#define STICK_TAG_RIGHT_X           0xccU
#define STICK_TAG_RIGHT_Y           0x33U

#define TRIGGER_DEAD_ZONE           8U
#define TRIGGER_TRAVEL_NUMERATOR    3U
#define TRIGGER_TRAVEL_DENOMINATOR  4U

static u8 stick_frame[STICK_UART_FRAME_SIZE];
static u8 stick_frame_length;
static u16 stick_left_x = 0x8000U;
static u16 stick_left_y = 0x8000U;
static u16 stick_right_x = 0x8000U;
static u16 stick_right_y = 0x8000U;
static u16 trigger_l2_rest;
static u16 trigger_r2_rest;

struct digital_button {
    u32 gpio;
    u16 report_mask;
};

static const struct digital_button buttons[] = {
    {CYCLONE2_BUTTON_A,          CYCLONE2_REPORT_BUTTON_A},
    {CYCLONE2_BUTTON_B,          CYCLONE2_REPORT_BUTTON_B},
    {CYCLONE2_BUTTON_X,          CYCLONE2_REPORT_BUTTON_X},
    {CYCLONE2_BUTTON_Y,          CYCLONE2_REPORT_BUTTON_Y},
    {CYCLONE2_BUTTON_L1,         CYCLONE2_REPORT_BUTTON_L1},
    {CYCLONE2_BUTTON_R1,         CYCLONE2_REPORT_BUTTON_R1},
    {CYCLONE2_BUTTON_L3,         CYCLONE2_REPORT_BUTTON_L3},
    {CYCLONE2_BUTTON_R3,         CYCLONE2_REPORT_BUTTON_R3},
    {CYCLONE2_BUTTON_SELECT,     CYCLONE2_REPORT_BUTTON_SELECT},
    {CYCLONE2_BUTTON_START,      CYCLONE2_REPORT_BUTTON_START},
    {CYCLONE2_BUTTON_HOME,       CYCLONE2_REPORT_BUTTON_HOME},
    {CYCLONE2_BUTTON_SCREENSHOT, CYCLONE2_REPORT_BUTTON_SCREENSHOT},
    {CYCLONE2_BUTTON_L4,         CYCLONE2_REPORT_BUTTON_L4},
    {CYCLONE2_BUTTON_R4,         CYCLONE2_REPORT_BUTTON_R4},
    {CYCLONE2_BUTTON_M,          CYCLONE2_REPORT_BUTTON_M},
};

static const u32 dpad_gpios[] = {
    CYCLONE2_DPAD_UP,
    CYCLONE2_DPAD_RIGHT,
    CYCLONE2_DPAD_DOWN,
    CYCLONE2_DPAD_LEFT,
};

static void configure_active_low_input(u32 gpio)
{
    gpio_set_pull_down(gpio, 0);
    gpio_set_pull_up(gpio, 1);
    gpio_set_die(gpio, 1);
    gpio_direction_input(gpio);
}

static void configure_analog_input(u32 gpio)
{
    gpio_set_pull_down(gpio, 0);
    gpio_set_pull_up(gpio, 0);
    gpio_set_die(gpio, 0);
    gpio_direction_input(gpio);
}

static void sensor_power_enable(void)
{
    gpio_set_pull_down(CYCLONE2_SENSOR_POWER, 0);
    gpio_set_pull_up(CYCLONE2_SENSOR_POWER, 0);
    gpio_set_die(CYCLONE2_SENSOR_POWER, 1);
    gpio_direction_output(CYCLONE2_SENSOR_POWER, 1);
}

static u8 stick_crc8(const u8 *frame)
{
    u8 crc = 0;
    unsigned byte_index;
    unsigned bit_index;

    for (byte_index = 0; byte_index < 3U; byte_index++) {
        crc ^= frame[byte_index];
        for (bit_index = 0; bit_index < 8U; bit_index++) {
            crc = crc & 0x80U ? (u8)((crc << 1) ^ 0x31U) : (u8)(crc << 1);
        }
    }
    return crc;
}

static int stick_tag_is_known(u8 tag)
{
    return tag == STICK_TAG_LEFT_X || tag == STICK_TAG_LEFT_Y ||
           tag == STICK_TAG_RIGHT_X || tag == STICK_TAG_RIGHT_Y;
}

static void stick_accept_frame(const u8 *frame)
{
    u16 value = ((u16)frame[1] << 8) | frame[2];

    switch (frame[0]) {
    case STICK_TAG_LEFT_X:
        stick_left_x = value;
        break;
    case STICK_TAG_LEFT_Y:
        stick_left_y = value;
        break;
    case STICK_TAG_RIGHT_X:
        stick_right_x = value;
        break;
    case STICK_TAG_RIGHT_Y:
        stick_right_y = value;
        break;
    }
}

/* Use a sliding four-byte window so startup in the middle of a frame, or one
 * damaged byte, cannot permanently move the receiver out of phase. */
static void stick_consume_byte(u8 byte)
{
    unsigned index;

    stick_frame[stick_frame_length++] = byte;
    if (stick_frame_length != STICK_UART_FRAME_SIZE) {
        return;
    }
    if (stick_tag_is_known(stick_frame[0]) &&
        stick_crc8(stick_frame) == stick_frame[3]) {
        stick_accept_frame(stick_frame);
        stick_frame_length = 0;
        return;
    }
    for (index = 1; index < STICK_UART_FRAME_SIZE; index++) {
        stick_frame[index - 1U] = stick_frame[index];
    }
    stick_frame_length = STICK_UART_FRAME_SIZE - 1U;
}

static void stick_uart_drain(void)
{
    u32 captured = 0;
    u32 iterations;

    /* Do not burn time when the sensor stream is absent. Once a byte is
     * pending, stay in the proven polled receive loop long enough to collect
     * complete frames at 1 Mbaud. */
    if (!(JL_UART2->CON0 & UART_RX_PENDING)) {
        return;
    }
    for (iterations = 0;
         iterations < STICK_UART_POLL_ITERATIONS &&
         captured < STICK_UART_CAPTURE_BYTES;
         iterations++) {
        if (JL_UART2->CON0 & UART_RX_PENDING) {
            stick_consume_byte((u8)JL_UART2->BUF);
            JL_UART2->CON0 |= UART_RX_PENDING_CLEAR;
            __asm__ volatile("csync");
            captured++;
        }
    }
}

static void stick_uart_initialize(void)
{
    u32 uart_clock = clk_get("uart");
    u32 divisor = ((uart_clock + STICK_UART_BAUD / 2U) /
                   STICK_UART_BAUD) / 4U - 1U;

    JL_UART2->CON0 = UART_TX_PENDING_CLEAR | UART_RX_PENDING_CLEAR |
                     UART_TIMEOUT_PENDING_CLEAR;
    gpio_set_pull_down(CYCLONE2_STICK_UART_RX, 0);
    gpio_set_pull_up(CYCLONE2_STICK_UART_RX, 1);
    gpio_set_die(CYCLONE2_STICK_UART_RX, 1);
    gpio_direction_input(CYCLONE2_STICK_UART_RX);
    gpio_uart_rx_input(CYCLONE2_STICK_UART_RX, 2U, INPUT_CH2);
    JL_UART2->BAUD = divisor;
    JL_UART2->CON0 = UART_TX_PENDING_CLEAR | UART_RX_PENDING_CLEAR |
                     UART_TIMEOUT_PENDING_CLEAR | UART_ENABLE;
}

static u8 stick_axis_to_report(u16 value, int invert)
{
    u8 result = (u8)(value >> 8);

    return invert ? (u8)(0xffU - result) : result;
}

static u8 trigger_to_report(u32 sample, u16 *rest)
{
    u32 travel;
    u32 displacement;

    if (sample > *rest && sample <= 0x3ffU) {
        *rest = (u16)sample;
    }
    if (*rest < 64U || sample + TRIGGER_DEAD_ZONE >= *rest) {
        return 0;
    }
    displacement = *rest - sample - TRIGGER_DEAD_ZONE;
    travel = (*rest * TRIGGER_TRAVEL_NUMERATOR) /
             TRIGGER_TRAVEL_DENOMINATOR;
    if (displacement >= travel) {
        return 0xffU;
    }
    return (u8)((displacement * 0xffU) / travel);
}

static u8 read_hat(void)
{
    u8 up = !gpio_read(CYCLONE2_DPAD_UP);
    u8 right = !gpio_read(CYCLONE2_DPAD_RIGHT);
    u8 down = !gpio_read(CYCLONE2_DPAD_DOWN);
    u8 left = !gpio_read(CYCLONE2_DPAD_LEFT);

    if (up == down) {
        up = 0;
        down = 0;
    }
    if (left == right) {
        left = 0;
        right = 0;
    }

    if (up) {
        if (right) {
            return CYCLONE2_HAT_UP_RIGHT;
        }
        if (left) {
            return CYCLONE2_HAT_UP_LEFT;
        }
        return CYCLONE2_HAT_UP;
    }
    if (down) {
        if (right) {
            return CYCLONE2_HAT_DOWN_RIGHT;
        }
        if (left) {
            return CYCLONE2_HAT_DOWN_LEFT;
        }
        return CYCLONE2_HAT_DOWN;
    }
    if (right) {
        return CYCLONE2_HAT_RIGHT;
    }
    if (left) {
        return CYCLONE2_HAT_LEFT;
    }
    return CYCLONE2_HAT_NEUTRAL;
}

void cyclone2_input_initialize(void)
{
    unsigned index;

    for (index = 0; index < sizeof(buttons) / sizeof(buttons[0]); index++) {
        configure_active_low_input(buttons[index].gpio);
    }
    for (index = 0;
         index < sizeof(dpad_gpios) / sizeof(dpad_gpios[0]);
         index++) {
        configure_active_low_input(dpad_gpios[index]);
    }

    sensor_power_enable();
    configure_analog_input(CYCLONE2_TRIGGER_L2);
    configure_analog_input(CYCLONE2_TRIGGER_R2);
    adc_add_sample_ch(CYCLONE2_TRIGGER_L2_ADC_CHANNEL);
    adc_add_sample_ch(CYCLONE2_TRIGGER_R2_ADC_CHANNEL);
    stick_uart_initialize();
}

void cyclone2_input_read(struct cyclone2_input_report *report)
{
    unsigned index;

    stick_uart_drain();

    report->buttons = 0;
    for (index = 0; index < sizeof(buttons) / sizeof(buttons[0]); index++) {
        if (!gpio_read(buttons[index].gpio)) {
            report->buttons |= buttons[index].report_mask;
        }
    }

    report->hat = read_hat();
    report->left_x = stick_axis_to_report(stick_left_x, 0);
    report->left_y = stick_axis_to_report(stick_left_y, 0);
    report->right_x = stick_axis_to_report(stick_right_x, 1);
    report->right_y = stick_axis_to_report(stick_right_y, 1);
    report->left_trigger = trigger_to_report(
        adc_get_value(CYCLONE2_TRIGGER_L2_ADC_CHANNEL), &trigger_l2_rest);
    report->right_trigger = trigger_to_report(
        adc_get_value(CYCLONE2_TRIGGER_R2_ADC_CHANNEL), &trigger_r2_rest);
}

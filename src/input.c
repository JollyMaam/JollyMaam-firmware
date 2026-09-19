#include <asm/gpio.h>
#include <asm/br23.h>
#include <asm/uart_dev.h>
#include <system/timer.h>

#include "input.h"

#define STICK_UART_DIVISOR          24U
#define STICK_UART_FRAME_SIZE       4U
#define STICK_UART_DMA_BUFFER_SIZE  512U
#define STICK_UART_DMA_FRAME_SIZE   32U
#define STICK_UART_READ_CHUNK       128U

/* Live sensor endpoints with a nominal 0x4000 center. Vertical low endpoints
 * differ slightly on this unit. Keep these explicit until EEPROM/user
 * calibration replaces them. */
#define STICK_LX_RAW_MINIMUM        0x1600U
#define STICK_LX_RAW_CENTER         0x4000U
#define STICK_LX_RAW_MAXIMUM        0x6600U
#define STICK_LY_RAW_MINIMUM        0x1850U
#define STICK_LY_RAW_CENTER         0x4000U
#define STICK_LY_RAW_MAXIMUM        0x6600U
#define STICK_RX_RAW_MINIMUM        0x1600U
#define STICK_RX_RAW_CENTER         0x4000U
#define STICK_RX_RAW_MAXIMUM        0x6600U
#define STICK_RY_RAW_MINIMUM        0x1a40U
#define STICK_RY_RAW_CENTER         0x4000U
#define STICK_RY_RAW_MAXIMUM        0x6600U

#define STICK_TAG_LEFT_X            0xaaU
#define STICK_TAG_LEFT_Y            0x55U
#define STICK_TAG_RIGHT_X           0xccU
#define STICK_TAG_RIGHT_Y           0x33U

#define TRIGGER_DEAD_ZONE           8U
#define TRIGGER_TRAVEL_NUMERATOR    3U
#define TRIGGER_TRAVEL_DENOMINATOR  4U

enum stick_receive_status {
    STICK_RECEIVE_NO_BYTES,
    STICK_RECEIVE_BAD_FRAMES,
    STICK_RECEIVE_VALID_FRAMES,
};

static u8 stick_dma_buffer[STICK_UART_DMA_BUFFER_SIZE] __attribute__((aligned(4)));
static u8 stick_frame[STICK_UART_FRAME_SIZE];
static u8 stick_frame_length;
static u16 stick_left_x = 0x8000U;
static u16 stick_left_y = 0x8000U;
static u16 stick_right_x = 0x8000U;
static u16 stick_right_y = 0x8000U;
static enum stick_receive_status stick_receive_status;
static u16 trigger_l2_rest;
static u16 trigger_r2_rest;
static uart_bus_t *stick_uart;
static volatile u8 stick_uart_work_pending;
static struct cyclone2_input_stats stick_stats;

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

    stick_receive_status = STICK_RECEIVE_VALID_FRAMES;
    stick_stats.valid_frames++;
    stick_stats.last_axes_timestamp_ms = sys_timer_get_ms();
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
    stick_stats.crc_failures++;
    for (index = 1; index < STICK_UART_FRAME_SIZE; index++) {
        stick_frame[index - 1U] = stick_frame[index];
    }
    stick_frame_length = STICK_UART_FRAME_SIZE - 1U;
}

static void stick_uart_isr(void *arg, u32 status)
{
    (void)arg;
    (void)status;
    /* The SDK ISR owns DMA advancement. Defer all parsing to input_read(). */
    stick_uart_work_pending = 1;
}

static void stick_uart_drain(void)
{
    u8 capture[STICK_UART_READ_CHUNK];
    u32 available;
    u32 captured;
    u32 chunk;
    u32 index;

    if (!stick_uart || !stick_uart_work_pending) {
        return;
    }
    /* buf_in/out are the SDK producer/consumer counters. Once the producer is
     * more than one ring ahead, old bytes have been overwritten; retain only
     * the newest complete ring and invalidate parser state. */
    available = stick_uart->kfifo.buf_in - stick_uart->kfifo.buf_out;
    if (available > STICK_UART_DMA_BUFFER_SIZE) {
        stick_stats.dropped_bytes += available - STICK_UART_DMA_BUFFER_SIZE;
        stick_stats.overflow_events++;
        stick_uart->kfifo.buf_out = stick_uart->kfifo.buf_in -
                                    STICK_UART_DMA_BUFFER_SIZE;
        stick_frame_length = 0;
        available = STICK_UART_DMA_BUFFER_SIZE;
        stick_receive_status = STICK_RECEIVE_BAD_FRAMES;
    }
    while (available) {
        chunk = available < sizeof(capture) ? available : sizeof(capture);
        captured = stick_uart->read(capture, chunk, 0);
        for (index = 0; index < captured; index++) {
            if (stick_receive_status == STICK_RECEIVE_NO_BYTES) {
                stick_receive_status = STICK_RECEIVE_BAD_FRAMES;
            }
            stick_consume_byte(capture[index]);
        }
        if (captured == 0) {
            break;
        }
        available -= captured;
    }
    stick_uart_work_pending = stick_uart->kfifo.buf_in !=
                              stick_uart->kfifo.buf_out;
}

static void stick_uart_initialize(void)
{
    struct uart_platform_data_t config = {0};

    config.tx_pin = (u8)-1;
    config.rx_pin = CYCLONE2_STICK_UART_RX;
    config.rx_cbuf = stick_dma_buffer;
    config.rx_cbuf_size = STICK_UART_DMA_BUFFER_SIZE;
    config.frame_length = STICK_UART_DMA_FRAME_SIZE;
    config.rx_timeout = 1U;
    config.isr_cbfun = stick_uart_isr;
    config.baud = 1000000U;
    config.is_9bit = 0;
    stick_uart = (uart_bus_t *)uart_dev_open(&config);
    if (!stick_uart) {
        stick_receive_status = STICK_RECEIVE_BAD_FRAMES;
        return;
    }
    /* The board's measured-good divisor is 24. The SDK's baud calculation is
     * intentionally not used because it produced CRC-invalid data here. */
    /* Match UT2_set_baud(): BAUD is programmed while UART enable is clear,
     * then the prior DMA/IRQ configuration is resumed. */
    JL_UART2->CON0 &= ~BIT(0);
    JL_UART2->BAUD = STICK_UART_DIVISOR;
    __asm__ volatile("csync");
    JL_UART2->CON0 |= BIT(0);
    /* Channel zero pairs PA3 RX with PA2 TX. The sensor only transmits, so
     * restore PA2 as d-pad up after enabling the exact proven UART route. */
    configure_active_low_input(CYCLONE2_DPAD_UP);
    stick_uart_work_pending = 0;
}

void cyclone2_input_get_stats(struct cyclone2_input_stats *stats)
{
    if (stats) {
        *stats = stick_stats;
    }
}

static u8 stick_axis_to_report(u16 value, u16 minimum, u16 center,
                               u16 maximum, int invert)
{
    u32 result;

    if (value <= minimum) {
        result = 0U;
    } else if (value < center) {
        result = ((u32)value - minimum) * 128U / (center - minimum);
    } else if (value < maximum) {
        result = 128U + ((u32)value - center) * 127U /
                        (maximum - center);
    } else {
        result = 0xffU;
    }

    return invert ? (u8)(0xffU - result) : (u8)result;
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
    if (stick_receive_status == STICK_RECEIVE_NO_BYTES) {
        report->left_x = 0x00U;
        report->left_y = 0x00U;
        report->right_x = 0x80U;
        report->right_y = 0x80U;
    } else if (stick_receive_status == STICK_RECEIVE_BAD_FRAMES) {
        report->left_x = 0x40U;
        report->left_y = 0x40U;
        report->right_x = 0x80U;
        report->right_y = 0x80U;
    } else {
        report->left_x = stick_axis_to_report(
            stick_left_x, STICK_LX_RAW_MINIMUM, STICK_LX_RAW_CENTER,
            STICK_LX_RAW_MAXIMUM, 0);
        report->left_y = stick_axis_to_report(
            stick_left_y, STICK_LY_RAW_MINIMUM, STICK_LY_RAW_CENTER,
            STICK_LY_RAW_MAXIMUM, 0);
        report->right_x = stick_axis_to_report(
            stick_right_x, STICK_RX_RAW_MINIMUM, STICK_RX_RAW_CENTER,
            STICK_RX_RAW_MAXIMUM, 1);
        report->right_y = stick_axis_to_report(
            stick_right_y, STICK_RY_RAW_MINIMUM, STICK_RY_RAW_CENTER,
            STICK_RY_RAW_MAXIMUM, 1);
    }
    report->left_trigger = trigger_to_report(
        adc_get_value(CYCLONE2_TRIGGER_L2_ADC_CHANNEL), &trigger_l2_rest);
    report->right_trigger = trigger_to_report(
        adc_get_value(CYCLONE2_TRIGGER_R2_ADC_CHANNEL), &trigger_r2_rest);
}

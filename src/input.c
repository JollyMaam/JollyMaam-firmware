#include <asm/gpio.h>
#include <asm/br23.h>
#include <asm/irq.h>
#include <asm/rtc.h>
#include <asm/clock.h>
#include <system/timer.h>

#include "calibration.h"
#include "input.h"

#define STICK_UART_DIVISOR          24U
#define STICK_UART_FRAME_SIZE       4U
#define STICK_UART_DMA_BUFFER_SIZE  512U
#define STICK_UART_DMA_FRAME_SIZE   32U
#define STICK_UART_RX_TIMEOUT_MS    10U
#define STICK_UART_READ_CHUNK       128U

#define STICK_TAG_LEFT_X            0xaaU
#define STICK_TAG_LEFT_Y            0x55U
#define STICK_TAG_RIGHT_X           0xccU
#define STICK_TAG_RIGHT_Y           0x33U

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
static struct cyclone2_raw_input raw_input;
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

static void configure_trigger_switch(u8 port)
{
    rtc_port_pr_in(port);
    rtc_port_pr_pd(port, 0);
    rtc_port_pr_pu(port, 1);
    rtc_port_pr_die(port, 1);
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

/* UART2 receives into stick_dma_buffer by DMA; the hardware wraps at RXEADR.
 * stick_rx_in counts the bytes the DMA has written, stick_rx_out the bytes
 * consumed, and the buffer size is a power of two.  This is the SDK's
 * UT2_open()/UT2_isr_fun() register sequence for a receive-only channel. */
static volatile u32 stick_rx_in;
static u32 stick_rx_out;

___interrupt
static void stick_uart_isr(void)
{
    u32 received;

    if ((JL_UART2->CON0 & BIT(3)) && (JL_UART2->CON0 & BIT(14))) {
        /* A full DMA frame. */
        JL_UART2->CON0 |= BIT(7);
        JL_UART2->CON0 |= BIT(10);
        JL_UART2->CON0 |= BIT(12);
        (void)JL_UART2->HRXCNT;
        stick_rx_in += STICK_UART_DMA_FRAME_SIZE;
        stick_stats.dma_interrupts++;
        stick_uart_work_pending = 1;
    }
    if ((JL_UART2->CON0 & BIT(5)) && (JL_UART2->CON0 & BIT(11))) {
        /* The line went idle part way into a frame. */
        JL_UART2->CON0 |= BIT(7);
        JL_UART2->CON0 |= BIT(10);
        JL_UART2->CON0 |= BIT(12);
        received = JL_UART2->HRXCNT;
        if (received) {
            stick_rx_in += received;
            stick_stats.timeout_interrupts++;
            stick_uart_work_pending = 1;
        }
    }
}

static u32 stick_uart_read(u8 *buffer, u32 length)
{
    u32 offset = stick_rx_out & (STICK_UART_DMA_BUFFER_SIZE - 1U);
    u32 first = STICK_UART_DMA_BUFFER_SIZE - offset;

    if (first > length) first = length;
    memcpy(buffer, stick_dma_buffer + offset, first);
    memcpy(buffer + first, stick_dma_buffer, length - first);
    stick_rx_out += length;
    return length;
}

static void stick_uart_drain(void)
{
    u8 capture[STICK_UART_READ_CHUNK];
    u32 available;
    u32 captured;
    u32 chunk;
    u32 index;

    if (!stick_uart_work_pending) {
        return;
    }

    available = stick_rx_in - stick_rx_out;
    if (available > stick_stats.max_backlog) {
        stick_stats.max_backlog = available;
    }
    if (available > STICK_UART_DMA_BUFFER_SIZE) {
        stick_stats.dropped_bytes += available - STICK_UART_DMA_BUFFER_SIZE;
        stick_stats.overflow_events++;
        stick_rx_out = stick_rx_in - STICK_UART_DMA_BUFFER_SIZE;
        stick_frame_length = 0;
        available = STICK_UART_DMA_BUFFER_SIZE;
        stick_receive_status = STICK_RECEIVE_BAD_FRAMES;
    }
    while (available) {
        chunk = available < sizeof(capture) ? available : sizeof(capture);
        captured = stick_uart_read(capture, chunk);
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
    stick_uart_work_pending = stick_rx_in != stick_rx_out;
}

static void stick_uart_initialize(void)
{
    /* PA03 is UART2's RX on input channel 0; the pad never transmits. */
    gpio_set_uart2(0);
    JL_UART2->CON0 = BIT(13) | BIT(12) | BIT(10);
    request_irq(IRQ_UART2_IDX, 3, stick_uart_isr, 0);
    stick_rx_in = 0;
    stick_rx_out = 0;
    JL_UART2->RXSADR = (u32)stick_dma_buffer;
    JL_UART2->RXEADR = (u32)(stick_dma_buffer + STICK_UART_DMA_BUFFER_SIZE);
    JL_UART2->RXCNT = STICK_UART_DMA_FRAME_SIZE;
    /* DMA receive, idle timeout interrupt, receive interrupt; 8-bit frames. */
    JL_UART2->CON0 |= BIT(6) | BIT(5) | BIT(3);
    JL_UART2->CON0 &= ~BIT(1);
    /* 1 Mbaud.  The board's measured-good divisor is 24; the SDK's baud
     * calculation produced CRC-invalid data here. */
    JL_UART2->BAUD = STICK_UART_DIVISOR;
    JL_UART2->OTCNT = STICK_UART_RX_TIMEOUT_MS * clk_get("lsb") / 1000U;
    __asm__ volatile("csync");
    JL_UART2->CON0 |= BIT(13) | BIT(12) | BIT(10) | BIT(0);

    configure_active_low_input(CYCLONE2_DPAD_UP);
    stick_uart_work_pending = 0;
}

void cyclone2_input_get_stats(struct cyclone2_input_stats *stats)
{
    if (stats) {
        *stats = stick_stats;
    }
}

void cyclone2_input_reset_stats(void)
{
    local_irq_disable();
    memset(&stick_stats, 0, sizeof(stick_stats));
    local_irq_enable();
}

void cyclone2_input_get_raw(struct cyclone2_raw_input *raw)
{
    *raw = raw_input;
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
    configure_trigger_switch(CYCLONE2_TRIGGER_L2_SWITCH_PR);
    configure_trigger_switch(CYCLONE2_TRIGGER_R2_SWITCH_PR);
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
    report->reserved = 0U;

    raw_input.stick[0] = stick_left_x;
    raw_input.stick[1] = stick_left_y;
    raw_input.stick[2] = stick_right_x;
    raw_input.stick[3] = stick_right_y;
    raw_input.sticks_valid =
        stick_receive_status == STICK_RECEIVE_VALID_FRAMES;
    raw_input.trigger[0] = adc_get_value(CYCLONE2_TRIGGER_L2_ADC_CHANNEL);
    raw_input.trigger[1] = adc_get_value(CYCLONE2_TRIGGER_R2_ADC_CHANNEL);
    raw_input.trigger_stop[0] =
        !rtc_port_pr_read(CYCLONE2_TRIGGER_L2_SWITCH_PR);
    raw_input.trigger_stop[1] =
        !rtc_port_pr_read(CYCLONE2_TRIGGER_R2_SWITCH_PR);
    cyclone2_calibration_apply(&raw_input, report);

    if (stick_receive_status == STICK_RECEIVE_NO_BYTES) {
        report->left_x = -CYCLONE2_STICK_MAX;
        report->left_y = -CYCLONE2_STICK_MAX;
        report->right_x = 0;
        report->right_y = 0;
    } else if (stick_receive_status == STICK_RECEIVE_BAD_FRAMES) {
        report->left_x = -CYCLONE2_STICK_MAX / 2;
        report->left_y = -CYCLONE2_STICK_MAX / 2;
        report->right_x = 0;
        report->right_y = 0;
    }

    if (raw_input.trigger_stop[0]) {
        report->left_trigger = CYCLONE2_TRIGGER_MAX;
    }
    if (raw_input.trigger_stop[1]) {
        report->right_trigger = CYCLONE2_TRIGGER_MAX;
    }
}

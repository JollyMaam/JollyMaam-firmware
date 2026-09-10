#include "system/includes.h"
#include "asm/gpio.h"
#include "asm/power_interface.h"

#include "board.h"
#include "rom.h"

#define PIN(number)              (1U << (number))
#define IOMAP_CON1               (*(volatile u32 *)0x001e511cU)
#define SPI1_PORT_B              PIN(4)
#define SPI1_BASE                0x001e1d00U
#define SPI1_CON                 (*(volatile u32 *)(SPI1_BASE + 0x00U))
#define SPI1_BAUD                (*(volatile u32 *)(SPI1_BASE + 0x04U))
#define SPI1_BUF                 (*(volatile u32 *)(SPI1_BASE + 0x08U))
#define SPI1_ADR                 (*(volatile u32 *)(SPI1_BASE + 0x0cU))
#define SPI1_CNT                 (*(volatile u32 *)(SPI1_BASE + 0x10U))
#define SPI_ENABLE               PIN(0)
#define SPI_PENDING_CLEAR        PIN(14)
#define SPI_PENDING              PIN(15)
#define SPI_CONFIG               0x40b8U
#define LED_SPI_BAUD             5U /* hardware-proven for this board */
#define LED_COUNT                CYCLONE2_LED_COUNT
#define LED_FRAME_BYTES          (LED_COUNT * 24U + 2U)
#define HOME_LED_RED             8U

#define M_DEBOUNCE_SAMPLES       4U
#define CONTROL_POLL_TICKS       2U

static u8 led_dma_buffer[LED_FRAME_BYTES] __attribute__((aligned(4)));

static void encode_led_byte(u8 value, u8 *output)
{
    u32 bit;
    for (bit = 0U; bit < 8U; bit++) {
        output[bit] = (value & PIN(7U - bit)) ? 0xf8U : 0xe0U;
    }
}

static void led_power(u32 enabled)
{
    gpio_set_pull_up(CYCLONE2_LED_POWER, 0);
    gpio_set_pull_down(CYCLONE2_LED_POWER, 0);
    gpio_set_die(CYCLONE2_LED_POWER, 1);
    gpio_direction_output(CYCLONE2_LED_POWER, enabled ? 0 : 1);
}

static void led_initialize(void)
{
    led_power(0U);
    rom_mdelay(10U);
    gpio_set_pull_up(CYCLONE2_LED_SPI_DI, 0);
    gpio_set_pull_down(CYCLONE2_LED_SPI_DI, 0);
    gpio_set_die(CYCLONE2_LED_SPI_DI, 1);
    gpio_set_direction(CYCLONE2_LED_SPI_DI, GPIO_IN);

    gpio_set_pull_up(CYCLONE2_LED_SPI_CLOCK, 0);
    gpio_set_pull_down(CYCLONE2_LED_SPI_CLOCK, 0);
    gpio_set_die(CYCLONE2_LED_SPI_CLOCK, 1);
    gpio_direction_output(CYCLONE2_LED_SPI_CLOCK, 0);

    gpio_set_pull_up(CYCLONE2_LED_SPI_DATA, 0);
    gpio_set_pull_down(CYCLONE2_LED_SPI_DATA, 0);
    gpio_set_die(CYCLONE2_LED_SPI_DATA, 1);
    gpio_direction_output(CYCLONE2_LED_SPI_DATA, 0);

    IOMAP_CON1 |= SPI1_PORT_B;
    SPI1_CON = SPI_CONFIG | SPI_ENABLE;
    SPI1_BAUD = LED_SPI_BAUD;
    SPI1_BUF = 0U;
    SPI1_CON |= SPI_PENDING_CLEAR;
    rom_mdelay(2U);
    led_power(1U);
    rom_mdelay(20U);
}

static void led_set_one(u32 selected, u8 red, u8 green, u8 blue)
{
    u32 led;
    u32 timeout;

    for (led = 0U; led < LED_COUNT; led++) {
        u8 *frame = led_dma_buffer + led * 24U;
        /* The physical byte order is GRB. Every unselected LED is explicitly
         * sent black so stale chain state cannot leave other zones lit. */
        encode_led_byte(led == selected ? green : 0U, frame + 0U);
        encode_led_byte(led == selected ? red : 0U, frame + 8U);
        encode_led_byte(led == selected ? blue : 0U, frame + 16U);
    }
    led_dma_buffer[LED_COUNT * 24U] = 0U;
    led_dma_buffer[LED_COUNT * 24U + 1U] = 0U;
    rom_flush_dcache(led_dma_buffer, sizeof(led_dma_buffer));

    SPI1_CON |= SPI_PENDING_CLEAR;
    SPI1_CON &= ~PIN(12);
    SPI1_ADR = (u32)led_dma_buffer;
    SPI1_CNT = sizeof(led_dma_buffer);
    timeout = 1000000U;
    while ((SPI1_CON & SPI_PENDING) == 0U && timeout != 0U) {
        timeout--;
    }
    SPI1_CON |= SPI_PENDING_CLEAR;
}

static void cyclone2_control_task(void *argument)
{
    u32 armed = 0U;
    u32 pressed_samples = 0U;
    (void)argument;

    gpio_set_pull_down(CYCLONE2_BUTTON_M, 0);
    gpio_set_pull_up(CYCLONE2_BUTTON_M, 1);
    gpio_set_die(CYCLONE2_BUTTON_M, 1);
    gpio_set_direction(CYCLONE2_BUTTON_M, GPIO_IN);

    led_initialize();
    led_set_one(CYCLONE2_LED_HOME_INDEX, HOME_LED_RED, 0U, 0U);

    for (;;) {
        if (gpio_read(CYCLONE2_BUTTON_M)) {
            /* Require an observed release after every boot/wake. */
            armed = 1U;
            pressed_samples = 0U;
        } else if (armed && ++pressed_samples >= M_DEBOUNCE_SAMPLES) {
            armed = 0U;
            pressed_samples = 0U;
            led_set_one(CYCLONE2_LED_HOME_INDEX, 16U, 0U, 16U);
            os_time_dly(100U); /* one second: make the transition observable */
            led_set_one(CYCLONE2_LED_HOME_INDEX, 0U, 0U, 0U);
            os_time_dly(5U);
            led_power(0U);
            power_set_soft_poweroff();
        }
        os_time_dly(CONTROL_POLL_TICKS);
    }
}

void cyclone2_control_start(void)
{
    int error = task_create(cyclone2_control_task, NULL, "c2_control");
    if (error) {
        printf("cyclone2: failed to create control task: %d\n", error);
    }
}

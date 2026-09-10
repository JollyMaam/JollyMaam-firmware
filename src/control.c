#include "system/includes.h"
#include "asm/gpio.h"
#include "asm/wdt.h"

#include "board.h"
#include "control.h"
#include "rom.h"

#define LED_COUNT                CYCLONE2_LED_COUNT
#define LED_FRAME_BYTES          (LED_COUNT * 24U)
#define CONTROL_POLL_TICKS       10U
#define STATUS_LED_RED           8U

static u8 led_dma_buffer[LED_FRAME_BYTES] __attribute__((aligned(4)));

/* CPU-level SDK driver; this SDK revision does not provide a public header. */
extern void led_spi_init(void);
extern void led_spi_rgb_to_24byte(u8 red, u8 green, u8 blue,
                                  u8 *buffer, int index);
extern void led_spi_send_rgbbuf(u8 *buffer, u16 led_count);

static void led_power(u32 enabled)
{
    gpio_set_pull_up(CYCLONE2_LED_POWER, 0);
    gpio_set_pull_down(CYCLONE2_LED_POWER, 0);
    gpio_set_die(CYCLONE2_LED_POWER, 1);
    gpio_direction_output(CYCLONE2_LED_POWER, enabled ? 0 : 1);
}

void cyclone2_led_initialize(void)
{
    led_power(0U);
    rom_mdelay(10U);
    led_spi_init();
    led_power(1U);
    rom_mdelay(20U);
}

void cyclone2_led_set_all(u8 red, u8 green, u8 blue)
{
    u32 led;

    for (led = 0U; led < LED_COUNT; led++) {
        led_spi_rgb_to_24byte(red, green, blue, led_dma_buffer, led);
    }
    rom_flush_dcache(led_dma_buffer, sizeof(led_dma_buffer));
    led_spi_send_rgbbuf(led_dma_buffer, LED_COUNT);
}

static void cyclone2_control_task(void *argument)
{
    (void)argument;

    cyclone2_led_initialize();
    cyclone2_led_set_all(STATUS_LED_RED, 0U, 0U);

    for (;;) {
        wdt_clear();
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

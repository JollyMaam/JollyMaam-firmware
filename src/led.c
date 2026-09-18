#include "system/includes.h"
#include "asm/gpio.h"

#include "input.h"
#include "led.h"
#include "rom.h"

#define LED_COUNT                CYCLONE2_LED_COUNT
#define LED_FRAME_BYTES          (LED_COUNT * 24U)

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

void cyclone2_led_set_home(u8 red, u8 green, u8 blue)
{
    u32 led;

    for (led = 0U; led < LED_COUNT; led++) {
        led_spi_rgb_to_24byte(0U, 0U, 0U, led_dma_buffer, led);
    }
    led_spi_rgb_to_24byte(red, green, blue, led_dma_buffer,
                          CYCLONE2_LED_HOME_INDEX);
    rom_flush_dcache(led_dma_buffer, sizeof(led_dma_buffer));
    led_spi_send_rgbbuf(led_dma_buffer, LED_COUNT);
}

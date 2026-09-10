#include "system/includes.h"
#include "asm/gpio.h"
#include "asm/power_interface.h"
#include "cyclone2_app.h"

/* Cyclone 2 signals established with the RAM diagnostics. */
#define CYCLONE2_START_PIN       IO_PORTA_14
#define CYCLONE2_M_PIN           IO_PORTB_07
#define CYCLONE2_HOME_PIN        IO_PORTB_11
#define CYCLONE2_USB_SENSE_PIN   IO_PORTC_03

#define GPIO_PORT_A_BASE         0x001e5000U
#define GPIO_PORT_B_BASE         0x001e5040U
#define GPIO_PORT_C_BASE         0x001e5080U
#define GPIO_OUT(base)           (*(volatile u32 *)((base) + 0x00U))
#define GPIO_IN(base)            (*(volatile u32 *)((base) + 0x04U))
#define GPIO_DIR(base)           (*(volatile u32 *)((base) + 0x08U))
#define GPIO_DIE(base)           (*(volatile u32 *)((base) + 0x0cU))
#define GPIO_PU(base)            (*(volatile u32 *)((base) + 0x10U))
#define GPIO_PD(base)            (*(volatile u32 *)((base) + 0x14U))
#define PIN(number)              (1U << (number))

#define START_BUTTON_BIT         PIN(14)
#define HOME_BUTTON_BIT          PIN(11)
#define USB_SENSE_BIT            PIN(3)

#define USB_CON0                 (*(volatile u32 *)0x001e1800U)
#define USB_CON1                 (*(volatile u32 *)0x001e1804U)
#define USB_IO_CON0              (*(volatile u32 *)0x001e5100U)
#define USB_FORCE_SE0            PIN(11)

#define LED_POWER_BIT            PIN(9) /* PB9, active low */
#define LED_SPI_DI_BIT           PIN(3) /* PC3 */
#define LED_SPI_CLK_BIT          PIN(4) /* PC4 */
#define LED_SPI_DO_BIT           PIN(5) /* PC5 */
#define LED_SPI_BITS             (LED_SPI_DI_BIT | LED_SPI_CLK_BIT | LED_SPI_DO_BIT)
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
#define LED_SPI_BAUD             2U /* 24 MHz SDK clock; baud 5 was for ~48 MHz */
#define LED_COUNT                8U
#define LED_FRAME_BYTES          (LED_COUNT * 24U + 2U)

#define UBOOT_BOOT_STATE         2U
#define BOOT_DEBOUNCE_SAMPLES    8U
#define BOOT_DEBOUNCE_MS         20U
#define M_DEBOUNCE_SAMPLES       4U
#define CONTROL_POLL_TICKS       2U

static u8 led_dma_buffer[LED_FRAME_BYTES] __attribute__((aligned(4)));

/* Calls from flash cannot reach these MaskROM stubs with PI32's limited-range
 * direct CALL instruction. Volatile pointers force full-address calls. */
static void rom_mdelay(u32 milliseconds)
{
    void (* volatile function)(u32) = (void (*)(u32))0x00111524U;
    function(milliseconds);
}

static void rom_flush_dcache(const void *address, u32 size)
{
    void (* volatile function)(const void *, u32) =
        (void (*)(const void *, u32))0x001127ccU;
    function(address, size);
}

static void rom_nvram_set_boot_state(u32 state)
{
    void (* volatile function)(u32) = (void (*)(u32))0x001127f0U;
    function(state);
}

static void rom_chip_reset(void)
{
    void (* volatile function)(void) = (void (*)(void))0x001127f4U;
    function();
}

static void configure_early_input(u32 base, u32 bit, u32 pull_up)
{
    GPIO_PD(base) &= ~bit;
    if (pull_up) {
        GPIO_PU(base) |= bit;
    } else {
        GPIO_PU(base) &= ~bit;
    }
    GPIO_DIE(base) |= bit;
    GPIO_DIR(base) |= bit;
}

static void encode_led_byte(u8 value, u8 *output)
{
    u32 bit;
    for (bit = 0U; bit < 8U; bit++) {
        output[bit] = (value & PIN(7U - bit)) ? 0xf8U : 0xe0U;
    }
}

static void led_power(u32 enabled)
{
    GPIO_PU(GPIO_PORT_B_BASE) &= ~LED_POWER_BIT;
    GPIO_PD(GPIO_PORT_B_BASE) &= ~LED_POWER_BIT;
    GPIO_DIE(GPIO_PORT_B_BASE) |= LED_POWER_BIT;
    if (enabled) {
        GPIO_OUT(GPIO_PORT_B_BASE) &= ~LED_POWER_BIT;
    } else {
        GPIO_OUT(GPIO_PORT_B_BASE) |= LED_POWER_BIT;
    }
    GPIO_DIR(GPIO_PORT_B_BASE) &= ~LED_POWER_BIT;
}

static void led_initialize(void)
{
    led_power(0U);
    rom_mdelay(10U);
    GPIO_PU(GPIO_PORT_C_BASE) &= ~LED_SPI_BITS;
    GPIO_PD(GPIO_PORT_C_BASE) &= ~LED_SPI_BITS;
    GPIO_DIE(GPIO_PORT_C_BASE) |= LED_SPI_BITS;
    GPIO_DIR(GPIO_PORT_C_BASE) |= LED_SPI_DI_BIT;
    GPIO_DIR(GPIO_PORT_C_BASE) &= ~(LED_SPI_CLK_BIT | LED_SPI_DO_BIT);
    GPIO_OUT(GPIO_PORT_C_BASE) &= ~(LED_SPI_CLK_BIT | LED_SPI_DO_BIT);

    IOMAP_CON1 |= SPI1_PORT_B;
    SPI1_CON = SPI_CONFIG | SPI_ENABLE;
    SPI1_BAUD = LED_SPI_BAUD;
    SPI1_BUF = 0U;
    SPI1_CON |= SPI_PENDING_CLEAR;
    rom_mdelay(2U);
    led_power(1U);
    rom_mdelay(20U);
}

static void led_set_all(u8 red, u8 green, u8 blue)
{
    u32 led;
    u32 timeout;

    for (led = 0U; led < LED_COUNT; led++) {
        u8 *frame = led_dma_buffer + led * 24U;
        encode_led_byte(green, frame + 0U); /* physical order is GRB */
        encode_led_byte(red, frame + 8U);
        encode_led_byte(blue, frame + 16U);
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

static u32 early_recovery_condition(void)
{
    u32 sample;

    for (sample = 0; sample < BOOT_DEBOUNCE_SAMPLES; sample++) {
        if ((GPIO_IN(GPIO_PORT_A_BASE) & START_BUTTON_BIT) != 0U ||
            (GPIO_IN(GPIO_PORT_B_BASE) & HOME_BUTTON_BIT) != 0U ||
            (GPIO_IN(GPIO_PORT_C_BASE) & USB_SENSE_BIT) == 0U) {
            return 0U;
        }
        rom_mdelay(BOOT_DEBOUNCE_MS);
    }
    return 1U;
}

__attribute__((noreturn))
static void enter_maskrom_uboot(void)
{
    /* Force a real host-visible USB detach before resetting. A CPU reset by
     * itself leaves enough USB state behind that MaskROM often cannot
     * enumerate until the cable is physically removed. */
    USB_CON0 = 0U;
    USB_CON1 = 0U;
    USB_IO_CON0 = USB_FORCE_SE0;
    rom_mdelay(1000U);
    USB_IO_CON0 = 0U;
    rom_mdelay(250U);

    /* Do not commit to UBOOT if VBUS disappeared during the detach window. */
    while ((GPIO_IN(GPIO_PORT_C_BASE) & USB_SENSE_BIT) == 0U) {
        rom_mdelay(100U);
    }

    rom_nvram_set_boot_state(UBOOT_BOOT_STATE);
    rom_chip_reset();
    for (;;) {
    }
}

/* Called immediately after the SDK has initialized RAM, before port_init().
 * The same path executes after a soft-off wake/reset, so Home+Start works at
 * both cold boot and wake time. */
void cyclone2_early_boot(void)
{
    configure_early_input(GPIO_PORT_A_BASE, START_BUTTON_BIT, 1U);
    configure_early_input(GPIO_PORT_B_BASE, HOME_BUTTON_BIT, 1U);
    configure_early_input(GPIO_PORT_C_BASE, USB_SENSE_BIT, 0U);

    if (early_recovery_condition()) {
        enter_maskrom_uboot();
    }
}

static void cyclone2_control_task(void *argument)
{
    u32 armed = 0U;
    u32 pressed_samples = 0U;
    (void)argument;

    gpio_set_pull_down(CYCLONE2_M_PIN, 0);
    gpio_set_pull_up(CYCLONE2_M_PIN, 1);
    gpio_set_die(CYCLONE2_M_PIN, 1);
    gpio_set_direction(CYCLONE2_M_PIN, 1);

    led_initialize();
    led_set_all(32U, 0U, 0U); /* red: SDK control task is running */

    for (;;) {
        if (gpio_read(CYCLONE2_M_PIN)) {
            /* Require an observed release after every boot/wake. */
            armed = 1U;
            pressed_samples = 0U;
        } else if (armed && ++pressed_samples >= M_DEBOUNCE_SAMPLES) {
            armed = 0U;
            pressed_samples = 0U;
            led_set_all(32U, 0U, 32U); /* magenta: soft-off accepted */
            os_time_dly(100U); /* one second: make the transition observable */
            led_set_all(0U, 0U, 0U);
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

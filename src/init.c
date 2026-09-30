#include <asm/charge.h>
#include <asm/power/p33.h>
#include <asm/power/power_port.h>
#include <media/includes.h>
#include <system/includes.h>

#include "app_config.h"
#include "btstack_jl.h"
#include "input.h"
#include "config.h"

#define LOG_TAG_CONST       BOARD
#define LOG_TAG             "[BOARD]"
#define LOG_ERROR_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_INFO_ENABLE

#define LOG_CLI_ENABLE
#include "debug.h"

void board_power_init(void);

static u8 cyclone2_early_profile_chord;
static u8 cyclone2_early_uboot_chord;

static void cyclone2_prepare_early_button(u32 gpio)
{
    gpio_set_pull_down(gpio, 0);
    gpio_set_pull_up(gpio, 1);
    gpio_set_die(gpio, 1);
    gpio_direction_input(gpio);
}

void cyclone2_latch_early_chords(void)
{
    u8 chord = 0U;

    cyclone2_prepare_early_button(CYCLONE2_BUTTON_HOME);
    cyclone2_prepare_early_button(CYCLONE2_BUTTON_A);
    cyclone2_prepare_early_button(CYCLONE2_BUTTON_B);
    cyclone2_prepare_early_button(CYCLONE2_BUTTON_X);
    cyclone2_prepare_early_button(CYCLONE2_BUTTON_Y);
    cyclone2_prepare_early_button(CYCLONE2_BUTTON_START);

    if (gpio_read(CYCLONE2_BUTTON_HOME)) return;
    if (!gpio_read(CYCLONE2_BUTTON_START)) cyclone2_early_uboot_chord = 1U;
    if (!gpio_read(CYCLONE2_BUTTON_A)) chord = 1U;
    if (!gpio_read(CYCLONE2_BUTTON_B)) chord = chord ? 0xffU : 2U;
    if (!gpio_read(CYCLONE2_BUTTON_X)) chord = chord ? 0xffU : 3U;
    if (!gpio_read(CYCLONE2_BUTTON_Y)) chord = chord ? 0xffU : 4U;
    if (chord != 0xffU) cyclone2_early_profile_chord = chord;
}

u8 cyclone2_take_early_uboot_chord(void)
{
    u8 chord = cyclone2_early_uboot_chord;
    cyclone2_early_uboot_chord = 0U;
    return chord;
}

u8 cyclone2_take_early_profile_chord(void)
{
    u8 chord = cyclone2_early_profile_chord;
    cyclone2_early_profile_chord = 0U;
    return chord;
}

const struct low_power_param power_param = {
    .config         = TCFG_LOWPOWER_LOWPOWER_SEL,
    .btosc_hz       = TCFG_CLOCK_OSC_HZ,
    .delay_us       = TCFG_CLOCK_SYS_HZ / 1000000L,
    .btosc_disable  = TCFG_LOWPOWER_BTOSC_DISABLE,
    .vddiom_lev     = TCFG_LOWPOWER_VDDIOM_LEVEL,
    .vddiow_lev     = TCFG_LOWPOWER_VDDIOW_LEVEL,
    .osc_type       = OSC_TYPE_LRC,
    .dcdc_port      = TCFG_DCDC_PORT_SEL,
};

void board_init()
{
    cyclone2_latch_early_chords();

    config_init();
    board_power_init();
    adc_vbg_init();
    adc_init();
    devices_init();

    CHARGE_EN(0);
    CHGBG_EN(0);

    power_set_mode(PWR_LDO15);
}

struct port_wakeup port0 = {
    .pullup_down_enable = ENABLE,
    .edge               = FALLING_EDGE,
    .attribute          = BLUETOOTH_RESUME,
    .iomap              = IO_PORTB_11,
    .filter_enable      = ENABLE,
};

/* PC3 is the cable-detect input after the USB controller and LED driver have
 * been shut down for soft power-off. It shares the LED SPI input at runtime. */
struct port_wakeup usb_port = {
    .pullup_down_enable = DISABLE,
    .edge               = RISING_EDGE,
    .attribute          = BLUETOOTH_RESUME,
    .iomap              = CYCLONE2_USB_SENSE,
    .filter_enable      = ENABLE,
};

const struct sub_wakeup sub_wkup = {
    .attribute  = BLUETOOTH_RESUME,
};

const struct charge_wakeup charge_wkup = {
    .attribute  = BLUETOOTH_RESUME,
};

const struct wakeup_param wk_param = {
    .port[1] = &port0,
    .port[2] = &usb_port,
    .sub = &sub_wkup,
    .charge = &charge_wkup,
};

static void close_gpio(u8 is_softoff)
{
    u16 port_group[] = {
        [PORTA_GROUP] = 0xffff & ~(BIT(13)|BIT(14)),
        [PORTB_GROUP] = 0xffff,
        [PORTC_GROUP] = 0xffff,
    };

    gpio_dir(GPIOA, 0, 16, port_group[PORTA_GROUP], GPIO_OR);
    gpio_set_pu(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_set_pd(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_die(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_dieh(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);

    gpio_dir(GPIOB, 0, 16, port_group[PORTB_GROUP], GPIO_OR);
    gpio_set_pu(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_set_pd(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_die(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_dieh(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);

    gpio_dir(GPIOC, 0, 16, port_group[PORTC_GROUP], GPIO_OR);
    gpio_set_pu(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_set_pd(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_die(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_dieh(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);

    usb_iomode(1);
}

void board_set_soft_poweroff(void)
{
    u16 port_group[] = {
        [PORTA_GROUP] = 0xffff,
        [PORTB_GROUP] = 0xffff,
        [PORTC_GROUP] = 0xffff,
        [PORTD_GROUP] = 0Xffff,
    };

    if(spi_get_port()==0){
        port_protect(port_group, SPI0_PWR_A);
        port_protect(port_group, SPI0_CS_A);
        port_protect(port_group, SPI0_CLK_A);
        port_protect(port_group, SPI0_DO_D0_A);
        port_protect(port_group, SPI0_DI_D1_A);
        if(get_sfc_bit_mode()==4){
            port_protect(port_group, SPI0_WP_D2_A);
            port_protect(port_group, SPI0_HOLD_D3_A);
        }
    }else{
        port_protect(port_group, SPI0_PWR_B);
        port_protect(port_group, SPI0_CS_B);
        port_protect(port_group, SPI0_CLK_B);
        port_protect(port_group, SPI0_DO_D0_B);
        port_protect(port_group, SPI0_DI_D1_B);
        if(get_sfc_bit_mode()==4){
            port_protect(port_group, SPI0_WP_D2_B);
            port_protect(port_group, SPI0_HOLD_D3_B);
        }
    }

    port_protect(port_group, CYCLONE2_BUTTON_HOME);
    port_protect(port_group, CYCLONE2_BUTTON_A);
    port_protect(port_group, CYCLONE2_BUTTON_B);
    port_protect(port_group, CYCLONE2_BUTTON_X);
    port_protect(port_group, CYCLONE2_BUTTON_Y);
    port_protect(port_group, CYCLONE2_USB_SENSE);

    gpio_dir(GPIOA, 0, 16, port_group[PORTA_GROUP], GPIO_OR);
    gpio_set_pu(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_set_pd(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_die(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_dieh(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);

    gpio_dir(GPIOB, 0, 16, port_group[PORTB_GROUP], GPIO_OR);
    gpio_set_pu(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_set_pd(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_die(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_dieh(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);

    gpio_dir(GPIOC, 0, 16, port_group[PORTC_GROUP], GPIO_OR);
    gpio_set_pu(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_set_pd(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_die(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_dieh(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);

    gpio_dir(GPIOD, 0, 16, port_group[PORTD_GROUP], GPIO_OR);
    gpio_set_pu(GPIOD, 0, 16, ~port_group[PORTD_GROUP], GPIO_AND);
    gpio_set_pd(GPIOD, 0, 16, ~port_group[PORTD_GROUP], GPIO_AND);
    gpio_die(GPIOD, 0, 16, ~port_group[PORTD_GROUP], GPIO_AND);
    gpio_dieh(GPIOD, 0, 16, ~port_group[PORTD_GROUP], GPIO_AND);

    usb_iomode(1);

    gpio_set_pull_up(IO_PORT_DP, 0);
    gpio_set_pull_down(IO_PORT_DP, 0);
    gpio_set_direction(IO_PORT_DP, 1);
    gpio_set_die(IO_PORT_DP, 0);
    gpio_set_dieh(IO_PORT_DP, 0);

    gpio_set_pull_up(IO_PORT_DM, 0);
    gpio_set_pull_down(IO_PORT_DM, 0);
    gpio_set_direction(IO_PORT_DM, 1);
    gpio_set_die(IO_PORT_DM, 0);
    gpio_set_dieh(IO_PORT_DM, 0);

    gpio_set_pull_up(CYCLONE2_USB_SENSE, 0);
    gpio_set_pull_down(CYCLONE2_USB_SENSE, 0);
    gpio_direction_input(CYCLONE2_USB_SENSE);
    gpio_set_die(CYCLONE2_USB_SENSE, 1);
}

#define     APP_IO_DEBUG_0(i,x)
#define     APP_IO_DEBUG_1(i,x)

void sleep_exit_callback(u32 usec)
{
    putchar('>');
    APP_IO_DEBUG_1(A, 6);
}

void sleep_enter_callback(u8  step)
{

    if (step == 1) {
        putchar('<');
        APP_IO_DEBUG_0(A, 6);
    } else {
        close_gpio(0);

        gpio_set_pull_up(IO_PORT_DP, 0);
        gpio_set_pull_down(IO_PORT_DP, 0);
        gpio_set_direction(IO_PORT_DP, 1);
        gpio_set_die(IO_PORT_DP, 0);

        gpio_set_pull_up(IO_PORT_DM, 0);
        gpio_set_pull_down(IO_PORT_DM, 0);
        gpio_set_direction(IO_PORT_DM, 1);
        gpio_set_die(IO_PORT_DM, 0);
    }
}

void board_power_init(void)
{
    log_info("Power init : %s", __FILE__);
    power_init(&power_param);
    gpio_longpress_pin0_reset_config(IO_PORTB_01, 0, 0);
    gpio_shortpress_reset_config(0);
    power_set_callback(TCFG_LOWPOWER_LOWPOWER_SEL, sleep_enter_callback, sleep_exit_callback, board_set_soft_poweroff);
    power_keep_dacvdd_en(0);
    power_wakeup_init(&wk_param);
}

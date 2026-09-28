#include <asm/charge.h>
#include <asm/power/p33.h>
#include <asm/power/power_port.h>
#include <media/includes.h>
#include <system/includes.h>

#include "app_config.h"
#include "btstack_jl.h"
#include "input.h"
#include "config.h"
#include "identity.h"

#define LOG_TAG_CONST       BOARD
#define LOG_TAG             "[BOARD]"
#define LOG_ERROR_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_INFO_ENABLE
/* #define LOG_DUMP_ENABLE */
#define LOG_CLI_ENABLE
#include "debug.h"

void board_power_init(void);

static u8 cyclone2_early_profile_chord;

static void cyclone2_prepare_early_button(u32 gpio)
{
    gpio_set_pull_down(gpio, 0);
    gpio_set_pull_up(gpio, 1);
    gpio_set_die(gpio, 1);
    gpio_direction_input(gpio);
}

void cyclone2_latch_early_profile_chord(void)
{
    u8 chord = 0U;

    cyclone2_prepare_early_button(CYCLONE2_BUTTON_HOME);
    cyclone2_prepare_early_button(CYCLONE2_BUTTON_A);
    cyclone2_prepare_early_button(CYCLONE2_BUTTON_B);
    cyclone2_prepare_early_button(CYCLONE2_BUTTON_X);
    cyclone2_prepare_early_button(CYCLONE2_BUTTON_Y);

    if (gpio_read(CYCLONE2_BUTTON_HOME)) return;
    if (!gpio_read(CYCLONE2_BUTTON_A)) chord = 1U;
    if (!gpio_read(CYCLONE2_BUTTON_B)) chord = chord ? 0xffU : 2U;
    if (!gpio_read(CYCLONE2_BUTTON_X)) chord = chord ? 0xffU : 3U;
    if (!gpio_read(CYCLONE2_BUTTON_Y)) chord = chord ? 0xffU : 4U;
    if (chord != 0xffU) cyclone2_early_profile_chord = chord;
}

u8 cyclone2_take_early_profile_chord(void)
{
    u8 chord = cyclone2_early_profile_chord;
    cyclone2_early_profile_chord = 0U;
    return chord;
}

const struct low_power_param power_param = {
    .config         = TCFG_LOWPOWER_LOWPOWER_SEL,          // TRANSLATED: 0: stay active during sniff; 1: enter powerdown during sniff.
    .btosc_hz       = TCFG_CLOCK_OSC_HZ,                   // TRANSLATED: External crystal frequency.
    .delay_us       = TCFG_CLOCK_SYS_HZ / 1000000L,        // TRANSLATED: Delay parameter for the low-power module; normally no adjustment is needed.
    .btosc_disable  = TCFG_LOWPOWER_BTOSC_DISABLE,         // TRANSLATED: Whether to retain BTOSC during low power.
    .vddiom_lev     = TCFG_LOWPOWER_VDDIOM_LEVEL,          // TRANSLATED: Strong VDDIO levels: 2.0, 2.2, 2.4, 2.6, 2.8, 3.0, 3.2, or 3.6 V.
    .vddiow_lev     = TCFG_LOWPOWER_VDDIOW_LEVEL,          // TRANSLATED: Weak VDDIO levels: 2.1, 2.4, 2.8, or 3.2 V.
    .osc_type       = OSC_TYPE_LRC,
    .dcdc_port      = TCFG_DCDC_PORT_SEL,
};

void board_init()
{
    cyclone2_latch_early_profile_chord();
    /* The settings, read from flash before the radio ever runs: nothing
     * reads syscfg on the Bluetooth paths. */
    config_init();
    /* Let the Bluetooth stack persist link keys; without this the remote
     * device database is a silent no-op and reconnection cannot work. */
    cyclone2_remote_db_init();
    board_power_init();
    adc_vbg_init();
    adc_init();
    devices_init();

    /*close FAST CHARGE */
    CHARGE_EN(0);
    CHGBG_EN(0);

    power_set_mode(PWR_LDO15);
}

struct port_wakeup port0 = {
    .pullup_down_enable = ENABLE,                            // TRANSLATED: Enable internal I/O pull-up/down resistors.
    .edge               = FALLING_EDGE,                      // TRANSLATED: Wakeup edge: rising or falling.
    .attribute          = BLUETOOTH_RESUME,                  // TRANSLATED: Reserved parameter.
    .iomap              = IO_PORTB_11,                       // Cyclone 2 Home
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
    .sub = &sub_wkup,
    .charge = &charge_wkup,
};

/* TRANSLATED: By default, set I/O pins to high impedance before soft poweroff. Modify this function to preserve selected pin states. */
static void close_gpio(u8 is_softoff)
{
    u16 port_group[] = {
        [PORTA_GROUP] = 0xffff & ~(BIT(13)|BIT(14)),
        [PORTB_GROUP] = 0xffff,
        [PORTC_GROUP] = 0xffff,
    };

    //< close gpio
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

    //< close usb io
    usb_iomode(1);
}

//-----------------------------------------------

/* TRANSLATED: By default, set I/O pins to high impedance before soft poweroff. Modify this function to preserve selected pin states. */
void board_set_soft_poweroff(void)
{
    u16 port_group[] = {
        [PORTA_GROUP] = 0xffff,
        [PORTB_GROUP] = 0xffff,
        [PORTC_GROUP] = 0xffff,
        [PORTD_GROUP] = 0Xffff,
    };

    // TRANSLATED: Flash power.
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

    /* Keep the wake chord electrically readable from the first instruction
     * after Home wakes the chip. cyclone2_input_initialize() has already made
     * these active-low inputs with pull-ups. */
    port_protect(port_group, CYCLONE2_BUTTON_HOME);
    port_protect(port_group, CYCLONE2_BUTTON_A);
    port_protect(port_group, CYCLONE2_BUTTON_B);
    port_protect(port_group, CYCLONE2_BUTTON_X);
    port_protect(port_group, CYCLONE2_BUTTON_Y);

    //< close gpio
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
}

#define     APP_IO_DEBUG_0(i,x)       //{JL_PORT##i->DIR &= ~BIT(x), JL_PORT##i->OUT &= ~BIT(x);}
#define     APP_IO_DEBUG_1(i,x)       //{JL_PORT##i->DIR &= ~BIT(x), JL_PORT##i->OUT |= BIT(x);}

void sleep_exit_callback(u32 usec)
{
    putchar('>');
    APP_IO_DEBUG_1(A, 6);
}

void sleep_enter_callback(u8  step)
{
    /* TRANSLATED: Do not add debug prints in this function. */
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
    gpio_shortpress_reset_config(0);//1--enable 0--disable
    power_set_callback(TCFG_LOWPOWER_LOWPOWER_SEL, sleep_enter_callback, sleep_exit_callback, board_set_soft_poweroff);
    power_keep_dacvdd_en(0);
    power_wakeup_init(&wk_param);
}

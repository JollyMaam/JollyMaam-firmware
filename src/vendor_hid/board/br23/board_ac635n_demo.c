#include "app_config.h"

#ifdef CONFIG_BOARD_AC635N_DEMO

#include "system/includes.h"
#include "asm/charge.h"
#include "device/key_driver.h"
#include "asm/power/p33.h"
#include "asm/iic_soft.h"
#include "usb/otg.h"
#include "asm/power/power_port.h"
#include "board.h"

#include "media/includes.h"

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

static void cyclone2_latch_early_profile_chord(void)
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

const struct soft_iic_config soft_iic_cfg[] = {
    //iic0 data
    {
        .scl = IO_PORTC_04,                    // TRANSLATED: I2C clock pin.
        .sda = IO_PORTC_05,                   // TRANSLATED: I2C data pin.
        .delay = 50,                // TRANSLATED: Software I2C delay; controls the bus clock frequency.
        .io_pu = 1,                                     // TRANSLATED: Enable internal pull-ups if the board has no external pull-up resistors.
    },
};

/************************** LOW POWER config ****************************/
const struct low_power_param power_param = {
    .config         = TCFG_LOWPOWER_LOWPOWER_SEL,          // TRANSLATED: 0: stay active during sniff; 1: enter powerdown during sniff.
    .btosc_hz         = TCFG_CLOCK_OSC_HZ,                   // TRANSLATED: External crystal frequency.
    .delay_us       = TCFG_CLOCK_SYS_HZ / 1000000L,        // TRANSLATED: Delay parameter for the low-power module; normally no adjustment is needed.
    .btosc_disable  = TCFG_LOWPOWER_BTOSC_DISABLE,         // TRANSLATED: Whether to retain BTOSC during low power.
    .vddiom_lev     = TCFG_LOWPOWER_VDDIOM_LEVEL,          // TRANSLATED: Strong VDDIO levels: 2.0, 2.2, 2.4, 2.6, 2.8, 3.0, 3.2, or 3.6 V.
    .vddiow_lev     = TCFG_LOWPOWER_VDDIOW_LEVEL,          // TRANSLATED: Weak VDDIO levels: 2.1, 2.4, 2.8, or 3.2 V.
    .osc_type       = OSC_TYPE_LRC,
    .dcdc_port      = TCFG_DCDC_PORT_SEL,
};

/************************** KEY MSG****************************/
/* TRANSLATED: Key messages. When USE_CONFIG_KEY_SETTING is enabled in USER_CFG, populate this structure from the configuration file. */
u8 key_table[KEY_NUM_MAX][KEY_EVENT_MAX] = {
    // SHORT           LONG              HOLD              UP              DOUBLE           TRIPLE
};

// *INDENT-OFF*
/************************** UART config****************************/
UART0_PLATFORM_DATA_BEGIN(uart0_data)
    .tx_pin = TCFG_UART0_TX_PORT,                             // TRANSLATED: Debug UART TX pin.
    .rx_pin = TCFG_UART0_RX_PORT,                             // TRANSLATED: Debug UART RX pin.
    .baudrate = TCFG_UART0_BAUDRATE,                          // TRANSLATED: UART baud rate.

    .flags = UART_DEBUG,                                      // TRANSLATED: Use UART_DEBUG when the UART carries debug prints.
UART0_PLATFORM_DATA_END()

/************************** CHARGE config****************************/

    /************************** AD KEY ****************************/
const struct adkey_platform_data adkey_data = {
    .enable = TCFG_ADKEY_ENABLE,                              // TRANSLATED: Enable ADC keys.
    .adkey_pin = TCFG_ADKEY_PORT,                             // TRANSLATED: ADC key pin.
    .ad_channel = TCFG_ADKEY_AD_CHANNEL,                      // TRANSLATED: ADC channel.
    .extern_up_en = TCFG_ADKEY_EXTERN_UP_ENABLE,              // TRANSLATED: Use an external pull-up resistor.
    .ad_value = {                                             // TRANSLATED: Voltage thresholds calculated from the resistors.
        TCFG_ADKEY_VOLTAGE0,
        TCFG_ADKEY_VOLTAGE1,
        TCFG_ADKEY_VOLTAGE2,
        TCFG_ADKEY_VOLTAGE3,
        TCFG_ADKEY_VOLTAGE4,
        TCFG_ADKEY_VOLTAGE5,
        TCFG_ADKEY_VOLTAGE6,
        TCFG_ADKEY_VOLTAGE7,
        TCFG_ADKEY_VOLTAGE8,
        TCFG_ADKEY_VOLTAGE9,
    },
    .key_value = {                                             // TRANSLATED: Key codes for the ADC keys.
        TCFG_ADKEY_VALUE0,
        TCFG_ADKEY_VALUE1,
        TCFG_ADKEY_VALUE2,
        TCFG_ADKEY_VALUE3,
        TCFG_ADKEY_VALUE4,
        TCFG_ADKEY_VALUE5,
        TCFG_ADKEY_VALUE6,
        TCFG_ADKEY_VALUE7,
        TCFG_ADKEY_VALUE8,
        TCFG_ADKEY_VALUE9,
    },
};

/************************** IO KEY ****************************/

/************************** otg data****************************/
#if TCFG_OTG_MODE
struct otg_dev_data otg_data = {
    .usb_dev_en = TCFG_OTG_USB_DEV_EN,
	.slave_online_cnt = TCFG_OTG_SLAVE_ONLINE_CNT,
	.slave_offline_cnt = TCFG_OTG_SLAVE_OFFLINE_CNT,
	.host_online_cnt = TCFG_OTG_HOST_ONLINE_CNT,
	.host_offline_cnt = TCFG_OTG_HOST_OFFLINE_CNT,
	.detect_mode = TCFG_OTG_MODE,
	.detect_time_interval = TCFG_OTG_DET_INTERVAL,
};
#endif
REGISTER_DEVICES(device_table) = {
#if TCFG_OTG_MODE
    { "otg",     &usb_dev_ops, (void *) &otg_data},
#endif
};
void debug_uart_init(const struct uart_platform_data *data)
{
    if (data) {
        uart_init(data);
    } else {
        uart_init(&uart0_data);
    }
}

/* TRANSLATED: Port this function for boards using other chip packages. */
u8 get_power_on_status(void)
{

    if (adkey_data.enable) {
        return 1;
    }

    return 0;
}

static void board_devices_init(void)
{

	key_driver_init();

}

extern void set_dcdc_ctl_port(u8 port);
extern void cfg_file_parse(u8 idx);
void board_init()
{
    cyclone2_latch_early_profile_chord();
    board_power_init();
    adc_vbg_init();
    adc_init();
    cfg_file_parse(0);
    devices_init();

	board_devices_init();

	/*close FAST CHARGE */
	CHARGE_EN(0);
	CHGBG_EN(0);

	log_info("board_init");

	if(get_charge_online_flag()){
		log_info("charge...select LDO15");
		power_set_mode(PWR_LDO15);
	}else{
		power_set_mode(TCFG_LOWPOWER_POWER_SEL);
	}

}

/************************** DAC ****************************/

/************************** ADC ****************************/

/************************** PWR config ****************************/
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

//-----------------------------------------------
static struct port_wakeup port1 = {
	.pullup_down_enable = ENABLE,                            // TRANSLATED: Enable internal I/O pull-up/down resistors.
	.edge               = FALLING_EDGE,                      // TRANSLATED: Wakeup edge: rising or falling.
	.attribute          = BLUETOOTH_RESUME,                  // TRANSLATED: Reserved parameter.
	.iomap              = 0,                                 // TRANSLATED: Wakeup pin selection.
    .filter_enable      = DISABLE,
};

/* TRANSLATED: By default, set I/O pins to high impedance before soft poweroff. Modify this function to preserve selected pin states. */
static void close_gpio(u8 is_softoff)
{
    u16 port_group[] = {
        [PORTA_GROUP] = 0xffff & ~(BIT(13)|BIT(14)),
        [PORTB_GROUP] = 0xffff,
        [PORTC_GROUP] = 0xffff,
    };

    /*
       u8 mode = (JL_SFC->CON >> 8) & 0x0f;
       extern u32 spi_get_port(void);
       if (spi_get_port() != 0) {
       if ((mode == 0b0101) || (mode == 0b0111)) {
       port_group[PORTA_GROUP] = 0x1fff;
       } else {
       port_group[PORTA_GROUP] = 0x9fff;
       }
       }
     */

    port_protect(port_group,TCFG_ADKEY_PORT);

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
extern void dac_power_off(void);

void board_set_soft_poweroff(void)
{
    u16 port_group[] = {
        [PORTA_GROUP] = 0xffff,
        [PORTB_GROUP] = 0xffff,
        [PORTC_GROUP] = 0xffff,
		[PORTD_GROUP] = 0Xffff,
    };

	if(TCFG_LOWPOWER_POWER_SEL == PWR_DCDC15){
		power_set_mode(PWR_LDO15);
	}

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

// TRANSLATED: ADC key / GPIO pins can be retained as wakeup sources.

    port_protect(port_group,TCFG_ADKEY_PORT);

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

	/* dac_power_off(); */
}

#define     APP_IO_DEBUG_0(i,x)       //{JL_PORT##i->DIR &= ~BIT(x), JL_PORT##i->OUT &= ~BIT(x);}
#define     APP_IO_DEBUG_1(i,x)       //{JL_PORT##i->DIR &= ~BIT(x), JL_PORT##i->OUT |= BIT(x);}

void sleep_exit_callback(u32 usec)
{
	putchar('>');
    APP_IO_DEBUG_1(A, 6);

	if(TCFG_LOWPOWER_POWER_SEL == PWR_DCDC15){
		/* putchar('}'); */
		power_set_mode(TCFG_LOWPOWER_POWER_SEL);
	}

}

void sleep_enter_callback(u8  step)
{
    /* TRANSLATED: Do not add debug prints in this function. */
    if (step == 1) {
		putchar('<');
        APP_IO_DEBUG_0(A, 6);
		if(TCFG_LOWPOWER_POWER_SEL == PWR_DCDC15){
			/* putchar('{'); */
			power_set_mode(PWR_LDO15);
		}
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

    /*sdpg_config(1);*/

    power_set_callback(TCFG_LOWPOWER_LOWPOWER_SEL, sleep_enter_callback, sleep_exit_callback, board_set_soft_poweroff);

	power_keep_dacvdd_en(0);

	power_wakeup_init(&wk_param);

}
#endif

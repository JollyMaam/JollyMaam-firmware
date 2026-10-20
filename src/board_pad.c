#include <system/includes.h>
#include <asm/power_interface.h>
#include <asm/power/p33.h>
#include <asm/power/power_port.h>
#include <asm/rtc.h>
#include <asm/wdt.h>
#include <asm/adc_api.h>
#include <asm/mcpwm.h>
#include "app_config.h"
#include "board.h"

/* The Cyclone 2: buttons and d-pad, sticks from the stick MCU, analog
 * triggers with stop switches, the IMU on I2C, 8 LEDs on SPI, two motors. */

#define PIN_HOME        IO_PORTB_11
#define PIN_M           IO_PORTB_07
#define PIN_START       IO_PORTA_14
#define PIN_USB_SENSE   IO_PORTC_03
#define PIN_LED_POWER   IO_PORTB_09
#define PIN_SENSOR_POWER IO_PORTB_05
#define PIN_TRIGGER_L2  IO_PORTB_03
#define PIN_TRIGGER_R2  IO_PORTB_10
#define PIN_AMP_ENABLE  IO_PORTA_08     /* audio amplifier, also on while floating */
#define ADC_TRIGGER_L2  AD_CH_PB3
#define ADC_TRIGGER_R2  AD_CH_PB10
#define PIN_JACK        IO_PORTA_12
#define ADC_JACK        AD_CH_PA12
#define PR_STOP_L2      1
#define PR_STOP_R2      0

extern void led_spi_init(void);
extern void led_spi_rgb_to_24byte(u8 r, u8 g, u8 b, u8 *buffer, int index);

/* ---- buttons: debounced by the platform's tick ---------------------------- */

const struct line board_lines[] = {
    {IO_PORTA_00, PAD_A}, {IO_PORTA_06, PAD_B}, {IO_PORTA_07, PAD_X},
    {IO_PORTA_13, PAD_Y}, {IO_PORTC_07, PAD_L1}, {IO_PORTC_00, PAD_R1},
    {IO_PORTC_01, PAD_L3}, {IO_PORTC_02, PAD_R3}, {IO_PORTA_15, PAD_SELECT},
    {IO_PORTA_14, PAD_START}, {IO_PORTB_11, PAD_HOME}, {IO_PORTD_05, PAD_SCREENSHOT},
    {IO_PORTA_05, PAD_L4}, {IO_PORTC_04, PAD_R4}, {IO_PORTB_07, PAD_M},
    {IO_PORTA_02, PAD_UP}, {IO_PORTA_10, PAD_RIGHT}, {IO_PORTA_04, PAD_DOWN},
    {IO_PORTB_08, PAD_LEFT},
};
const u8 board_line_count = sizeof(board_lines) / sizeof(board_lines[0]);

static u32 buttons_of(u32 down)
{
    u32 i, buttons = 0;

    for (i = 0; i < board_line_count; i++)
        if (down & BIT(i)) buttons |= board_lines[i].bit;
    return buttons;
}

/* ---- I2C: one transfer at a time, a step per byte ---------------------------
 * The BR23 block moves one byte and raises PENDING; each step starts the
 * next.  Steps run from the I2C interrupt, and from the 250 us tick as a
 * fallback should that interrupt not come.  Bus: PB4 SCL, PB6 SDA, 400 kHz. */

#define IIC_ENABLE        BIT(0)
#define IIC_CONFIG_DONE   BIT(2)
#define IIC_DIRECTION_IN  BIT(3)
#define IIC_PRESET_STOP   BIT(4)
#define IIC_PRESET_START  BIT(5)
#define IIC_RECEIVE_NACK  BIT(6)
#define IIC_SENT_NACK     BIT(7)
#define IIC_IE            BIT(8)
#define IIC_STOP_CLEAR    BIT(12)
#define IIC_STOP_PENDING  BIT(13)
#define IIC_PENDING_CLEAR BIT(14)
#define IIC_PENDING       BIT(15)
#define IIC_TIMEOUT_TICKS 8

enum { I2C_IDLE, I2C_ADDR_W, I2C_REG, I2C_ADDR_R, I2C_DATA_IN, I2C_DATA_OUT, I2C_STOP };

static struct {
    volatile u8 phase;
    volatile s8 result;          /* 1 done, 0 busy, -1 failed */
    u8 dev, reg, value, len, n, write, failed;
    u8 *buf;
    u8 ticks;
} i2c;

static HOT void i2c_send(u8 byte, int start)
{
    if (start) JL_IIC->CON0 |= IIC_PRESET_START;
    JL_IIC->CON0 &= ~IIC_DIRECTION_IN;
    JL_IIC->BUF = byte;
    JL_IIC->CON0 |= IIC_CONFIG_DONE;
}

static HOT void i2c_receive(int last)
{
    JL_IIC->CON0 |= IIC_DIRECTION_IN;
    if (last) JL_IIC->CON0 |= IIC_RECEIVE_NACK;
    else JL_IIC->CON0 &= ~IIC_RECEIVE_NACK;
    JL_IIC->BUF = 0xff;
    JL_IIC->CON0 |= IIC_CONFIG_DONE;
}

static HOT void i2c_stop(int failed)
{
    i2c.failed = (u8)failed;
    i2c.phase = I2C_STOP;
    JL_IIC->CON0 |= IIC_PRESET_STOP | IIC_CONFIG_DONE;
}

static HOT void i2c_step(void)
{
    u32 con = JL_IIC->CON0;

    if (i2c.phase == I2C_IDLE) return;
    if (i2c.phase == I2C_STOP) {
        if (!(con & IIC_STOP_PENDING)) return;
        JL_IIC->CON0 |= IIC_STOP_CLEAR;
        i2c.phase = I2C_IDLE;
        i2c.result = i2c.failed ? -1 : 1;
        plat_raise(EV_IMU);
        return;
    }
    if (!(con & IIC_PENDING)) return;
    JL_IIC->CON0 |= IIC_PENDING_CLEAR;
    i2c.ticks = 0;
    if (i2c.phase != I2C_DATA_IN && (con & IIC_SENT_NACK)) {
        i2c_stop(1);
        return;
    }
    switch (i2c.phase) {
    case I2C_ADDR_W:
        i2c.phase = I2C_REG;
        i2c_send(i2c.reg, 0);
        break;
    case I2C_REG:
        if (i2c.write) {
            i2c.phase = I2C_DATA_OUT;
            i2c_send(i2c.value, 0);
        } else {
            i2c.phase = I2C_ADDR_R;
            i2c_send(i2c.dev | 1, 1);
        }
        break;
    case I2C_ADDR_R:
        i2c.phase = I2C_DATA_IN;
        i2c.n = 0;
        i2c_receive(i2c.len == 1);
        break;
    case I2C_DATA_IN:
        i2c.buf[i2c.n++] = (u8)JL_IIC->BUF;
        if (i2c.n < i2c.len) i2c_receive(i2c.n + 1 == i2c.len);
        else i2c_stop(0);
        break;
    case I2C_DATA_OUT:
        i2c_stop(0);
        break;
    }
}

___interrupt
static HOT void i2c_isr(void)
{
    i2c_step();
}

/* From the tick: steps a transfer whose interrupt did not come, and gives up
 * on one that stopped moving. */
HOT void board_tick(void)
{
    if (i2c.phase == I2C_IDLE) return;
    i2c_step();
    if (i2c.phase != I2C_IDLE && ++i2c.ticks > IIC_TIMEOUT_TICKS) {
        JL_IIC->CON0 |= IIC_PENDING_CLEAR | IIC_STOP_CLEAR;
        i2c.phase = I2C_IDLE;
        i2c.result = -1;
        plat_raise(EV_IMU);
    }
}

static void i2c_init(void)
{
    JL_IOMAP->CON1 = (JL_IOMAP->CON1 & ~(3u << 18)) | (2u << 18);
    gpio_set_hd(IO_PORTB_04, 1);
    gpio_set_hd(IO_PORTB_06, 1);
    gpio_set_pull_up(IO_PORTB_04, 0);
    gpio_set_pull_up(IO_PORTB_06, 0);
    gpio_set_pull_down(IO_PORTB_04, 0);
    gpio_set_pull_down(IO_PORTB_06, 0);
    gpio_set_die(IO_PORTB_04, 1);
    gpio_set_die(IO_PORTB_06, 1);
    JL_IIC->CON0 = 0;
    JL_IIC->BAUD = 29;                      /* lsb / (2 * (29 + 1)) = 400 kHz */
    JL_IIC->CON1 |= IIC_PENDING_CLEAR;
    JL_IIC->CON0 = IIC_PENDING_CLEAR | IIC_STOP_CLEAR | IIC_ENABLE;
    request_irq(IRQ_IIC_IDX, 1, i2c_isr, 0);   /* the tick's priority: they never nest */
    JL_IIC->CON0 |= IIC_IE;
}

static void i2c_begin(u8 dev, u8 reg, u8 *buf, u8 len, int write, u8 value)
{
    local_irq_disable();
    i2c.dev = dev;
    i2c.reg = reg;
    i2c.buf = buf;
    i2c.len = len;
    i2c.write = (u8)write;
    i2c.value = value;
    i2c.result = 0;
    i2c.ticks = 0;
    i2c.phase = I2C_ADDR_W;
    i2c_send(dev & 0xfe, 1);
    local_irq_enable();
}

void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len)
{
    i2c_begin(dev, reg, buf, len, 0, 0);
}

void plat_i2c_write(u8 dev, u8 reg, u8 value)
{
    i2c_begin(dev, reg, NULL, 0, 1, value);
}

int plat_i2c_done(void)
{
    return i2c.result;
}

/* ---- sticks: 4-byte frames from the stick MCU on UART2, by DMA ----------- */

#define STICK_BUF   512
#define STICK_CHUNK 32

static u8 stick_dma[STICK_BUF] __attribute__((aligned(4)));
static volatile u32 stick_in;
static u32 stick_out;
static u8 stick_frame[4];
static u8 stick_len;
static u16 stick[4] = { 0x8000, 0x8000, 0x8000, 0x8000 };
static u8 sticks_valid;
static u8 crc_table[256];

___interrupt
static HOT void stick_isr(void)
{
    if ((JL_UART2->CON0 & BIT(3)) && (JL_UART2->CON0 & BIT(14))) {
        JL_UART2->CON0 |= BIT(7) | BIT(10) | BIT(12);
        (void)JL_UART2->HRXCNT;
        stick_in += STICK_CHUNK;
        util_trace(TR_STICK, STICK_CHUNK);
    }
    if ((JL_UART2->CON0 & BIT(5)) && (JL_UART2->CON0 & BIT(11))) {
        JL_UART2->CON0 |= BIT(7) | BIT(10) | BIT(12);
        stick_in += JL_UART2->HRXCNT;
        util_trace(TR_STICK, (u16)(0x8000 | (stick_in & 0x7fff)));
    }
}

static void stick_byte(u8 b)
{
    static const u8 tags[4] = { 0xaa, 0x55, 0xcc, 0x33 };
    u32 i;

    stick_frame[stick_len++] = b;
    if (stick_len < 4) return;
    for (i = 0; i < 4; i++) {
        if (stick_frame[0] == tags[i] &&
            crc_table[crc_table[crc_table[stick_frame[0]] ^ stick_frame[1]] ^ stick_frame[2]] ==
            stick_frame[3]) {
            stick[i] = (u16)(stick_frame[1] << 8 | stick_frame[2]);
            sticks_valid = 1;
            plat_counters.stick_frames++;
            stick_len = 0;
            return;
        }
    }
    plat_counters.stick_resyncs++;
    stick_frame[0] = stick_frame[1];
    stick_frame[1] = stick_frame[2];
    stick_frame[2] = stick_frame[3];
    stick_len = 3;
}

static void sticks_drain(void)
{
    u32 in = stick_in;

    if (in - stick_out > STICK_BUF) {
        stick_out = in - STICK_BUF;
        stick_len = 0;
    }
    while (stick_out != in) stick_byte(stick_dma[stick_out++ & (STICK_BUF - 1)]);
}

static void sticks_init(void)
{
    u32 v, b;

    for (v = 0; v < 256; v++) {
        u8 c = (u8)v;
        for (b = 0; b < 8; b++) c = c & 0x80 ? (u8)(c << 1 ^ 0x31) : (u8)(c << 1);
        crc_table[v] = c;
    }
    gpio_set_uart2(0);
    JL_UART2->CON0 = BIT(13) | BIT(12) | BIT(10);
    request_irq(IRQ_UART2_IDX, 3, stick_isr, 0);
    JL_UART2->RXSADR = (u32)stick_dma;
    JL_UART2->RXEADR = (u32)(stick_dma + STICK_BUF);
    JL_UART2->RXCNT = STICK_CHUNK;
    JL_UART2->CON0 |= BIT(6) | BIT(5) | BIT(3);
    JL_UART2->CON0 &= ~BIT(1);
    JL_UART2->BAUD = clk_get("lsb") / 1000000;
    JL_UART2->OTCNT = 10 * clk_get("lsb") / 1000;
    __asm__ volatile("csync");
    JL_UART2->CON0 |= BIT(13) | BIT(12) | BIT(10) | BIT(0);
}

/* ---- inputs --------------------------------------------------------------- */

static void analog_input(u32 gpio)
{
    gpio_set_pull_down(gpio, 0);
    gpio_set_pull_up(gpio, 0);
    gpio_set_die(gpio, 0);
    gpio_direction_input(gpio);
}

static void stop_switch(u8 pr)
{
    rtc_port_pr_in(pr);
    rtc_port_pr_pd(pr, 0);
    rtc_port_pr_pu(pr, 1);
    rtc_port_pr_die(pr, 1);
}

static void inputs_init(void)
{
    u32 i;

    gpio_set_pull_up(PIN_AMP_ENABLE, 0);
    gpio_set_pull_down(PIN_AMP_ENABLE, 1);
    gpio_direction_output(PIN_AMP_ENABLE, 0);
    for (i = 0; i < board_line_count; i++) plat_pull_up_input(board_lines[i].gpio);
    gpio_set_pull_down(PIN_SENSOR_POWER, 0);
    gpio_set_pull_up(PIN_SENSOR_POWER, 0);
    gpio_set_die(PIN_SENSOR_POWER, 1);
    gpio_direction_output(PIN_SENSOR_POWER, 1);
    analog_input(PIN_TRIGGER_L2);
    analog_input(PIN_TRIGGER_R2);
    adc_add_sample_ch(ADC_TRIGGER_L2);
    adc_add_sample_ch(ADC_TRIGGER_R2);
    analog_input(PIN_JACK);
    adc_add_sample_ch(ADC_JACK);
    stop_switch(PR_STOP_L2);
    stop_switch(PR_STOP_R2);
    sticks_init();
    plat_pull_up_input(IO_PORTA_02);
    i2c_init();    /* d-pad up shares the UART2 input mux */
}

/* PA12 reads near the top of the ADC with a plug in and 0 without
 * (docs/audio.md); stock takes the upper side as inserted too. */
int board_jack(void)
{
    return adc_get_value(ADC_JACK) > 512;
}

static u32 stop_next;
static u8 stops[2];

void plat_input(struct in_raw *raw)
{
    u32 i;

    u32 changed_us;

    raw->buttons = buttons_of(plat_lines(&changed_us));
    sticks_drain();
    for (i = 0; i < 4; i++) raw->stick[i] = stick[i];
    raw->sticks_valid = sticks_valid;
    raw->trigger[0] = adc_get_value(ADC_TRIGGER_L2);
    raw->trigger[1] = adc_get_value(ADC_TRIGGER_R2);
    /* The stop switches sit behind the slow P33 bus: every 4 ms is plenty
     * for a switch at the end of the trigger's travel. */
    if ((s32)(plat_now_ms - stop_next) >= 0) {
        stop_next = plat_now_ms + 4;
        stops[0] = !rtc_port_pr_read(PR_STOP_L2);
        stops[1] = !rtc_port_pr_read(PR_STOP_R2);
    }
    raw->trigger_stop[0] = stops[0];
    raw->trigger_stop[1] = stops[1];
    raw->changed_us = changed_us;
}


/* ---- boot: what woke the chip and what was held --------------------------- */

static struct plat_boot boot;

/* Home+Start with USB at the very start goes to the ROM loader before any
 * of this firmware can hang: the way back from a bad build. */
void board_latch(void)
{
    u32 i;

    for (i = 0; i < board_line_count; i++) plat_pull_up_input(board_lines[i].gpio);
    boot.buttons_held = buttons_of(plat_read_lines());
    gpio_set_pull_up(PIN_USB_SENSE, 0);
    gpio_set_pull_down(PIN_USB_SENSE, 0);
    gpio_set_die(PIN_USB_SENSE, 1);
    gpio_direction_input(PIN_USB_SENSE);
    if ((boot.buttons_held & (PAD_HOME | PAD_START)) == (PAD_HOME | PAD_START) &&
        gpio_read(PIN_USB_SENSE)) {
        plat_enter_loader();
    }
    if (boot.buttons_held & PAD_HOME) boot.cause = BOOT_HOME;
    else if (gpio_read(PIN_USB_SENSE)) boot.cause = BOOT_USB;
    else boot.cause = BOOT_POWER_ON;
}

/* Until power_init(), the lines parked at soft power-off read low: only the
 * loader check uses the early read. */
void plat_boot_info(struct plat_boot *out)
{
    boot.buttons_held = buttons_of(plat_read_lines());
    *out = boot;
}

/* ---- LEDs: a chain of 8 on SPI ------------------------------------------- */

static u8 led_dma[LIGHT_COUNT * 24] __attribute__((aligned(4)));
static u8 led_sending;

static void led_power(int on)
{
    gpio_set_pull_up(PIN_LED_POWER, 0);
    gpio_set_pull_down(PIN_LED_POWER, 0);
    gpio_set_die(PIN_LED_POWER, 1);
    gpio_direction_output(PIN_LED_POWER, on ? 0 : 1);
}

static void leds_init(void)
{
    led_power(0);
    plat_mdelay(10);
    led_spi_init();
    JL_SPI1->BAUD = clk_get("lsb") / 8000000 - 1;   /* the data rate the SDK's send sets each time */
    led_power(1);
    plat_mdelay(20);
}

/* The last frame out: under 0.2 ms after it started. */
static void led_wait(void)
{
    while (led_sending && !(JL_SPI1->CON & BIT(15)))
        ;
    led_sending = 0;
}

/* The SDK's send clocks a 128 us reset at 1 MHz first and waits out the
 * 192 us of data, every frame; dithering refreshes often.  This starts the
 * data and returns.  The line rests low between frames, milliseconds apart,
 * which is the chain's reset; whether the chain needs the SDK's leading
 * reset as well is not established. */
void plat_leds(const struct led_frame *f)
{
    u32 i;

    led_wait();
    for (i = 0; i < LIGHT_COUNT; i++)
        led_spi_rgb_to_24byte(f->light[i].r, f->light[i].g, f->light[i].b, led_dma, i);
    plat_flush_dcache(led_dma, sizeof(led_dma));
    JL_SPI1->CON |= BIT(14);     /* clear the last frame's done */
    JL_SPI1->ADR = (u32)led_dma;
    JL_SPI1->CNT = sizeof(led_dma);
    led_sending = 1;
}

/* ---- motors: PWM at 25 kHz on PA9 (left) and PB0 (right) ----------------- */

static const struct { u32 pin; pwm_ch_num_type ch; pwm_timer_num_type timer; } motor[2] = {
    { IO_PORTA_09, pwm_ch4, pwm_timer4 }, { IO_PORTB_00, pwm_ch1, pwm_timer1 },
};
static u8 motor_level[2] = { 1, 1 };

static void motor_set(u32 i, u8 level)
{
    struct pwm_platform_data cfg;

    if (level == motor_level[i]) return;
    motor_level[i] = level;
    if (!level) {
        mcpwm_ch_open_or_close(motor[i].ch, 0);
        JL_MCPWM->MCPWM_CON0 &= ~BIT((u32)motor[i].timer + 8);
        gpio_set_pull_up(motor[i].pin, 0);
        gpio_set_pull_down(motor[i].pin, 0);
        gpio_set_die(motor[i].pin, 1);
        gpio_direction_output(motor[i].pin, 0);
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.frequency = 25000;
    cfg.pwm_ch_num = motor[i].ch;
    cfg.pwm_timer_num = motor[i].timer;
    cfg.duty = (u32)level * 10000 / 255;
    cfg.pwm_aligned_mode = pwm_edge_aligned;
    cfg.h_pin = (u8)motor[i].pin;
    cfg.l_pin = (u8)-1;
    mcpwm_init(&cfg);
}

void plat_motors(u8 left, u8 right)
{
    motor_set(0, left);
    motor_set(1, right);
}


/* ---- the rest of the board interface --------------------------------------- */

void board_start(void)
{
    leds_init();
    inputs_init();
    plat_motors(0, 0);
}

/* The lights hold the loader scene's last frame (leds.c) while the loader
 * runs: let it finish going out. */
void board_to_loader(void)
{
    led_wait();
}

int board_usb_sense(void)
{
    return gpio_read(PIN_USB_SENSE);
}

void plat_power_off(void)
{
    struct led_frame dark;

    plat_watch_stop();
    plat_audio_off();
    memset(&dark, 0, sizeof(dark));
    plat_leds(&dark);
    led_wait();
    power_set_soft_poweroff();
}

const u8 board_audio = 1;
const u32 board_amp_enable = PIN_AMP_ENABLE;

const u32 board_keep_pins[] = { PIN_HOME, IO_PORTA_00, IO_PORTA_06, IO_PORTA_07,
                                IO_PORTA_13, PIN_USB_SENSE };
const u8 board_keep_count = sizeof(board_keep_pins) / sizeof(board_keep_pins[0]);

static struct port_wakeup home_wakeup = {
    .pullup_down_enable = ENABLE, .edge = FALLING_EDGE,
    .attribute = BLUETOOTH_RESUME, .iomap = PIN_HOME, .filter_enable = ENABLE,
};
static struct port_wakeup usb_wakeup_port = {
    .pullup_down_enable = DISABLE, .edge = RISING_EDGE,
    .attribute = BLUETOOTH_RESUME, .iomap = PIN_USB_SENSE, .filter_enable = ENABLE,
};
static const struct sub_wakeup sub_wkup = { .attribute = BLUETOOTH_RESUME };
static const struct charge_wakeup charge_wkup = { .attribute = BLUETOOTH_RESUME };
static const struct wakeup_param wakeups = {
    .port[1] = &home_wakeup, .port[2] = &usb_wakeup_port,
    .sub = &sub_wkup, .charge = &charge_wkup,
};
const struct wakeup_param *const board_wakeups = &wakeups;

#include <system/includes.h>
#include <asm/power_interface.h>
#include <asm/power/p33.h>
#include <asm/power/power_port.h>
#include <asm/charge.h>
#include <asm/clock.h>
#include <asm/rtc.h>
#include <asm/wdt.h>
#include <asm/adc_api.h>
#include <usb/otg.h>
#include <usb/scsi.h>
#include <usb/device/msd.h>
#include <usb/device/usb_stack.h>
#include "app_config.h"
#include "pad.h"

#define HOT __attribute__((section(".volatile_ram_code")))

#define PIN_HOME        IO_PORTB_11
#define PIN_START       IO_PORTA_14
#define PIN_USB_SENSE   IO_PORTC_03
#define PIN_LED_POWER   IO_PORTB_09
#define PIN_SENSOR_POWER IO_PORTB_05
#define PIN_TRIGGER_L2  IO_PORTB_03
#define PIN_TRIGGER_R2  IO_PORTB_10
#define ADC_TRIGGER_L2  AD_CH_PB3
#define ADC_TRIGGER_R2  AD_CH_PB10
#define PR_STOP_L2      1
#define PR_STOP_R2      0

#define TICK_US         250
#define DEBOUNCE_TICKS  8

#define USB_CON0        (*(volatile u32 *)0x001e1800)
#define USB_CON1        (*(volatile u32 *)0x001e1804)
#define USB_IO_CON0     (*(volatile u32 *)0x001e5100)
#define USB_FORCE_SE0   BIT(11)
#define MASKROM_TOKEN   ((volatile u8 *)0x00010800)
#define USB_VENDOR_OP   0xf5
#define USB_SOF_STALE_MS 250

extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);
extern void nvram_set_boot_state(u32 state);
extern void flush_dcache(const void *address, u32 size);
extern void flushinv_dcache(const void *address, u32 size);
extern void led_spi_init(void);
extern void led_spi_rgb_to_24byte(u8 r, u8 g, u8 b, u8 *buffer, int index);
extern void led_spi_send_rgbbuf(u8 *buffer, u16 count);
extern u32 msd_mcu2usb(const struct usb_device_t *usb_device, const u8 *buffer, u32 len);
extern u8 *get_norflash_uuid(void) __attribute__((weak));

const struct task_info task_info_table[] = {
    {"app_core",  1, 0, 256,  64  },
    {"ctl",       3, 0, 1024, 0   },
    {"sys_event", 7, 0, 256,  0   },
    {"btctrler",  4, 0, 512,  256 },
    {"btencry",   1, 0, 512,  128 },
    {"btstack",   2, 0, 2048, 256 },
    {"systimer",  7, 0, 128,  0   },
    {"update",    1, 0, 512,  0   },
    {0, 0},
};

/* ---- MaskROM calls: out of range of a direct CALL from flash ------------- */

static void rom_mdelay(u32 ms)
{
    void (* volatile f)(u32) = (void (*)(u32))0x00111524;
    f(ms);
}

static void rom_flush_dcache(const void *address, u32 size)
{
    void (* volatile f)(const void *, u32) = flush_dcache;
    f(address, size);
}

/* ---- retained RAM: survives a soft reset, not a power-off ---------------- */

static volatile struct { u32 magic; u8 slot, check, pad[2]; } retained_slot
    __attribute__((section(".reset_mailbox"), used, aligned(4)));

u8 plat_retained_slot(void)
{
    if (retained_slot.magic != 0x43324c53 || retained_slot.check != (u8)~retained_slot.slot ||
        retained_slot.slot >= SLOTS) return SLOT_NONE;
    return retained_slot.slot;
}

void plat_retain_slot(u8 slot)
{
    retained_slot.slot = slot;
    retained_slot.check = (u8)~slot;
    retained_slot.magic = 0x43324c53;
}

/* ---- buttons: sampled and debounced in the 250 us timer interrupt -------- */

struct line { u32 gpio; u16 bit; };

static const struct line lines[] = {
    {IO_PORTA_00, PAD_A}, {IO_PORTA_06, PAD_B}, {IO_PORTA_07, PAD_X},
    {IO_PORTA_13, PAD_Y}, {IO_PORTC_07, PAD_L1}, {IO_PORTC_00, PAD_R1},
    {IO_PORTC_01, PAD_L3}, {IO_PORTC_02, PAD_R3}, {IO_PORTA_15, PAD_SELECT},
    {IO_PORTA_14, PAD_START}, {IO_PORTB_11, PAD_HOME}, {IO_PORTD_05, PAD_SCREENSHOT},
    {IO_PORTA_05, PAD_L4}, {IO_PORTC_04, PAD_R4}, {IO_PORTB_07, PAD_M},
    /* the d-pad: bit 0 up, 1 right, 2 down, 3 left */
    {IO_PORTA_02, 1 << 0}, {IO_PORTA_10, 1 << 1}, {IO_PORTA_04, 1 << 2},
    {IO_PORTB_08, 1 << 3},
};
#define BUTTON_LINES 15
#define LINE_COUNT (sizeof(lines) / sizeof(lines[0]))

static volatile u32 ms_now;
static u8 quarter;
static u32 line_state;
static u8 line_lock[LINE_COUNT];
static volatile u32 events;
static volatile u32 deadline;
static volatile u8 deadline_set;
static volatile u8 posted;
static OS_SEM wake_sem;

static HOT u32 read_lines(void)
{
    u32 port[4] = { JL_PORTA->IN, JL_PORTB->IN, JL_PORTC->IN, JL_PORTD->IN };
    u32 down = 0;
    u32 i;

    for (i = 0; i < LINE_COUNT; i++)
        if (!(port[lines[i].gpio / 16] & BIT(lines[i].gpio % 16))) down |= BIT(i);
    return down;
}

static HOT void raise(u32 ev)
{
    events |= ev;
    if (!posted) {
        posted = 1;
        os_sem_post(&wake_sem);
    }
}

___interrupt
static HOT void tick_isr(void)
{
    u32 raw;
    u32 changed = 0;
    u32 i;

    JL_TIMER3->CON |= BIT(14);
    if (++quarter == 4) {
        quarter = 0;
        ms_now++;
    }
    raw = read_lines();
    for (i = 0; i < LINE_COUNT; i++) {
        if (line_lock[i]) {
            line_lock[i]--;
        } else if ((raw ^ line_state) & BIT(i)) {
            line_state ^= BIT(i);
            line_lock[i] = DEBOUNCE_TICKS;
            changed = 1;
        }
    }
    if (changed) raise(EV_BUTTONS);
    if (deadline_set && (s32)(ms_now - deadline) >= 0) {
        deadline_set = 0;
        raise(EV_DEADLINE);
    }
}

static void tick_start(void)
{
    line_state = read_lines();
    os_sem_create(&wake_sem, 0);
    JL_TIMER3->CON = BIT(14);
    JL_TIMER3->CNT = 0;
    JL_TIMER3->PRD = 24 * TICK_US;
    request_irq(IRQ_TIME3_IDX, 1, tick_isr, 0);
    JL_TIMER3->CON = BIT(0) | BIT(3);
}

u32 plat_ms(void)
{
    return ms_now;
}

void plat_lock(void)
{
    local_irq_disable();
}

void plat_unlock(void)
{
    local_irq_enable();
}

u32 plat_wait(u32 deadline_ms)
{
    u32 ev;

    for (;;) {
        local_irq_disable();
        ev = events;
        events = 0;
        posted = 0;
        if (!ev && deadline_ms && (s32)(ms_now - deadline_ms) >= 0) ev = EV_DEADLINE;
        if (!ev) {
            deadline = deadline_ms;
            deadline_set = deadline_ms != 0;
        }
        local_irq_enable();
        if (ev) return ev;
        os_sem_pend(&wake_sem, 100);
    }
}

static u8 hat_from(u32 d)
{
    static const u8 hat[16] = {
        HAT_NEUTRAL, HAT_UP, HAT_RIGHT, HAT_UP_RIGHT, HAT_DOWN, HAT_NEUTRAL,
        HAT_DOWN_RIGHT, HAT_RIGHT, HAT_LEFT, HAT_UP_LEFT, HAT_NEUTRAL, HAT_UP,
        HAT_DOWN_LEFT, HAT_LEFT, HAT_DOWN, HAT_NEUTRAL,
    };
    return hat[d & 15];
}

static void split_lines(u32 down, u16 *buttons, u8 *hat)
{
    u32 i;

    *buttons = 0;
    for (i = 0; i < BUTTON_LINES; i++)
        if (down & BIT(i)) *buttons |= lines[i].bit;
    *hat = hat_from(down >> BUTTON_LINES);
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
    }
    if ((JL_UART2->CON0 & BIT(5)) && (JL_UART2->CON0 & BIT(11))) {
        JL_UART2->CON0 |= BIT(7) | BIT(10) | BIT(12);
        stick_in += JL_UART2->HRXCNT;
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
            stick_len = 0;
            return;
        }
    }
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

static void pull_up_input(u32 gpio)
{
    gpio_set_pull_down(gpio, 0);
    gpio_set_pull_up(gpio, 1);
    gpio_set_die(gpio, 1);
    gpio_direction_input(gpio);
}

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

    for (i = 0; i < LINE_COUNT; i++) pull_up_input(lines[i].gpio);
    gpio_set_pull_down(PIN_SENSOR_POWER, 0);
    gpio_set_pull_up(PIN_SENSOR_POWER, 0);
    gpio_set_die(PIN_SENSOR_POWER, 1);
    gpio_direction_output(PIN_SENSOR_POWER, 1);
    analog_input(PIN_TRIGGER_L2);
    analog_input(PIN_TRIGGER_R2);
    adc_add_sample_ch(ADC_TRIGGER_L2);
    adc_add_sample_ch(ADC_TRIGGER_R2);
    stop_switch(PR_STOP_L2);
    stop_switch(PR_STOP_R2);
    sticks_init();
    pull_up_input(IO_PORTA_02);    /* d-pad up shares the UART2 input mux */
}

void plat_input(struct in_raw *raw)
{
    u32 i;

    split_lines(line_state, &raw->buttons, &raw->hat);
    sticks_drain();
    for (i = 0; i < 4; i++) raw->stick[i] = stick[i];
    raw->sticks_valid = sticks_valid;
    raw->trigger[0] = adc_get_value(ADC_TRIGGER_L2);
    raw->trigger[1] = adc_get_value(ADC_TRIGGER_R2);
    raw->trigger_stop[0] = !rtc_port_pr_read(PR_STOP_L2);
    raw->trigger_stop[1] = !rtc_port_pr_read(PR_STOP_R2);
}

u16 plat_battery_mv(void)
{
    return adc_get_voltage(AD_CH_VBAT) * 4;
}

const u8 *plat_unique_id(void)
{
    return get_norflash_uuid ? get_norflash_uuid() : NULL;
}

/* ---- boot: what woke the chip and what was held --------------------------- */

static struct plat_boot boot;

static void arm_loader_token(void);

/* Home+Start with USB at the very start goes to the ROM loader before any
 * of this firmware can hang: the way back from a bad build. */
static void boot_latch(void)
{
    u32 i;

    for (i = 0; i < LINE_COUNT; i++) pull_up_input(lines[i].gpio);
    split_lines(read_lines(), &boot.buttons_held, &boot.hat_held);
    gpio_set_pull_up(PIN_USB_SENSE, 0);
    gpio_set_pull_down(PIN_USB_SENSE, 0);
    gpio_set_die(PIN_USB_SENSE, 1);
    gpio_direction_input(PIN_USB_SENSE);
    if ((boot.buttons_held & (PAD_HOME | PAD_START)) == (PAD_HOME | PAD_START) &&
        gpio_read(PIN_USB_SENSE)) {
        local_irq_disable();
        arm_loader_token();
        wdt_init(WDT_256MS);
        for (;;) {
        }
    }
    if (boot.buttons_held & PAD_HOME) boot.cause = BOOT_HOME;
    else if (gpio_read(PIN_USB_SENSE)) boot.cause = BOOT_USB;
    else boot.cause = BOOT_POWER_ON;
}

void plat_boot_info(struct plat_boot *out)
{
    *out = boot;
}

/* ---- LEDs: a chain of 8 on SPI ------------------------------------------- */

static u8 led_dma[LIGHT_COUNT * 24] __attribute__((aligned(4)));

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
    rom_mdelay(10);
    led_spi_init();
    led_power(1);
    rom_mdelay(20);
}

void plat_leds(const struct led_frame *f)
{
    u32 i;

    for (i = 0; i < LIGHT_COUNT; i++)
        led_spi_rgb_to_24byte(f->light[i].r, f->light[i].g, f->light[i].b, led_dma, i);
    rom_flush_dcache(led_dma, sizeof(led_dma));
    led_spi_send_rgbbuf(led_dma, LIGHT_COUNT);
}

void plat_motors(u8 left, u8 right)
{
    (void)left;
    (void)right;
}

/* ---- USB: mass storage with no medium; vendor commands to usb.c ----------- */

static const struct usb_device_t *volatile usb_pending;
static u8 usb_buf[512] __attribute__((aligned(4)));

static void usb_wakeup(struct usb_device_t *usb_device)
{
    usb_pending = usb_device;
    raise(EV_USB);
}

u32 private_scsi_cmd(const struct usb_device_t *usb_device, struct usb_scsi_cbw *cbw)
{
    const u8 *cdb = &cbw->operationCode;
    u32 len = cbw->dCBWDataTransferLength;

    if (cdb[0] != USB_VENDOR_OP && cdb[0] != 0xfb && cdb[0] != 0xfc && cdb[0] != 0xfd)
        return 0;
    if (len > sizeof(usb_buf)) len = sizeof(usb_buf);
    memset(usb_buf, 0, len);
    usb_command(cdb, usb_buf, len);
    if ((cbw->bmCBWFlags & 0x80) && len) msd_mcu2usb(usb_device, usb_buf, len);
    return 1;
}

void plat_usb_service(void)
{
    const struct usb_device_t *dev = usb_pending;

    if (!dev) return;
    usb_pending = NULL;
    USB_MassStorage(dev);
}

int plat_usb_present(void)
{
    static u16 frame;
    static u32 seen_ms;
    static u8 started;
    u16 f = usb_read_sofframe(USB0);

    if (!started) {
        started = 1;
        frame = f;
        seen_ms = ms_now - USB_SOF_STALE_MS;
    } else if (f != frame) {
        frame = f;
        seen_ms = ms_now;
    }
    return ms_now - seen_ms < USB_SOF_STALE_MS;
}

static void usb_init(void)
{
    usb_device_mode(0, MASSSTORAGE_CLASS);
    msd_set_wakeup_handle(usb_wakeup);
}

/* ---- power ---------------------------------------------------------------- */

static void arm_loader_token(void)
{
    nvram_set_boot_state(2);
    __asm__ volatile("csync");
    flushinv_dcache((const void *)MASKROM_TOKEN, 16);
}

void plat_reboot_loader(void)
{
    static const struct led_frame green = { .light = {
        {0, 16, 0}, {0, 16, 0}, {0, 16, 0}, {0, 16, 0},
        {0, 16, 0}, {0, 16, 0}, {0, 16, 0}, {0, 16, 0},
    } };

    plat_leds(&green);
    rom_mdelay(100);
    local_irq_disable();
    USB_CON0 = 0;
    USB_CON1 = 0;
    USB_IO_CON0 = USB_FORCE_SE0;
    rom_mdelay(1000);
    USB_IO_CON0 = 0;
    rom_mdelay(250);
    while (!gpio_read(PIN_USB_SENSE)) rom_mdelay(100);
    arm_loader_token();
    wdt_init(WDT_256MS);
    for (;;) {
    }
}

void plat_reboot(void)
{
    p33_soft_reset();
}

void plat_power_off(void)
{
    struct led_frame dark;

    memset(&dark, 0, sizeof(dark));
    plat_leds(&dark);
    power_set_soft_poweroff();
}

static void board_set_soft_poweroff(void)
{
    static const u32 keep[] = { PIN_HOME, IO_PORTA_00, IO_PORTA_06, IO_PORTA_07,
                                IO_PORTA_13, PIN_USB_SENSE };
    u16 group[4] = { 0xffff, 0xffff, 0xffff, 0xffff };
    u32 i;

    if (spi_get_port() == 0) {
        port_protect(group, SPI0_PWR_A);
        port_protect(group, SPI0_CS_A);
        port_protect(group, SPI0_CLK_A);
        port_protect(group, SPI0_DO_D0_A);
        port_protect(group, SPI0_DI_D1_A);
        if (get_sfc_bit_mode() == 4) {
            port_protect(group, SPI0_WP_D2_A);
            port_protect(group, SPI0_HOLD_D3_A);
        }
    } else {
        port_protect(group, SPI0_PWR_B);
        port_protect(group, SPI0_CS_B);
        port_protect(group, SPI0_CLK_B);
        port_protect(group, SPI0_DO_D0_B);
        port_protect(group, SPI0_DI_D1_B);
        if (get_sfc_bit_mode() == 4) {
            port_protect(group, SPI0_WP_D2_B);
            port_protect(group, SPI0_HOLD_D3_B);
        }
    }
    for (i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) port_protect(group, keep[i]);
    gpio_dir(GPIOA, 0, 16, group[0], GPIO_OR);
    gpio_set_pu(GPIOA, 0, 16, ~group[0], GPIO_AND);
    gpio_set_pd(GPIOA, 0, 16, ~group[0], GPIO_AND);
    gpio_die(GPIOA, 0, 16, ~group[0], GPIO_AND);
    gpio_dieh(GPIOA, 0, 16, ~group[0], GPIO_AND);
    gpio_dir(GPIOB, 0, 16, group[1], GPIO_OR);
    gpio_set_pu(GPIOB, 0, 16, ~group[1], GPIO_AND);
    gpio_set_pd(GPIOB, 0, 16, ~group[1], GPIO_AND);
    gpio_die(GPIOB, 0, 16, ~group[1], GPIO_AND);
    gpio_dieh(GPIOB, 0, 16, ~group[1], GPIO_AND);
    gpio_dir(GPIOC, 0, 16, group[2], GPIO_OR);
    gpio_set_pu(GPIOC, 0, 16, ~group[2], GPIO_AND);
    gpio_set_pd(GPIOC, 0, 16, ~group[2], GPIO_AND);
    gpio_die(GPIOC, 0, 16, ~group[2], GPIO_AND);
    gpio_dieh(GPIOC, 0, 16, ~group[2], GPIO_AND);
    gpio_dir(GPIOD, 0, 16, group[3], GPIO_OR);
    gpio_set_pu(GPIOD, 0, 16, ~group[3], GPIO_AND);
    gpio_set_pd(GPIOD, 0, 16, ~group[3], GPIO_AND);
    gpio_die(GPIOD, 0, 16, ~group[3], GPIO_AND);
    gpio_dieh(GPIOD, 0, 16, ~group[3], GPIO_AND);
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
    gpio_set_pull_up(PIN_USB_SENSE, 0);
    gpio_set_pull_down(PIN_USB_SENSE, 0);
    gpio_direction_input(PIN_USB_SENSE);
    gpio_set_die(PIN_USB_SENSE, 1);
}

static void sleep_enter(u8 step)
{
    (void)step;
}

static void sleep_exit(u32 usec)
{
    (void)usec;
}

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
static const struct wakeup_param wk_param = {
    .port[1] = &home_wakeup, .port[2] = &usb_wakeup_port,
    .sub = &sub_wkup, .charge = &charge_wkup,
};
static const struct low_power_param power_param = {
    .config = TCFG_LOWPOWER_LOWPOWER_SEL, .btosc_hz = TCFG_CLOCK_OSC_HZ,
    .delay_us = TCFG_CLOCK_SYS_HZ / 1000000L, .btosc_disable = TCFG_LOWPOWER_BTOSC_DISABLE,
    .vddiom_lev = TCFG_LOWPOWER_VDDIOM_LEVEL, .vddiow_lev = TCFG_LOWPOWER_VDDIOW_LEVEL,
    .osc_type = OSC_TYPE_LRC, .dcdc_port = TCFG_DCDC_PORT_SEL,
};

/* ---- start-up --------------------------------------------------------------- */

void board_init(void)
{
    boot_latch();
    power_init(&power_param);
    gpio_longpress_pin0_reset_config(IO_PORTB_01, 0, 0);
    gpio_shortpress_reset_config(0);
    power_set_callback(TCFG_LOWPOWER_LOWPOWER_SEL, sleep_enter, sleep_exit,
                       board_set_soft_poweroff);
    power_keep_dacvdd_en(0);
    power_wakeup_init(&wk_param);
    adc_vbg_init();
    adc_init();
    devices_init();
    CHARGE_EN(0);
    CHGBG_EN(0);
    power_set_mode(PWR_LDO15);
}

static void ctl_thread(void *arg)
{
    (void)arg;
    clk_set("sys", BT_NORMAL_HZ);
    bt_pll_para(TCFG_CLOCK_OSC_HZ, clk_get("sys"), 0, 0);
    util_log_init();
    leds_init();
    inputs_init();
    usb_init();
    tick_start();
    ctl_main();
}

void app_main(void)
{
    task_create(ctl_thread, NULL, "ctl");
}

#include "sdk.h"
#include "board.h"

/* 8BitDo Ultimate 2C Wireless: a Telink TLSR8208 (B80).  Pins are in
 * docs/8bitdo-ultimate2c-wireless/README.md.  The scanners run from the
 * platform's wait loop, each in one go when due: the matrix in about 20 us,
 * the six ADC channels in about 0.3 ms.  A step per pass of the loop made a
 * matrix scan take up to 64 ms with a busy link (the debounce dropped quick
 * taps), and the sticks were older still. */

const char board_name[] = "Ultimate 2C Wireless";

#include "settings_defaults.inc"

/* ---- the button matrix: a row driven low, the columns pulled up ---------- */

#define ROWS 4
#define COLUMNS 5
/* A held key's column reads the same with 1 us as with 20 us, and its
 * neighbour in the next row never ghosted: 2 us. */
#define ROW_SETTLE_US 2
#define SCAN_GAP_US 1000             /* from one full scan to the next */
#define DEBOUNCE_SCANS 2             /* a release: up this many scans in a row */

static const GPIO_PinTypeDef rows[ROWS] = { GPIO_PD6, GPIO_PA0, GPIO_PC0, GPIO_PA3 };
static const GPIO_PinTypeDef columns[COLUMNS] = { GPIO_PA5, GPIO_PC1, GPIO_PA6, GPIO_PC3, GPIO_PA7 };
#define HOME GPIO_PA4                /* on its own, active low */
static u8 boot_home;                 /* Home down as the board started */

/* Each position's PAD_* bit, row by row; 0: not a button (Sync and the
 * rear mode switch, which are the board's own). */
static const u32 keys[ROWS][COLUMNS] = {
    { PAD_UP, PAD_NORTH, PAD_START, PAD_R4, PAD_L4 },
    { PAD_LEFT, PAD_WEST, PAD_M, PAD_R1, PAD_L1 },             /* M: the Turbo button */
    { PAD_DOWN, PAD_EAST, PAD_SELECT, PAD_R3, PAD_L3 },
    { PAD_RIGHT, PAD_SOUTH, PAD_SCREENSHOT, 0, 0 },
};

static struct {
    u32 scanned;                     /* clock_time(): the last full scan ended */
    u32 reading;                     /* PAD_* this scan */
    u32 stable;                      /* debounced */
    u32 changed_us;
    u8 same[32];                     /* scans a bit has differed from stable */
    u8 pair_reading, pair_stable, pair_same;
    /* the rear switch closed: Bluetooth; open: 2.4G (README) */
    u8 bt_reading, bt_stable, bt_same;
} mx;

/* A board switch debounced as a release is: DEBOUNCE_SCANS alike. */
static u32 debounce(u8 *reading, u8 *stable, u8 *same)
{
    u32 changed = 0;

    if (*reading == *stable) *same = 0;
    else if (++*same >= DEBOUNCE_SCANS) {
        *stable = *reading;
        *same = 0;
        changed = 1;
    }
    *reading = 0;
    return changed;
}

static void matrix_init(void)
{
    u32 i;

    for (i = 0; i < ROWS; i++) {
        gpio_write(rows[i], 1);      /* high before it drives */
        gpio_set_func(rows[i], AS_GPIO);
        gpio_set_output_en(rows[i], 1);
        gpio_set_input_en(rows[i], 0);
    }
    for (i = 0; i <= COLUMNS; i++) {
        GPIO_PinTypeDef pin = i < COLUMNS ? columns[i] : HOME;

        gpio_set_output_en(pin, 0);
        gpio_set_func(pin, AS_GPIO);
        gpio_set_input_en(pin, 1);
        gpio_setup_up_down_resistor(pin, PM_PIN_PULLUP_10K);
    }
    mx.scanned = clock_time();
}

static u32 matrix_poll(void)
{
    u32 i, r, b, changed = 0;

    if (!clock_time_exceed(mx.scanned, SCAN_GAP_US)) return 0;
    for (r = 0; r < ROWS; r++) {
        gpio_write(rows[r], 0);
        sleep_us(ROW_SETTLE_US);
        for (i = 0; i < COLUMNS; i++) {
            if (gpio_read(columns[i])) continue;
            mx.reading |= keys[r][i];
            if (r == 3 && i == 3) mx.pair_reading = 1;
            if (r == 3 && i == 4) mx.bt_reading = 1;
        }
        gpio_write(rows[r], 1);
    }
    mx.scanned = clock_time();
    if (!gpio_read(HOME)) mx.reading |= PAD_HOME;
    /* A press counts at once; a release after DEBOUNCE_SCANS, so a bounce
     * while held cannot end the press. */
    for (i = 0; i < 32; i++) {
        b = 1u << i;
        if (!((mx.reading ^ mx.stable) & b)) {
            mx.same[i] = 0;
        } else if ((mx.reading & b) || ++mx.same[i] >= DEBOUNCE_SCANS) {
            mx.stable ^= b;
            mx.same[i] = 0;
            changed = 1;
        }
    }
    mx.reading = 0;
    changed |= debounce(&mx.pair_reading, &mx.pair_stable, &mx.pair_same);
    changed |= debounce(&mx.bt_reading, &mx.bt_stable, &mx.bt_same);
    if (changed) mx.changed_us = plat_us();
    return changed ? EV_BUTTONS : 0;
}

/* ---- sticks, triggers and battery: one shared ADC ------------------------ */

#define AXES 6
#define BATTERY_CHANNEL AXES
#define BATTERY_LEVEL_EMPTY_MV 3500
#define BATTERY_LEVEL_FULL_MV 4100
#define BATTERY_LOW_MV 3500
#define BATTERY_OKAY_MV 3600
#define CHARGE_STATUS GPIO_PD1
#define BATTERY_TICK_SHIFT 24    /* a 16 MHz timer tick every ~1.05 seconds */
/* With no settling at all, R2 read 2 codes low next to a fully pulled L2
 * (1650 codes away), at rest within the noise: 5 us. */
#define MUX_SETTLE_US 5
#define SAMPLE_TIMEOUT_US 1000       /* the four samples take about 42 us */
#define SWEEP_PERIOD_US 1600         /* from one sweep's start to the next: as fresh as
                                        with 50 us settles and a 1 ms gap after each */
#define SWEEP_DEMAND_US 8000         /* the same while reports read them for themselves */
#define MOVED 16                     /* codes an axis moves to wake the loop; rest noise is ~2 */
#define DMA_EMPTY 0xa5a5a5a5u

/* LX LY RX RY L2 R2 battery; channel = pin number + 1. */
static const adc_input_pin_def_e axis_pins[AXES + 1] = {
    ADC_GPIO_PB0, ADC_GPIO_PB1, ADC_GPIO_PB4, ADC_GPIO_PB5, ADC_GPIO_PB2, ADC_GPIO_PB3, ADC_GPIO_PB6,
};
static const u8 axis_channels[AXES + 1] = { 1, 2, 5, 6, 3, 4, 7 };

static volatile u32 dma[4] __attribute__((aligned(16)));

static struct {
    u8 valid;
    u8 battery_tick;                 /* clock_time() >> BATTERY_TICK_SHIFT */
    u16 battery_mv;                  /* uses the padding before tick */
    u32 tick;                        /* clock_time(): the last sweep started */
    u16 code[AXES];                  /* 13-bit, the middle two of four samples */
    u16 woke[AXES];                  /* the codes at the last EV_STICKS */
} ad;

static void adc_start(void)
{
    u32 i;

    adc_init();
    for (i = 0; i <= AXES; i++) {
        adc_base_pin_init(axis_pins[i]);
        gpio_setup_up_down_resistor(axis_pins[i] & 0xfff, PM_PIN_UP_DOWN_FLOAT);
    }
    adc_set_vref_vbat_divider(ADC_VBAT_DIVIDER_OFF);
    adc_set_ain_pre_scaler(ADC_PRESCALER_1);   /* as stock: analog 0xfa = 0x3d */
    adc_power_on_sar_adc(1);
    ad.tick = clock_time();
}

static int adc_read(u32 channel, u16 *out)
{
    u16 sorted[4];
    u32 i, j, start, complete;

    adc_set_ain_chn_misc(channel, GND);
    sleep_us(MUX_SETTLE_US);
    adc_reset_adc_module();
    for (i = 0; i < 4; i++) dma[i] = DMA_EMPTY;
    adc_config_misc_channel_buf((u16 *)dma, sizeof(dma));
    dfifo_enable_dfifo2();
    start = clock_time();
    do {
        complete = 1;
        for (i = 0; i < 4; i++)
            if (dma[i] == DMA_EMPTY) complete = 0;
    } while (!complete && !clock_time_exceed(start, SAMPLE_TIMEOUT_US));
    dfifo_disable_dfifo2();
    if (!complete) return 0;
    for (i = 0; i < 4; i++) {
        u16 c = dma[i] & (1u << 13) ? 0 : (u16)(dma[i] & 0x1fff);   /* negative: 0 */

        for (j = i; j > 0 && sorted[j - 1] > c; j--) sorted[j] = sorted[j - 1];
        sorted[j] = c;
    }
    *out = (u16)((sorted[1] + sorted[2]) / 2);
    return 1;
}

/* Match stock's two voltage truncations, then apply this board's warning
 * hysteresis and approximate 3.5--4.1 V level scale. No charger control. */
static void battery_sample(void)
{
    struct battery_status b = {0};
    u16 code;
    u32 mv = 0;

    if (adc_read(axis_channels[BATTERY_CHANNEL], &code)) {
        ad.valid |= (u8)(1u << BATTERY_CHANNEL);
        mv = ((u32)code * 1175) >> 13;
    } else ad.valid &= (u8)~(1u << BATTERY_CHANNEL);
    b.flags = ad.valid & 0x80 ? JM_BATTERY_LOW : 0;
    battery_voltage(&b, (u16)(mv * 37 / 10), 0, 0);
    battery_low(&b, BATTERY_LOW_MV, BATTERY_OKAY_MV);
    ad.battery_mv = b.mv;
    ad.valid = (ad.valid & 0x7f) | (b.flags & JM_BATTERY_LOW ? 0x80 : 0);
    ad.battery_tick = (u8)(clock_time() >> BATTERY_TICK_SHIFT);
}

/* A sweep that moved an axis by MOVED wakes the control loop, as a button
 * does, so the movement need not wait for its next poll. */
static u32 sweep(void)
{
    u32 axis, moved = 0;

    ad.tick = clock_time();
    for (axis = 0; axis < AXES; axis++) {
        if (adc_read(axis_channels[axis], &ad.code[axis])) ad.valid |= (u8)(1u << axis);
        else ad.valid &= (u8)~(1u << axis);
        if (ad.code[axis] > ad.woke[axis] + MOVED || ad.code[axis] + MOVED < ad.woke[axis]) moved = 1;
    }
    if (moved) memcpy(ad.woke, ad.code, sizeof(ad.woke));
    return moved ? EV_STICKS : 0;
}

void board_sweep(u32 max_age_us)
{
    if (clock_time_exceed(ad.tick, max_age_us)) sweep();
}

/* ---- motors and lights ---------------------------------------------------
 * PWM at 24 kHz (silent for the motors), duty 0-1000; PF1 cannot take a
 * PWM channel and is on or off.  Each light is one colour: lit at the
 * strongest of its frame colour's components. */

#define PWM_CLOCK_HZ 24000000        /* whatever the CPU clock */
#define PWM_CYCLE 1000               /* 24 MHz PWM clock: 24 kHz */

enum { OUT_MOTOR_LEFT, OUT_MOTOR_RIGHT, OUT_LIGHT };
static const struct output {
    GPIO_PinTypeDef pin;
    gpio_func_e func;                /* its PWM channel; AS_GPIO: on or off */
    pwm_id id;
    u8 kind, light;                  /* OUT_*; LIGHT_*: the frame's light it shows */
} outputs[] = {
    { GPIO_PD3, PWM0, PWM0_ID, OUT_MOTOR_LEFT, 0 },
    { GPIO_PD2, PWM1, PWM1_ID, OUT_MOTOR_RIGHT, 0 },
    { GPIO_PB7, PWM2, PWM2_ID, OUT_LIGHT, LIGHT_HOME },      /* the Home ring */
    { GPIO_PF1, AS_GPIO, PWM0_ID, OUT_LIGHT, LIGHT_P0 },     /* under Home; the boot loader blinks it */
    { GPIO_PD5, PWM3, PWM3_ID, OUT_LIGHT, LIGHT_P1 },        /* red, beside the USB port */
};
#define OUTPUTS (sizeof(outputs) / sizeof(outputs[0]))

static void output_set(const struct output *o, u32 level)        /* 0-255 */
{
    if (o->func == AS_GPIO) gpio_write(o->pin, level != 0);
    else pwm_set_cycle_and_duty(o->id, PWM_CYCLE, (u16)(level * PWM_CYCLE / 255));
}

static void outputs_init(void)
{
    u32 i;

    reg_clk_en0 |= FLD_CLK0_PWM_EN;
    pwm_set_clk(CLOCK_SYS_CLOCK_HZ, PWM_CLOCK_HZ);
    for (i = 0; i < OUTPUTS; i++) {
        const struct output *o = &outputs[i];

        gpio_write(o->pin, 0);
        gpio_setup_up_down_resistor(o->pin, PM_PIN_UP_DOWN_FLOAT);   /* sleep's pull-down */
        gpio_set_input_en(o->pin, 0);
        gpio_set_output_en(o->pin, 1);
        gpio_set_func(o->pin, o->func);
        if (o->func == AS_GPIO) continue;
        pwm_set_mode(o->id, PWM_NORMAL_MODE);
        output_set(o, 0);
        pwm_start(o->id);
    }
}

void plat_leds(const struct led_frame *f)
{
    u32 i;

    for (i = 0; i < OUTPUTS; i++) {
        const struct rgb *c = &f->light[outputs[i].light];
        u32 v = c->r > c->g ? c->r : c->g;

        if (outputs[i].kind != OUT_LIGHT) continue;
        output_set(&outputs[i], v > c->b ? v : c->b);
    }
}

void plat_motors(u8 left, u8 right)
{
    u32 i;

    for (i = 0; i < OUTPUTS; i++)
        if (outputs[i].kind != OUT_LIGHT)
            output_set(&outputs[i], outputs[i].kind == OUT_MOTOR_LEFT ? left : right);
}

/* ---- the board's interface to the platform ------------------------------- */

static u8 started;                   /* board_init() ran: the OTP as it is */

void board_init(void)
{
    started = 1;                     /* the OTP as it is from here (below) */
    /* Stock holds PD7 high.  Home powers the board only while held unless
     * this output keeps the battery supply on.  Set the latch before
     * enabling the output so start-up never drives it low. */
    gpio_write(GPIO_PD7, 1);
    gpio_set_func(GPIO_PD7, AS_GPIO);
    gpio_set_input_en(GPIO_PD7, 0);
    gpio_set_output_en(GPIO_PD7, 1);
    /* Stock's power-off path tests PD0 and keeps the supply on while high. */
    gpio_set_func(GPIO_PD0, AS_GPIO);
    gpio_set_output_en(GPIO_PD0, 0);
    gpio_set_input_en(GPIO_PD0, 1);
    gpio_setup_up_down_resistor(GPIO_PD0, PM_PIN_UP_DOWN_FLOAT);
    gpio_set_func(CHARGE_STATUS, AS_GPIO);
    gpio_set_output_en(CHARGE_STATUS, 0);
    gpio_set_input_en(CHARGE_STATUS, 1);
    gpio_setup_up_down_resistor(CHARGE_STATUS, PM_PIN_UP_DOWN_FLOAT);
    outputs_init();
    matrix_init();
    sleep_us(20);                    /* the pull-up */
    boot_home = !gpio_read(HOME);
    gpio_write(rows[3], 0);          /* the rear switch as it is, before any scan */
    sleep_us(ROW_SETTLE_US);
    mx.bt_stable = !gpio_read(columns[4]);
    gpio_write(rows[3], 1);
    adc_start();
    battery_sample();
}

/* The keys down now, the matrix read in one go (matrix_poll spreads it
 * over the loop). */
static u32 held_now(void)
{
    u32 r, i, b = gpio_read(HOME) ? 0 : PAD_HOME;

    for (r = 0; r < ROWS; r++) {
        gpio_write(rows[r], 0);
        sleep_us(ROW_SETTLE_US);
        for (i = 0; i < COLUMNS; i++)
            if (!gpio_read(columns[i])) b |= keys[r][i];
        gpio_write(rows[r], 1);
    }
    return b;
}

/* Home supplies power while held: a boot with it down is Home's.  Otherwise
 * a cable powered the pad up, or it restarted (not told apart). */
void plat_boot_info(struct plat_boot *out)
{
    out->cause = boot_home ? BOOT_HOME : board_usb_present() ? BOOT_USB : BOOT_POWER_ON;
    out->buttons_held = held_now() | (boot_home ? PAD_HOME : 0);
}

int board_usb_present(void)
{
    return gpio_read(GPIO_PD0) != 0;
}

/* Power-off releases PD7 after Home is up. U2CW_DEEP_SLEEP keeps the supply
 * held and asks the chip to sleep instead: Home low or USB power high wakes
 * it through a reset. NO_DEEP_SLEEP keeps probe/profiler builds in power-off.
 * A failed sleep falls through to releasing the hold. */
void board_power_off(void)
{
    u32 i;

    /* Off, with pull-downs ready for the low-power pin state. */
    for (i = 0; i < OUTPUTS; i++) {
        output_set(&outputs[i], 0);
        gpio_setup_up_down_resistor(outputs[i].pin, PM_PIN_PULLDOWN_100K);
    }
    dfifo_disable_dfifo2();
    adc_power_on_sar_adc(0);
    usb_dp_pullup_en(0);
    irq_disable();
    wd_stop();
    while (!gpio_read(HOME)) if (board_usb_present()) start_reboot();
#if defined(U2CW_DEEP_SLEEP) && !defined(NO_DEEP_SLEEP)
    if (!board_usb_present()) {
        gpio_setup_up_down_resistor(GPIO_PD7, PM_PIN_PULLUP_10K);
        cpu_set_gpio_wakeup(HOME, Level_Low, 1);
        cpu_set_gpio_wakeup(GPIO_PD0, Level_High, 1);
        blc_pm_select_internal_32k_crystal();
        cpu_sleep_wakeup(DEEPSLEEP_MODE, PM_WAKEUP_PAD, 0);
    }
#endif
    gpio_write(GPIO_PD7, 0);
    gpio_setup_up_down_resistor(GPIO_PD7, PM_PIN_UP_DOWN_FLOAT);
    for (;;) if (board_usb_present()) start_reboot();
}

u32 board_poll(int sticks_on_demand)
{
    u32 e = matrix_poll();

    if (ad.battery_tick != (u8)(clock_time() >> BATTERY_TICK_SHIFT)) {
        battery_sample();
    }
    if (clock_time_exceed(ad.tick, sticks_on_demand ? SWEEP_DEMAND_US : SWEEP_PERIOD_US)) e |= sweep();
    return e;
}

/* Return a cached sensor sample and the live charger-activity input.
 * PD1 is active low; high means not charging, not necessarily full. */
void board_battery(struct battery_status *out)
{
    out->flags = ad.valid & 0x80 ? JM_BATTERY_LOW : 0;
    battery_voltage(out, ad.battery_mv, BATTERY_LEVEL_EMPTY_MV, BATTERY_LEVEL_FULL_MV);
    battery_low(out, BATTERY_LOW_MV, BATTERY_OKAY_MV);
    battery_charge(out, board_usb_present(), !gpio_read(CHARGE_STATUS));
}

static u16 clamp10(s32 v)
{
    return (u16)(v < 0 ? 0 : v > 1023 ? 1023 : v);
}

/* The Cyclone 2's raw scales and directions (input_calibration.c): sticks
 * near 0x4000 at rest, the left one growing right and down, the right one
 * left and up; triggers in 10 bits, falling as they are pressed.  Codes
 * here: sticks 1300-6600 about 4000 at rest, right X growing right; triggers
 * about 3650 at rest and 2100 pressed in. */
void board_input(struct in_raw *raw)
{
    raw->buttons = mx.stable;
    raw->pair_button = mx.pair_stable;
    raw->radio_24g = !mx.bt_stable;
    raw->changed_us = mx.changed_us;
    raw->sticks_valid = (ad.valid & 0x0f) == 0x0f;
    raw->stick[0] = (u16)(ad.code[0] << 2);
    raw->stick[1] = (u16)(ad.code[1] << 2);
    raw->stick[2] = (u16)((8191 - ad.code[2]) << 2);
    raw->stick[3] = (u16)(ad.code[3] << 2);
    raw->trigger[0] = clamp10(((s32)ad.code[4] - 1900) / 2);
    raw->trigger[1] = clamp10(((s32)ad.code[5] - 1900) / 2);
    raw->trigger_stop[0] = raw->trigger_stop[1] = 0;
}

/* ---- this pad's OTP --------------------------------------------------------
 * Its OTP word at 0x3fb4 reads 0x10000000, which the stock boot loader runs
 * flash firmware with, but the SDK's cpu_wakeup_init() wants 0xffffffff
 * there and resets the chip otherwise.  Linked with --wrap=otp_read: until
 * board_init() (start-up), only that word, with that value, reads as the
 * SDK wants; after it, every read is the OTP as it is. */

void __real_otp_read(unsigned int address, unsigned int words, unsigned int *buffer);

void __wrap_otp_read(unsigned int address, unsigned int words, unsigned int *buffer)
{
    __real_otp_read(address, words, buffer);
    if (!started && address == 0x3fb4 && words == 1 && buffer[0] == 0x10000000) buffer[0] = 0xffffffff;
}
